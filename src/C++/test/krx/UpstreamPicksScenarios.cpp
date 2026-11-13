// Phase 7 of docs/V1_BDD_CHECKLIST.md: upstream fixes picked into the fork, each with its upstream
// tests in `ut` (src/C++/test) and a scenario here.
// Row 7.1 (P1-5, quickfix/quickfix#715, d5a5cfdd): a GapFill drops the queued messages it skips
// (they stayed queued until the disconnect) and says so; it still processes the queued messages
// beyond the gap at once (the fork already did).
// Row 7.2 (P1-6, quickfix/quickfix#764): a field map assigned to itself keeps its fields and
// groups, and one moved onto a map with groups frees those groups.
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <catch_amalgamated.hpp>

#include "FieldMap.h"
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

// A repeating group entry that counts its own destruction.
class CountedGroup : public FIX::FieldMap {
 public:
  explicit CountedGroup(int& destroyed) : destroyed_(destroyed) {}

  ~CountedGroup() override { ++destroyed_; }

  CountedGroup(const CountedGroup&) = delete;
  CountedGroup& operator=(const CountedGroup&) = delete;
  CountedGroup(CountedGroup&&) = delete;
  CountedGroup& operator=(CountedGroup&&) = delete;

 private:
  int& destroyed_;
};

// A field map with field 1 and one group entry under tag 10.
FIX::FieldMap WithFieldAndGroup() {
  FIX::FieldMap map;
  map.setField(1, "field1");
  map.addGroup(10, FIX::FieldMap());
  return map;
}

}  // namespace

SCENARIO("A field map moved onto itself keeps its contents", "[krx][v1][p1-6]") {
  GIVEN("a field map with a field and a group") {
    FIX::FieldMap map = WithFieldAndGroup();
    WHEN("it is copied and then moved onto itself") {
      const FIX::FieldMap& copy_source = map;
      map = copy_source;
      FIX::FieldMap& move_source = map;
      map = std::move(move_source);
      THEN("the field and the group are still there") {
        REQUIRE((map.getField(1) == "field1" && map.groupCount(10) == 1));
      }
    }
  }
}

SCENARIO("A field map moved onto a map with groups frees those groups", "[krx][v1][p1-6]") {
  GIVEN("a map holding one group entry and another map holding a different one") {
    int replaced_destroyed = 0;
    int moved_destroyed = 0;
    FIX::FieldMap destination;
    // addGroupPtr takes ownership of the raw pointer.
    destination.addGroupPtr(10, std::make_unique<CountedGroup>(replaced_destroyed).release(),
                            false);
    FIX::FieldMap source;
    source.addGroupPtr(20, std::make_unique<CountedGroup>(moved_destroyed).release(), false);
    WHEN("the second is moved onto the first") {
      destination = std::move(source);
      THEN("the replaced entry is freed at once, and the moved one is kept") {
        REQUIRE((replaced_destroyed == 1 && moved_destroyed == 0 &&
                 destination.groupCount(10) == 0 && destination.groupCount(20) == 1));
      }
    }
  }
}

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
