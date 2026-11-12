// Phase 5 of docs/V1_BDD_CHECKLIST.md: no silent loss. A send reports what happened to the order.
// Rows 5.1 and 5.2 (P0-6, Q2): send() returns false when the order was not written to the
// exchange: before Logon, or when the socket write fails. (It may still be stored: a resend can
// send it later as a possible duplicate, which the gateway refuses in toApp.)
// Row 5.3 (P0-6): when the store keeps an order but cannot write its seqnum, the seqnum is not
// consumed: the next message reuses it and replaces the stored copy, so a resend cannot send the
// failed order.
// Row 5.4 (P0-8, Q3): every Reject (35=3) and BusinessMessageReject (35=j) the session sends is
// logged as an error with the refused message's seqnum and the reason, for the gateway to alert.
#include <chrono>
#include <string>
#include <vector>

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
// Text, all `fill`.
FIX::Message Order(std::size_t text_size = 50, char fill = 'o') {
  FIX::Message order;
  order.getHeader().setField(FIX::MsgType("j"));
  order.setField(FIX::RefMsgType("D"));
  order.setField(FIX::BusinessRejectReason(0));
  order.setField(FIX::Text(std::string(text_size, fill)));
  return order;
}

// What the store keeps under `seq_num`, or "<missing>".
std::string Stored(const FIX::MessageStore* store, int seq_num) {
  std::vector<std::string> messages;
  store->get(seq_num, seq_num, messages);
  return messages.empty() ? "<missing>" : messages.front();
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

SCENARIO("Every outbound Reject is logged at error level with its seqnum and reason",
         "[krx][v1][p0-8]") {
  GIVEN("a logged-on session") {
    krx::SessionPair pair("KRXRJ54");
    LogOn(pair);
    WHEN("the exchange sends a ResendRequest from 0, which the session rejects") {
      FIX::Message request;
      request.getHeader().setField(FIX::MsgType("2"));
      request.setField(FIX::BeginSeqNo(0));
      request.setField(FIX::EndSeqNo(0));
      pair.Write(pair.FromCounterparty(request, 2));
      const bool rejected = krx::MsgTypeOf(pair.ReadFrame(kWait)) == "3";
      THEN("an error names the Reject, the refused seqnum and the reason") {
        INFO(pair.Events().Dump());
        REQUIRE((rejected && pair.Events().Contains(
                                 "ERROR: sent Reject (35=3) for RefSeqNum 2: Value is incorrect")));
      }
    }
  }
}

SCENARIO("Every outbound BusinessMessageReject is logged at error level with its seqnum and reason",
         "[krx][v1][p0-8]") {
  GIVEN("a logged-on session") {
    krx::SessionPair pair("KRXRJ54B");
    LogOn(pair);
    WHEN("a BusinessMessageReject for message 7 goes out") {
      FIX::Message reject = Order(10, 'r');
      reject.setField(FIX::RefSeqNum(7));
      pair.Send(reject);
      THEN("an error names the BusinessMessageReject, the refused seqnum and the reason") {
        INFO(pair.Events().Dump());
        REQUIRE(pair.Events().Contains(
            "ERROR: sent BusinessMessageReject (35=j) for RefSeqNum 7: rrrrrrrrrr"));
      }
    }
  }
}

SCENARIO(
    "A store failure while persisting an order leaves no stored copy that a retry would "
    "duplicate",
    "[krx][v1][p0-6]") {
  GIVEN("a logged-on session whose store will keep the next order but fail to write its seqnum") {
    krx::PairOptions options;
    options.seqnum_fails = true;
    krx::SessionPair pair("KRXST53", options);
    LogOn(pair);
    FIX::MessageStore* store = pair.Store();
    const int seq_num = store->getNextSenderMsgSeqNum();
    dynamic_cast<krx::SeqNumFailingStore&>(*store).FailNextSenderSeqNumWrite();
    WHEN("the gateway sends order A, then, with the store working again, order B") {
      const bool sent_a = pair.Send(Order(50, 'A'));
      pair.Send(Order(50, 'B'));
      THEN("A reports not sent, and B took A's seqnum, so the store's copy is B") {
        const std::string kept = Stored(store, seq_num);
        INFO("seqnum " << seq_num << " holds: " << kept);
        REQUIRE((!sent_a && kept.contains("58=" + std::string(50, 'B'))));
      }
    }
  }
}
