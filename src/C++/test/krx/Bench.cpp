// Plan item 1.8: the latency and throughput baseline (docs/V1_PLAN.md). A ThreadedSocketInitiator
// sends FIX.4.4 NewOrderSingles to a ThreadedSocketAcceptor on 127.0.0.1, dictionary validation on.
//   throughput: one thread sends N orders; time until the acceptor's fromApp has seen all of them.
//   ping-pong:  the acceptor echoes each order; round trip from the steady-clock nanoseconds the
//               order carries in Text(58). Prints p50/p99/p99.9/max.
// Usage: krx_bench [mem|file|sync] [orders=200000] [round_trips=20000] [port=7800] [workdir]
//   sync: the file store with FileStoreSync=Y (plan item 4.4).
// Numbers from a shared machine are rough; compare runs on the same machine only.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Application.h"
#include "FileStore.h"
#include "MessageStore.h"
#include "Session.h"
#include "SessionSettings.h"
#include "ThreadedSocketAcceptor.h"
#include "ThreadedSocketInitiator.h"
#include "fix44/NewOrderSingle.h"

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::int64_t kWarmup = 2000;
constexpr std::chrono::seconds kGiveUp{300};
constexpr std::chrono::seconds kEchoTimeout{5};
constexpr std::chrono::milliseconds kPollStep{1};

std::int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

// Counts what arrives; the acceptor side can echo, the initiator side records round trips.
class BenchApp : public FIX::Application {
 public:
  explicit BenchApp(bool acceptor) : acceptor_(acceptor) {}

  void onCreate(const FIX::SessionID& /*id*/) override {}

  void onLogon(const FIX::SessionID& /*id*/) override { logged_on_.store(true); }

  void onLogout(const FIX::SessionID& /*id*/) override { logged_on_.store(false); }

  void toAdmin(FIX::Message& /*message*/, const FIX::SessionID& /*id*/) override {}

  void toApp(FIX::Message& /*message*/, const FIX::SessionID& /*id*/) override {}

  void fromAdmin(const FIX::Message& /*message*/, const FIX::SessionID& /*id*/) override {}

  void fromApp(const FIX::Message& message, const FIX::SessionID& id) override {
    if (!acceptor_) {
      FIX::Text sent;
      message.getField(sent);
      round_trip_ns_.store(NowNs() - std::stoll(sent.getValue()));
      return;
    }
    received_.fetch_add(1);
    if (echo_.load()) {
      FIX::Message echo(message);
      FIX::Session::sendToTarget(echo, id);
    }
  }

  [[nodiscard]] bool LoggedOn() const { return logged_on_.load(); }

  [[nodiscard]] std::int64_t Received() const { return received_.load(); }

  void ResetReceived() { received_.store(0); }

  void Echo() { echo_.store(true); }

  [[nodiscard]] std::int64_t TakeRoundTrip() { return round_trip_ns_.exchange(-1); }

 private:
  bool acceptor_;
  std::atomic<bool> logged_on_{false};
  std::atomic<bool> echo_{false};
  std::atomic<std::int64_t> received_{0};
  std::atomic<std::int64_t> round_trip_ns_{-1};
};

struct Options {
  bool file_store = false;
  bool sync = false;
  std::int64_t orders = 200000;
  std::int64_t round_trips = 20000;
  int port = 7800;
  std::filesystem::path workdir = "bench_work";
};

Options Parse(std::span<char*> args) {
  Options options;
  if (args.size() > 1) {
    const std::string store = args[1];
    options.sync = store == "sync";
    options.file_store = store == "file" || options.sync;
  }
  if (args.size() > 2) {
    options.orders = std::stoll(args[2]);
  }
  if (args.size() > 3) {
    options.round_trips = std::stoll(args[3]);
  }
  if (args.size() > 4) {
    options.port = std::stoi(args[4]);
  }
  if (args.size() > 5) {
    options.workdir = args[5];
  }
  return options;
}

FIX::SessionSettings Settings(const Options& options, bool acceptor) {
  const std::string side = acceptor ? "acceptor" : "initiator";
  std::ostringstream text;
  text << "[DEFAULT]\nConnectionType=" << side << "\nSocketAcceptPort=" << options.port
       << "\nSocketConnectHost=127.0.0.1\nSocketConnectPort=" << options.port
       << "\nSocketReuseAddress=Y\nSocketNodelay=Y\nHeartBtInt=30\nReconnectInterval=1\n"
       << "StartTime=00:00:00\nEndTime=00:00:00\nFileStorePath="
       << (options.workdir / side).string() << "\nFileStoreSync=" << (options.sync ? "Y" : "N")
       << "\nUseDataDictionary=Y\nDataDictionary=" << KRX_SPEC_DIR
       << "/FIX44.xml\n[SESSION]\nBeginString=FIX.4.4\nSenderCompID=" << (acceptor ? "SRV" : "CLI")
       << "\nTargetCompID=" << (acceptor ? "CLI" : "SRV") << "\n";
  std::istringstream stream(text.str());
  return {stream};
}

std::unique_ptr<FIX::MessageStoreFactory> Store(const Options& options,
                                                const FIX::SessionSettings& settings) {
  if (options.file_store) {
    return std::make_unique<FIX::FileStoreFactory>(settings);
  }
  return std::make_unique<FIX::MemoryStoreFactory>();
}

FIX44::NewOrderSingle Order(std::int64_t number) {
  FIX44::NewOrderSingle order(FIX::ClOrdID("o" + std::to_string(number)), FIX::Side(FIX::Side_BUY),
                              FIX::TransactTime(), FIX::OrdType(FIX::OrdType_LIMIT));
  order.set(FIX::Symbol("VNM"));
  order.set(FIX::OrderQty(100));
  order.set(FIX::Price(65000));
  order.set(FIX::Text(std::to_string(NowNs())));
  return order;
}

void Send(std::int64_t number, const FIX::SessionID& id) {
  FIX44::NewOrderSingle order = Order(number);
  FIX::Session::sendToTarget(order, id);
}

void WaitFor(const BenchApp& acceptor, std::int64_t count) {
  const auto start = Clock::now();
  while (acceptor.Received() < count && Clock::now() - start < kGiveUp) {
    std::this_thread::sleep_for(kPollStep);
  }
}

void Throughput(BenchApp& acceptor, const FIX::SessionID& id, std::int64_t orders) {
  acceptor.ResetReceived();
  const auto start = Clock::now();
  for (std::int64_t number = 0; number < orders; ++number) {
    Send(number, id);
  }
  const std::chrono::duration<double> sending = Clock::now() - start;
  WaitFor(acceptor, orders);
  const std::chrono::duration<double> total = Clock::now() - start;
  std::cout << std::format(
      "throughput: orders={} received={} send-loop {:.0f} msg/s, end-to-end {:.0f} msg/s "
      "({:.2f} s)\n",
      orders, acceptor.Received(), static_cast<double>(orders) / sending.count(),
      static_cast<double>(acceptor.Received()) / total.count(), total.count());
}

std::int64_t OneRoundTrip(BenchApp& initiator, const FIX::SessionID& id, std::int64_t number) {
  Send(number, id);
  const auto start = Clock::now();
  for (std::int64_t ns = initiator.TakeRoundTrip(); Clock::now() - start < kEchoTimeout;
       ns = initiator.TakeRoundTrip()) {
    if (ns >= 0) {
      return ns;
    }
  }
  return -1;
}

void PingPong(BenchApp& acceptor, BenchApp& initiator, const FIX::SessionID& id,
              std::int64_t round_trips) {
  acceptor.Echo();
  std::vector<std::int64_t> samples;
  for (std::int64_t number = 0; number < round_trips + kWarmup; ++number) {
    const std::int64_t ns = OneRoundTrip(initiator, id, number);
    if (number >= kWarmup && ns >= 0) {
      samples.push_back(ns);
    }
  }
  std::ranges::sort(samples);
  const auto at = [&samples](double fraction) {
    if (samples.empty()) {
      return 0.0;
    }
    const auto index = static_cast<std::size_t>(fraction * static_cast<double>(samples.size()));
    return static_cast<double>(samples[std::min(samples.size() - 1, index)]) / 1000.0;
  };
  std::cout << std::format(
      "ping-pong RTT (us): n={} min={:.1f} p50={:.1f} p90={:.1f} p99={:.1f} p99.9={:.1f} "
      "max={:.1f}\n",
      samples.size(), at(0.0), at(0.5), at(0.9), at(0.99), at(0.999), at(1.0));
}

int Run(std::span<char*> args) {
  const Options options = Parse(args);
  std::filesystem::remove_all(options.workdir);
  std::filesystem::create_directories(options.workdir);
  const FIX::SessionSettings acceptor_settings = Settings(options, true);
  const FIX::SessionSettings initiator_settings = Settings(options, false);
  const auto acceptor_store = Store(options, acceptor_settings);
  const auto initiator_store = Store(options, initiator_settings);
  BenchApp acceptor_app(true);
  BenchApp initiator_app(false);
  FIX::ThreadedSocketAcceptor acceptor(acceptor_app, *acceptor_store, acceptor_settings);
  FIX::ThreadedSocketInitiator initiator(initiator_app, *initiator_store, initiator_settings);
  acceptor.start();
  initiator.start();
  while (!initiator_app.LoggedOn()) {
    std::this_thread::sleep_for(kPollStep);
  }
  std::string store = options.file_store ? "file" : "mem";
  if (options.sync) {
    store = "file+sync";
  }
  std::cout << std::format("store={} hw_threads={}\n", store, std::thread::hardware_concurrency());
  const FIX::SessionID id("FIX.4.4", "CLI", "SRV");
  for (std::int64_t number = 0; number < kWarmup; ++number) {
    Send(number, id);
  }
  WaitFor(acceptor_app, kWarmup);
  Throughput(acceptor_app, id, options.orders);
  PingPong(acceptor_app, initiator_app, id, options.round_trips);
  initiator.stop();
  acceptor.stop();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return Run(std::span<char*>(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& error) {
    std::cerr << "krx_bench: " << error.what() << '\n';
    return 1;
  }
}
