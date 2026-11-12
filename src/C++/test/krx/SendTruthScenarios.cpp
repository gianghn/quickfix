// Phase 5 of docs/V1_BDD_CHECKLIST.md: no silent loss. A send reports what happened to the order.
// Rows 5.1 and 5.2 (P0-6, Q2): send() returns false when the order was not written to the
// exchange: before Logon, or when the socket write fails. (It may still be stored: a resend can
// send it later as a possible duplicate, which the gateway refuses in toApp.)
#include <chrono>
#include <string>

#include <catch_amalgamated.hpp>

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

// An order from the gateway (a BusinessMessageReject stands in for it) with `text_size` bytes of
// Text.
FIX::Message Order(std::size_t text_size = 50) {
  FIX::Message order;
  order.getHeader().setField(FIX::MsgType("j"));
  order.setField(FIX::RefMsgType("D"));
  order.setField(FIX::BusinessRejectReason(0));
  order.setField(FIX::Text(std::string(text_size, 'o')));
  return order;
}

}  // namespace

SCENARIO("An order sent before Logon reports not sent", "[krx][v1][p0-6]") {
  GIVEN("a session the exchange has not logged on to") {
    krx::SessionPair pair("KRXST51");
    WHEN("the gateway sends an order") {
      const bool sent = pair.Send(Order());
      THEN("send reports that it was not sent") {
        REQUIRE_FALSE(sent);
      }
    }
  }
}

SCENARIO("An order whose socket write fails reports not sent", "[krx][v1][p0-6]") {
  GIVEN("a logged-on session with a 300 ms send timeout, and an exchange that reads nothing") {
    krx::PairOptions options;
    options.send_timeout_ms = 300;
    krx::SessionPair pair("KRXST52", options);
    LogOn(pair);
    WHEN("the gateway sends an order too large for the socket's buffers") {
      const bool sent = pair.Send(Order(std::size_t{600} * 1024));
      THEN("send reports that it was not sent") {
        INFO(pair.Events().Dump());
        REQUIRE_FALSE(sent);
      }
    }
  }
}
