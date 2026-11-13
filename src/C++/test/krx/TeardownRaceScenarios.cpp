// Phase 6 of docs/V1_BDD_CHECKLIST.md: teardown and locking are race-free (run by the TSan job).
// Item 6.3 (P1-1, ports of quickfix/quickfix#772, #773, #774, #781, pulled forward for Phase 3):
// FIX::Mutex is a std::recursive_mutex. The old one read its owner and count unsynchronised, so any
// two threads sharing a session lock raised TSan reports; a copy also copied its lock state. The
// session flags and the last sent time, read by the reading thread without the session lock, are
// atomic.
// Rows 6.1 and 6.2 (P0-11): a connection that closes itself and is deleted while gateway threads
// send went around Session::disconnect(): the session kept its logon state, never called onLogout,
// and cleared its responder without the lock a sender holds while it writes through it.
// Row 6.4 (P1-2): onLogout runs without the session lock. Row 6.5 (P1-7, quickfix/quickfix#740):
// stop() joins the thread running onStart before onStop. Row 6.6 (P2-3): a connection never takes
// a session of another port, and a stop's wait for logouts sleeps between checks.
#include <poll.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
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
#include "ThreadedSocketInitiator.h"
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
    // U-4 (8.3): what Session::disconnect() from another thread resets.
    state.receivedReset(on);
    state.sentReset(!on);
    state.resendRange(on ? i + 1 : 0, on ? i + 2 : 0);
  }
}

struct Reads {
  int whole_times{0};   // timestamps of the writer's day, never torn
  int logged_on{0};     // reads that saw both logon flags set
  int whole_ranges{0};  // resend ranges the writer set, never half of one and half of another
  int reset_flags{0};   // reads that saw a reset flag set
};

// A port no one listens on now.
int FreePort() {
  const FIX::socket_handle probe = FIX::socket_createAcceptor(0, true);
  const int port = static_cast<std::uint16_t>(FIX::socket_hostport(probe));  // a short: 32768+
  FIX::socket_close(probe);
  return port;
}

// `count` different ports no one listens on now: all probes are open while the ports are picked,
// so the system cannot hand out one twice (FreePort() twice could: a duplicate session, CI #61).
std::vector<int> FreePorts(std::size_t count) {
  std::vector<FIX::socket_handle> probes;
  std::vector<int> ports;
  for (std::size_t i = 0; i < count; ++i) {
    probes.push_back(FIX::socket_createAcceptor(0, true));
    ports.push_back(static_cast<std::uint16_t>(FIX::socket_hostport(probes.back())));
  }
  for (const FIX::socket_handle probe : probes) {
    FIX::socket_close(probe);
  }
  return ports;
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

// The gateway's threaded acceptor with one session on each of `ports` (KRXGW and KRXSTOP<port>),
// started; its events (every session's too) are kept.
class StartedAcceptor {
 public:
  explicit StartedAcceptor(const std::vector<int>& ports)
      : settings_(AcceptorOn(ports)), acceptor_(application_, store_, settings_, logs_) {
    acceptor_.start();
  }

  ~StartedAcceptor() { Stop(); }

  StartedAcceptor(const StartedAcceptor&) = delete;
  StartedAcceptor& operator=(const StartedAcceptor&) = delete;
  StartedAcceptor(StartedAcceptor&&) = delete;
  StartedAcceptor& operator=(StartedAcceptor&&) = delete;

  // The gateway stops it without waiting for counterparties to log out.
  void Stop() { acceptor_.stop(true); }

  [[nodiscard]] const krx::EventLog& Events() const { return logs_.Events(); }

 private:
  FIX::NullApplication application_;
  FIX::MemoryStoreFactory store_;
  krx::EventLogFactory logs_;
  FIX::SessionSettings settings_;
  FIX::ThreadedSocketAcceptor acceptor_;
};

// A Logon (MsgSeqNum 1, HeartBtInt 30) from KRXSTOP<port> to the gateway.
std::string LogonFor(int port) {
  FIX::Message logon;
  FIX::Header& header = logon.getHeader();
  header.setField(FIX::BeginString("FIX.4.4"));
  header.setField(FIX::MsgType("A"));
  header.setField(FIX::SenderCompID("KRXSTOP" + std::to_string(port)));
  header.setField(FIX::TargetCompID("KRXGW"));
  header.setField(FIX::MsgSeqNum(1));
  header.setField(FIX::SendingTime(FIX::UtcTimeStamp::now()));
  logon.setField(FIX::EncryptMethod(0));
  logon.setField(FIX::HeartBtInt(30));
  return logon.toString();
}

// A counterparty's connection to the gateway's `port`, which sends `bytes` at once.
class Counterparty {
 public:
  Counterparty(int port, const std::string& bytes) : socket_(FIX::socket_createConnector()) {
    FIX::socket_connect(socket_, "127.0.0.1", port);
    FIX::socket_send(socket_, bytes.data(), bytes.size());
  }

  ~Counterparty() { FIX::socket_close(socket_); }

  Counterparty(const Counterparty&) = delete;
  Counterparty& operator=(const Counterparty&) = delete;
  Counterparty(Counterparty&&) = delete;
  Counterparty& operator=(Counterparty&&) = delete;

  // Everything the gateway sends until it closes the connection or `timeout` passes.
  [[nodiscard]] std::string Received(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string received;
    while (std::chrono::steady_clock::now() < deadline) {
      pollfd ready{.fd = socket_, .events = POLLIN, .revents = 0};
      if (poll(&ready, 1, 50) <= 0) {
        continue;
      }
      std::array<char, 4096> buffer{};
      const ssize_t got = FIX::socket_recv(socket_, buffer.data(), buffer.size());
      if (got <= 0) {
        break;
      }
      received.append(buffer.data(), static_cast<std::size_t>(got));
    }
    return received;
  }

 private:
  FIX::socket_handle socket_;
};

// Process CPU time (user and system) used so far.
std::chrono::microseconds CpuTime() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  const auto seconds = usage.ru_utime.tv_sec + usage.ru_stime.tv_sec;
  const auto micros = usage.ru_utime.tv_usec + usage.ru_stime.tv_usec;
  return std::chrono::seconds(seconds) + std::chrono::microseconds(micros);
}

// An exchange on `port` that answers the gateway's Logon and then never answers anything, a
// Logout included; it reads (and drops) what the gateway sends until the gateway closes.
class SilentExchange {
 public:
  explicit SilentExchange(int port)
      : listener_(FIX::socket_createAcceptor(port, true)), thread_([this] { Serve(); }) {}

  ~SilentExchange() {
    stop_.store(true);
    thread_.join();
    FIX::socket_close(listener_);
  }

  SilentExchange(const SilentExchange&) = delete;
  SilentExchange& operator=(const SilentExchange&) = delete;
  SilentExchange(SilentExchange&&) = delete;
  SilentExchange& operator=(SilentExchange&&) = delete;

 private:
  // True once `socket` has data, false after 50 ms or when the exchange stops.
  [[nodiscard]] bool Readable(FIX::socket_handle socket) const {
    pollfd ready{.fd = socket, .events = POLLIN, .revents = 0};
    return !stop_.load() && poll(&ready, 1, 50) > 0;
  }

  void Serve() {
    while (!stop_.load() && !Readable(listener_)) {
    }
    if (stop_.load()) {
      return;
    }
    const FIX::socket_handle connection = FIX::socket_accept(listener_);
    std::string received;
    bool answered = false;
    std::array<char, 4096> buffer{};
    while (!stop_.load()) {
      if (!Readable(connection)) {
        continue;
      }
      const ssize_t got = FIX::socket_recv(connection, buffer.data(), buffer.size());
      if (got <= 0) {
        break;
      }
      received.append(buffer.data(), static_cast<std::size_t>(got));
      if (!answered && received.contains("\00135=A\001")) {
        const std::string logon = Logon();
        FIX::socket_send(connection, logon.c_str(), logon.size());
        answered = true;
      }
    }
    FIX::socket_close(connection);
  }

  // The exchange's Logon (MsgSeqNum 1, HeartBtInt 30) to the gateway.
  static std::string Logon() {
    FIX::Message logon;
    FIX::Header& header = logon.getHeader();
    header.setField(FIX::BeginString("FIX.4.4"));
    header.setField(FIX::MsgType("A"));
    header.setField(FIX::SenderCompID("KRXEXCH"));
    header.setField(FIX::TargetCompID("KRXGWI"));
    header.setField(FIX::MsgSeqNum(1));
    header.setField(FIX::SendingTime(FIX::UtcTimeStamp::now()));
    logon.setField(FIX::EncryptMethod(0));
    logon.setField(FIX::HeartBtInt(30));
    return logon.toString();
  }

  FIX::socket_handle listener_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

// Settings for the gateway's threaded initiator (KRXGWI to KRXEXCH) connecting to `port`.
FIX::SessionSettings InitiatorTo(int port) {
  std::istringstream stream(
      "[DEFAULT]\nConnectionType=initiator\nSocketConnectHost=127.0.0.1\nSocketConnectPort=" +
      std::to_string(port) +
      "\nHeartBtInt=30\nReconnectInterval=1\nStartTime=00:00:00\nEndTime=00:00:00\n"
      "UseDataDictionary=N\n[SESSION]\nBeginString=FIX.4.4\nSenderCompID=KRXGWI\n"
      "TargetCompID=KRXEXCH\n");
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
    const FIX::SessionState::ResendRange range = state.resendRange();
    const bool whole = (range.first == 0 && range.second == 0) || range.second == range.first + 1;
    reads.whole_ranges += whole ? 1 : 0;
    reads.reset_flags += (state.receivedReset() || state.sentReset()) ? 1 : 0;
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
         "[krx][v1][tsan][p1-1][u-4]") {
  GIVEN("a session state whose last sent time is on the writer's day") {
    FIX::SessionState state(At(9, 0, 0, 0));
    WHEN("one thread sets the flags and the time while another reads them") {
      std::thread writer([&] { SetStateRepeatedly(state); });
      const Reads reads = ReadStateRepeatedly(state);
      writer.join();
      THEN("every read is whole and the last values stand (TSan checks the reads in between)") {
        INFO(reads.whole_times << " whole times, " << reads.whole_ranges << " whole ranges, "
                               << reads.logged_on << " logged on, " << reads.reset_flags
                               << " reset");
        REQUIRE((reads.whole_times == kIncrements && reads.whole_ranges == kIncrements &&
                 !state.receivedLogon() && state.sentLogout() && !state.receivedReset() &&
                 state.sentReset() && !state.resendRequested() &&
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
    const std::vector<int> ports = FreePorts(4);
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

SCENARIO("A connection that fails to attach to its session leaves the session free",
         "[krx][v1][security][p2-3]") {
  GIVEN("an acceptor with one counterparty's session on one port and another's on a second port") {
    const std::vector<int> ports = FreePorts(2);
    const StartedAcceptor gateway(ports);
    WHEN("the first counterparty logs on twice in one write on the second port, then on its own") {
      const std::string logon = LogonFor(ports[0]);
      const std::string wrong_port = Counterparty(ports[1], logon + logon).Received(kWait);
      const std::string own_port = Counterparty(ports[0], logon).Received(std::chrono::seconds(1));
      THEN("the wrong port never reaches the session, which then logs on on its own port") {
        INFO("events:\n" << gateway.Events().Dump());
        REQUIRE((wrong_port.empty() && own_port.contains("\00135=A\001")));
      }
    }
  }
}

SCENARIO("A forced stop waiting for a counterparty to log out does not spin a CPU",
         "[krx][v1][p2-3]") {
  GIVEN("an acceptor with a logged-on counterparty that never answers a Logout") {
    const std::vector<int> ports{FreePort()};
    StartedAcceptor gateway(ports);
    const Counterparty exchange(ports[0], LogonFor(ports[0]));
    REQUIRE(gateway.Events().WaitFor("Responding to logon request", kWait));
    WHEN("the gateway stops the acceptor without waiting") {
      const auto cpu_before = CpuTime();
      const auto started = std::chrono::steady_clock::now();
      gateway.Stop();
      const auto took = std::chrono::steady_clock::now() - started;
      const auto cpu = CpuTime() - cpu_before;
      THEN("the stop's wait for the logout uses little CPU time") {
        INFO(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
             << " ms, " << std::chrono::duration_cast<std::chrono::milliseconds>(cpu).count()
             << " ms CPU");
        REQUIRE((cpu < std::chrono::milliseconds(500)));
      }
    }
  }
}

// Row U-4 (8.2, P0-10): stop() gave the session up while its connection still read, so a
// reconnect took it over and two threads drove one session (TSan, krx_stress chaos).
namespace {
// A counterparty logged on to an acceptor on `port` never answers the Logout of a graceful stop,
// and logs on again on a second connection meanwhile; what the second connection received.
std::string ReconnectWhileStopping(int port, krx::EventLogFactory& logs) {
  const FIX::SessionSettings settings = AcceptorOn({port});
  FIX::NullApplication application;
  FIX::MemoryStoreFactory store;
  FIX::ThreadedSocketAcceptor acceptor(application, store, settings, logs);
  acceptor.start();
  const Counterparty first(port, LogonFor(port));
  REQUIRE(logs.Events().WaitFor("Responding to logon request", kWait));
  const std::jthread stopper([&acceptor] { acceptor.stop(); });
  REQUIRE(logs.Events().WaitFor("Initiated logout request", kWait));
  return Counterparty(port, LogonFor(port)).Received(kWait);
}
}  // namespace

SCENARIO("A reconnect while the acceptor stops waits for the session's connection to end",
         "[krx][v1][tsan][u-4]") {
  GIVEN("an acceptor with a logged-on counterparty that never answers a Logout") {
    const int port = FreePort();
    krx::EventLogFactory logs;
    WHEN("the gateway stops the acceptor and the counterparty connects again meanwhile") {
      const std::string second = ReconnectWhileStopping(port, logs);
      THEN("the second connection reaches the session only once the first one is gone") {
        const std::string events = logs.Events().Dump();
        INFO("events:\n" << events);
        REQUIRE((events.find("Disconnecting") < events.find("Session is not enabled for logon") &&
                 !second.contains("\00135=A\001")));
      }
    }
  }
}

namespace {
// Whether the gateway closed `socket` (or never took it) within `timeout`: EOF or an error.
bool ClosedWithin(FIX::socket_handle socket, std::chrono::milliseconds timeout) {
  pollfd ready{.fd = socket, .events = POLLIN, .revents = 0};
  if (poll(&ready, 1, static_cast<int>(timeout.count())) <= 0) {
    return false;
  }
  std::array<char, 64> buffer{};
  return FIX::socket_recv(socket, buffer.data(), buffer.size()) <= 0;
}

// Connects to `port` again and again until `stop`; every socket it opened.
std::vector<FIX::socket_handle> ConnectStorm(int port, const std::atomic<bool>& stop) {
  std::vector<FIX::socket_handle> sockets;
  while (!stop.load()) {
    const FIX::socket_handle socket = FIX::socket_createConnector();
    if (FIX::socket_connect(socket, "127.0.0.1", port) < 0) {
      FIX::socket_close(socket);
      continue;
    }
    sockets.push_back(socket);
  }
  return sockets;
}

// Starts an acceptor on `port` and force-stops it `rounds` times while connections keep coming;
// how many of them the stops left open.
int LeftOpenByStops(int port, int rounds) {
  const FIX::SessionSettings settings = AcceptorOn({port});
  FIX::NullApplication application;
  FIX::MemoryStoreFactory store;
  int left_open = 0;
  for (int round = 0; round < rounds; ++round) {
    FIX::ThreadedSocketAcceptor acceptor(application, store, settings);
    acceptor.start();
    std::atomic<bool> stop_connecting{false};
    auto storm = std::async(std::launch::async, ConnectStorm, port, std::cref(stop_connecting));
    std::this_thread::sleep_for(std::chrono::milliseconds(round % 5));
    acceptor.stop(true);
    stop_connecting.store(true);
    for (const FIX::socket_handle socket : storm.get()) {
      left_open += ClosedWithin(socket, std::chrono::milliseconds(200)) ? 0 : 1;
      FIX::socket_close(socket);
    }
  }
  return left_open;
}
}  // namespace

// Row U-4 (8.2, P0-10): a connection accepted as stop() began was spawned after onStop had
// collected the threads it joins: it outlived the stop, kept reading, and after a restart a new
// connection took the same session (two threads on one session, TSan, krx_stress chaos).
SCENARIO("Every connection accepted while the acceptor stops is closed by the stop",
         "[krx][v1][tsan][u-4]") {
  GIVEN("an acceptor that a counterparty keeps connecting to") {
    const int port = FreePort();
    WHEN("the gateway starts and force-stops it 100 times while the connections come") {
      const int left_open = LeftOpenByStops(port, 100);
      THEN("none of them is left open") {
        INFO(left_open << " connections still open after the stop");
        REQUIRE((left_open == 0));
      }
    }
  }
}

// Q11 (8.7): the initiator's side of row 6.6.
SCENARIO("A forced initiator stop waiting for the exchange to log out does not spin a CPU",
         "[krx][v1][u-8]") {
  GIVEN("an initiator logged on to an exchange that never answers a Logout") {
    const int port = FreePort();
    const SilentExchange exchange(port);
    const FIX::SessionSettings settings = InitiatorTo(port);
    FIX::NullApplication application;
    FIX::MemoryStoreFactory store;
    krx::EventLogFactory logs;
    FIX::ThreadedSocketInitiator initiator(application, store, settings, logs);
    initiator.start();
    REQUIRE(logs.Events().WaitFor("Received logon response", kWait));
    WHEN("the gateway stops the initiator without waiting") {
      const auto cpu_before = CpuTime();
      const auto started = std::chrono::steady_clock::now();
      initiator.stop(true);
      const auto took = std::chrono::steady_clock::now() - started;
      const auto cpu = CpuTime() - cpu_before;
      THEN("the stop's wait for the logout uses little CPU time") {
        INFO(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
             << " ms, " << std::chrono::duration_cast<std::chrono::milliseconds>(cpu).count()
             << " ms CPU");
        REQUIRE((cpu < std::chrono::milliseconds(500)));
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
