#include "SessionInput.h"

#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "FieldNumbers.h"
#include "FixFields.h"
#include "FixValues.h"
#include "Message.h"
#include "SessionPair.h"
#include "fix44/Heartbeat.h"
#include "fix44/Logon.h"
#include "fix44/Logout.h"
#include "fix44/NewOrderSingle.h"
#include "fix44/ResendRequest.h"
#include "fix44/SequenceReset.h"
#include "fix44/TestRequest.h"

namespace krx {
namespace {

enum class State : unsigned char { kBeforeLogon, kMidResend, kDuringLogout };

constexpr std::size_t kStates = 3;
constexpr std::size_t kChunkSpread = 17;
constexpr int kOrdersSent = 5;
// The counterparty's first message after its Logon: seq 2 to 9 are missing, so the session asks.
constexpr int kAfterGap = 10;
// A session whose replies fill the socket gives up soon, so an input never stalls the run.
constexpr int kSendTimeoutMs = 200;
constexpr int kHeartBtInt = 30;
constexpr const char* kCounterparty = "FUZZ";
constexpr const char* kFixedTime = "20261110-10:00:00.000";

FIX44::NewOrderSingle Order() {
  FIX44::NewOrderSingle order;
  order.set(FIX::ClOrdID("1"));
  order.set(FIX::Side(FIX::Side_BUY));
  order.setField(FIX::FIELD::TransactTime, kFixedTime);
  order.set(FIX::OrdType(FIX::OrdType_LIMIT));
  order.set(FIX::Symbol("VNM"));
  order.set(FIX::OrderQty(100));
  order.set(FIX::Price(1000));
  return order;
}

FIX44::Logon CounterpartyLogon() {
  FIX44::Logon logon;
  logon.set(FIX::EncryptMethod(FIX::EncryptMethod_NONE_OTHER));
  logon.set(FIX::HeartBtInt(kHeartBtInt));
  return logon;
}

// `message` from the counterparty, as a seed: a fixed SendingTime, so a seed is the same bytes on
// every run.
std::string Frame(FIX::Message message, int seq_num) {
  FIX::Header& header = message.getHeader();
  header.setField(FIX::BeginString("FIX.4.4"));
  header.setField(FIX::SenderCompID(kCounterparty));
  header.setField(FIX::TargetCompID("KRXGW"));
  header.setField(FIX::MsgSeqNum(seq_num));
  header.setField(FIX::FIELD::SendingTime, kFixedTime);
  return message.toString();
}

FIX44::SequenceReset GapFill(int new_seq_num) {
  FIX44::SequenceReset reset;
  reset.set(FIX::GapFillFlag(true));
  reset.set(FIX::NewSeqNo(new_seq_num));
  return reset;
}

FIX::Message PossDup(FIX::Message message) {
  message.getHeader().setField(FIX::PossDupFlag(true));
  message.getHeader().setField(FIX::FIELD::OrigSendingTime, kFixedTime);
  return message;
}

// The session as `state` needs it; true when it got there.
bool Bring(SessionPair& pair, State state) {
  if (state == State::kBeforeLogon) {
    return !pair.LoggedOn();
  }
  pair.Write(pair.Logon(1, kHeartBtInt));
  pair.Pump();
  for (int i = 0; i < kOrdersSent; ++i) {
    pair.Send(Order());
  }
  if (state == State::kMidResend) {
    pair.Write(pair.FromCounterparty(FIX44::Heartbeat(), kAfterGap));
    pair.Pump();
    return pair.LoggedOn() && pair.Events().Contains("Sent ResendRequest");
  }
  pair.Logout();
  pair.Tick();
  pair.Pump();
  return pair.LoggedOn() && pair.Events().Contains("Initiated logout request");
}

void Add(std::vector<std::string>& seeds, State state, std::initializer_list<std::string> frames) {
  std::string seed(1, static_cast<char>(state));
  for (const std::string& frame : frames) {
    seed += frame;
  }
  seeds.push_back(seed);
}

}  // namespace

bool FeedSessionInput(std::string_view input) {
  if (input.empty()) {
    return false;
  }
  const auto state = static_cast<State>(static_cast<unsigned char>(input.front()) % kStates);
  const std::string_view bytes = input.substr(1);
  PairOptions options;
  options.send_timeout_ms = kSendTimeoutMs;
  options.manual_reading = true;
  options.check_latency = false;
  SessionPair pair(kCounterparty, options);
  const bool reached = Bring(pair, state);
  // In chunks of 1 to 17, as from a socket, each handled before the next is written.
  const std::size_t chunk = 1 + (bytes.size() % kChunkSpread);
  for (std::size_t pos = 0; pos < bytes.size() && pair.Pump(); pos += chunk) {
    pair.Write(bytes.substr(pos, chunk));
  }
  pair.Pump();
  pair.Tick();
  pair.Pump();
  return reached;
}

std::vector<std::string> SessionSeeds() {
  const FIX44::ResendRequest resend_all(FIX::BeginSeqNo(1), FIX::EndSeqNo(0));
  std::vector<std::string> seeds;
  Add(seeds, State::kBeforeLogon,
      {
          Frame(CounterpartyLogon(), 1),
          Frame(FIX44::Heartbeat(), 2),
          Frame(Order(), 3),
          Frame(FIX44::TestRequest(FIX::TestReqID("T")), 4),
          Frame(FIX44::Logout(), 5),
      });
  Add(seeds, State::kBeforeLogon, {Frame(CounterpartyLogon(), 1), Frame(resend_all, 2)});
  Add(seeds, State::kBeforeLogon, {Frame(CounterpartyLogon(), kAfterGap)});
  Add(seeds, State::kBeforeLogon, {Frame(Order(), 1)});
  Add(seeds, State::kMidResend,
      {
          Frame(PossDup(GapFill(kAfterGap + 1)), 2),
          Frame(FIX44::TestRequest(FIX::TestReqID("T")), kAfterGap + 1),
      });
  Add(seeds, State::kMidResend, {Frame(PossDup(Order()), 2), Frame(resend_all, 3)});
  Add(seeds, State::kMidResend, {Frame(FIX44::Logout(), 2)});
  Add(seeds, State::kDuringLogout, {Frame(FIX44::Logout(), 2)});
  Add(seeds, State::kDuringLogout, {Frame(Order(), 2), Frame(FIX44::Logout(), 3)});
  Add(seeds, State::kDuringLogout, {Frame(resend_all, 2), Frame(FIX44::Logout(), 3)});
  return seeds;
}

}  // namespace krx
