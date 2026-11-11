// Rows 2.1-2.4 of docs/V1_BDD_CHECKLIST.md (P0-1): a data field's length comes from the wire, so a
// forged one must be rejected as an invalid message instead of moving the parser past the buffer.
// Out-of-bounds reads show as wrong results here and as reports under AddressSanitizer (Linux CI).
// Rows 2.5-2.7 (P0-2): an error while a message is handled ends that connection, never the process.
// Rows 2.8-2.10 (P0-7): a garbled frame is skipped; the good frames around it are read, once each.
#include <chrono>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "DataDictionary.h"
#include "Exceptions.h"
#include "FixFields.h"
#include "FixInput.h"
#include "Message.h"
#include "Parser.h"
#include "SessionPair.h"

namespace {

constexpr std::chrono::seconds kWait{5};

// Frames for the parser alone (it checks framing, not checksums): the upstream ParserTestCase ones.
constexpr const char* kGoodFrame = "8=FIX.4.2\0019=12\00135=A\001108=30\00110=31\001";
// A BodyLength that is not a number; "108=" in its body is where a naive "8=" search resyncs.
constexpr const char* kBadLengthFrame = "8=FIX.4.2\0019=A\00135=A\001108=30\00110=31\001";
// No BodyLength at all: a search for "9=" would borrow the next frame's.
constexpr const char* kNoLengthFrame = "8=FIX.4.2\00135=A\001108=30\00110=31\001";
// A garbled frame for a FIX.4.4 session: BodyLength is not a number.
constexpr const char* kGarbledFix44 = "8=FIX.4.4\0019=abc\00135=0\00134=99\00110=000\001";

// Every message the parser gives back while `bytes` arrive one at a time.
std::vector<std::string> ReadByteByByte(const std::string& bytes) {
  FIX::Parser parser;
  std::vector<std::string> messages;
  std::string message;
  for (const char byte : bytes) {
    parser.addToStream(&byte, 1);
    while (parser.readNextFixMessage(message)) {
      messages.push_back(message);
    }
  }
  return messages;
}

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
    krx::SessionPair pair("KRXSE25");
    WHEN("the counterparty resets to the number the session expects") {
      pair.Write(SequenceReset(pair, 1, 1));
      THEN("the connection stays open and a Logon still logs on") {
        REQUIRE_FALSE(pair.WaitClosed(std::chrono::milliseconds(500)));
        pair.Write(pair.Logon(1));
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

SCENARIO("The parser skips a garbled frame and returns the good one behind it",
         "[krx][v1][security][p0-7]") {
  GIVEN("a frame whose BodyLength is not a number, then a good frame, in one read") {
    FIX::Parser parser;
    parser.addToStream(std::string(kBadLengthFrame) + kGoodFrame);
    WHEN("the next message is read") {
      std::string message;
      const bool read = parser.readNextFixMessage(message);
      THEN("it is the good frame, whole") {
        INFO(message);
        REQUIRE((read && message == kGoodFrame));
      }
    }
  }
}

SCENARIO("A frame without BodyLength does not borrow the next frame's",
         "[krx][v1][security][p0-7]") {
  GIVEN("a frame with no BodyLength, then a good frame, arriving one byte at a time") {
    const std::string bytes = std::string(kNoLengthFrame) + kGoodFrame;
    WHEN("the parser reads as the bytes arrive") {
      const std::vector<std::string> messages = ReadByteByByte(bytes);
      THEN("it gives back the good frame only") {
        INFO(messages.size());
        REQUIRE((messages == std::vector<std::string>{kGoodFrame}));
      }
    }
  }
}

SCENARIO("Good frames around a garbled one are each read once", "[krx][v1][security][p0-7]") {
  GIVEN("a good frame, a garbled frame and a good frame, arriving one byte at a time") {
    const std::string bytes = std::string(kGoodFrame) + kBadLengthFrame + kGoodFrame;
    WHEN("the parser reads as the bytes arrive") {
      const std::vector<std::string> messages = ReadByteByByte(bytes);
      THEN("it gives back the two good frames") {
        INFO(messages.size());
        REQUIRE((messages == std::vector<std::string>{kGoodFrame, kGoodFrame}));
      }
    }
  }
}

SCENARIO("A frame whose second field is not BodyLength is rejected as soon as it shows",
         "[krx][v1][security][p0-7]") {
  GIVEN("just the BeginString and the first three bytes of a MsgType field") {
    FIX::Parser parser;
    parser.addToStream(std::string("8=FIX.4.2\00135"));
    THEN("reading it throws, without waiting for more bytes") {
      std::string message;
      REQUIRE_THROWS_AS(parser.readFixMessage(message), FIX::MessageParseError);
    }
  }
}

SCENARIO("A frame start that arrives with the garbled frame before it is kept",
         "[krx][v1][security][p0-7]") {
  GIVEN("a garbled frame and the first byte of a good frame in one read, the rest in the next") {
    const std::string good = kGoodFrame;
    FIX::Parser parser;
    parser.addToStream(std::string("8=FIX.4.2\0019=A\001") + good.front());
    std::string message;
    const bool early = parser.readNextFixMessage(message);
    WHEN("the rest of the good frame arrives") {
      parser.addToStream(good.substr(1));
      const bool read = parser.readNextFixMessage(message);
      THEN("the good frame is read, whole") {
        INFO(message);
        REQUIRE((!early && read && message == good));
      }
    }
  }
}

SCENARIO("After a garbled frame the next good message is processed", "[krx][v1][security][p0-7]") {
  GIVEN("a logged-on session") {
    krx::SessionPair pair("KRXG28");
    LogOn(pair);
    WHEN("a frame without BodyLength and a TestRequest arrive together") {
      pair.Write(std::string(kNoLengthFrame) + TestRequest(pair, 2));
      THEN("the session answers the TestRequest with a Heartbeat and stays logged on") {
        const std::string answer = krx::MsgTypeOf(pair.ReadFrame(kWait));
        INFO(answer);
        REQUIRE((answer == "0"));
        REQUIRE(pair.WaitLoggedOn(kWait));
      }
    }
  }
}

SCENARIO("A garbled frame never re-delivers the previous message", "[krx][v1][security][p0-7]") {
  GIVEN("a logged-on session") {
    krx::SessionPair pair("KRXG29");
    LogOn(pair);
    WHEN("a TestRequest and then a garbled frame arrive together, then another TestRequest") {
      pair.Write(TestRequest(pair, 2) + kGarbledFix44);
      const std::string first = krx::MsgTypeOf(pair.ReadFrame(kWait));
      pair.Write(TestRequest(pair, 3));
      const std::string second = krx::MsgTypeOf(pair.ReadFrame(kWait));
      THEN("both are answered with a Heartbeat, and no Logout for a repeated sequence number") {
        INFO(first << " then " << second);
        REQUIRE((first == "0" && second == "0"));
      }
    }
  }
}

SCENARIO("Good messages queued behind a garbled frame are still delivered",
         "[krx][v1][security][p0-7]") {
  GIVEN("a logged-on session") {
    krx::SessionPair pair("KRXG210");
    LogOn(pair);
    WHEN("a garbled frame and two TestRequests arrive together") {
      pair.Write(kGarbledFix44 + TestRequest(pair, 2) + TestRequest(pair, 3));
      const std::string first = krx::MsgTypeOf(pair.ReadFrame(kWait));
      const std::string second = krx::MsgTypeOf(pair.ReadFrame(kWait));
      THEN("both TestRequests are answered") {
        INFO(first << " then " << second);
        REQUIRE((first == "0" && second == "0"));
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
