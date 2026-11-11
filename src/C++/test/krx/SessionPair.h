#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Application.h"
#include "Log.h"
#include "Message.h"
#include "MessageStore.h"
#include "Parser.h"
#include "Session.h"
#include "SessionID.h"
#include "ThreadedSocketConnection.h"
#include "Utility.h"

namespace krx {

// Keeps the session's events, written from its connection thread, for the test thread to read.
class EventLog : public FIX::Log {
 public:
  [[nodiscard]] bool Contains(std::string_view text) const;
  // Every event so far, one per line, for a failing test to print.
  [[nodiscard]] std::string Dump() const;

  void clear() override {}

  void backup() override {}

  void onIncoming(const std::string& /*message*/) override {}

  void onOutgoing(const std::string& /*message*/) override {}

  void onEvent(const std::string& text) override;

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> events_;
};

// Hands the session the one EventLog it owns; nothing to free.
class EventLogFactory : public FIX::LogFactory {
 public:
  [[nodiscard]] const EventLog& Events() const { return log_; }

  FIX::Log* create() override { return &log_; }

  FIX::Log* create(const FIX::SessionID& /*id*/) override { return &log_; }

  void destroy(FIX::Log* /*log*/) override {}

 private:
  EventLog log_;
};

// Tells a test when the session logged on, from the session's own callback: reading the session's
// state from another thread is a data race in this engine (P1-1). ThrowFromApp makes fromApp fail
// the way a gateway callback can.
class LogonWatcher : public FIX::Application {
 public:
  [[nodiscard]] bool LoggedOn() const { return logged_on_.load(); }

  void ThrowFromApp() { throw_from_app_.store(true); }

  void ThrowFromAdmin() { throw_from_admin_.store(true); }

  void onCreate(const FIX::SessionID& /*id*/) override {}

  void onLogon(const FIX::SessionID& /*id*/) override { logged_on_.store(true); }

  void onLogout(const FIX::SessionID& /*id*/) override { logged_on_.store(false); }

  void toAdmin(FIX::Message& /*message*/, const FIX::SessionID& /*id*/) override {
    if (throw_from_admin_.load()) {
      throw std::runtime_error("gateway admin callback failed");
    }
  }

  void toApp(FIX::Message& /*message*/, const FIX::SessionID& /*id*/) override {}

  void fromAdmin(const FIX::Message& /*message*/, const FIX::SessionID& /*id*/) override {}

  void fromApp(const FIX::Message& /*message*/, const FIX::SessionID& /*id*/) override {
    if (throw_from_app_.load()) {
      throw std::runtime_error("gateway callback failed");
    }
  }

 private:
  std::atomic<bool> logged_on_{false};
  std::atomic<bool> throw_from_app_{false};
  std::atomic<bool> throw_from_admin_{false};
};

// Keeps the one MemoryStore it creates, so a test can seed it before the session runs.
class KeptStoreFactory : public FIX::MemoryStoreFactory {
 public:
  [[nodiscard]] FIX::MessageStore* Store() const { return store_; }

  FIX::MessageStore* create(const FIX::UtcTimeStamp& now, const FIX::SessionID& id) override {
    store_ = FIX::MemoryStoreFactory::create(now, id);
    return store_;
  }

 private:
  FIX::MessageStore* store_{nullptr};
};

// How a SessionPair is set up: the dictionary's PreserveMessageFieldsOrder, and messages stored by
// sequence number before the session runs (the next one it sends follows the last).
struct PairOptions {
  bool preserve_field_order{false};
  std::map<int, std::string> stored_messages;
  int send_timeout_ms{0};  // 0: the session's default
  bool tcp{false};         // a loopback TCP pair instead of a socketpair (for TCP options)
};

// An acceptor session on one end of a socketpair; a test plays the counterparty on the other end.
// The real ThreadedSocketConnection reads, parses and answers, with no network. Sessions live in a
// process-wide registry, so every pair needs its own counterparty comp ID.
class SessionPair {
 public:
  explicit SessionPair(const std::string& counterparty, const PairOptions& options = {});
  ~SessionPair();
  SessionPair(const SessionPair&) = delete;
  SessionPair& operator=(const SessionPair&) = delete;
  SessionPair(SessionPair&&) = delete;
  SessionPair& operator=(SessionPair&&) = delete;

  // Bytes the session will read, all at once or one send per byte.
  void Write(std::string_view bytes) const;
  void WriteByteByByte(std::string_view bytes) const;
  // The next whole message the session sent; empty after the timeout or when the session closed.
  [[nodiscard]] std::string ReadFrame(std::chrono::milliseconds timeout);
  [[nodiscard]] bool WaitLoggedOn(std::chrono::milliseconds timeout) const;
  // True once the session closed the connection (what it sent before is dropped); false on timeout.
  [[nodiscard]] bool WaitClosed(std::chrono::milliseconds timeout);
  // A Logon from the counterparty with this sequence number and heartbeat interval, sent now.
  [[nodiscard]] std::string Logon(int seq_num, int heart_bt_int = 30) const;
  // `message` (its MsgType set) from the counterparty with this sequence number, sent now.
  [[nodiscard]] std::string FromCounterparty(FIX::Message message, int seq_num) const;

  [[nodiscard]] const EventLog& Events() const { return logs_.Events(); }

  void ThrowFromApp() { application_.ThrowFromApp(); }

  void ThrowFromAdmin() { application_.ThrowFromAdmin(); }

  [[nodiscard]] int SendTimeoutMs() { return session_.getSendTimeoutMs(); }

  // The session's end of the connection.
  [[nodiscard]] FIX::socket_handle SessionSocket() const { return connection_->getSocket(); }

  // True once the session logged an event containing `text`; false on timeout.
  [[nodiscard]] bool WaitForEvent(std::string_view text, std::chrono::milliseconds timeout) const;

 private:
  LogonWatcher application_;
  EventLogFactory logs_;
  KeptStoreFactory store_;
  FIX::SessionID id_;
  FIX::Session session_;
  FIX::socket_handle peer_{-1};
  std::unique_ptr<FIX::ThreadedSocketConnection> connection_;
  std::thread reader_;
  FIX::Parser parser_;
};

// The MsgType (35) of a raw FIX frame; empty when the frame has none.
[[nodiscard]] std::string MsgTypeOf(const std::string& frame);

}  // namespace krx
