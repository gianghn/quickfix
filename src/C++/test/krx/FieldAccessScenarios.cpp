// Row 1.5 of docs/V1_BDD_CHECKLIST.md (P2-5): the engine reads typed fields without casting the
// stored FieldBase to a type it is not. UBSan's vptr check (Linux CI) is the oracle; the values are
// checked everywhere.
#include <string>

#include <catch_amalgamated.hpp>

#include "FieldMap.h"
#include "FixFields.h"
#include "Message.h"
#include "SessionID.h"

SCENARIO("Reading a field from a message never treats it as a type it is not", "[krx][v1][p2-5]") {
  GIVEN("a message whose header names the sender, the target and a sequence number") {
    FIX::Message message;
    message.getHeader().setField(FIX::BeginString("FIX.4.4"));
    message.getHeader().setField(FIX::SenderCompID("EXCH"));
    message.getHeader().setField(FIX::TargetCompID("KRXGW"));
    message.getHeader().setField(FIX::MsgSeqNum(42));
    WHEN("the engine reads its session ID and sequence number") {
      const FIX::SessionID id = message.getSessionID();
      const auto seq_num = message.getHeader().getField<FIX::MsgSeqNum>();
      THEN("it gets the values in the header") {
        REQUIRE((id.getSenderCompID().getValue() == "EXCH" &&
                 id.getTargetCompID().getValue() == "KRXGW" && seq_num.getValue() == 42));
      }
    }
  }
}
