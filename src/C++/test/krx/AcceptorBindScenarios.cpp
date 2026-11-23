// Go-live review B6 (gateway): SocketAcceptHost binds the threaded acceptor (the class the gateway
// runs) to one IPv4 address instead of every interface, so a broker port is not reachable through
// an interface it was not meant for. Unit tests of the setting: SocketAcceptorTestCase.cpp.
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>

#include <catch_amalgamated.hpp>

#include "Application.h"
#include "Exceptions.h"
#include "MessageStore.h"
#include "Session.h"
#include "SessionID.h"
#include "SessionSettings.h"
#include "ThreadedSocketAcceptor.h"
#include "ThreadedSocketInitiator.h"
#include "Utility.h"

namespace {

constexpr std::chrono::seconds kWait{5};
constexpr std::chrono::milliseconds kPollStep{10};

// A port nothing listens on now: the kernel picks it, then it is given back.
int FreePort() {
  const FIX::socket_handle socket = FIX::socket_createAcceptor(0, true);
  const auto port = static_cast<std::uint16_t>(FIX::socket_hostport(socket));
  FIX::socket_close(socket);
  return port;
}

// An IPv4 address of this host other than 127.0.0.1, or "" when there is none. The source
// address the kernel picks for a route out (192.0.2.1 is TEST-NET-1: the SYN goes nowhere);
// failing that, on Linux, 127.0.0.2, which is local there (macOS answers only 127.0.0.1).
std::string OtherLocalAddress() {
  const FIX::socket_handle socket = FIX::socket_createConnector();
  FIX::socket_setnonblock(socket);
  FIX::socket_connect(socket, "192.0.2.1", 9);
  const char* name = FIX::socket_hostname(socket);
  std::string address = name == nullptr ? "" : name;
  FIX::socket_close(socket);
  if (address != "0.0.0.0" && !address.empty() && !address.starts_with("127.")) {
    return address;
  }
#ifdef __linux__
  return "127.0.0.2";
#else
  return "";
#endif
}

// Whether a TCP connection to ADDRESS:PORT is accepted.
bool Connects(const std::string& address, int port) {
  const FIX::socket_handle socket = FIX::socket_createConnector();
  const bool connected = FIX::socket_connect(socket, address.c_str(), port) == 0;
  FIX::socket_close(socket);
  return connected;
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

// The gateway's broker port: a threaded acceptor bound to HOST.
FIX::SessionSettings AcceptorSettings(int port, const std::string& host) {
  return Settings(
      "[DEFAULT]\nConnectionType=acceptor\nStartTime=00:00:00\nEndTime=00:00:00\n"
      "UseDataDictionary=N\nHeartBtInt=30\nSocketAcceptPort=" +
      std::to_string(port) + "\nSocketAcceptHost=" + host +
      "\n[SESSION]\nBeginString=FIX.4.4\nSenderCompID=KRXGW\nTargetCompID=BROKER\n");
}

// The broker: a threaded initiator connecting to ADDRESS.
FIX::SessionSettings BrokerSettings(int port, const std::string& address) {
  return Settings(
      "[DEFAULT]\nConnectionType=initiator\nStartTime=00:00:00\nEndTime=00:00:00\n"
      "UseDataDictionary=N\nHeartBtInt=30\nReconnectInterval=1\nSocketConnectPort=" +
      std::to_string(port) + "\nSocketConnectHost=" + address +
      "\n[SESSION]\nBeginString=FIX.4.4\nSenderCompID=BROKER\nTargetCompID=KRXGW\n");
}

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

// The ConfigError text start() threw, or "" when it started (then it is stopped again).
std::string ConfigErrorOnStart(FIX::Acceptor& acceptor) {
  try {
    acceptor.start();
  } catch (const FIX::ConfigError& error) {
    return error.what();
  }
  acceptor.stop(true);
  return "";
}

}  // namespace

SCENARIO("A broker logs on through the address the port is bound to", "[krx][b6]") {
  GIVEN("a broker port bound to 127.0.0.1") {
    const int port = FreePort();
    FIX::NullApplication gateway_app;
    FIX::MemoryStoreFactory gateway_store;
    FIX::ThreadedSocketAcceptor acceptor(gateway_app, gateway_store,
                                         AcceptorSettings(port, "127.0.0.1"));
    acceptor.start();
    WHEN("the broker connects to 127.0.0.1") {
      FIX::NullApplication broker_app;
      FIX::MemoryStoreFactory broker_store;
      FIX::ThreadedSocketInitiator broker(broker_app, broker_store,
                                          BrokerSettings(port, "127.0.0.1"));
      broker.start();
      THEN("its session logs on") {
        REQUIRE(WaitLoggedOn(GatewaySide()));
      }
      broker.stop(true);
    }
    acceptor.stop(true);
  }
}

SCENARIO("A connection through another address of the host never reaches the port", "[krx][b6]") {
  const std::string other = OtherLocalAddress();
  if (other.empty()) {
    SKIP("this host has no IPv4 address besides 127.0.0.1 (macOS without a network)");
  }
  GIVEN("a broker port bound to 127.0.0.1") {
    const int port = FreePort();
    FIX::NullApplication gateway_app;
    FIX::MemoryStoreFactory gateway_store;
    FIX::ThreadedSocketAcceptor acceptor(gateway_app, gateway_store,
                                         AcceptorSettings(port, "127.0.0.1"));
    acceptor.start();
    WHEN("someone connects to the same port through another address of the host") {
      const bool refused = !Connects(other, port);
      const bool loopback_connects = Connects("127.0.0.1", port);
      THEN("the connection is refused, while 127.0.0.1 still connects") {
        INFO("other address: " << other << ", port " << port);
        REQUIRE((refused && loopback_connects));
      }
    }
    acceptor.stop(true);
  }
}

SCENARIO("A broker port with a bind address that is not an IPv4 address does not start",
         "[krx][b6]") {
  GIVEN("a broker port whose bind address is a host name") {
    FIX::NullApplication gateway_app;
    FIX::MemoryStoreFactory gateway_store;
    FIX::ThreadedSocketAcceptor acceptor(gateway_app, gateway_store,
                                         AcceptorSettings(FreePort(), "localhost"));
    WHEN("the gateway starts it") {
      const std::string error = ConfigErrorOnStart(acceptor);
      THEN("it refuses with a configuration error naming the setting") {
        INFO("error: " << error);
        REQUIRE(error.contains("SocketAcceptHost"));
      }
    }
  }
}
