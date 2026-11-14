// Phase 1 rows 1.1 and 1.2 of docs/V1_BDD_CHECKLIST.md: the harness the later phases build on.
#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "FixInput.h"
#include "SessionInput.h"
#include "SessionPair.h"

namespace {

constexpr std::chrono::seconds kReplyTimeout{5};
constexpr std::size_t kFewestSeeds = 1000;
constexpr std::size_t kSessionStates = 3;

std::size_t RejectedCount(const krx::FixDictionaries& dictionaries,
                          const std::vector<std::string>& seeds) {
  std::size_t rejected = 0;
  for (const std::string& seed : seeds) {
    rejected += krx::FeedFixInput(dictionaries, seed);
  }
  return rejected;
}

}  // namespace

SCENARIO("A session can be driven byte by byte over a socketpair without a network",
         "[krx][v1][harness]") {
  GIVEN("an acceptor session on one end of a socketpair") {
    krx::SessionPair pair("ROW1-1");
    WHEN("the counterparty writes a Logon one byte at a time") {
      pair.WriteByteByByte(pair.Logon(1));
      THEN("the session answers with a Logon and is logged on") {
        const std::string reply = pair.ReadFrame(kReplyTimeout);
        INFO("reply: " << reply);
        REQUIRE((krx::MsgTypeOf(reply) == "A"));
        REQUIRE(pair.WaitLoggedOn(kReplyTimeout));
      }
    }
  }
}

SCENARIO("Every acceptance-test message replays through the parser and the dictionaries",
         "[krx][v1][harness]") {
  GIVEN("the fuzz seeds taken from the acceptance definitions") {
    const krx::FixDictionaries dictionaries(KRX_SPEC_DIR);
    const std::vector<std::string> seeds = krx::SeedsFromDefinitions(KRX_DEFINITIONS_DIR);
    WHEN("each seed is fed through its fuzz mode") {
      const std::size_t rejected = RejectedCount(dictionaries, seeds);
      THEN("none crashes, the deliberately bad ones are rejected and the rest parse") {
        INFO("rejected " << rejected << " of " << seeds.size());
        REQUIRE((seeds.size() > kFewestSeeds && rejected > 0 && rejected < seeds.size()));
      }
    }
  }
}

SCENARIO("Every stateful fuzz seed replays through a live session in the state it was made for",
         "[krx][v1][harness]") {
  GIVEN("the stateful fuzz seeds: before logon, mid-resend and during logout") {
    const std::vector<std::string> seeds = krx::SessionSeeds();
    WHEN("each seed is fed to a new session brought to its state") {
      std::size_t reached = 0;
      for (const std::string& seed : seeds) {
        reached += krx::FeedSessionInput(seed) ? 1 : 0;
      }
      THEN("none crashes or hangs, and every session was in its state when the bytes arrived") {
        INFO("in state for " << reached << " of " << seeds.size());
        REQUIRE((seeds.size() >= kSessionStates && reached == seeds.size()));
      }
    }
  }
}
