// Phase 3 of docs/V1_BDD_CHECKLIST.md: a socket never freezes a session.
// Row 3.1 (P0-3): when the counterparty stops reading, a send fails within the send timeout and the
// session disconnects, instead of blocking the thread (and the session lock) for ever.
// Row 3.3 (P0-3): every session socket has keepalive and, on Linux, a TCP user timeout of two
// heartbeat intervals, so the kernel drops a peer that stops acknowledging.
// Rows 3.2 and 3.4 (P0-3): disconnect() releases a sender stuck in send at once, before it waits
// for the session lock that sender holds.
#include <chrono>
#include <string>

#include <catch_amalgamated.hpp>

#include "Dictionary.h"
#include "FieldTypes.h"
#include "FixFields.h"
#include "Message.h"
#include "MessageStore.h"
#include "Session.h"
#include "SessionFactory.h"
#include "SessionID.h"
#include "SessionPair.h"

namespace {

constexpr std::chrono::seconds kWait{5};
// Larger than any socketpair's buffers, smaller than the parser's 1 MiB BodyLength cap.
constexpr std::size_t kLargeText = std::size_t{600} * 1024;

// The counterparty logs on with this heartbeat interval; the session answers with its Logon.
void LogOn(krx::SessionPair& pair, int heart_bt_int = 30) {
  pair.Write(pair.Logon(1, heart_bt_int));
  REQUIRE(pair.WaitLoggedOn(kWait));
  const std::string answer = krx::MsgTypeOf(pair.ReadFrame(kWait));
  INFO(answer);
  REQUIRE((answer == "A"));
}

// A BusinessMessageReject the session sent as its message 5, with a Text too large for the socket.
std::string LargeStoredMessage(const std::string& counterparty) {
  FIX::Message stored;
  FIX::Header& header = stored.getHeader();
  header.setField(FIX::BeginString("FIX.4.4"));
  header.setField(FIX::MsgType("j"));
  header.setField(FIX::SenderCompID("KRXGW"));
  header.setField(FIX::TargetCompID(counterparty));
  header.setField(FIX::MsgSeqNum(5));
  header.setField(FIX::SendingTime(FIX::UtcTimeStamp::now()));
  stored.setField(FIX::RefMsgType("D"));
  stored.setField(FIX::BusinessRejectReason(0));
  stored.setField(FIX::Text(std::string(kLargeText, 'x')));
  return stored.toString();
}

// The counterparty asks for message 5 again, so the session sends the large message.
void AskForMessage5(const krx::SessionPair& pair) {
  FIX::Message request;
  request.getHeader().setField(FIX::MsgType("2"));  // ResendRequest
  request.setField(FIX::BeginSeqNo(5));
  request.setField(FIX::EndSeqNo(0));
  pair.Write(pair.FromCounterparty(request, 2));
}

// An integer socket option of `s`.
int SocketOption(FIX::socket_handle s, int level, int name) {
  int value = 0;
  socklen_t size = sizeof(value);
  getsockopt(s, level, name, &value, &size);
  return value;
}

// How long unacknowledged data may stay before the kernel drops the connection: Linux's
// TCP_USER_TIMEOUT, macOS's TCP_RXT_CONNDROPTIME (seconds).
int DropAfterMs(FIX::socket_handle s) {
#ifdef TCP_USER_TIMEOUT
  return SocketOption(s, IPPROTO_TCP, TCP_USER_TIMEOUT);
#else
  return SocketOption(s, IPPROTO_TCP, TCP_RXT_CONNDROPTIME) * 1000;
#endif
}

krx::PairOptions WithLargeMessage5(const std::string& counterparty, int send_timeout_ms) {
  krx::PairOptions options;
  options.stored_messages[5] = LargeStoredMessage(counterparty);
  options.send_timeout_ms = send_timeout_ms;
  return options;
}

}  // namespace

SCENARIO("The send timeout is half the heartbeat interval by default",
         "[krx][v1][security][p0-3]") {
  GIVEN("a session whose counterparty logged on with HeartBtInt 8") {
    krx::SessionPair pair("KRXST1");
    LogOn(pair, 8);
    THEN("a send may take 4 seconds") {
      const int timeout = pair.SendTimeoutMs();
      INFO(timeout);
      REQUIRE((timeout == 4000));
    }
  }
}

SCENARIO("Before the heartbeat interval is known the send timeout is 15 seconds",
         "[krx][v1][security][p0-3]") {
  GIVEN("an acceptor session that has not received a Logon") {
    krx::SessionPair pair("KRXST2");
    THEN("a send may take 15 seconds") {
      const int timeout = pair.SendTimeoutMs();
      INFO(timeout);
      REQUIRE((timeout == 15000));
    }
  }
}

SCENARIO("A configured SendTimeout overrides the default", "[krx][v1][security][p0-3]") {
  GIVEN("a session configured with a 300 ms send timeout") {
    krx::PairOptions options;
    options.send_timeout_ms = 300;
    krx::SessionPair pair("KRXST3", options);
    LogOn(pair);
    THEN("a send may take 300 ms, whatever the heartbeat interval") {
      const int timeout = pair.SendTimeoutMs();
      INFO(timeout);
      REQUIRE((timeout == 300));
    }
  }
}

SCENARIO("A session's TCP socket has keepalive on", "[krx][v1][security][p0-3]") {
  GIVEN("a session on a TCP connection") {
    krx::PairOptions options;
    options.tcp = true;
    krx::SessionPair pair("KRXKA33", options);
    WHEN("the counterparty logs on and the session answers") {
      LogOn(pair, 8);
      THEN("keepalive is on for the session's socket") {
        REQUIRE((SocketOption(pair.SessionSocket(), SOL_SOCKET, SO_KEEPALIVE) != 0));
      }
    }
  }
}

SCENARIO("A session's TCP user timeout is two heartbeat intervals", "[krx][v1][security][p0-3]") {
  GIVEN("a session on a TCP connection") {
    krx::PairOptions options;
    options.tcp = true;
    krx::SessionPair pair("KRXUT33", options);
    WHEN("the counterparty logs on with HeartBtInt 8 and the session answers") {
      LogOn(pair, 8);
      THEN("unacknowledged data drops the connection after 16 seconds") {
        const int timeout = DropAfterMs(pair.SessionSocket());
        INFO(timeout);
        REQUIRE((timeout == 16000));
      }
    }
  }
}

SCENARIO("The SendTimeout setting is read in seconds", "[krx][v1][security][p0-3]") {
  GIVEN("an acceptor session configured with SendTimeout=7") {
    FIX::Dictionary settings;
    settings.setString("ConnectionType", "acceptor");
    settings.setBool("UseDataDictionary", false);
    settings.setString("StartTime", "00:00:00");
    settings.setString("EndTime", "00:00:00");
    settings.setInt("SendTimeout", 7);
    krx::LogonWatcher application;
    FIX::MemoryStoreFactory stores;
    FIX::SessionFactory factory(application, stores, nullptr);
    const FIX::SessionID id(FIX::BeginString("FIX.4.4"), FIX::SenderCompID("KRXGW"),
                            FIX::TargetCompID("KRXSTF"));
    FIX::Session* session = factory.create(id, settings);
    THEN("a send may take 7 seconds") {
      const int timeout = session->getSendTimeoutMs();
      factory.destroy(session);
      INFO(timeout);
      REQUIRE((timeout == 7000));
    }
  }
}

SCENARIO("When the counterparty stops reading the send fails and the session disconnects",
         "[krx][v1][security][p0-3]") {
  GIVEN("a logged-on session with a 300 ms send timeout and a large message to resend") {
    krx::SessionPair pair("KRXST31", WithLargeMessage5("KRXST31", 300));
    LogOn(pair);
    WHEN("the counterparty asks for it and then reads nothing") {
      const auto asked = std::chrono::steady_clock::now();
      AskForMessage5(pair);
      const bool timed_out = pair.WaitForEvent("Send timed out", kWait);
      const auto took = std::chrono::steady_clock::now() - asked;
      THEN("the send fails within the timeout (plus scheduling slack) and the connection closes") {
        INFO(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
             << " ms; events:\n"
             << pair.Events().Dump());
        REQUIRE(
            (timed_out && took < std::chrono::milliseconds(300 + 1000) && pair.WaitClosed(kWait)));
      }
    }
  }
}

SCENARIO("disconnect releases a sender stuck in send before it waits for the session lock",
         "[krx][v1][security][p0-3]") {
  GIVEN("a session stuck sending a large resend to a counterparty that reads nothing") {
    krx::SessionPair pair("KRXDC34", WithLargeMessage5("KRXDC34", 10000));
    LogOn(pair);
    AskForMessage5(pair);
    REQUIRE(pair.WaitForEvent("Received ResendRequest", kWait));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // into the stuck send
    WHEN("the gateway disconnects the session") {
      const auto start = std::chrono::steady_clock::now();
      pair.Disconnect();
      const auto took = std::chrono::steady_clock::now() - start;
      THEN("disconnect returns at once, not after the 10 s send timeout") {
        INFO(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
             << " ms; events:\n"
             << pair.Events().Dump());
        REQUIRE((took < std::chrono::seconds(2) && !pair.Events().Contains("Send timed out")));
      }
    }
  }
}

SCENARIO("A counterparty that reads within the send timeout gets the whole message",
         "[krx][v1][security][p0-3]") {
  GIVEN("a logged-on session with a 3 s send timeout and a large message to resend") {
    krx::SessionPair pair("KRXST32", WithLargeMessage5("KRXST32", 3000));
    LogOn(pair);
    WHEN("the counterparty asks for it and starts reading after half a second") {
      AskForMessage5(pair);
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      const std::string again = pair.ReadFrame(kWait);
      THEN("the message arrives whole and nothing timed out") {
        INFO(pair.Events().Dump());
        REQUIRE((krx::MsgTypeOf(again) == "j" && again.size() > kLargeText &&
                 !pair.Events().Contains("Send timed out")));
      }
    }
  }
}
