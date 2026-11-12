// Phase 6 of docs/V1_BDD_CHECKLIST.md: teardown and locking are race-free (run by the TSan job).
// Item 6.3, first part (P1-1, port of quickfix/quickfix#772, pulled forward for Phase 3):
// FIX::Mutex is a std::recursive_mutex. The old one read its owner and count unsynchronised, so any
// two threads sharing a session lock raised TSan reports; a copy also copied its lock state.
#include <chrono>
#include <future>
#include <thread>

#include <catch_amalgamated.hpp>

#include "Mutex.h"

namespace {

constexpr int kIncrements = 20000;

}  // namespace

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
