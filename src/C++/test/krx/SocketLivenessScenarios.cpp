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
