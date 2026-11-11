#include "SessionPair.h"

#include <poll.h>
#include <sys/types.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "DataDictionary.h"
#include "DataDictionaryProvider.h"
#include "FieldTypes.h"
#include "FixFields.h"
#include "FixValues.h"
#include "TimeRange.h"
#include "fix44/Logon.h"

namespace krx {
namespace {

constexpr std::chrono::milliseconds kPollStep{5};
constexpr std::chrono::milliseconds kLongestPoll{60'000};
constexpr const char* kFix44 = "FIX.4.4";
// A session is an acceptor when built with HeartBtInt 0; the counterparty's Logon sets it.
constexpr int kAcceptor = 0;

FIX::DataDictionaryProvider Fix44Provider(bool preserve_field_order) {
  const auto dictionary = std::make_shared<FIX::DataDictionary>();
  // Read after the flag: the dictionary keeps each message's field order only while reading.
  dictionary->preserveMessageFieldsOrder(preserve_field_order);
  dictionary->readFromURL(std::string(KRX_SPEC_DIR) + "/FIX44.xml");
  FIX::DataDictionaryProvider provider;
  provider.addTransportDataDictionary(FIX::BeginString(kFix44), dictionary);
  return provider;
}

// The same start and end: the session is open all day.
FIX::TimeRange AllDay() {
  return {FIX::UtcTimeOnly(0, 0, 0), FIX::UtcTimeOnly(0, 0, 0)};
}

}  // namespace

SessionPair::SessionPair(const std::string& counterparty, const PairOptions& options)
    : id_(FIX::BeginString(kFix44), FIX::SenderCompID("KRXGW"), FIX::TargetCompID(counterparty)),
      session_([] { return FIX::UtcTimeStamp::now(); }, application_, store_, id_,
               Fix44Provider(options.preserve_field_order), AllDay(), kAcceptor, &logs_) {
  // Before the reader thread starts, so the store needs no lock.
  for (const auto& [seq_num, message] : options.stored_messages) {
    store_.Store()->set(seq_num, message);
    store_.Store()->setNextSenderMsgSeqNum(seq_num + 1);
  }
  // As every acceptor and initiator does: a write to a closed socket must not kill the process.
  FIX::socket_init();
  const auto [ours, theirs] = FIX::socket_createpair();
  peer_ = theirs;
  connection_ = std::make_unique<FIX::ThreadedSocketConnection>(
      ours, FIX::ThreadedSocketConnection::Sessions{id_}, nullptr);
  reader_ = std::thread([this] {
    while (connection_->read()) {
    }
  });
}

SessionPair::~SessionPair() {
  // The session reads the end of the stream, disconnects, and its reader stops.
  FIX::socket_close(peer_);
  if (reader_.joinable()) {
    reader_.join();
  }
}

void SessionPair::Write(std::string_view bytes) const {
  while (!bytes.empty()) {
    const ssize_t sent = FIX::socket_send(peer_, bytes.data(), bytes.size());
    if (sent <= 0) {
      return;
    }
    bytes.remove_prefix(static_cast<std::size_t>(sent));
  }
}

void SessionPair::WriteByteByByte(std::string_view bytes) const {
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    Write(bytes.substr(i, 1));
  }
}

std::string SessionPair::ReadFrame(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string frame;
  while (!parser_.readFixMessage(frame)) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      return {};
    }
    pollfd ready{.fd = peer_, .events = POLLIN, .revents = 0};
    if (poll(&ready, 1, static_cast<int>(std::min(left, kLongestPoll).count())) <= 0) {
      return {};
    }
    std::array<char, 4096> buffer{};
    const ssize_t got = FIX::socket_recv(peer_, buffer.data(), buffer.size());
    if (got <= 0) {
      return {};
    }
    parser_.addToStream(buffer.data(), static_cast<std::size_t>(got));
  }
  return frame;
}

bool SessionPair::WaitLoggedOn(std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!application_.LoggedOn()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(kPollStep);
  }
  return true;
}

bool SessionPair::WaitClosed(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (true) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0) {
      return false;
    }
    pollfd ready{.fd = peer_, .events = POLLIN, .revents = 0};
    if (poll(&ready, 1, static_cast<int>(std::min(left, kLongestPoll).count())) <= 0) {
      return false;
    }
    std::array<char, 4096> buffer{};
    if (FIX::socket_recv(peer_, buffer.data(), buffer.size()) <= 0) {
      return true;
    }
  }
}

std::string SessionPair::Logon(int seq_num, int heart_bt_int) const {
  FIX44::Logon logon;
  logon.set(FIX::EncryptMethod(FIX::EncryptMethod_NONE_OTHER));
  logon.set(FIX::HeartBtInt(heart_bt_int));
  return FromCounterparty(logon, seq_num);
}

std::string SessionPair::FromCounterparty(FIX::Message message, int seq_num) const {
  FIX::Header& header = message.getHeader();
  header.setField(FIX::BeginString(kFix44));
  header.setField(FIX::SenderCompID(id_.getTargetCompID()));
  header.setField(FIX::TargetCompID(id_.getSenderCompID()));
  header.setField(FIX::MsgSeqNum(seq_num));
  header.setField(FIX::SendingTime(FIX::UtcTimeStamp::now()));
  return message.toString();
}

bool EventLog::Contains(std::string_view text) const {
  const std::scoped_lock lock(mutex_);
  return std::ranges::any_of(events_,
                             [text](const std::string& event) { return event.contains(text); });
}

void EventLog::onEvent(const std::string& text) {
  const std::scoped_lock lock(mutex_);
  events_.push_back(text);
}

std::string MsgTypeOf(const std::string& frame) {
  constexpr std::string_view kTag = "\00135=";  // SOH, then 35=
  const std::size_t start = frame.find(kTag);
  if (start == std::string::npos) {
    return {};
  }
  const std::size_t value = start + kTag.size();
  return frame.substr(value, frame.find('\x01', value) - value);
}

}  // namespace krx
