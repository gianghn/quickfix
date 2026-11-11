// Rows 2.1-2.4 of docs/V1_BDD_CHECKLIST.md (P0-1): a data field's length comes from the wire, so a
// forged one must be rejected as an invalid message instead of moving the parser past the buffer.
// Out-of-bounds reads show as wrong results here and as reports under AddressSanitizer (Linux CI).
// Rows 2.5-2.7 (P0-2): an error while a message is handled ends that connection, never the process.
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>

#include <catch_amalgamated.hpp>

#include "DataDictionary.h"
#include "Exceptions.h"
#include "FixFields.h"
#include "FixInput.h"
#include "Message.h"
#include "SessionPair.h"

namespace {

constexpr std::chrono::seconds kWait{5};

// A SequenceReset (reset mode) from the counterparty: its next sequence number is new_seq_no.
std::string SequenceReset(const krx::SessionPair& pair, int seq_num, int new_seq_no) {
  FIX::Message reset;
  reset.getHeader().setField(FIX::MsgType("4"));  // SequenceReset
  reset.setField(FIX::NewSeqNo(new_seq_no));
  return pair.FromCounterparty(reset, seq_num);
}

// An application message every FIX.4.4 session accepts: a BusinessMessageReject of an order.
std::string BusinessReject(const krx::SessionPair& pair, int seq_num) {
  FIX::Message reject;
  reject.getHeader().setField(FIX::MsgType("j"));  // BusinessMessageReject
  reject.setField(FIX::RefMsgType("D"));
  reject.setField(FIX::BusinessRejectReason(0));
  return pair.FromCounterparty(reject, seq_num);
}

// The counterparty logs on with sequence number 1; the session answers with its own Logon.
void LogOn(krx::SessionPair& pair, int heart_bt_int = 30) {
  pair.Write(pair.Logon(1, heart_bt_int));
  REQUIRE(pair.WaitLoggedOn(kWait));
  const std::string answer = krx::MsgTypeOf(pair.ReadFrame(kWait));
  INFO(answer);
  REQUIRE((answer == "A"));
}

std::string TestRequest(const krx::SessionPair& pair, int seq_num) {
  FIX::Message request;
  request.getHeader().setField(FIX::MsgType("1"));  // TestRequest
  request.setField(FIX::TestReqID("still-there"));
  return pair.FromCounterparty(request, seq_num);
}

std::string Fix44() {
  return std::string(KRX_SPEC_DIR) + "/FIX44.xml";
}

// A FIX.4.4 News whose RawData is `data` and whose RawDataLength says `data_length`.
std::string NewsWithDataLength(int data_length, const std::string& data = "abc") {
  FIX::Message news;
  news.getHeader().setField(FIX::BeginString("FIX.4.4"));
  news.getHeader().setField(FIX::MsgType("B"));  // News
  news.setField(FIX::Headline("forged"));
  news.setField(FIX::RawDataLength(data_length));
  news.setField(FIX::RawData(data));
  return news.toString();
}

void RequireRejected(int data_length) {
  const FIX::DataDictionary dictionary(Fix44());
  const std::string wire = NewsWithDataLength(data_length);
  FIX::Message parsed;
  INFO(wire);
  REQUIRE_THROWS_AS(parsed.setString(wire, false, &dictionary, &dictionary), FIX::InvalidMessage);
}

}  // namespace

SCENARIO("A data length pointing past the end of the frame is rejected",
         "[krx][v1][security][p0-1]") {
  GIVEN("a News whose RawData is 3 bytes but whose RawDataLength says 99999") {
    THEN("parsing it throws InvalidMessage and the process carries on") {
      RequireRejected(99999);
    }
  }
}

SCENARIO("A negative data length is rejected", "[krx][v1][security][p0-1]") {
  GIVEN("a News whose RawDataLength is -1") {
    THEN("parsing it throws InvalidMessage") {
      RequireRejected(-1);
    }
  }
}

SCENARIO("A data length of two billion is rejected", "[krx][v1][security][p0-1]") {
  GIVEN("a News whose RawDataLength is 2000000000") {
    THEN("parsing it throws InvalidMessage") {
      RequireRejected(2000000000);
    }
  }
}

SCENARIO("A data length that ends inside the value is rejected", "[krx][v1][security][p0-1]") {
  GIVEN("a News whose RawData is 3 bytes but whose RawDataLength says 2") {
    THEN("parsing it throws InvalidMessage, since the byte after the data is not SOH") {
      RequireRejected(2);
    }
  }
}

SCENARIO("A correct data length still parses", "[krx][v1][security][p0-1]") {
  GIVEN("a News whose RawDataLength matches its 3-byte RawData") {
    const FIX::DataDictionary dictionary(Fix44());
    const std::string wire = NewsWithDataLength(3);
    WHEN("it is parsed") {
      FIX::Message parsed;
      parsed.setString(wire, false, &dictionary, &dictionary);
      THEN("the data comes back whole") {
        const std::string& data = parsed.getField(FIX::FIELD::RawData);
        INFO(data);
        REQUIRE((data == "abc"));
      }
    }
  }
}

SCENARIO("An empty data field with length 0 still parses", "[krx][v1][security][p0-1]") {
  GIVEN("a News whose RawDataLength is 0 and whose RawData is empty") {
    const FIX::DataDictionary dictionary(Fix44());
    const std::string wire = NewsWithDataLength(0, "");
    WHEN("it is parsed") {
      FIX::Message parsed;
      parsed.setString(wire, false, &dictionary, &dictionary);
      THEN("the data comes back empty") {
        const std::string& data = parsed.getField(FIX::FIELD::RawData);
        INFO(data);
        REQUIRE(data.empty());
      }
    }
  }
}

SCENARIO("A SequenceReset before Logon ends the connection and not the process",
         "[krx][v1][security][p0-2]") {
  GIVEN("a session that has not received a Logon") {
    krx::SessionPair pair("KRXSR25");
    WHEN("the counterparty sends a SequenceReset moving its sequence number back") {
      pair.Write(SequenceReset(pair, 1, 0));
      THEN("the session logs it and closes the connection") {
        REQUIRE(pair.WaitClosed(kWait));
        REQUIRE(pair.Events().Contains("before Logon"));
      }
    }
  }
}

SCENARIO("A SequenceReset to the expected number before Logon changes nothing",
         "[krx][v1][security][p0-2]") {
  GIVEN("a session that has not received a Logon") {
    const krx::SessionPair pair("KRXSE25");
    WHEN("the counterparty resets to the number the session expects, then logs on") {
      pair.Write(SequenceReset(pair, 1, 1));
      pair.Write(pair.Logon(1));
      THEN("the session logs on") {
        REQUIRE(pair.WaitLoggedOn(kWait));
      }
    }
  }
}

SCENARIO("A SequenceReset moving back after Logon is rejected and the session stays",
         "[krx][v1][security][p0-2]") {
  GIVEN("a logged-on session expecting sequence number 2") {
    krx::SessionPair pair("KRXSL25");
    LogOn(pair);
    WHEN("the counterparty sends a SequenceReset back to 1") {
      pair.Write(SequenceReset(pair, 2, 1));
      THEN("the session answers with a Reject") {
        const std::string answer = krx::MsgTypeOf(pair.ReadFrame(kWait));
        INFO(answer);
        REQUIRE((answer == "3"));
      }
    }
  }
}

SCENARIO("An error raised in fromApp disconnects the session and is logged",
         "[krx][v1][security][p0-2]") {
  GIVEN("a logged-on session whose fromApp throws a std::runtime_error") {
    krx::SessionPair pair("KRXFA26");
    pair.ThrowFromApp();
    LogOn(pair);
    WHEN("an application message arrives") {
      pair.Write(BusinessReject(pair, 2));
      THEN("the session logs the error and closes the connection") {
        REQUIRE(pair.WaitClosed(kWait));
        REQUIRE(pair.Events().Contains("gateway callback failed"));
      }
    }
  }
}

SCENARIO("An error in one connection thread disconnects only that session",
         "[krx][v1][security][p0-2]") {
  GIVEN("two logged-on sessions, the first with a fromApp that throws") {
    krx::SessionPair failing("KRXF27");
    krx::SessionPair healthy("KRXH27");
    failing.ThrowFromApp();
    LogOn(failing);
    LogOn(healthy);
    WHEN("the first session's fromApp throws") {
      failing.Write(BusinessReject(failing, 2));
      REQUIRE(failing.WaitClosed(kWait));
      THEN("the second session still answers a TestRequest with a Heartbeat") {
        healthy.Write(TestRequest(healthy, 2));
        const std::string answer = krx::MsgTypeOf(healthy.ReadFrame(kWait));
        INFO(answer);
        REQUIRE((answer == "0"));
      }
    }
  }
}

SCENARIO("An error while the session sends a heartbeat ends the connection",
         "[krx][v1][security][p0-2]") {
  GIVEN("a logged-on session with a 1-second heartbeat whose toAdmin then throws") {
    krx::SessionPair pair("KRXHB27");
    LogOn(pair, 1);
    pair.ThrowFromAdmin();
    WHEN("the heartbeat falls due") {
      THEN("the session logs the error and closes the connection") {
        REQUIRE(pair.WaitClosed(kWait));
        REQUIRE(pair.Events().Contains("gateway admin callback failed"));
      }
    }
  }
}

SCENARIO("The fuzzer's recorded crash input no longer crashes the parser",
         "[krx][v1][security][p0-1]") {
  GIVEN("the input libFuzzer saved for P0-1 (test/krx/corpus)") {
    std::ifstream file(std::string(KRX_CORPUS_DIR) + "/p0-1-forged-data-length.bin",
                       std::ios::binary);
    REQUIRE(file.good());
    const std::string input{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const krx::FixDictionaries dictionaries(KRX_SPEC_DIR);
    WHEN("it is fed to the parser as the fuzzer did") {
      std::size_t rejected = 0;
      REQUIRE_NOTHROW(rejected = krx::FeedFixInput(dictionaries, input));
      THEN("the engine rejects it with a FIX exception") {
        INFO(rejected);
        REQUIRE((rejected == 1));
      }
    }
  }
}
