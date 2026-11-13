// Phase 6 of docs/V1_BDD_CHECKLIST.md: teardown and locking are race-free (run by the TSan job).
// Item 6.3 (P1-1, ports of quickfix/quickfix#772, #773, #774, #781, pulled forward for Phase 3):
// FIX::Mutex is a std::recursive_mutex. The old one read its owner and count unsynchronised, so any
// two threads sharing a session lock raised TSan reports; a copy also copied its lock state. The
// session flags and the last sent time, read by the reading thread without the session lock, are
// atomic.
// Rows 6.1 and 6.2 (P0-11): a connection that closes itself and is deleted while gateway threads
// send went around Session::disconnect(): the session kept its logon state, never called onLogout,
// and cleared its responder without the lock a sender holds while it writes through it.
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <catch_amalgamated.hpp>

#include "Application.h"
#include "Exceptions.h"
#include "FieldTypes.h"
#include "FixFields.h"
#include "Message.h"
#include "Mutex.h"
#include "SessionPair.h"
#include "SessionSettings.h"
#include "SessionState.h"
#include "ThreadedSocketAcceptor.h"
#include "Utility.h"

namespace {

constexpr int kIncrements = 20000;
constexpr int kSenders = 4;
constexpr std::chrono::seconds kWait{5};

void LogOn(krx::SessionPair& pair) {
  pair.Write(pair.Logon(1));
  REQUIRE(pair.WaitLoggedOn(kWait));
  REQUIRE((krx::MsgTypeOf(pair.ReadFrame(kWait)) == "A"));
}

// An order from the gateway (a BusinessMessageReject stands in for it).
FIX::Message Order() {
  FIX::Message order;
  order.getHeader().setField(FIX::MsgType("j"));
  order.setField(FIX::RefMsgType("D"));
  order.setField(FIX::BusinessRejectReason(0));
  return order;
}

// kSenders gateway threads sending orders through `pair` without pause until Stop().
class Senders {
 public:
  explicit Senders(krx::SessionPair& pair) {
    threads_.reserve(kSenders);
    for (int i = 0; i < kSenders; ++i) {
      threads_.emplace_back([this, &pair] {
        while (!stop_.load()) {
          pair.Send(Order());
          ++sends_;
        }
      });
    }
  }

  // Returns once they have sent `count` orders between them.
  void WaitForSends(int count) const {
    while (sends_.load() < count) {
      std::this_thread::yield();
    }
  }

  void Stop() {
    stop_.store(true);
    for (std::thread& thread : threads_) {
      thread.join();
    }
    threads_.clear();
  }

  ~Senders() { Stop(); }

  Senders(const Senders&) = delete;
  Senders& operator=(const Senders&) = delete;
  Senders(Senders&&) = delete;
  Senders& operator=(Senders&&) = delete;

 private:
  std::atomic<bool> stop_{false};
  std::atomic<int> sends_{0};
  std::vector<std::thread> threads_;
};

// A UTC timestamp to nanosecond precision.
FIX::UtcTimeStamp At(int hour, int minute, int second, int nanos) {
  return {FIX::DateTime(2026, 11, 12, hour, minute, second, nanos, 9)};
}

// Sets every racy field kIncrements times; the last pass leaves the session logged out.
void SetStateRepeatedly(FIX::SessionState& state) {
  for (int i = 0; i < kIncrements; ++i) {
    const bool on = (i % 2) == 0;
    state.receivedLogon(on);
    state.sentLogon(on);
    state.sentLogout(!on);
    state.enabled(on);
    state.lastSentTime(At(9, 0, 0, i));
  }
}

struct Reads {
  int whole_times{0};  // timestamps of the writer's day, never torn
  int logged_on{0};    // reads that saw both logon flags set
};

// A port no one listens on now.
int FreePort() {
  const FIX::socket_handle probe = FIX::socket_createAcceptor(0, true);
  const int port = static_cast<std::uint16_t>(FIX::socket_hostport(probe));  // a short: 32768+
  FIX::socket_close(probe);
  return port;
}

// Whether a new listening socket can take `port` now (no reuse: an open listener holds it).
bool CanListenOn(int port) {
  const FIX::socket_handle socket = FIX::socket_createAcceptor(port, false);
  if (!FIX::socket_isValid(socket)) {
    return false;
  }
  FIX::socket_close(socket);
  return true;
}

// How many file descriptors the process has open.
int OpenDescriptors() {
  int open = 0;
  for (int descriptor = 0; descriptor < 4096; ++descriptor) {
    struct stat status{};
    open += fstat(descriptor, &status) == 0 ? 1 : 0;
  }
  return open;
}

// Settings for a threaded acceptor with one session on each of `ports`.
FIX::SessionSettings AcceptorOn(const std::vector<int>& ports) {
  std::string text =
      "[DEFAULT]\nConnectionType=acceptor\nSocketReuseAddress=Y\nStartTime=00:00:00\n"
      "EndTime=00:00:00\nUseDataDictionary=N\n";
  for (const int port : ports) {
    text += "[SESSION]\nBeginString=FIX.4.4\nSenderCompID=KRXGW\nTargetCompID=KRXSTOP" +
            std::to_string(port) + "\nSocketAcceptPort=" + std::to_string(port) + "\n";
  }
  std::istringstream stream(text);
  FIX::SessionSettings settings;
  stream >> settings;
  return settings;
}

// Reads every racy field kIncrements times, as the reading thread does without the session lock.
Reads ReadStateRepeatedly(const FIX::SessionState& state) {
  const int day = At(9, 0, 0, 0).m_date;
  Reads reads;
  for (int i = 0; i < kIncrements; ++i) {
    reads.logged_on += (state.receivedLogon() && state.sentLogon()) ? 1 : 0;
    reads.whole_times += (state.lastSentTime().m_date == day) ? 1 : 0;
  }
  return reads;
}

}  // namespace

SCENARIO("The last sent time gives back exactly what was stored", "[krx][v1][tsan][p1-1]") {
  GIVEN("a session state") {
    FIX::SessionState state(FIX::UtcTimeStamp::now());
    WHEN("times at the start, middle and end of a day are stored one after another") {
      const FIX::UtcTimeStamp midnight = At(0, 0, 0, 1);
      const FIX::UtcTimeStamp afternoon = At(13, 45, 30, 123456789);
      const FIX::UtcTimeStamp last = At(23, 59, 59, 999999999);
      state.lastSentTime(midnight);
      const bool kept_midnight = state.lastSentTime() == midnight;
      state.lastSentTime(afternoon);
      const bool kept_afternoon = state.lastSentTime() == afternoon;
      state.lastSentTime(last);
      const bool kept_last = state.lastSentTime() == last;
      THEN("each comes back to the nanosecond") {
        REQUIRE((kept_midnight && kept_afternoon && kept_last));
      }
    }
  }
}

SCENARIO("Session flags and the last sent time can be read while another thread sets them",
         "[krx][v1][tsan][p1-1]") {
  GIVEN("a session state whose last sent time is on the writer's day") {
    FIX::SessionState state(At(9, 0, 0, 0));
    WHEN("one thread sets the flags and the time while another reads them") {
      std::thread writer([&] { SetStateRepeatedly(state); });
      const Reads reads = ReadStateRepeatedly(state);
      writer.join();
      THEN("every read is whole and the last values stand (TSan checks the reads in between)") {
        INFO(reads.whole_times << " whole, " << reads.logged_on << " logged on");
        REQUIRE((reads.whole_times == kIncrements && !state.receivedLogon() && state.sentLogout() &&
                 state.lastSentTime() == At(9, 0, 0, kIncrements - 1)));
      }
    }
  }
}

SCENARIO("Two threads taking a mutex recursively never lose an increment",
         "[krx][v1][tsan][p1-1]") {
  GIVEN("one mutex and a counter") {
    FIX::Mutex mutex;
    int counter = 0;
    WHEN("two threads each lock it twice around every increment") {
      const auto increment = [&] {
        for (int i = 0; i < kIncrements; ++i) {
          const FIX::Locker outer(mutex);
          const FIX::Locker inner(mutex);
          ++counter;
        }
      };
      std::thread first(increment);
      std::thread second(increment);
      first.join();
      second.join();
      THEN("every increment counts") {
        INFO(counter);
        REQUIRE((counter == 2 * kIncrements));
      }
    }
  }
}

SCENARIO("A mutex locked twice is free only after the outer lock goes", "[krx][v1][tsan][p1-1]") {
  GIVEN("a thread holding a mutex twice, and another thread waiting for it") {
    FIX::Mutex mutex;
    WHEN("the inner lock is released, then the outer one") {
      std::promise<void> acquired;
      const auto entered = acquired.get_future();
      bool entered_early = false;
      std::thread contender;
      {
        const FIX::Locker outer(mutex);
        {
          const FIX::Locker inner(mutex);
          contender = std::thread([&] {
            const FIX::Locker lock(mutex);
            acquired.set_value();
          });
        }
        entered_early =
            entered.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready;
      }
      const bool entered_after =
          entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
      contender.join();
      THEN("the other thread gets in only after the outer lock is released too") {
        REQUIRE((!entered_early && entered_after));
      }
    }
  }
}

SCENARIO("Many senders while the connection drops raise no data race and no use after free",
         "[krx][v1][tsan][p0-11]") {
  GIVEN("a logged-on session and four gateway threads sending orders without pause") {
    krx::SessionPair pair("KRXTD61");
    LogOn(pair);
    Senders senders(pair);
    WHEN("the connection closes itself and is deleted while they send") {
      senders.WaitForSends(100);
      pair.DropConnection();
      const bool sent_after = pair.Send(Order());
      senders.Stop();
      THEN("every sender returns, and an order after the drop reports not sent") {
        INFO(pair.Events().Dump());
        REQUIRE_FALSE(sent_after);
      }
    }
  }
}

SCENARIO("A dropped connection always clears the session's logon state", "[krx][v1][p0-11]") {
  GIVEN("a logged-on session") {
    krx::SessionPair pair("KRXTD62");
    LogOn(pair);
    WHEN("the connection closes itself and is deleted") {
      pair.DropConnection();
      THEN("the gateway is told the session logged out") {
        INFO(pair.Events().Dump());
        REQUIRE_FALSE(pair.LoggedOn());
      }
    }
  }
}

SCENARIO("onLogout runs after the session lock is released", "[krx][v1][p1-2]") {
  GIVEN("a logged-on session whose gateway sends an order from another thread in onLogout") {
    krx::SessionPair pair("KRXLO64");
    LogOn(pair);
    pair.ProbeDuringLogout([&pair] { pair.Send(Order()); });
    WHEN("the gateway disconnects the session") {
      pair.Disconnect();
      THEN("the other thread's send returns while onLogout is still running") {
        INFO(pair.Events().Dump());
        REQUIRE((!pair.LoggedOn() && pair.ProbeReturned()));
      }
    }
  }
}

SCENARIO("Repeated forced and graceful acceptor stops leave no port or descriptor behind",
         "[krx][v1][tsan][p1-7]") {
  GIVEN("a threaded acceptor listening on four ports") {
    const std::vector<int> ports{FreePort(), FreePort(), FreePort(), FreePort()};
    const FIX::SessionSettings settings = AcceptorOn(ports);
    FIX::NullApplication application;
    FIX::MemoryStoreFactory store;
    const int open_before = OpenDescriptors();
    WHEN("it is started and stopped at once 40 times, forced and graceful in turn") {
      int failed_starts = 0;
      for (int i = 0; i < 40; ++i) {
        FIX::ThreadedSocketAcceptor acceptor(application, store, settings);
        try {
          acceptor.start();
        } catch (const FIX::RuntimeError&) {
          ++failed_starts;  // a port still held by a listener from an earlier round
        }
        acceptor.stop(i % 2 == 0);
      }
      THEN("every start could listen, and every port and descriptor is free again") {
        const bool ports_free = std::ranges::all_of(ports, CanListenOn);
        const int open_after = OpenDescriptors();
        INFO(failed_starts << " failed starts; descriptors " << open_before << " -> "
                           << open_after);
        REQUIRE((failed_starts == 0 && ports_free && open_after == open_before));
      }
    }
  }
}

SCENARIO("A copy of a locked mutex is a new, unlocked mutex", "[krx][v1][tsan][p1-1]") {
  GIVEN("a mutex this thread holds, and a copy of it") {
    FIX::Mutex mutex;
    const FIX::Locker held(mutex);
    FIX::Mutex copy(mutex);
    WHEN("another thread locks the copy") {
      std::promise<void> acquired;
      const auto entered = acquired.get_future();
      std::thread contender([&] {
        const FIX::Locker lock(copy);
        acquired.set_value();
      });
      const bool got_it = entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
      contender.join();
      THEN("it gets it at once") {
        REQUIRE(got_it);
      }
    }
  }
}
