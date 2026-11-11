// Row 1.8 of docs/V1_BDD_CHECKLIST.md (K-6): the plain SocketAcceptor driven by poll() owns its
// socket server: destroying it frees the server and gives the listening port back, and every socket
// is closed once (Trap-3: quickfix-rs v0.2.2 would move the gateway to this class).
#include <sys/stat.h>

#include <cstdio>
#include <memory>
#include <sstream>
#include <string>

#include <catch_amalgamated.hpp>

#include "Application.h"
#include "MessageStore.h"
#include "SessionID.h"
#include "SessionSettings.h"
#include "SocketAcceptor.h"
#include "Utility.h"

namespace {

// A plain acceptor on an ephemeral port, polled once so it is listening.
class PolledAcceptor {
 public:
  PolledAcceptor()
      : settings_(Settings()),
        acceptor_(std::make_unique<FIX::SocketAcceptor>(application_, store_, settings_)) {
    acceptor_->poll();
  }

  [[nodiscard]] int Port() const {
    return acceptor_->sessionToPort().find(FIX::SessionID("FIX.4.2", "KRXA", "KRXB"))->second;
  }

  void Stop() const { acceptor_->stop(true); }

  void Destroy() { acceptor_.reset(); }

 private:
  static FIX::SessionSettings Settings() {
    std::istringstream stream(
        "[DEFAULT]\nConnectionType=acceptor\nSocketAcceptPort=0\nSocketReuseAddress=Y\n"
        "StartTime=00:00:00\nEndTime=00:00:00\nUseDataDictionary=N\n"
        "[SESSION]\nBeginString=FIX.4.2\nSenderCompID=KRXA\nTargetCompID=KRXB\n");
    FIX::SessionSettings settings;
    stream >> settings;
    return settings;
  }

  FIX::NullApplication application_;
  FIX::MemoryStoreFactory store_;
  FIX::SessionSettings settings_;
  std::unique_ptr<FIX::SocketAcceptor> acceptor_;
};

// Whether a new listening socket can take PORT now (no reuse: an open listener holds it).
bool CanListenOn(int port) {
  const FIX::socket_handle socket = FIX::socket_createAcceptor(port, false);
  if (!FIX::socket_isValid(socket)) {
    return false;
  }
  FIX::socket_close(socket);
  return true;
}

// The inode behind DESCRIPTOR, or 0 when it is closed.
ino_t InodeOf(int descriptor) {
  struct stat status{};
  return fstat(descriptor, &status) == 0 ? status.st_ino : 0;
}

// A temporary file on the lowest free descriptor: a number closed twice closes this file.
class Canary {
 public:
  Canary() : file_(std::tmpfile()), inode_(file_ == nullptr ? 0 : InodeOf(fileno(file_))) {}

  ~Canary() {
    if (file_ != nullptr) {
      std::fclose(file_);
    }
  }

  Canary(const Canary&) = delete;
  Canary& operator=(const Canary&) = delete;
  Canary(Canary&&) = delete;
  Canary& operator=(Canary&&) = delete;

  [[nodiscard]] bool StillOpen() const { return inode_ != 0 && InodeOf(fileno(file_)) == inode_; }

 private:
  std::FILE* file_;
  ino_t inode_;
};

}  // namespace

SCENARIO("A polled plain acceptor gives its port back when it is destroyed", "[krx][v1][k-6]") {
  GIVEN("a plain acceptor that was polled, so it listens on a port") {
    PolledAcceptor acceptor;
    const int port = acceptor.Port();
    WHEN("it is destroyed without being stopped") {
      acceptor.Destroy();
      THEN("the port can be listened on again") {
        REQUIRE(CanListenOn(port));
      }
    }
  }
}

SCENARIO("A stopped plain acceptor closes its listening socket once", "[krx][v1][k-6]") {
  GIVEN("a polled plain acceptor that was stopped") {
    PolledAcceptor acceptor;
    acceptor.Stop();
    WHEN("a file takes the freed descriptor and the acceptor is then destroyed") {
      const Canary canary;
      acceptor.Destroy();
      THEN("the file is still open") {
        REQUIRE(canary.StillOpen());
      }
    }
  }
}
