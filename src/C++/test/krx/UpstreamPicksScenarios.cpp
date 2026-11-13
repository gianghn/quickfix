// Phase 7 of docs/V1_BDD_CHECKLIST.md: upstream fixes picked into the fork, each with its upstream
// tests in `ut` (src/C++/test) and a scenario here.
// Row 7.1 (P1-5, quickfix/quickfix#715, d5a5cfdd): a GapFill drops the queued messages it skips
// (they stayed queued until the disconnect) and says so; it still processes the queued messages
// beyond the gap at once (the fork already did).
// Row 7.2 (P1-6, quickfix/quickfix#764): a field map assigned to itself keeps its fields and
// groups, and one moved onto a map with groups frees those groups.
// Row 7.5 (P0-8 support, quickfix/quickfix#743): validation checks the count of a repeating
// group nested in another (Parties' PartySubIDs), not only of top-level groups.
#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include <catch_amalgamated.hpp>

#include "DataDictionary.h"
#include "Exceptions.h"
#include "FieldMap.h"
#include "FieldTypes.h"
#include "FixFields.h"
#include "FixValues.h"
#include "Message.h"
#include "SessionPair.h"
#include "fix44/NewOrderSingle.h"

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

// The FIX.4.4 dictionary from spec/.
const FIX::DataDictionary& Fix44() {
  static const FIX::DataDictionary dictionary(std::string(KRX_SPEC_DIR) + "/FIX44.xml");
  return dictionary;
}

// A NewOrderSingle (as the dictionary parses it off the wire) with one party carrying
// `sub_ids` PartySubID entries, its NoPartySubIDs (802) saying `stated`.
FIX::Message OrderWithPartySubIDs(int sub_ids, int stated) {
  FIX44::NewOrderSingle order(FIX::ClOrdID("1"), FIX::Side(FIX::Side_BUY),
                              FIX::TransactTime(FIX::UtcTimeStamp::now()),
                              FIX::OrdType(FIX::OrdType_MARKET));
  FIX::Header& header = order.getHeader();
  header.setField(FIX::SenderCompID("KRXEX"));
  header.setField(FIX::TargetCompID("KRXGW"));
  header.setField(FIX::MsgSeqNum(2));
  header.setField(FIX::SendingTime(FIX::UtcTimeStamp::now()));
  order.set(FIX::Symbol("VNM"));
  FIX44::NewOrderSingle::NoPartyIDs party;
  party.set(FIX::PartyID("TRADER1"));
  party.set(FIX::PartyIDSource(FIX::PartyIDSource_PROPRIETARY));
  party.set(FIX::PartyRole(FIX::PartyRole_EXECUTING_TRADER));
  for (int i = 0; i < sub_ids; ++i) {
    FIX44::NewOrderSingle::NoPartyIDs::NoPartySubIDs sub_id;
    sub_id.set(FIX::PartySubID("DESK" + std::to_string(i)));
    sub_id.set(FIX::PartySubIDType(1));
    party.addGroup(sub_id);
  }
  party.setField(FIX::NoPartySubIDs(stated));
  order.addGroup(party);
  return {order.toString(), Fix44()};
}

}  // namespace

SCENARIO("Nested repeating groups are validated by their counts", "[krx][v1][p0-8]") {
  GIVEN("the FIX.4.4 dictionary and an order whose party has nested PartySubIDs") {
    WHEN("the nested count matches its entries, and when it says 2 for one entry") {
      const FIX::Message correct = OrderWithPartySubIDs(2, 2);
      const FIX::Message miscounted = OrderWithPartySubIDs(1, 2);
      THEN("the correct order passes and the miscounted one is refused") {
        REQUIRE_NOTHROW(Fix44().validate(correct));
        REQUIRE_THROWS_AS(Fix44().validate(miscounted), FIX::RepeatingGroupCountMismatch);
      }
    }
  }
}

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
