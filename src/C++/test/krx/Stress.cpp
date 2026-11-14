// Plan items 8.1-8.5: the chaos stress run (docs/V1_PLAN.md, from the research harness).
// One ThreadedSocketAcceptor (SRV) and one ThreadedSocketInitiator with N FIX.4.4 sessions
// (CLI0..CLIn-1) on 127.0.0.1. T threads per session send NewOrderSingles; the acceptor echoes each
// one back unless --no-echo. A chaos loop meanwhile logs sessions out and on, disconnects them from
// a foreign thread, stops and starts both sides and destroys and recreates the initiator, each step
// under a watchdog. Then every session logs back on, and the run waits until both sides agree on
// the sequence numbers and nothing more arrives. Per stream (session, thread) it counts:
//   LOST       sendToTarget returned true, the peer's fromApp never saw it
//   UNEXPECTED delivered, never sent
//   DUP, OOO   delivered twice, out of order
// Each lost message is traced to its MsgSeqNum and to the GapFill or Reject that skipped it.
// sendToTarget returning false may still deliver the message later by a resend (P0-6): "late".
// Usage: krx_stress [seconds=30] [sessions=4] [threads=4] [port=7700] [seed=1] [workdir]
//                   [--no-echo] [--file-log] [--settle=<seconds, 120>] [--hang-grace=<seconds, 0>]
//                   [--stop-limit=<seconds a stop may take, 15; more under a sanitizer>]
//                   [--event-log] [--soak] [--rate=<orders/s per sender thread>] [--chaos-ms=<150>]
//                   [--report=<seconds between progress lines, 5>]
// A soak (plan item 8.5, hours): --soak --rate=200 --chaos-ms=5000 --report=60; each progress
// line has the process RSS and open descriptors, which must stay flat.
// Exit status: 0 PASS, 1 anomalies, 3 a step hung (the watchdog), 2 bad usage.
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Application.h"
#include "FileLog.h"
#include "FileStore.h"
#include "Session.h"
#include "SessionSettings.h"
#include "ThreadedSocketAcceptor.h"
#include "ThreadedSocketInitiator.h"
#include "fix44/NewOrderSingle.h"

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;
using std::chrono::seconds;

using std::string_view_literals::operator""sv;

constexpr int kHung = 3;
constexpr int kExamples = 8;
constexpr std::string_view kReject = "3"sv;
constexpr std::string_view kSequenceReset = "4"sv;

struct Options {
  int seconds = 30;
  int sessions = 4;
  int threads = 4;
  int port = 7700;
  unsigned seed = 1;
  std::filesystem::path workdir = "stress_work";
  bool echo = true;
  bool file_log = false;
  bool event_log = false;  // only each session's events (small, unlike --file-log)
  int settle_seconds = 120;
  int hang_grace_seconds = 0;
  int stop_limit_seconds = 15;  // plan item 8.7: every stop under load within 15 s
  bool soak = false;            // no per-message seqnum records (a lost order prints no seq)
  int rate = 0;                 // orders a second per sender thread; 0: as fast as it goes
  int chaos_ms = 150;           // the least time between chaos steps (up to 350 ms more)
  int report_seconds = 5;       // how often a progress line is printed
};

// The value of a "--name=value" argument.
int FlagValue(const std::string& text) {
  return std::stoi(text.substr(text.find('=') + 1));
}

// Sets the option a "--name=value" argument names; false when it names none.
bool SetIntFlag(const std::string& text, Options& options) {
  using Flag = std::pair<std::string_view, int Options::*>;
  const std::array<Flag, 6> flags{
      {
          {"--settle=", &Options::settle_seconds},
          {"--hang-grace=", &Options::hang_grace_seconds},
          {"--stop-limit=", &Options::stop_limit_seconds},
          {"--rate=", &Options::rate},
          {"--chaos-ms=", &Options::chaos_ms},
          {"--report=", &Options::report_seconds},
      },
  };
  for (const auto& [name, field] : flags) {
    if (text.starts_with(name)) {
      options.*field = FlagValue(text);
      return true;
    }
  }
  return false;
}

Options Parse(std::span<char*> args) {
  Options options;
  std::vector<std::string> positional;
  for (const char* arg : args.subspan(1)) {
    const std::string text = arg;
    if (text == "--no-echo") {
      options.echo = false;
    } else if (text == "--file-log") {
      options.file_log = true;
    } else if (text == "--event-log") {
      options.event_log = true;
    } else if (text == "--soak") {
      options.soak = true;
    } else if (!SetIntFlag(text, options)) {
      positional.push_back(text);
    }
  }
  const auto at = [&positional](std::size_t index) { return index < positional.size(); };
  options.seconds = at(0) ? std::stoi(positional[0]) : options.seconds;
  options.sessions = at(1) ? std::stoi(positional[1]) : options.sessions;
  options.threads = at(2) ? std::stoi(positional[2]) : options.threads;
  options.port = at(3) ? std::stoi(positional[3]) : options.port;
  options.seed = at(4) ? static_cast<unsigned>(std::stoul(positional[4])) : options.seed;
  options.workdir = at(5) ? std::filesystem::path(positional[5]) : options.workdir;
  return options;
}

// ---------- what was sent and received, per stream "s<session>.t<thread>[.echo]" ----------

// One bit per message number; a stream's numbers are dense from 0, so a soak's hundreds of
// millions of messages fit in a few MB.
class Bits {
 public:
  // True when the bit was already set.
  bool Set(std::int64_t number) {
    const auto index = static_cast<std::size_t>(number);
    words_.resize(std::max(words_.size(), (index / kWord) + 1));
    const std::uint64_t mask = std::uint64_t{1} << (index % kWord);
    const bool was = (words_[index / kWord] & mask) != 0;
    words_[index / kWord] |= mask;
    return was;
  }

  [[nodiscard]] bool Test(std::int64_t number) const {
    const auto index = static_cast<std::size_t>(number);
    return index / kWord < words_.size() &&
           (words_[index / kWord] & (std::uint64_t{1} << (index % kWord))) != 0;
  }

  // One past the highest number that can be set.
  [[nodiscard]] std::int64_t End() const {
    return static_cast<std::int64_t>(words_.size()) * static_cast<std::int64_t>(kWord);
  }

 private:
  static constexpr std::size_t kWord = 64;
  std::vector<std::uint64_t> words_;
};

struct Stream {
  Bits sent_ok;
  Bits sent_false;
  Bits received;
  std::int64_t received_count = 0;
  std::int64_t last = -1;
  std::int64_t dup = 0;
  std::int64_t ooo = 0;
};

class Tracker {
 public:
  void Sent(const std::string& key, std::int64_t number, bool ok) {
    const std::scoped_lock lock(mutex_);
    (ok ? streams_[key].sent_ok : streams_[key].sent_false).Set(number);
  }

  void Received(const std::string& key, std::int64_t number) {
    const std::scoped_lock lock(mutex_);
    Stream& stream = streams_[key];
    if (stream.received.Set(number)) {
      ++stream.dup;
    } else {
      ++stream.received_count;
    }
    if (number <= stream.last) {
      ++stream.ooo;
    } else {
      stream.last = number;
    }
  }

  [[nodiscard]] std::int64_t Count() const {
    const std::scoped_lock lock(mutex_);
    std::int64_t count = 0;
    for (const auto& entry : streams_) {
      count += entry.second.received_count;
    }
    return count;
  }

  [[nodiscard]] std::map<std::string, Stream> Snapshot() const {
    const std::scoped_lock lock(mutex_);
    return streams_;
  }

 private:
  mutable std::mutex mutex_;
  std::map<std::string, Stream> streams_;
};

// The MsgSeqNum each message first went out with (toApp runs after the header is filled); 4 bytes
// a message, so a soak (--soak) records none.
class Sequences {
 public:
  void Disable() { enabled_.store(false); }

  void Record(const std::string& key, std::int64_t number, int seq) {
    if (!enabled_.load()) {
      return;
    }
    const std::scoped_lock lock(mutex_);
    std::vector<int>& seqs = seqs_[key];
    const auto index = static_cast<std::size_t>(number);
    seqs.resize(std::max(seqs.size(), index + 1));
    if (seqs[index] == 0) {
      seqs[index] = seq;
    }
  }

  [[nodiscard]] int Find(const std::string& key, std::int64_t number) const {
    const std::scoped_lock lock(mutex_);
    const auto stream = seqs_.find(key);
    const auto index = static_cast<std::size_t>(number);
    return stream == seqs_.end() || index >= stream->second.size() ? 0 : stream->second[index];
  }

 private:
  std::atomic<bool> enabled_{true};
  mutable std::mutex mutex_;
  std::map<std::string, std::vector<int>> seqs_;
};

// What made a receiving session skip incoming sequence numbers: GapFills it received, Rejects it
// sent. Keyed by the client's CompID (CLI<n>).
class Skips {
 public:
  void GapFill(const std::string& client, int from, int to) {
    const std::scoped_lock lock(mutex_);
    gap_fills_[client].emplace_back(from, to);
  }

  void Rejected(const std::string& client, int seq, const std::string& text) {
    const std::scoped_lock lock(mutex_);
    rejects_[client][seq] = text;
  }

  [[nodiscard]] std::string Explain(const std::string& client, int seq) const {
    const std::scoped_lock lock(mutex_);
    if (const auto rejects = rejects_.find(client); rejects != rejects_.end()) {
      if (const auto found = rejects->second.find(seq); found != rejects->second.end()) {
        return "rejected: " + found->second;
      }
    }
    const auto fills = gap_fills_.find(client);
    if (fills == gap_fills_.end()) {
      return {};
    }
    const auto covers = [seq](const std::pair<int, int>& fill) {
      return fill.first <= seq && seq < fill.second;
    };
    const auto fill = std::ranges::find_if(fills->second, covers);
    return fill == fills->second.end()
               ? std::string()
               : std::format("gap-filled {}..{}", fill->first, fill->second - 1);
  }

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::vector<std::pair<int, int>>> gap_fills_;
  std::map<std::string, std::map<int, std::string>> rejects_;
};

struct Direction {
  Tracker tracker;
  Sequences sequences;  // the sending side's
  Skips skips;          // the receiving side's
};

bool ParseId(const std::string& id, std::string& key, std::int64_t& number) {
  const auto at = id.rfind(".n");
  if (at == std::string::npos) {
    return false;
  }
  key = id.substr(0, at);
  number = std::stoll(id.substr(at + 2));
  return true;
}

std::string ClientOf(const std::string& key) {
  return "CLI" + key.substr(1, key.find('.') - 1);
}

// ---------- the application ----------

class StressApp : public FIX::Application {
 public:
  StressApp(bool acceptor, bool echo, Direction& outgoing, Direction& incoming)
      : acceptor_(acceptor), echo_(echo), outgoing_(outgoing), incoming_(incoming) {}

  void onCreate(const FIX::SessionID& /*id*/) override {}

  void onLogon(const FIX::SessionID& /*id*/) override { logons_.fetch_add(1); }

  void onLogout(const FIX::SessionID& /*id*/) override { logouts_.fetch_add(1); }

  void toAdmin(FIX::Message& message, const FIX::SessionID& id) override {
    if (message.getHeader().getField(FIX::FIELD::MsgType) != kReject) {
      return;
    }
    FIX::RefSeqNum ref;
    FIX::Text text;
    message.getFieldIfSet(ref);
    message.getFieldIfSet(text);
    incoming_.skips.Rejected(Client(id), ref.getValue(), text.getValue());
  }

  void toApp(FIX::Message& message, const FIX::SessionID& /*id*/) override {
    std::string key;
    std::int64_t number = 0;
    if (ParseId(message.getField(FIX::FIELD::ClOrdID), key, number)) {
      FIX::MsgSeqNum seq;
      message.getHeader().getField(seq);
      outgoing_.sequences.Record(key, number, seq.getValue());
    }
  }

  void fromAdmin(const FIX::Message& message, const FIX::SessionID& id) override {
    FIX::GapFillFlag gap_fill(false);
    if (message.getHeader().getField(FIX::FIELD::MsgType) != kSequenceReset ||
        !message.getFieldIfSet(gap_fill) || !gap_fill.getValue()) {
      return;
    }
    FIX::MsgSeqNum seq;
    FIX::NewSeqNo next;
    message.getHeader().getField(seq);
    message.getField(next);
    incoming_.skips.GapFill(Client(id), seq.getValue(), next.getValue());
  }

  void fromApp(const FIX::Message& message, const FIX::SessionID& id) override {
    std::string key;
    std::int64_t number = 0;
    if (!ParseId(message.getField(FIX::FIELD::ClOrdID), key, number)) {
      std::cerr << "bad ClOrdID\n";
      return;
    }
    incoming_.tracker.Received(key, number);
    if (acceptor_ && echo_) {
      Echo(message, key, number, id);
    }
  }

  [[nodiscard]] std::int64_t Logons() const { return logons_.load(); }

  [[nodiscard]] std::int64_t Logouts() const { return logouts_.load(); }

 private:
  [[nodiscard]] std::string Client(const FIX::SessionID& id) const {
    return acceptor_ ? id.getTargetCompID().getValue() : id.getSenderCompID().getValue();
  }

  // The echo of order n is message n of the echo stream: dense numbers, in the order's order.
  void Echo(const FIX::Message& message, const std::string& key, std::int64_t number,
            const FIX::SessionID& id) {
    FIX::Message echo(message);
    echo.getHeader().removeField(FIX::FIELD::PossDupFlag);
    echo.getHeader().removeField(FIX::FIELD::OrigSendingTime);
    const std::string echo_key = key + ".echo";
    echo.setField(FIX::ClOrdID(std::format("{}.n{}", echo_key, number)));
    bool ok = false;
    try {
      ok = FIX::Session::sendToTarget(echo, id);
    } catch (const std::exception&) {
      ok = false;
    }
    outgoing_.tracker.Sent(echo_key, number, ok);
  }

  bool acceptor_;
  bool echo_;
  Direction& outgoing_;
  Direction& incoming_;
  std::atomic<std::int64_t> logons_{0};
  std::atomic<std::int64_t> logouts_{0};
};

// ---------- the watchdog: a step past its deadline ends the run with status 3 ----------

class Watchdog {
 public:
  explicit Watchdog(seconds grace) : grace_(grace), thread_([this] { Watch(); }) {}

  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;
  Watchdog(Watchdog&&) = delete;
  Watchdog& operator=(Watchdog&&) = delete;

  ~Watchdog() {
    {
      const std::scoped_lock lock(mutex_);
      quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
  }

  void Guard(const std::string& step, int limit_seconds, const std::function<void()>& run) {
    {
      const std::scoped_lock lock(mutex_);
      step_ = step;
      deadline_ = Clock::now() + seconds(limit_seconds);
      armed_ = true;
    }
    run();
    const std::scoped_lock lock(mutex_);
    armed_ = false;
  }

 private:
  void Watch() {
    std::unique_lock lock(mutex_);
    while (!quit_) {
      if (armed_ && Clock::now() > deadline_) {
        std::cerr << std::format("\n*** HANG: '{}' exceeded its deadline (pid {}) ***\n", step_,
                                 ::getpid());
        std::cerr.flush();
        std::this_thread::sleep_for(grace_);  // time to take the stacks (gdb, lldb, sample)
        std::_Exit(kHung);
      }
      wake_.wait_for(lock, milliseconds(200));
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::string step_;
  Clock::time_point deadline_;
  bool armed_ = false;
  bool quit_ = false;
  seconds grace_;
  std::thread thread_;
};

// ---------- file descriptors: a canary closed or replaced by someone else is a P0-1 regression

int CountFds() {
  for (const char* folder : {"/proc/self/fd", "/dev/fd"}) {
    std::error_code error;
    const std::filesystem::directory_iterator entries(folder, error);
    if (!error) {
      return static_cast<int>(std::distance(entries, std::filesystem::directory_iterator()));
    }
  }
  return -1;
}

class Canaries {
 public:
  Canaries() = default;
  Canaries(const Canaries&) = delete;
  Canaries& operator=(const Canaries&) = delete;
  Canaries(Canaries&&) = delete;
  Canaries& operator=(Canaries&&) = delete;

  ~Canaries() {
    for (const Canary& canary : canaries_) {
      std::fclose(canary.file);
    }
  }

  // At most kMax: a soak's thousands of stops would otherwise grow the descriptor count it watches.
  void Add(const std::filesystem::path& folder) {
    constexpr std::size_t kMax = 8;
    if (canaries_.size() >= kMax) {
      return;
    }
    const std::string path = (folder / std::format("canary{}", canaries_.size())).string();
    std::FILE* file = std::fopen(path.c_str(), "w+");
    struct stat status{};
    if (file == nullptr) {
      return;
    }
    if (::fstat(::fileno(file), &status) != 0) {
      std::fclose(file);
      return;
    }
    canaries_.push_back({.file = file, .inode = status.st_ino});
  }

  void Check(const std::string& when) {
    for (const Canary& canary : canaries_) {
      const int fd = ::fileno(canary.file);
      struct stat status{};
      if (::fstat(fd, &status) != 0 || status.st_ino != canary.inode) {
        std::cerr << std::format("*** CANARY fd {} closed or replaced ({})\n", fd, when);
        ++bad_;
      }
    }
  }

  [[nodiscard]] int Bad() const { return bad_; }

  [[nodiscard]] std::size_t Size() const { return canaries_.size(); }

 private:
  struct Canary {
    std::FILE* file;
    ino_t inode;
  };

  std::vector<Canary> canaries_;
  int bad_ = 0;
};

// ---------- the two engines ----------

std::string SettingsText(const Options& options, bool acceptor) {
  const std::string side = acceptor ? "acceptor" : "initiator";
  std::ostringstream text;
  text << "[DEFAULT]\nConnectionType=" << side << "\nSocketAcceptPort=" << options.port
       << "\nSocketConnectHost=127.0.0.1\nSocketConnectPort=" << options.port
       << "\nSocketReuseAddress=Y\nHeartBtInt=1\nReconnectInterval=1\n"
       << "StartTime=00:00:00\nEndTime=00:00:00\nResetOnLogon=N\nResetOnLogout=N\n"
       << "ResetOnDisconnect=N\nFileStorePath=" << (options.workdir / (side + "-store")).string()
       << "\nFileLogPath=" << (options.workdir / (side + "-log")).string()
       << "\nUseDataDictionary=Y\nDataDictionary=" << KRX_SPEC_DIR
       << "/FIX44.xml\nBeginString=FIX.4.4\n";
  for (int session = 0; session < options.sessions; ++session) {
    text << "[SESSION]\n"
         << (acceptor ? "SenderCompID=SRV\nTargetCompID=CLI" : "SenderCompID=CLI") << session
         << (acceptor ? "\n" : "\nTargetCompID=SRV\n");
  }
  return text.str();
}

FIX::SessionSettings Settings(const Options& options, bool acceptor) {
  std::istringstream stream(SettingsText(options, acceptor));
  return {stream};
}

// --event-log: each session's events only, to <side>-log/<session>.event.current.log. FileLog also
// writes every message: gigabytes in a minute of echo-on chaos, more than a pod's disk.
class EventFileLog : public FIX::Log {
 public:
  explicit EventFileLog(const std::filesystem::path& path) : file_(path, std::ios::app) {}

  void clear() override {}

  void backup() override {}

  void onIncoming(const std::string& /*message*/) override {}

  void onOutgoing(const std::string& /*message*/) override {}

  // Buffered: a flush per event (a write per resent message) slowed a large resend until the
  // counterparty's send timed out. The file is complete once the engines are gone.
  void onEvent(const std::string& text) override {
    const std::scoped_lock lock(mutex_);
    file_ << FIX::UtcTimeStampConvertor::convert(FIX::UtcTimeStamp::now(), 6) << " : " << text
          << '\n';
  }

 private:
  std::mutex mutex_;
  std::ofstream file_;
};

class EventFileLogFactory : public FIX::LogFactory {
 public:
  explicit EventFileLogFactory(std::filesystem::path folder) : folder_(std::move(folder)) {
    std::filesystem::create_directories(folder_);
  }

  FIX::Log* create() override { return Open("GLOBAL"); }

  FIX::Log* create(const FIX::SessionID& id) override {
    std::string name = id.toString();
    std::ranges::replace(name, ':', '-');
    return Open(name);
  }

  void destroy(FIX::Log* log) override { const std::unique_ptr<FIX::Log> owned(log); }

 private:
  FIX::Log* Open(const std::string& name) {
    return std::make_unique<EventFileLog>(folder_ / (name + ".event.current.log")).release();
  }

  std::filesystem::path folder_;
};

std::unique_ptr<FIX::LogFactory> MakeLog(const Options& options,
                                         const FIX::SessionSettings& settings, bool acceptor) {
  if (options.file_log) {
    return std::make_unique<FIX::FileLogFactory>(settings);
  }
  if (options.event_log) {
    return std::make_unique<EventFileLogFactory>(options.workdir /
                                                 (acceptor ? "acceptor-log" : "initiator-log"));
  }
  return nullptr;
}

class Side {
 public:
  Side(const Options& options, bool acceptor)
      : settings_(Settings(options, acceptor)),
        store_(settings_),
        log_(MakeLog(options, settings_, acceptor)) {}

  template <class Engine>
  std::unique_ptr<Engine> Make(FIX::Application& application) {
    if (log_) {
      return std::make_unique<Engine>(application, store_, settings_, *log_);
    }
    return std::make_unique<Engine>(application, store_, settings_);
  }

 private:
  FIX::SessionSettings settings_;
  FIX::FileStoreFactory store_;
  std::unique_ptr<FIX::LogFactory> log_;
};

struct Rig {
  Rig(const Options& run_options, StressApp& acceptor_app, StressApp& initiator_app)
      : options(run_options),
        acceptor_side(run_options, true),
        initiator_side(run_options, false),
        initiator_application(initiator_app),
        acceptor(acceptor_side.Make<FIX::ThreadedSocketAcceptor>(acceptor_app)),
        initiator(initiator_side.Make<FIX::ThreadedSocketInitiator>(initiator_app)) {
    for (int session = 0; session < run_options.sessions; ++session) {
      const std::string client = std::format("CLI{}", session);
      initiator_ids.emplace_back("FIX.4.4", client, "SRV");
      acceptor_ids.emplace_back("FIX.4.4", "SRV", client);
    }
  }

  void RecreateInitiator() {
    initiator = initiator_side.Make<FIX::ThreadedSocketInitiator>(initiator_application);
  }

  [[nodiscard]] bool AllLoggedOn() const {
    const auto on = [](const FIX::SessionID& id) {
      FIX::Session* session = FIX::Session::lookupSession(id);
      return session != nullptr && session->isLoggedOn();
    };
    return std::ranges::all_of(initiator_ids, on) && std::ranges::all_of(acceptor_ids, on);
  }

  // Both sides agree on every session's sequence numbers: nothing is in flight or owed.
  [[nodiscard]] bool InStep() const {
    for (std::size_t index = 0; index < initiator_ids.size(); ++index) {
      FIX::Session* client = FIX::Session::lookupSession(initiator_ids[index]);
      FIX::Session* server = FIX::Session::lookupSession(acceptor_ids[index]);
      if (client == nullptr || server == nullptr ||
          client->getExpectedSenderNum() != server->getExpectedTargetNum() ||
          server->getExpectedSenderNum() != client->getExpectedTargetNum()) {
        return false;
      }
    }
    return true;
  }

  void PrintSequences() const {
    for (std::size_t index = 0; index < initiator_ids.size(); ++index) {
      FIX::Session* client = FIX::Session::lookupSession(initiator_ids[index]);
      FIX::Session* server = FIX::Session::lookupSession(acceptor_ids[index]);
      if (client == nullptr || server == nullptr) {
        std::cout << std::format("  CLI{}: session missing\n", index);
        continue;
      }
      std::cout << std::format("  CLI{}: on={}/{} ini next out={} in={}, acc next in={} out={}\n",
                               index, client->isLoggedOn(), server->isLoggedOn(),
                               client->getExpectedSenderNum(), client->getExpectedTargetNum(),
                               server->getExpectedTargetNum(), server->getExpectedSenderNum());
    }
  }

  const Options& options;
  Side acceptor_side;
  Side initiator_side;
  StressApp& initiator_application;
  std::unique_ptr<FIX::ThreadedSocketAcceptor> acceptor;
  std::unique_ptr<FIX::ThreadedSocketInitiator> initiator;
  std::vector<FIX::SessionID> initiator_ids;
  std::vector<FIX::SessionID> acceptor_ids;
  std::shared_mutex initiator_life;  // senders hold it shared; destroy/recreate takes it alone
  std::atomic<bool> pause{false};    // lets the writer in (glibc's rwlock prefers readers)
  std::atomic<bool> running{true};
};

// ---------- senders ----------

FIX44::NewOrderSingle Order(const std::string& key, std::int64_t number) {
  FIX44::NewOrderSingle order(FIX::ClOrdID(std::format("{}.n{}", key, number)),
                              FIX::Side(FIX::Side_BUY), FIX::TransactTime(),
                              FIX::OrdType(FIX::OrdType_MARKET));
  order.set(FIX::Symbol("VNM"));
  order.set(FIX::OrderQty(100));
  return order;
}

// Sends while the session is logged on, as a gateway does; nothing when it is not. A logout can
// still come between the check and the send: sendToTarget then returns false and the store keeps
// the order for a resend (P0-6). Sending into a logged-off session at full speed instead built a
// backlog of hundreds of thousands of messages that no settle time could resend.
std::optional<bool> SendIfLoggedOn(Rig& rig, const FIX::SessionID& id, FIX::Message& order) {
  const std::shared_lock lock(rig.initiator_life);
  FIX::Session* session = FIX::Session::lookupSession(id);
  if (session == nullptr || !session->isLoggedOn()) {
    return std::nullopt;
  }
  try {
    return FIX::Session::sendToTarget(order, id);
  } catch (const std::exception&) {
    return false;
  }
}

void SendLoop(Rig& rig, Tracker& tracker, int session, int thread) {
  constexpr std::int64_t kBurst = 64;
  const std::string key = std::format("s{}.t{}", session, thread);
  const FIX::SessionID& id = rig.initiator_ids[static_cast<std::size_t>(session)];
  for (std::int64_t number = 0; rig.running.load();) {
    while (rig.pause.load() && rig.running.load()) {
      std::this_thread::sleep_for(milliseconds(1));
    }
    FIX44::NewOrderSingle order = Order(key, number);
    const std::optional<bool> sent = SendIfLoggedOn(rig, id, order);
    if (!sent.has_value()) {
      std::this_thread::sleep_for(milliseconds(5));
      continue;
    }
    tracker.Sent(key, number, *sent);
    if (rig.options.rate > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(1'000'000 / rig.options.rate));
    } else if (number % kBurst == kBurst - 1) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    ++number;
  }
}

// ---------- chaos ----------

class Chaos {
 public:
  Chaos(Rig& rig, Watchdog& watchdog, Canaries& canaries, unsigned seed)
      : rig_(rig), watchdog_(watchdog), canaries_(canaries), random_(seed) {}

  void Step() {
    const int roll = Pick(0, 99);
    const auto session = static_cast<std::size_t>(Pick(0, rig_.options.sessions - 1));
    std::string name;
    if (roll < 20) {
      name = Relog(rig_.initiator_ids[session], "ini-session logout/logon");
    } else if (roll < 35) {
      name = Relog(rig_.acceptor_ids[session], "acc-session logout/logon");
    } else if (roll < 50) {
      name =
          Disconnect(rig_.initiator_ids[session], "ini-session disconnect() from another thread");
    } else if (roll < 60) {
      name = Disconnect(rig_.acceptor_ids[session], "acc-session disconnect() from another thread");
    } else if (roll < 72) {
      name = Restart(*rig_.initiator, "initiator");
    } else if (roll < 84) {
      name = Restart(*rig_.acceptor, "acceptor");
    } else if (roll < 92) {
      name = Recreate();
    } else {
      name = RestartBoth();
    }
    ++counts_[name];
  }

  void Print() const {
    std::cout << "chaos actions:\n";
    for (const auto& [name, count] : counts_) {
      std::cout << std::format("  {:5}  {}\n", count, name);
    }
  }

  int Pick(int low, int high) { return std::uniform_int_distribution(low, high)(random_); }

 private:
  [[nodiscard]] int StopLimit() const { return rig_.options.stop_limit_seconds; }

  static constexpr int kStartLimit = 10;

  std::string Relog(const FIX::SessionID& id, const std::string& name) {
    watchdog_.Guard(name, StopLimit(), [&] {
      if (FIX::Session* session = FIX::Session::lookupSession(id); session != nullptr) {
        session->logout("chaos");
      }
      std::this_thread::sleep_for(milliseconds(Pick(100, 499)));
      if (FIX::Session* session = FIX::Session::lookupSession(id); session != nullptr) {
        session->logon();
      }
    });
    return name;
  }

  std::string Disconnect(const FIX::SessionID& id, const std::string& name) {
    watchdog_.Guard(name, StopLimit(), [&] {
      if (FIX::Session* session = FIX::Session::lookupSession(id); session != nullptr) {
        session->disconnect();
      }
    });
    return name;
  }

  template <class Engine>
  std::string Restart(Engine& engine, const std::string& side) {
    const bool force = Pick(0, 1) == 1;
    const std::string name = side + (force ? " stop(force)/start" : " stop/start");
    canaries_.Add(rig_.options.workdir);
    watchdog_.Guard(name + " [stop]", StopLimit(), [&] { engine.stop(force); });
    canaries_.Add(rig_.options.workdir);
    canaries_.Check(name);
    std::this_thread::sleep_for(milliseconds(Pick(0, 299)));
    watchdog_.Guard(name + " [start]", kStartLimit, [&] { engine.start(); });
    return name;
  }

  std::string Recreate() {
    const std::string name = "initiator destroy/recreate";
    rig_.pause.store(true);
    const std::unique_lock lock(rig_.initiator_life);
    watchdog_.Guard(name + " [stop]", StopLimit(), [&] { rig_.initiator->stop(); });
    watchdog_.Guard(name + " [delete]", kStartLimit, [&] { rig_.initiator.reset(); });
    canaries_.Check(name);
    rig_.RecreateInitiator();
    watchdog_.Guard(name + " [start]", kStartLimit, [&] { rig_.initiator->start(); });
    rig_.pause.store(false);
    return name;
  }

  std::string RestartBoth() {
    const std::string name = "both stop/start";
    watchdog_.Guard(name + " [ini stop]", StopLimit(), [&] { rig_.initiator->stop(); });
    watchdog_.Guard(name + " [acc stop]", StopLimit(), [&] { rig_.acceptor->stop(); });
    canaries_.Check(name);
    watchdog_.Guard(name + " [acc start]", kStartLimit, [&] { rig_.acceptor->start(); });
    watchdog_.Guard(name + " [ini start]", kStartLimit, [&] { rig_.initiator->start(); });
    return name;
  }

  Rig& rig_;
  Watchdog& watchdog_;
  Canaries& canaries_;
  std::mt19937 random_;
  std::map<std::string, int> counts_;
};

// ---------- settle and verify ----------

// Every session logs back on; done once both sides are in step and nothing arrived for 4 s.
bool Settle(const Rig& rig, const Direction& orders, const Direction& echoes) {
  constexpr int kQuietPolls = 8;
  for (const auto& ids : {rig.initiator_ids, rig.acceptor_ids}) {
    for (const FIX::SessionID& id : ids) {
      if (FIX::Session* session = FIX::Session::lookupSession(id); session != nullptr) {
        session->logon();
      }
    }
  }
  const auto start = Clock::now();
  std::int64_t previous = -1;
  int quiet = 0;
  while (Clock::now() - start < seconds(rig.options.settle_seconds) && quiet < kQuietPolls) {
    std::this_thread::sleep_for(milliseconds(500));
    const std::int64_t now = orders.tracker.Count() + echoes.tracker.Count();
    quiet = rig.AllLoggedOn() && rig.InStep() && now == previous ? quiet + 1 : 0;
    previous = now;
  }
  const bool settled = quiet >= kQuietPolls;
  std::cout << std::format("settle: {} after {} ms\n", settled ? "in step" : "NOT in step",
                           std::chrono::duration_cast<milliseconds>(Clock::now() - start).count());
  rig.PrintSequences();
  return settled;
}

struct Totals {
  std::int64_t sent_ok = 0;
  std::int64_t sent_false = 0;
  std::int64_t received = 0;
  std::int64_t lost = 0;
  std::int64_t explained = 0;
  std::int64_t late = 0;
  std::int64_t unexpected = 0;
  std::int64_t dup = 0;
  std::int64_t ooo = 0;

  [[nodiscard]] std::int64_t Bad() const { return lost + unexpected + dup + ooo; }
};

// Each lost message with its MsgSeqNum and what skipped it; the first few unexplained ones in full.
void TraceLost(const Direction& direction, const std::string& key, const Stream& stream,
               Totals& totals) {
  int shown = 0;
  for (std::int64_t number = 0; number < stream.sent_ok.End(); ++number) {
    if (!stream.sent_ok.Test(number) || stream.received.Test(number)) {
      continue;
    }
    ++totals.lost;
    const int seq = direction.sequences.Find(key, number);
    const std::string why = direction.skips.Explain(ClientOf(key), seq);
    if (!why.empty()) {
      ++totals.explained;
    }
    if (shown < kExamples) {
      ++shown;
      std::cout << std::format("    lost {}.n{} seq={} {}\n", key, number, seq,
                               why.empty() ? "UNEXPLAINED" : why);
    }
  }
}

void CountStream(const Stream& stream, Totals& totals) {
  totals.received += stream.received_count;
  totals.dup += stream.dup;
  totals.ooo += stream.ooo;
  const std::int64_t end =
      std::max({stream.sent_ok.End(), stream.sent_false.End(), stream.received.End()});
  for (std::int64_t number = 0; number < end; ++number) {
    totals.sent_ok += stream.sent_ok.Test(number) ? 1 : 0;
    totals.sent_false += stream.sent_false.Test(number) ? 1 : 0;
    if (!stream.received.Test(number)) {
      continue;
    }
    if (stream.sent_false.Test(number)) {
      ++totals.late;
    } else if (!stream.sent_ok.Test(number)) {
      ++totals.unexpected;
    }
  }
}

std::int64_t Verify(const Direction& direction, const std::string& name) {
  Totals totals;
  for (const auto& [key, stream] : direction.tracker.Snapshot()) {
    CountStream(stream, totals);
    TraceLost(direction, key, stream, totals);
  }
  std::cout << std::format(
      "{}: sentOK={} sentFalse={} received={} LOST={} (explained {}) late={} UNEXPECTED={} "
      "DUP={} OOO={}\n",
      name, totals.sent_ok, totals.sent_false, totals.received, totals.lost, totals.explained,
      totals.late, totals.unexpected, totals.dup, totals.ooo);
  return totals.Bad();
}

// ---------- the run ----------

// The process's resident memory in MB (Linux /proc; elsewhere 0).
std::int64_t RssMb() {
  std::ifstream statm("/proc/self/statm");
  std::int64_t pages = 0;
  std::int64_t resident = 0;
  if (!(statm >> pages >> resident)) {
    return 0;
  }
  constexpr std::int64_t kMb = std::int64_t{1024} * 1024;
  return resident * ::sysconf(_SC_PAGESIZE) / kMb;
}

void Report(const StressApp& acceptor_app, const Direction& orders, const Direction& echoes,
            std::int64_t elapsed) {
  std::cout << std::format("[{:4}s] recvAcc={} recvIni={} logons={} logouts={} fds={} rssMB={}\n",
                           elapsed, orders.tracker.Count(), echoes.tracker.Count(),
                           acceptor_app.Logons(), acceptor_app.Logouts(), CountFds(), RssMb());
  std::cout.flush();
}

void RunChaos(const Rig& rig, Chaos& chaos, const StressApp& acceptor_app, const Direction& orders,
              const Direction& echoes) {
  const auto start = Clock::now();
  std::int64_t last_report = 0;
  const int gap = rig.options.chaos_ms;
  while (Clock::now() - start < seconds(rig.options.seconds)) {
    std::this_thread::sleep_for(milliseconds(chaos.Pick(gap, gap + 349)));
    const std::int64_t elapsed = std::chrono::duration_cast<seconds>(Clock::now() - start).count();
    if (elapsed >= last_report + rig.options.report_seconds) {
      last_report = elapsed;
      Report(acceptor_app, orders, echoes, elapsed);
    }
    chaos.Step();
  }
}

bool WaitForLogon(const Rig& rig) {
  const auto start = Clock::now();
  while (!rig.AllLoggedOn() && Clock::now() - start < seconds(20)) {
    std::this_thread::sleep_for(milliseconds(50));
  }
  return rig.AllLoggedOn();
}

int Run(const Options& options) {
  std::filesystem::remove_all(options.workdir);
  std::filesystem::create_directories(options.workdir);
  std::cout << std::format(
      "stress: {}s sessions={} threads/session={} port={} seed={} echo={} filelog={}\n",
      options.seconds, options.sessions, options.threads, options.port, options.seed, options.echo,
      options.file_log);
  Watchdog watchdog(seconds(options.hang_grace_seconds));
  Canaries canaries;
  const int fds_before = CountFds();
  Direction orders;
  Direction echoes;
  if (options.soak) {
    orders.sequences.Disable();
    echoes.sequences.Disable();
  }
  StressApp acceptor_app(true, options.echo, echoes, orders);
  StressApp initiator_app(false, options.echo, orders, echoes);
  Rig rig(options, acceptor_app, initiator_app);
  canaries.Add(options.workdir);
  watchdog.Guard("acceptor start", 10, [&] { rig.acceptor->start(); });
  watchdog.Guard("initiator start", 10, [&] { rig.initiator->start(); });
  std::cout << "initial logon: " << (WaitForLogon(rig) ? "all" : "NOT all") << '\n';

  std::vector<std::jthread> senders;
  for (int session = 0; session < options.sessions; ++session) {
    for (int thread = 0; thread < options.threads; ++thread) {
      senders.emplace_back(
          [&rig, &orders, session, thread] { SendLoop(rig, orders.tracker, session, thread); });
    }
  }
  Chaos chaos(rig, watchdog, canaries, options.seed);
  RunChaos(rig, chaos, acceptor_app, orders, echoes);
  rig.running.store(false);
  senders.clear();
  chaos.Print();

  const bool settled = Settle(rig, orders, echoes);
  if (!settled && options.hang_grace_seconds > 0) {
    // Time to take the stacks of the stalled sessions' threads (test/krx/k8s/run.sh does).
    std::cout << std::format("*** STALLED: not in step (pid {}) ***\n", ::getpid());
    std::cout.flush();
    std::this_thread::sleep_for(seconds(options.hang_grace_seconds));
  }
  const std::int64_t bad = Verify(orders, "orders ini->acc") + Verify(echoes, "echoes acc->ini");
  watchdog.Guard("final initiator stop", options.stop_limit_seconds,
                 [&] { rig.initiator->stop(); });
  watchdog.Guard("final acceptor stop", options.stop_limit_seconds, [&] { rig.acceptor->stop(); });
  canaries.Check("final");
  rig.initiator.reset();
  rig.acceptor.reset();
  std::cout << std::format("fds: before={} after={} (canaries {}) canaryBad={}\n", fds_before,
                           CountFds(), canaries.Size(), canaries.Bad());
  const bool pass = settled && bad == 0 && canaries.Bad() == 0;
  std::cout << "RESULT: " << (pass ? "PASS" : "ANOMALIES") << '\n';
  return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(Parse(std::span<char*>(argv, static_cast<std::size_t>(argc))));
  } catch (const std::exception& error) {
    std::cerr << "krx_stress: " << error.what() << '\n';
    return 2;
  }
}
