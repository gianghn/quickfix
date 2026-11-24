// Go-live review B6 follow-up (gateway): AllowedRemoteAddresses lists the IPv4 addresses a session
// may be logged on from. A connection whose first message names that session from any other
// address is dropped before the session sees it, and the refusal is logged on the session's log.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <catch_amalgamated.hpp>

#include "Application.h"
#include "Exceptions.h"
#include "Log.h"
#include "MessageStore.h"
#include "Session.h"
#include "SessionID.h"
#include "SessionSettings.h"
#include "SocketAcceptor.h"
#include "ThreadedSocketAcceptor.h"
#include "ThreadedSocketInitiator.h"
#include "Utility.h"

namespace {

constexpr std::chrono::seconds kWait{5};
constexpr std::chrono::milliseconds kPollStep{10};
// TEST-NET-1: never the address a test broker connects from.
constexpr const char* kNotOurs = "192.0.2.10";

int FreePort() {
  const FIX::socket_handle socket = FIX::socket_createAcceptor(0, true);
  const auto port = static_cast<std::uint16_t>(FIX::socket_hostport(socket));
  FIX::socket_close(socket);
  return port;
}

FIX::SessionSettings Settings(const std::string& text) {
  std::istringstream stream(text);
  FIX::SessionSettings settings;
  stream >> settings;
  return settings;
}

FIX::SessionID GatewaySide() {
  return {"FIX.4.4", "KRXGW", "BROKER"};
}

// The gateway's broker port, whose session may be logged on only from ALLOWED.
FIX::SessionSettings AcceptorSettings(int port, const std::string& allowed) {
  return Settings(
      "[DEFAULT]\nConnectionType=acceptor\nStartTime=00:00:00\nEndTime=00:00:00\n"
      "UseDataDictionary=N\nHeartBtInt=30\nSocketAcceptPort=" +
      std::to_string(port) +
      "\n[SESSION]\nBeginString=FIX.4.4\nSenderCompID=KRXGW\nTargetCompID=BROKER\n"
      "AllowedRemoteAddresses=" +
      allowed + "\n");
}

// The broker: a threaded initiator connecting from 127.0.0.1.
FIX::SessionSettings BrokerSettings(int port) {
  return Settings(
      "[DEFAULT]\nConnectionType=initiator\nStartTime=00:00:00\nEndTime=00:00:00\n"
      "UseDataDictionary=N\nHeartBtInt=30\nReconnectInterval=1\nSocketConnectPort=" +
      std::to_string(port) +
      "\nSocketConnectHost=127.0.0.1\n"
      "[SESSION]\nBeginString=FIX.4.4\nSenderCompID=BROKER\nTargetCompID=KRXGW\n");
}

// Every event logged by the logs it made, in one list.
class EventLogFactory : public FIX::LogFactory {
 public:
  // The engine owns each log until it hands it back to destroy().
  FIX::Log* create() override {
    ++live_;
    return std::make_unique<EventLog>(*this).release();
  }

  FIX::Log* create(const FIX::SessionID& /*session_id*/) override {
    ++live_;
    return std::make_unique<EventLog>(*this).release();
  }

  void destroy(FIX::Log* log) override {
    --live_;
    const std::unique_ptr<FIX::Log> owned(log);
  }

  // Logs made and not yet handed back: the acceptor's own and one per session.
  int Live() const { return live_; }

  bool WaitForEvent(const std::string& text) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        const std::scoped_lock lock(mutex_);
        for (const std::string& event : events_) {
          if (event.contains(text)) {
            return true;
          }
        }
      }
      std::this_thread::sleep_for(kPollStep);
    }
    return false;
  }

 private:
  class EventLog : public FIX::Log {
   public:
    explicit EventLog(EventLogFactory& factory) : factory_(factory) {}

    void clear() override {}

    void backup() override {}

    void onIncoming(const std::string& /*message*/) override {}

    void onOutgoing(const std::string& /*message*/) override {}

    void onEvent(const std::string& text) override {
      const std::scoped_lock lock(factory_.mutex_);
      factory_.events_.push_back(text);
    }

   private:
    EventLogFactory& factory_;
  };

  std::atomic<int> live_{0};
  std::mutex mutex_;
  std::vector<std::string> events_;
};

bool WaitLoggedOn(const FIX::SessionID& id) {
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (std::chrono::steady_clock::now() < deadline) {
    FIX::Session* session = FIX::Session::lookupSession(id);
    if (session != nullptr && session->isLoggedOn()) {
      return true;
    }
    std::this_thread::sleep_for(kPollStep);
  }
  return false;
}

bool LoggedOn(const FIX::SessionID& id) {
  FIX::Session* session = FIX::Session::lookupSession(id);
  return session != nullptr && session->isLoggedOn();
}

// The ConfigError text building a threaded acceptor with SETTINGS threw, or "" when none.
std::string ConfigErrorOnCreate(const FIX::SessionSettings& settings) {
  FIX::NullApplication app;
  FIX::MemoryStoreFactory store;
  try {
    const FIX::ThreadedSocketAcceptor acceptor(app, store, settings);
  } catch (const FIX::ConfigError& error) {
    return error.what();
  }
  return "";
}

// Whether building a threaded acceptor with SETTINGS and LOGS threw a ConfigError.
bool RefusedOnCreate(const FIX::SessionSettings& settings, FIX::LogFactory& logs) {
  FIX::NullApplication app;
  FIX::MemoryStoreFactory store;
  try {
    const FIX::ThreadedSocketAcceptor acceptor(app, store, settings, logs);
  } catch (const FIX::ConfigError&) {
    return true;
  }
  return false;
}

}  // namespace

SCENARIO("A broker logs on from an address its session lists", "[krx][b6-allow]") {
  GIVEN("a broker session allowed from 192.0.2.10 and 127.0.0.1") {
    const int port = FreePort();
    FIX::NullApplication gateway_app;
    FIX::MemoryStoreFactory gateway_store;
    FIX::ThreadedSocketAcceptor acceptor(
        gateway_app, gateway_store, AcceptorSettings(port, std::string(kNotOurs) + ", 127.0.0.1"));
    acceptor.start();
    WHEN("the broker connects from 127.0.0.1") {
      FIX::NullApplication broker_app;
      FIX::MemoryStoreFactory broker_store;
      FIX::ThreadedSocketInitiator broker(broker_app, broker_store, BrokerSettings(port));
      broker.start();
      THEN("its session logs on") {
        CHECK(WaitLoggedOn(GatewaySide()));
      }
      broker.stop(true);
    }
    acceptor.stop(true);
  }
}

SCENARIO("A broker from an address its session does not list is dropped", "[krx][b6-allow]") {
  GIVEN("a broker session allowed from 192.0.2.10 only, on the threaded acceptor") {
    const int port = FreePort();
    FIX::NullApplication gateway_app;
    FIX::MemoryStoreFactory gateway_store;
    EventLogFactory gateway_log;
    FIX::ThreadedSocketAcceptor acceptor(gateway_app, gateway_store,
                                         AcceptorSettings(port, kNotOurs), gateway_log);
    acceptor.start();
    WHEN("the broker connects from 127.0.0.1") {
      FIX::NullApplication broker_app;
      FIX::MemoryStoreFactory broker_store;
      FIX::ThreadedSocketInitiator broker(broker_app, broker_store, BrokerSettings(port));
      broker.start();
      THEN("the refusal is logged and the session never logs on") {
        // CHECK, not REQUIRE: a failure must still reach the stop() calls below.
        CHECK(gateway_log.WaitForEvent("127.0.0.1 is not in AllowedRemoteAddresses"));
        CHECK_FALSE(LoggedOn(GatewaySide()));
      }
      broker.stop(true);
    }
    acceptor.stop(true);
  }
}

SCENARIO("The plain acceptor drops a broker from an address its session does not list",
         "[krx][b6-allow]") {
  GIVEN("a broker session allowed from 192.0.2.10 only, on the plain acceptor") {
    const int port = FreePort();
    FIX::NullApplication gateway_app;
    FIX::MemoryStoreFactory gateway_store;
    EventLogFactory gateway_log;
    FIX::SocketAcceptor acceptor(gateway_app, gateway_store, AcceptorSettings(port, kNotOurs),
                                 gateway_log);
    acceptor.start();
    WHEN("the broker connects from 127.0.0.1") {
      FIX::NullApplication broker_app;
      FIX::MemoryStoreFactory broker_store;
      FIX::ThreadedSocketInitiator broker(broker_app, broker_store, BrokerSettings(port));
      broker.start();
      THEN("the refusal is logged and the session never logs on") {
        // CHECK, not REQUIRE: a failure must still reach the stop() calls below.
        CHECK(gateway_log.WaitForEvent("127.0.0.1 is not in AllowedRemoteAddresses"));
        CHECK_FALSE(LoggedOn(GatewaySide()));
      }
      broker.stop(true);
    }
    acceptor.stop(true);
  }
}

SCENARIO("A session whose address list is not a list of IPv4 addresses does not start",
         "[krx][b6-allow]") {
  const int port = FreePort();
  WHEN("an entry is a host name") {
    const std::string error = ConfigErrorOnCreate(AcceptorSettings(port, "127.0.0.1,localhost"));
    THEN("the acceptor refuses with a configuration error naming the setting") {
      INFO("error: " << error);
      REQUIRE(error.contains("AllowedRemoteAddresses"));
    }
  }
  WHEN("the list is empty") {
    const std::string error = ConfigErrorOnCreate(AcceptorSettings(port, ""));
    THEN("the acceptor refuses with a configuration error naming the setting") {
      INFO("error: " << error);
      REQUIRE(error.contains("AllowedRemoteAddresses"));
    }
  }
}

// The ConfigError comes from the acceptor's constructor, whose destructor then never runs: what
// it made before (its log, the sessions created so far) must not outlive it.
SCENARIO("An acceptor refused for a bad address list leaves nothing behind", "[krx][b6-allow]") {
  GIVEN("a valid session, then one whose address list holds a host name") {
    const FIX::SessionID valid{"FIX.4.4", "KRXGW", "ABROKER"};
    const FIX::SessionSettings settings = Settings(
        "[DEFAULT]\nConnectionType=acceptor\nStartTime=00:00:00\nEndTime=00:00:00\n"
        "UseDataDictionary=N\nHeartBtInt=30\nSocketAcceptPort=" +
        std::to_string(FreePort()) +
        "\n[SESSION]\nBeginString=FIX.4.4\nSenderCompID=KRXGW\nTargetCompID=ABROKER\n"
        "[SESSION]\nBeginString=FIX.4.4\nSenderCompID=KRXGW\nTargetCompID=BROKER\n"
        "AllowedRemoteAddresses=localhost\n");
    WHEN("the gateway builds the acceptor") {
      EventLogFactory logs;
      const bool refused = RefusedOnCreate(settings, logs);
      THEN("it is refused, every log is handed back and no session stays registered") {
        const bool logs_back = logs.Live() == 0;
        const bool unregistered = FIX::Session::lookupSession(valid) == nullptr;
        CHECK(refused);
        CHECK(logs_back);
        CHECK(unregistered);
      }
    }
  }
}
