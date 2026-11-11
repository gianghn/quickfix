#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "Application.h"
#include "MessageStore.h"
#include "Parser.h"
#include "Session.h"
#include "SessionID.h"
#include "ThreadedSocketConnection.h"
#include "Utility.h"

namespace krx {

// Tells a test when the session logged on, from the session's own callback: reading the session's
// state from another thread is a data race in this engine (P1-1).
class LogonWatcher : public FIX::Application {
 public:
  [[nodiscard]] bool LoggedOn() const { return logged_on_.load(); }

  void onCreate(const FIX::SessionID& /*id*/) override {}

  void onLogon(const FIX::SessionID& /*id*/) override { logged_on_.store(true); }

  void onLogout(const FIX::SessionID& /*id*/) override { logged_on_.store(false); }

  void toAdmin(FIX::Message& /*message*/, const FIX::SessionID& /*id*/) override {}

  void toApp(FIX::Message& /*message*/, const FIX::SessionID& /*id*/) override {}

  void fromAdmin(const FIX::Message& /*message*/, const FIX::SessionID& /*id*/) override {}

  void fromApp(const FIX::Message& /*message*/, const FIX::SessionID& /*id*/) override {}

 private:
  std::atomic<bool> logged_on_{false};
};

// An acceptor session on one end of a socketpair; a test plays the counterparty on the other end.
// The real ThreadedSocketConnection reads, parses and answers, with no network. Sessions live in a
// process-wide registry, so every pair needs its own counterparty comp ID.
class SessionPair {
 public:
  explicit SessionPair(const std::string& counterparty);
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
  // A Logon from the counterparty with this sequence number, sent now.
  [[nodiscard]] std::string Logon(int seq_num) const;

 private:
  LogonWatcher application_;
  FIX::MemoryStoreFactory store_;
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
