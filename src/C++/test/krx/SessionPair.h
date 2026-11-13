#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Application.h"
#include "FileStore.h"
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
  // How many events so far contain `text`.
  [[nodiscard]] std::size_t Count(std::string_view text) const;
  // True once an event contains `text`; false on timeout.
  [[nodiscard]] bool WaitFor(std::string_view text, std::chrono::milliseconds timeout) const;
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

  // From the next onLogout on, `probe` runs on its own thread while onLogout waits up to a second
  // for it, as a gateway's onLogout handing work to another of its threads might.
  void ProbeDuringLogout(std::function<void()> probe) {
    const std::scoped_lock lock(mutex_);
    probe_ = std::move(probe);
  }

  // True when the probe returned while onLogout was still waiting for it.
  [[nodiscard]] bool ProbeReturned() const { return probe_returned_.load(); }

  // Waits for the probe's thread; before the session it uses goes away.
  void JoinProbe() {
    if (probe_thread_.joinable()) {
      probe_thread_.join();
    }
  }

  void onLogout(const FIX::SessionID& /*id*/) override;

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
  std::mutex mutex_;
  std::function<void()> probe_;
  std::thread probe_thread_;
  std::atomic<bool> probe_returned_{false};
};

// A MemoryStore whose next sender seqnum write can be made to fail once, as FileStore's does when
// its seqnums file cannot be written: the seqnum moves in memory, then the write throws.
class SeqNumFailingStore : public FIX::MessageStore {
 public:
  explicit SeqNumFailingStore(const FIX::UtcTimeStamp& now) : store_(now) {}

  // The next incrNextSenderMsgSeqNum fails, once.
  void FailNextSenderSeqNumWrite() { fail_.store(true); }

  bool set(int seq_num, const std::string& message) override {
    return store_.set(seq_num, message);
  }

  void get(int begin, int end, std::vector<std::string>& messages) const override {
    store_.get(begin, end, messages);
  }

  [[nodiscard]] int getNextSenderMsgSeqNum() const override {
    return store_.getNextSenderMsgSeqNum();
  }

  [[nodiscard]] int getNextTargetMsgSeqNum() const override {
    return store_.getNextTargetMsgSeqNum();
  }

  void setNextSenderMsgSeqNum(int value) override { store_.setNextSenderMsgSeqNum(value); }

  void setNextTargetMsgSeqNum(int value) override { store_.setNextTargetMsgSeqNum(value); }

  void incrNextSenderMsgSeqNum() override {
    store_.incrNextSenderMsgSeqNum();
    if (fail_.exchange(false)) {
      throw FIX::IOException("seqnums file could not be written (test)");
    }
  }

  void incrNextTargetMsgSeqNum() override { store_.incrNextTargetMsgSeqNum(); }

  [[nodiscard]] FIX::UtcTimeStamp getCreationTime() const override {
    return store_.getCreationTime();
  }

  void reset(const FIX::UtcTimeStamp& now) override { store_.reset(now); }

  void refresh() override { store_.refresh(); }

 private:
  FIX::MemoryStore store_;
  std::atomic<bool> fail_{false};
};

// Keeps the one store it creates, so a test can seed it before the session runs: a MemoryStore, or
// a FileStore in `file_dir` when that is not empty (syncing to disk when `sync`).
class KeptStoreFactory : public FIX::MemoryStoreFactory {
 public:
  explicit KeptStoreFactory(std::string file_dir = "", bool sync = false, bool seqnum_fails = false)
      : file_dir_(std::move(file_dir)), sync_(sync), seqnum_fails_(seqnum_fails) {}

  [[nodiscard]] FIX::MessageStore* Store() const { return store_; }

  FIX::MessageStore* create(const FIX::UtcTimeStamp& now, const FIX::SessionID& id) override {
    if (seqnum_fails_) {
      store_ = std::make_unique<SeqNumFailingStore>(now).release();
    } else {
      store_ = file_dir_.empty() ? FIX::MemoryStoreFactory::create(now, id)
                                 : FIX::FileStoreFactory(file_dir_, sync_).create(now, id);
    }
    return store_;
  }

 private:
  std::string file_dir_;
  bool sync_;
  bool seqnum_fails_;
  FIX::MessageStore* store_{nullptr};
};

// How a SessionPair is set up: the dictionary's PreserveMessageFieldsOrder, and messages stored by
// sequence number before the session runs (the next one it sends follows the last).
struct PairOptions {
  bool preserve_field_order{false};
  std::map<int, std::string> stored_messages;
  int send_timeout_ms{0};       // 0: the session's default
  bool tcp{false};              // a loopback TCP pair instead of a socketpair (for TCP options)
  std::string file_store_dir;   // a FileStore in this directory instead of a MemoryStore
  bool file_store_sync{false};  // that FileStore syncs to disk (FileStoreSync=Y)
  bool seqnum_fails{false};     // a SeqNumFailingStore (no FileStore)
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
  // The same, unless the session takes no byte for `timeout`; true when all went out.
  [[nodiscard]] bool WriteWithin(std::string_view bytes, std::chrono::milliseconds timeout) const;
  void WriteByteByByte(std::string_view bytes) const;
  // The next whole message the session sent; empty after the timeout or when the session closed.
  [[nodiscard]] std::string ReadFrame(std::chrono::milliseconds timeout);
  [[nodiscard]] bool WaitLoggedOn(std::chrono::milliseconds timeout) const;

  // True between the session's onLogon and onLogout callbacks.
  [[nodiscard]] bool LoggedOn() const { return application_.LoggedOn(); }

  // True once the session closed the connection (what it sent before is dropped); false on timeout.
  [[nodiscard]] bool WaitClosed(std::chrono::milliseconds timeout);
  // A Logon from the counterparty with this sequence number and heartbeat interval, sent now.
  [[nodiscard]] std::string Logon(int seq_num, int heart_bt_int = 30) const;
  // `message` (its MsgType set) from the counterparty with this sequence number, sent now.
  [[nodiscard]] std::string FromCounterparty(FIX::Message message, int seq_num) const;

  [[nodiscard]] const EventLog& Events() const { return logs_.Events(); }

  void ThrowFromApp() { application_.ThrowFromApp(); }

  void ThrowFromAdmin() { application_.ThrowFromAdmin(); }

  // See LogonWatcher::ProbeDuringLogout and ProbeReturned.
  void ProbeDuringLogout(std::function<void()> probe) {
    application_.ProbeDuringLogout(std::move(probe));
  }

  [[nodiscard]] bool ProbeReturned() const { return application_.ProbeReturned(); }

  [[nodiscard]] int SendTimeoutMs() { return session_.getSendTimeoutMs(); }

  // The session's store (a FileStore when PairOptions.file_store_dir was set).
  [[nodiscard]] FIX::MessageStore* Store() const { return store_.Store(); }

  // The gateway sends `message` (its MsgType set), from whichever thread calls this.
  bool Send(FIX::Message message) { return session_.send(message); }

  // The gateway disconnects the session, as on stop() or a risk limit.
  void Disconnect() { session_.disconnect(); }

  // Waits until the session's reading thread has stopped, after the connection closed.
  void WaitReaderStopped() {
    if (reader_.joinable()) {
      reader_.join();
    }
  }

  // The connection closes itself (as on a garbled message before Logon), its reading thread stops,
  // and it is deleted, as ThreadedSocketAcceptor::releaseConnection does.
  void DropConnection() {
    connection_->disconnect();
    WaitReaderStopped();
    connection_.reset();
  }

  // The thread reading for the session, for a test to aim signals at.
  [[nodiscard]] std::thread::native_handle_type ReaderThread() { return reader_.native_handle(); }

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
