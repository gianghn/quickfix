// Phase 6 of docs/V1_BDD_CHECKLIST.md: teardown and locking are race-free (run by the TSan job).
// Item 6.3 (P1-1, ports of quickfix/quickfix#772, #773, #774, #781, pulled forward for Phase 3):
// FIX::Mutex is a std::recursive_mutex. The old one read its owner and count unsynchronised, so any
// two threads sharing a session lock raised TSan reports; a copy also copied its lock state. The
// session flags and the last sent time, read by the reading thread without the session lock, are
// atomic.
#include <chrono>
#include <future>
#include <thread>

#include <catch_amalgamated.hpp>

#include "FieldTypes.h"
#include "Mutex.h"
#include "SessionState.h"

namespace {

constexpr int kIncrements = 20000;

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
