// Phase 3 of docs/V1_BDD_CHECKLIST.md: a socket never freezes a session.
// Row 3.1 (P0-3): when the counterparty stops reading, a send fails within the send timeout and the
// session disconnects, instead of blocking the thread (and the session lock) for ever.
// Row 3.3 (P0-3): every session socket has keepalive and, on Linux, a TCP user timeout of two
// heartbeat intervals, so the kernel drops a peer that stops acknowledging.
// Rows 3.2 and 3.4 (P0-3): disconnect() releases a sender stuck in send at once, before it waits
// for the session lock that sender holds; stop() returns with no thread left in send.
// Row 3.5 (P0-3): a large resend lets a sender in between chunks, and stops when the session
// disconnects.
// Rows 3.6 and 3.7 (P1-3): a signal that interrupts the reading thread's poll is not a socket
// error; the session stays logged on, idle or busy.
// Rows 3.8 and 3.9 (P1-4): a failed accept is logged and the acceptor keeps taking connections:
// at once after a signal, after a pause when it ran out of file descriptors.
// Row 3.10 (P1-10): the session's timers run once a second even while bytes keep arriving, so a
// peer trickling bytes is timed out.
// Row 3.11 (P1-12): a connect gives up after 2 s, so the initiator reaches the backup host fast
// when the primary drops every packet.
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <catch_amalgamated.hpp>

#include "Application.h"
#include "Dictionary.h"
#include "FieldTypes.h"
#include "FixFields.h"
#include "Message.h"
#include "MessageStore.h"
#include "Session.h"
#include "SessionFactory.h"
#include "SessionID.h"
#include "SessionPair.h"
#include "SessionSettings.h"
#include "ThreadedSocketAcceptor.h"
#include "ThreadedSocketInitiator.h"
#include "Utility.h"

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

// A BusinessMessageReject the session sent as message `seq_num`, with a Text of `text_size` bytes
// (by default too large for the socket).
std::string StoredMessage(const std::string& counterparty, int seq_num = 5,
                          std::size_t text_size = kLargeText) {
  FIX::Message stored;
  FIX::Header& header = stored.getHeader();
  header.setField(FIX::BeginString("FIX.4.4"));
  header.setField(FIX::MsgType("j"));
  header.setField(FIX::SenderCompID("KRXGW"));
  header.setField(FIX::TargetCompID(counterparty));
  header.setField(FIX::MsgSeqNum(seq_num));
  header.setField(FIX::SendingTime(FIX::UtcTimeStamp::now()));
  stored.setField(FIX::RefMsgType("D"));
  stored.setField(FIX::BusinessRejectReason(0));
  stored.setField(FIX::Text(std::string(text_size, 'x')));
  return stored.toString();
}

// The counterparty asks for every message from `begin_seq_no` again.
void AskForMessagesFrom(const krx::SessionPair& pair, int begin_seq_no) {
  FIX::Message request;
  request.getHeader().setField(FIX::MsgType("2"));  // ResendRequest
  request.setField(FIX::BeginSeqNo(begin_seq_no));
  request.setField(FIX::EndSeqNo(0));
  pair.Write(pair.FromCounterparty(request, 2));
}

// The counterparty asks for message 5 again, so the session sends the large message.
void AskForMessage5(const krx::SessionPair& pair) {
  AskForMessagesFrom(pair, 5);
}

// A new order from the gateway: a BusinessMessageReject whose Text marks it.
FIX::Message NewOrder() {
  FIX::Message order;
  order.getHeader().setField(FIX::MsgType("j"));
  order.setField(FIX::RefMsgType("D"));
  order.setField(FIX::BusinessRejectReason(0));
  order.setField(FIX::Text("new-order"));
  return order;
}

constexpr int kResent = 2000;  // messages 2..2001, about 2 MB: far more than a socket buffer holds

krx::PairOptions WithManyStoredMessages(const std::string& counterparty) {
  krx::PairOptions options;
  for (int seq = 2; seq < kResent + 2; ++seq) {
    options.stored_messages[seq] = StoredMessage(counterparty, seq, 1024);
  }
  return options;
}

// Reads what the session sends until the new order or the timeout, and counts the resent
// (PossDupFlag) messages that came before it; -1 when the order never came.
int ResentBeforeOrder(krx::SessionPair& pair) {
  int resent = 0;
  for (;;) {
    const std::string frame = pair.ReadFrame(kWait);
    if (frame.empty()) {
      return -1;
    }
    if (frame.contains("\00158=new-order\001")) {
      return resent;
    }
    resent += frame.contains("\00143=Y\001") ? 1 : 0;
  }
}

// Reads until `count` resent (PossDupFlag) messages came, or the timeout; how many came.
int ReadResent(krx::SessionPair& pair, int count) {
  int resent = 0;
  while (resent < count) {
    const std::string frame = pair.ReadFrame(kWait);
    if (frame.empty()) {
      return resent;
    }
    resent += frame.contains("\00143=Y\001") ? 1 : 0;
  }
  return resent;
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

// A handler that does nothing, installed without SA_RESTART so a signal interrupts poll (EINTR),
// as a profiler, debugger or runtime signal would.
void IgnoreSignal(int /*signal*/) {}

void InstallInterruptingHandler() {
  struct sigaction action{};
  action.sa_handler = IgnoreSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  sigaction(SIGUSR1, &action, nullptr);
}

// Sends `count` SIGUSR1s to `thread`, one a millisecond.
void SignalStorm(std::thread::native_handle_type thread, int count) {
  for (int i = 0; i < count; ++i) {
    pthread_kill(thread, SIGUSR1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

std::string TestRequest(const krx::SessionPair& pair, int seq_num) {
  FIX::Message request;
  request.getHeader().setField(FIX::MsgType("1"));  // TestRequest
  request.setField(FIX::TestReqID("still-there"));
  return pair.FromCounterparty(request, seq_num);
}

// Sends TestRequests 2..(count + 1) and counts the Heartbeats that answer them.
int Heartbeats(krx::SessionPair& pair, int count) {
  int answered = 0;
  for (int seq = 2; seq < count + 2; ++seq) {
    pair.Write(TestRequest(pair, seq));
    answered += krx::MsgTypeOf(pair.ReadFrame(kWait)) == "0" ? 1 : 0;
  }
  return answered;
}

// A port no one listens on now, for an acceptor to take.
int FreePort() {
  const FIX::socket_handle probe = FIX::socket_createAcceptor(0, true);
  const int port = static_cast<std::uint16_t>(FIX::socket_hostport(probe));  // a short: 32768+
  FIX::socket_close(probe);
  return port;
}

// The gateway's threaded acceptor, listening on its own port for one counterparty; its events
// (accepted connections, failed accepts) are kept. `extra_settings` go into [DEFAULT].
class ListeningAcceptor {
 public:
  explicit ListeningAcceptor(const std::string& counterparty,
                             const std::string& extra_settings = "")
      : port_(FreePort()),
        settings_(Settings(port_, counterparty, extra_settings)),
        acceptor_(application_, store_, settings_, logs_) {
    acceptor_.start();
  }

  ~ListeningAcceptor() { acceptor_.stop(true); }

  // The gateway stops the acceptor, as at the end of the day or on shutdown.
  void Stop() { acceptor_.stop(true); }

  ListeningAcceptor(const ListeningAcceptor&) = delete;
  ListeningAcceptor& operator=(const ListeningAcceptor&) = delete;
  ListeningAcceptor(ListeningAcceptor&&) = delete;
  ListeningAcceptor& operator=(ListeningAcceptor&&) = delete;

  [[nodiscard]] int Port() const { return port_; }

  [[nodiscard]] const krx::EventLog& Events() const { return logs_.Events(); }

 private:
  static FIX::SessionSettings Settings(int port, const std::string& counterparty,
                                       const std::string& extra_settings) {
    std::istringstream stream(
        "[DEFAULT]\nConnectionType=acceptor\nSocketAcceptPort=" + std::to_string(port) +
        "\nSocketReuseAddress=Y\nStartTime=00:00:00\nEndTime=00:00:00\n"
        "UseDataDictionary=N\n" +
        extra_settings +
        "[SESSION]\nBeginString=FIX.4.4\nSenderCompID=KRXGW\nTargetCompID=" + counterparty + "\n");
    FIX::SessionSettings settings;
    stream >> settings;
    return settings;
  }

  int port_;
  FIX::NullApplication application_;
  FIX::MemoryStoreFactory store_;
  krx::EventLogFactory logs_;
  FIX::SessionSettings settings_;
  FIX::ThreadedSocketAcceptor acceptor_;
};

// A Logon (MsgSeqNum 1, HeartBtInt 30) from `counterparty` to the gateway (KRXGW).
std::string LogonFrom(const std::string& counterparty) {
  FIX::Message logon;
  FIX::Header& header = logon.getHeader();
  header.setField(FIX::BeginString("FIX.4.4"));
  header.setField(FIX::MsgType("A"));
  header.setField(FIX::SenderCompID(counterparty));
  header.setField(FIX::TargetCompID("KRXGW"));
  header.setField(FIX::MsgSeqNum(1));
  header.setField(FIX::SendingTime(FIX::UtcTimeStamp::now()));
  logon.setField(FIX::EncryptMethod(0));
  logon.setField(FIX::HeartBtInt(30));
  return logon.toString();
}

// The exchange's end: connects to the gateway on `port` and logs on as `counterparty`; the test
// then reads nothing from it.
FIX::socket_handle ConnectAndLogOn(int port, const std::string& counterparty) {
  const FIX::socket_handle exchange = FIX::socket_createConnector();
  FIX::socket_connect(exchange, "127.0.0.1", port);
  const std::string logon = LogonFrom(counterparty);
  FIX::socket_send(exchange, logon.data(), logon.size());
  return exchange;
}

// A gateway thread sending a message too large for the socket to `counterparty`'s session.
class LargeSender {
 public:
  explicit LargeSender(const std::string& counterparty)
      : session_(FIX::Session::lookupSession(FIX::SessionID("FIX.4.4", "KRXGW", counterparty))),
        thread_([this] {
          if (session_ != nullptr) {
            FIX::Message order = NewOrder();
            order.setField(FIX::Text(std::string(kLargeText, 'x')));
            session_->send(order);
          }
          returned_.store(true);
        }) {}

  ~LargeSender() { thread_.join(); }

  LargeSender(const LargeSender&) = delete;
  LargeSender& operator=(const LargeSender&) = delete;
  LargeSender(LargeSender&&) = delete;
  LargeSender& operator=(LargeSender&&) = delete;

  // True once its send is over (sent, failed or timed out).
  [[nodiscard]] bool Returned() const { return returned_.load(); }

  // True once its send is over, waiting up to `timeout` for it.
  [[nodiscard]] bool WaitReturned(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!Returned() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return Returned();
  }

  // True when there was no such session, so nothing was sent.
  [[nodiscard]] bool Failed() const { return session_ == nullptr; }

 private:
  FIX::Session* session_;
  std::atomic<bool> returned_{false};
  std::thread thread_;
};

// A host that drops every packet (non-routable): a connect to it gets no answer at all.
constexpr const char* kUnreachableHost = "10.255.255.1";

// The gateway's threaded initiator for one session, with a primary host that drops every packet
// and a backup on `backup_port`, retrying every second; its events are kept.
class FailoverInitiator {
 public:
  // The primary is `primary_host`:`primary_port` (by default the host that drops every packet, on
  // the backup's port).
  FailoverInitiator(const std::string& counterparty, int backup_port,
                    const std::string& primary_host = kUnreachableHost, int primary_port = 0)
      : settings_(Settings(counterparty, backup_port, primary_host,
                           primary_port == 0 ? backup_port : primary_port)),
        initiator_(application_, store_, settings_, logs_) {
    initiator_.start();
  }

  ~FailoverInitiator() { initiator_.stop(true); }

  FailoverInitiator(const FailoverInitiator&) = delete;
  FailoverInitiator& operator=(const FailoverInitiator&) = delete;
  FailoverInitiator(FailoverInitiator&&) = delete;
  FailoverInitiator& operator=(FailoverInitiator&&) = delete;

  [[nodiscard]] const krx::EventLog& Events() const { return logs_.Events(); }

 private:
  static FIX::SessionSettings Settings(const std::string& counterparty, int backup_port,
                                       const std::string& primary_host, int primary_port) {
    std::istringstream stream(
        "[DEFAULT]\nConnectionType=initiator\nReconnectInterval=1\nHeartBtInt=30\n"
        "StartTime=00:00:00\nEndTime=00:00:00\nUseDataDictionary=N\n[SESSION]\n"
        "BeginString=FIX.4.4\nSenderCompID=" +
        counterparty + "\nTargetCompID=KRXGW\nSocketConnectHost=" + primary_host +
        "\nSocketConnectPort=" + std::to_string(primary_port) +
        "\nSocketConnectHost1=127.0.0.1\nSocketConnectPort1=" + std::to_string(backup_port) + "\n");
    FIX::SessionSettings settings;
    stream >> settings;
    return settings;
  }

  FIX::NullApplication application_;
  FIX::MemoryStoreFactory store_;
  krx::EventLogFactory logs_;
  FIX::SessionSettings settings_;
  FIX::ThreadedSocketInitiator initiator_;
};

// Blocks SIGUSR1 in the calling thread while it lives, so a signal sent to the process goes to a
// thread started before (the acceptor's).
class SignalsBlockedHere {
 public:
  SignalsBlockedHere() {
    sigset_t usr1{};
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &usr1, &saved_);
  }

  ~SignalsBlockedHere() { pthread_sigmask(SIG_SETMASK, &saved_, nullptr); }

  SignalsBlockedHere(const SignalsBlockedHere&) = delete;
  SignalsBlockedHere& operator=(const SignalsBlockedHere&) = delete;
  SignalsBlockedHere(SignalsBlockedHere&&) = delete;
  SignalsBlockedHere& operator=(SignalsBlockedHere&&) = delete;

 private:
  sigset_t saved_{};
};

// While it lives the process has no file descriptor left: the open-file limit is lowered to 256
// (when higher) and descriptors are opened up to it. Both are given back when it goes.
class DescriptorsExhausted {
 public:
  DescriptorsExhausted() : base_(FIX::socket_createConnector()) {
    getrlimit(RLIMIT_NOFILE, &saved_);
    rlimit lowered = saved_;
    lowered.rlim_cur = std::min<rlim_t>(saved_.rlim_cur, 256);
    setrlimit(RLIMIT_NOFILE, &lowered);
    held_.reserve(256);
    for (int fd = dup(base_); fd >= 0; fd = dup(base_)) {
      held_.push_back(fd);
    }
  }

  ~DescriptorsExhausted() {
    for (const int fd : held_) {
      close(fd);
    }
    close(base_);
    setrlimit(RLIMIT_NOFILE, &saved_);
  }

  DescriptorsExhausted(const DescriptorsExhausted&) = delete;
  DescriptorsExhausted& operator=(const DescriptorsExhausted&) = delete;
  DescriptorsExhausted(DescriptorsExhausted&&) = delete;
  DescriptorsExhausted& operator=(DescriptorsExhausted&&) = delete;

 private:
  int base_;
  rlimit saved_{};
  std::vector<int> held_;
};

krx::PairOptions WithLargeMessage5(const std::string& counterparty, int send_timeout_ms) {
  krx::PairOptions options;
  options.stored_messages[5] = StoredMessage(counterparty);
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

SCENARIO("A sender can place an order while a large resend is in progress",
         "[krx][v1][security][p0-3]") {
  GIVEN("a logged-on session resending 2000 messages to a counterparty that reads nothing yet") {
    krx::SessionPair pair("KRXRS35", WithManyStoredMessages("KRXRS35"));
    LogOn(pair);
    AskForMessagesFrom(pair, 2);
    REQUIRE(pair.WaitForEvent("Resending Message", kWait));
    WHEN("a sender places an order and the counterparty starts reading") {
      bool sent = false;
      std::thread sender([&pair, &sent] { sent = pair.Send(NewOrder()); });
      const int resent = ResentBeforeOrder(pair);
      sender.join();
      THEN("the order goes out before the resend has finished") {
        INFO(resent << " resent messages came before the order; events:\n" << pair.Events().Dump());
        REQUIRE((sent && resent >= 0 && resent < kResent));
      }
    }
  }
}

SCENARIO("A large resend with no sender waiting is not slowed down", "[krx][v1][security][p0-3]") {
  GIVEN("a logged-on session with 2000 messages to resend") {
    krx::SessionPair pair("KRXRS35S", WithManyStoredMessages("KRXRS35S"));
    LogOn(pair);
    WHEN("the counterparty asks for them all and reads as fast as it can") {
      const auto asked = std::chrono::steady_clock::now();
      AskForMessagesFrom(pair, 2);
      const int resent = ReadResent(pair, kResent);
      const auto took = std::chrono::steady_clock::now() - asked;
      THEN("all 2000 arrive within 5 seconds") {
        INFO(resent << " resent in "
                    << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
                    << " ms");
        REQUIRE((resent == kResent && took < std::chrono::seconds(5)));
      }
    }
  }
}

SCENARIO("A disconnect during a large resend stops the resend", "[krx][v1][security][p0-3]") {
  GIVEN("a logged-on session resending 2000 messages to a counterparty that reads nothing") {
    krx::SessionPair pair("KRXRS35D", WithManyStoredMessages("KRXRS35D"));
    LogOn(pair);
    AskForMessagesFrom(pair, 2);
    REQUIRE(pair.WaitForEvent("Resending Message", kWait));
    WHEN("the gateway disconnects the session") {
      pair.Disconnect();
      pair.WaitReaderStopped();
      const std::size_t resent = pair.Events().Count("Resending Message");
      THEN("the session stops resending instead of running through the rest") {
        INFO(resent << " messages resent");
        REQUIRE((resent < static_cast<std::size_t>(kResent)));
      }
    }
  }
}

// Row U-8 (8.7): the engine's onStop shuts each socket down and waits for its thread; a resend
// that went on after its sends failed kept stop() waiting (minutes under load, krx_stress seed 4).
SCENARIO("A large resend stops when its socket is shut down", "[krx][v1][security][u-8]") {
  GIVEN("a logged-on session resending 2000 messages to a counterparty that reads nothing") {
    krx::SessionPair pair("KRXRSU8", WithManyStoredMessages("KRXRSU8"));
    LogOn(pair);
    AskForMessagesFrom(pair, 2);
    REQUIRE(pair.WaitForEvent("Resending Message", kWait));
    WHEN("the engine shuts the socket down, as stop() does") {
      ::shutdown(pair.SessionSocket(), SHUT_RDWR);
      pair.WaitReaderStopped();
      const std::size_t resent = pair.Events().Count("Resending Message");
      THEN("the session stops resending instead of running through the rest") {
        INFO(resent << " messages resent; events:\n" << pair.Events().Dump());
        REQUIRE((resent < static_cast<std::size_t>(kResent)));
      }
    }
  }
}

// Row U-6 (8.5, owner decision Q14): after an outage both sides resend, each from its reading
// thread, so neither read and both sends timed out, again on every reconnect (krx_stress echo-on).
// The counterparty here only reads once its own backlog is written.
namespace {
std::string CounterpartyBacklog(const krx::SessionPair& pair, int first_seq, int count,
                                bool resent = false) {
  std::string backlog;
  for (int seq = first_seq; seq < first_seq + count; ++seq) {
    FIX::Message message;
    message.getHeader().setField(FIX::MsgType("j"));
    if (resent) {
      message.getHeader().setField(FIX::PossDupFlag(true));
      message.getHeader().setField(FIX::OrigSendingTime(FIX::UtcTimeStamp::now()));
    }
    message.setField(FIX::RefMsgType("D"));
    message.setField(FIX::BusinessRejectReason(0));
    message.setField(FIX::Text(std::string(1024, 'y')));
    backlog += pair.FromCounterparty(message, seq);
  }
  return backlog;
}

// Whether `count` application messages reached the gateway's fromApp within 5 s.
bool AppMessagesReach(const krx::SessionPair& pair, int count) {
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (pair.AppMessages() < count && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pair.AppMessages() == count;
}
}  // namespace

SCENARIO("A resend completes while the counterparty is resending its own backlog",
         "[krx][v1][security][u-6]") {
  GIVEN("a logged-on session with 2000 messages to resend and a 1 s send timeout") {
    krx::PairOptions options = WithManyStoredMessages("KRXRSU6");
    options.send_timeout_ms = 1000;
    krx::SessionPair pair("KRXRSU6", options);
    LogOn(pair);
    WHEN("the counterparty asks for them, then writes 2000 messages of its own before reading") {
      AskForMessagesFrom(pair, 2);
      const bool written = pair.WriteWithin(CounterpartyBacklog(pair, 3, kResent), kWait);
      const int resent = written ? ReadResent(pair, kResent) : 0;
      const bool delivered = AppMessagesReach(pair, kResent);
      THEN("every message is resent, the counterparty's all arrive, and no send timed out") {
        INFO("backlog written: " << written << ", " << resent << " resent, " << pair.AppMessages()
                                 << " delivered; events:\n"
                                 << pair.Events().Dump());
        REQUIRE((written && resent == kResent && delivered &&
                 !pair.Events().Contains("Send timed out")));
      }
    }
  }
}

namespace {
// Three gateway threads placing orders as fast as they can until `stop`.
class OrderFlood {
 public:
  explicit OrderFlood(krx::SessionPair& pair) {
    for (int i = 0; i < 3; ++i) {
      threads_.emplace_back([this, &pair] {
        while (!stop_.load()) {
          pair.Send(NewOrder());
        }
      });
    }
  }

  ~OrderFlood() { stop_.store(true); }

  OrderFlood(const OrderFlood&) = delete;
  OrderFlood& operator=(const OrderFlood&) = delete;
  OrderFlood(OrderFlood&&) = delete;
  OrderFlood& operator=(OrderFlood&&) = delete;

 private:
  std::atomic<bool> stop_{false};
  std::vector<std::jthread> threads_;
};

// The counterparty skips message 2, sends 3 to `count` + 2, waits for the ResendRequest and
// resends from 2; returns once the gateway's fromApp saw all of them, or after 10 s.
void SkipThenResend(const krx::SessionPair& pair, int count) {
  REQUIRE(pair.WriteWithin(CounterpartyBacklog(pair, 3, count), kWait));
  REQUIRE(pair.WaitForEvent("Sent ResendRequest", kWait));
  REQUIRE(pair.WriteWithin(CounterpartyBacklog(pair, 2, count + 1, true), kWait));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (pair.AppMessages() < count + 1 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}
}  // namespace

// Row 3.5 again (8.5, owner decision Q15): the resend let waiting senders in before every message,
// up to 10 ms each, so under a steady order flow it crawled at about 100 messages a second and
// the peer queued everything sent meanwhile (krx_stress echo-on: 1.9 GB, OOM).
SCENARIO("A large resend finishes quickly while senders keep placing orders",
         "[krx][v1][security][p0-3]") {
  GIVEN("a logged-on session with 2000 messages to resend and three senders placing orders") {
    krx::SessionPair pair("KRXRSQ15", WithManyStoredMessages("KRXRSQ15"));
    LogOn(pair);
    const OrderFlood flood(pair);
    WHEN("the counterparty asks for them all and reads as fast as it can") {
      const auto asked = std::chrono::steady_clock::now();
      AskForMessagesFrom(pair, 2);
      const int resent = ReadResent(pair, kResent);
      const auto took = std::chrono::steady_clock::now() - asked;
      THEN("all 2000 arrive within 5 seconds") {
        INFO(resent << " resent in "
                    << std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
                    << " ms");
        REQUIRE((resent == kResent && took < std::chrono::seconds(5)));
      }
    }
  }
}

// Row U-9 (8.3, owner decision Q16): messages above a gap were queued without a limit.
SCENARIO("At most 10,000 messages wait above a gap, and the resend brings the rest",
         "[krx][v1][security][u-9]") {
  GIVEN("a logged-on session") {
    krx::SessionPair pair("KRXRSU9");
    LogOn(pair);
    WHEN("the counterparty skips message 2, sends 3 to 20002, then resends from 2") {
      SkipThenResend(pair, 20000);
      THEN("the queue stopped at 10,000, said so, and every message arrived once") {
        INFO(pair.AppMessages() << " delivered");
        // 10,000 dropped: an event on the 1st, 1001st, … 9001st.
        REQUIRE((pair.Events().Count("Queue full") == 10 && pair.AppMessages() == 20001));
      }
    }
  }
}

namespace {
// The counterparty skips message 2, sends 3, then `admin` (its MsgType set) as 4; the session's
// next expected target seqnum after that.
int ExpectedAfterGapThen(const krx::SessionPair& pair, FIX::Message admin) {
  FIX::Message order;
  order.getHeader().setField(FIX::MsgType("j"));
  order.setField(FIX::RefMsgType("D"));
  order.setField(FIX::BusinessRejectReason(0));
  pair.Write(pair.FromCounterparty(order, 3));
  REQUIRE(pair.WaitForEvent("Sent ResendRequest", kWait));
  pair.Write(pair.FromCounterparty(std::move(admin), 4));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  return pair.Store()->getNextTargetMsgSeqNum();
}
}  // namespace

// Row U-4 (8.2, P0-10): a Logout or Reject above a gap moved the expected seqnum on by one, so the
// first missing message counted as received and the resend after the next logon skipped it
// (krx_stress echo-on: single orders lost with no GapFill or Reject; the research's seq 26173).
SCENARIO("A Logout above a gap leaves the missing message to the resend",
         "[krx][v1][security][u-4]") {
  GIVEN("a logged-on session") {
    krx::SessionPair pair("KRXRSU4L");
    LogOn(pair);
    WHEN("the counterparty skips message 2, sends 3, then logs out as 4") {
      FIX::Message logout;
      logout.getHeader().setField(FIX::MsgType("5"));
      const int expected = ExpectedAfterGapThen(pair, logout);
      THEN("the session still expects message 2") {
        INFO("expects " << expected << "; events:\n" << pair.Events().Dump());
        REQUIRE((expected == 2));
      }
    }
  }
}

SCENARIO("A Reject above a gap leaves the missing message to the resend",
         "[krx][v1][security][u-4]") {
  GIVEN("a logged-on session") {
    krx::SessionPair pair("KRXRSU4R");
    LogOn(pair);
    WHEN("the counterparty skips message 2, sends 3, then a Reject as 4") {
      FIX::Message reject;
      reject.getHeader().setField(FIX::MsgType("3"));
      reject.setField(FIX::RefSeqNum(1));
      const int expected = ExpectedAfterGapThen(pair, reject);
      THEN("the session still expects message 2") {
        INFO("expects " << expected << "; events:\n" << pair.Events().Dump());
        REQUIRE((expected == 2));
      }
    }
  }
}

// Row U-8 (8.7): what U-6's wait read was all handed to the session after stop() had shut the
// socket down, so stop() waited for up to 64 MB of messages (krx_stress echo-on, 2 of 20 runs).
SCENARIO("A shut-down connection drops the backlog it read while sending",
         "[krx][v1][security][u-8]") {
  GIVEN("a session resending 2000 messages while the counterparty writes 20,000 of its own") {
    krx::SessionPair pair("KRXRSU8D", WithManyStoredMessages("KRXRSU8D"));
    LogOn(pair);
    AskForMessagesFrom(pair, 2);
    REQUIRE(pair.WriteWithin(CounterpartyBacklog(pair, 3, 20000), kWait));
    WHEN("the engine shuts the socket down, as stop() does") {
      ::shutdown(pair.SessionSocket(), SHUT_RDWR);
      pair.WaitReaderStopped();
      THEN("the session stops instead of processing the rest of that backlog") {
        INFO(pair.AppMessages() << " of 20000 processed after the shutdown");
        REQUIRE((pair.AppMessages() < 20000));
      }
    }
  }
}

// Row U-9 (Q16): the cap is the MaxQueuedMessages setting; 0 is no limit, as upstream.
SCENARIO("The MaxQueuedMessages setting caps the queue above a gap", "[krx][v1][security][u-9]") {
  GIVEN("an acceptor session configured with MaxQueuedMessages=5") {
    FIX::Dictionary settings;
    settings.setString("ConnectionType", "acceptor");
    settings.setBool("UseDataDictionary", false);
    settings.setString("StartTime", "00:00:00");
    settings.setString("EndTime", "00:00:00");
    settings.setInt("MaxQueuedMessages", 5);
    krx::LogonWatcher application;
    FIX::MemoryStoreFactory stores;
    FIX::SessionFactory factory(application, stores, nullptr);
    const FIX::SessionID id(FIX::BeginString("FIX.4.4"), FIX::SenderCompID("KRXGW"),
                            FIX::TargetCompID("KRXMQM"));
    FIX::Session* session = factory.create(id, settings);
    THEN("at most 5 messages wait above a gap") {
      const int cap = session->getMaxQueuedMessages();
      factory.destroy(session);
      REQUIRE((cap == 5));
    }
  }
}

SCENARIO("A queue refuses messages past its cap, and 0 means no limit", "[krx][v1][u-9]") {
  GIVEN("a session state") {
    FIX::SessionState state(FIX::UtcTimeStamp::now());
    WHEN("its cap is 2 and three messages arrive, then the cap is 0 and one more arrives") {
      state.maxQueued(2);
      const bool first = state.queue(3, FIX::Message());
      const bool second = state.queue(4, FIX::Message());
      const bool third = state.queue(5, FIX::Message());
      state.maxQueued(0);
      const bool fourth = state.queue(6, FIX::Message());
      THEN("the third is refused and the fourth is kept") {
        REQUIRE((first && second && !third && fourth));
      }
    }
  }
}

SCENARIO("A signal storm on an idle session causes no disconnect", "[krx][v1][security][p1-3]") {
  GIVEN("a logged-on, idle session") {
    InstallInterruptingHandler();
    krx::SessionPair pair("KRXSG36");
    LogOn(pair);
    WHEN("its reading thread gets 200 signals") {
      SignalStorm(pair.ReaderThread(), 200);
      THEN("the session still answers a TestRequest") {
        INFO(pair.Events().Dump());
        REQUIRE((Heartbeats(pair, 1) == 1));
      }
    }
  }
}

SCENARIO("A signal storm on a busy session causes no disconnect", "[krx][v1][security][p1-3]") {
  GIVEN("a logged-on session") {
    InstallInterruptingHandler();
    krx::SessionPair pair("KRXSG37");
    LogOn(pair);
    WHEN("its reading thread gets signals while 50 TestRequests come in") {
      std::thread storm([&pair] { SignalStorm(pair.ReaderThread(), 300); });
      const int answered = Heartbeats(pair, 50);
      storm.join();
      THEN("every TestRequest is answered") {
        INFO(answered << " answered; events:\n" << pair.Events().Dump());
        REQUIRE((answered == 50));
      }
    }
  }
}

SCENARIO("The acceptor keeps taking connections after an interrupted accept",
         "[krx][v1][security][p1-4]") {
  GIVEN("a listening acceptor whose accepting thread alone takes the process's signals") {
    InstallInterruptingHandler();
    const ListeningAcceptor acceptor("KRXAC38");
    const SignalsBlockedHere blocked;
    WHEN("50 signals interrupt its accept and then a counterparty connects") {
      for (int i = 0; i < 50; ++i) {
        kill(getpid(), SIGUSR1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      const FIX::socket_handle counterparty = FIX::socket_createConnector();
      FIX::socket_connect(counterparty, "127.0.0.1", acceptor.Port());
      const bool accepted = acceptor.Events().WaitFor("Accepted connection from", kWait);
      FIX::socket_close(counterparty);
      THEN("the connection is accepted") {
        INFO(acceptor.Events().Dump());
        REQUIRE(accepted);
      }
    }
  }
}

SCENARIO("The acceptor backs off and recovers when it runs out of file descriptors",
         "[krx][v1][security][p1-4]") {
  GIVEN("a listening acceptor") {
    const ListeningAcceptor acceptor("KRXAC39");
    WHEN("a counterparty connects while no file descriptor is left, and again once they free up") {
      const FIX::socket_handle first = FIX::socket_createConnector();
      const FIX::socket_handle second = FIX::socket_createConnector();
      bool reported = false;
      std::size_t failures = 0;
      {
        const DescriptorsExhausted exhausted;
        FIX::socket_connect(first, "127.0.0.1", acceptor.Port());
        reported = acceptor.Events().WaitFor("Accept failed", std::chrono::seconds(2));
        // Half a second without descriptors: with the 100 ms pause about five failed accepts;
        // without it (Linux keeps the connection waiting) the loop would spin through thousands.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        failures = acceptor.Events().Count("Accept failed");
      }
      // macOS drops the connection whose accept failed; Linux keeps it waiting. Either way the
      // acceptor must take the next one.
      FIX::socket_connect(second, "127.0.0.1", acceptor.Port());
      const bool accepted = acceptor.Events().WaitFor("Accepted connection from", kWait);
      FIX::socket_close(first);
      FIX::socket_close(second);
      THEN("the failure is logged, retried after a pause, and a connection is accepted after") {
        INFO(failures << " failed accepts logged in half a second");
        REQUIRE((reported && failures <= 20 && accepted));
      }
    }
  }
}

SCENARIO("A counterparty trickling one byte every 500 ms is timed out per HeartBtInt",
         "[krx][v1][security][p1-10]") {
  GIVEN("a session whose counterparty logged on with HeartBtInt 1") {
    krx::SessionPair pair("KRXTR310");
    LogOn(pair, 1);
    WHEN("the counterparty then sends nothing but one byte every 500 ms") {
      std::atomic<bool> trickling{true};
      std::thread trickle([&pair, &trickling] {
        for (int i = 0; i < 20 && trickling.load(); ++i) {
          pair.Write("x");
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
      });
      const bool closed = pair.WaitClosed(std::chrono::seconds(8));
      trickling.store(false);
      trickle.join();
      THEN("the session times it out for missing heartbeats and disconnects") {
        INFO(pair.Events().Dump());
        REQUIRE((closed && pair.Events().Contains("Timed out waiting for heartbeat")));
      }
    }
  }
}

SCENARIO("When the primary exchange host is unreachable the initiator reaches the backup in 5 s",
         "[krx][v1][security][p1-12]") {
  GIVEN("an exchange listening on its backup host only") {
    const ListeningAcceptor exchange("KRXFO311");
    WHEN("the gateway's initiator starts with a primary host that drops every packet") {
      const auto started = std::chrono::steady_clock::now();
      const FailoverInitiator gateway("KRXFO311", exchange.Port());
      const bool reached = exchange.Events().WaitFor("Accepted connection from", kWait);
      const bool connected = gateway.Events().WaitFor("Connection succeeded", kWait);
      const auto took = std::chrono::steady_clock::now() - started;
      // The session writes through the connection only once it is connected (P0-11).
      const bool logged_on = gateway.Events().WaitFor("Received logon response", kWait);
      THEN("it tried the primary first, reached the backup within 5 seconds and logged on") {
        INFO(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
             << " ms; gateway events:\n"
             << gateway.Events().Dump());
        REQUIRE((gateway.Events().Contains(std::string("Connecting to ") + kUnreachableHost) &&
                 reached && connected && took < std::chrono::seconds(5) && logged_on));
      }
    }
  }
}

SCENARIO("When the primary exchange host refuses the connection the initiator moves to the backup",
         "[krx][v1][security][p1-12]") {
  GIVEN("an exchange listening on its backup host, and nothing listening on the primary's port") {
    const ListeningAcceptor exchange("KRXFO312");
    const int refusing = FreePort();
    WHEN("the gateway's initiator starts") {
      const FailoverInitiator gateway("KRXFO312", exchange.Port(), "127.0.0.1", refusing);
      const bool connected = gateway.Events().WaitFor("Connection succeeded", kWait);
      THEN("the refused attempt is reported as failed, and only the backup connection succeeds") {
        INFO(gateway.Events().Dump());
        REQUIRE((connected && gateway.Events().Contains("Connection failed") &&
                 gateway.Events().Count("Connection succeeded") == 1));
      }
    }
  }
}

SCENARIO("When the primary exchange host name does not resolve the initiator moves to the backup",
         "[krx][v1][security][p1-12]") {
  GIVEN("an exchange listening on its backup host, and a primary host name that cannot resolve") {
    const ListeningAcceptor exchange("KRXFO313");
    WHEN("the gateway's initiator starts") {
      const FailoverInitiator gateway("KRXFO313", exchange.Port(), "no-such-host.invalid");
      const bool connected = gateway.Events().WaitFor("Connection succeeded", kWait);
      THEN("the unresolved attempt is reported as failed, and only the backup one succeeds") {
        INFO(gateway.Events().Dump());
        REQUIRE((connected && gateway.Events().Contains("Connection failed") &&
                 gateway.Events().Count("Connection succeeded") == 1));
      }
    }
  }
}

SCENARIO("When the exchange stops reading, stop() returns and no thread is left in send",
         "[krx][v1][security][p0-3]") {
  GIVEN("a counterparty that reads nothing and a sender stuck in send (10 s send timeout)") {
    ListeningAcceptor gateway("KRXSP32", "SendTimeout=10\n");
    const FIX::socket_handle exchange = ConnectAndLogOn(gateway.Port(), "KRXSP32");
    const bool logged_on = gateway.Events().WaitFor("Responding to logon request", kWait);
    const LargeSender sender("KRXSP32");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // into the stuck send
    WHEN("the gateway stops the acceptor") {
      const auto start = std::chrono::steady_clock::now();
      gateway.Stop();
      const auto took = std::chrono::steady_clock::now() - start;
      // stop() releases the sender by shutting its socket down; the sender then needs a moment to
      // come out of send (longer under a sanitizer), so it gets a second, far below the timeout.
      const bool sender_out = sender.WaitReturned(std::chrono::seconds(1)) && !sender.Failed();
      FIX::socket_close(exchange);
      THEN("stop returns before the 10 s send timeout, and the sender is out of send at once") {
        INFO(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()
             << " ms; events:\n"
             << gateway.Events().Dump());
        REQUIRE((logged_on && took < std::chrono::seconds(8) && sender_out));
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
