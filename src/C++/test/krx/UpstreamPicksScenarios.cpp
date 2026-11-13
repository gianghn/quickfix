// Phase 7 of docs/V1_BDD_CHECKLIST.md: upstream fixes picked into the fork, each with its upstream
// tests in `ut` (src/C++/test) and a scenario here.
// Row 7.1 (P1-5, quickfix/quickfix#715, d5a5cfdd): a GapFill drops the queued messages it skips
// (they stayed queued until the disconnect) and says so; it still processes the queued messages
// beyond the gap at once (the fork already did).
#include <chrono>
#include <string>

#include <catch_amalgamated.hpp>

#include "FieldTypes.h"
#include "FixFields.h"
#include "Message.h"
#include "SessionPair.h"

namespace {

constexpr std::chrono::seconds kWait{5};

void LogOn(krx::SessionPair& pair) {
  pair.Write(pair.Logon(1));
  REQUIRE(pair.WaitLoggedOn(kWait));
  REQUIRE((krx::MsgTypeOf(pair.ReadFrame(kWait)) == "A"));
}

// A TestRequest from the counterparty with this seqnum and TestReqID.
std::string TestRequest(const krx::SessionPair& pair, int seq_num, const std::string& id) {
  FIX::Message request;
  request.getHeader().setField(FIX::MsgType("1"));
  request.setField(FIX::TestReqID(id));
  return pair.FromCounterparty(request, seq_num);
}

// A SequenceReset-GapFill from the counterparty: message `seq_num` fills up to `new_seq_num`.
std::string GapFill(const krx::SessionPair& pair, int seq_num, int new_seq_num) {
  FIX::Message reset;
  reset.getHeader().setField(FIX::MsgType("4"));
  reset.setField(FIX::GapFillFlag(true));
  reset.setField(FIX::NewSeqNo(new_seq_num));
  return pair.FromCounterparty(reset, seq_num);
}

}  // namespace

SCENARIO("A GapFill leaves no skipped message waiting in the queue", "[krx][v1][p1-5]") {
  GIVEN("a logged-on session holding TestRequests 5, 6 and 7, which arrived before 2 to 4") {
    krx::SessionPair pair("KRXGF71S");
    LogOn(pair);
    pair.Write(TestRequest(pair, 5, "A") + TestRequest(pair, 6, "B") + TestRequest(pair, 7, "C"));
    REQUIRE((krx::MsgTypeOf(pair.ReadFrame(kWait)) == "2"));  // its ResendRequest for 2 on
    WHEN("the counterparty gap-fills 2 to 7, over the messages it had sent") {
      pair.Write(GapFill(pair, 2, 8));
      pair.Write(TestRequest(pair, 8, "D"));
      const std::string answer = pair.ReadFrame(kWait);
      THEN("the three queued messages are dropped, logged, and the next one is answered") {
        INFO("answer: " << answer << "\nevents:\n" << pair.Events().Dump());
        REQUIRE((pair.Events().Contains("GapFill to 8 dropped 3 queued messages") &&
                 answer.contains("\001112=D\001")));
      }
    }
  }
}

SCENARIO("A GapFill processes the messages queued beyond the gap at once", "[krx][v1][p1-5]") {
  GIVEN("a logged-on session holding TestRequests 5 and 6, which arrived before 2 to 4") {
    krx::SessionPair pair("KRXGF71");
    LogOn(pair);
    pair.Write(TestRequest(pair, 5, "A") + TestRequest(pair, 6, "B"));
    REQUIRE((krx::MsgTypeOf(pair.ReadFrame(kWait)) == "2"));  // its ResendRequest for 2 on
    WHEN("the counterparty gap-fills 2 to 4") {
      pair.Write(GapFill(pair, 2, 5));
      const std::string first = pair.ReadFrame(kWait);
      const std::string second = pair.ReadFrame(kWait);
      THEN("the session answers both TestRequests, in order, without waiting for more") {
        INFO("first: " << first << "\nsecond: " << second << "\nevents:\n" << pair.Events().Dump());
        REQUIRE((first.contains("\001112=A\001") && second.contains("\001112=B\001")));
      }
    }
  }
}
