// Phase 4 of docs/V1_BDD_CHECKLIST.md: the message store survives disk errors and crashes.
// Rows 4.1 and 4.2 (P0-4): a write that fails (here: the file may not grow, as on a full disk)
// leaves the store as it was before the write: earlier messages still read back, the failed one is
// not there, and the store takes writes again once space returns.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "Exceptions.h"
#include "FaultyFile.h"
#include "FileStore.h"
#include "SessionID.h"
#include "Utility.h"

namespace {

FIX::SessionID StoreSession() {
  return {"FIX.4.4", "KRXGW", "KRXEX"};
}

// The store's body file (the messages themselves) in `dir`.
std::filesystem::path BodyFile(const krx::TempDir& dir) {
  return dir.Path() / "FIX.4.4-KRXGW-KRXEX.body";
}

std::filesystem::path HeaderFile(const krx::TempDir& dir) {
  return dir.Path() / "FIX.4.4-KRXGW-KRXEX.header";
}

std::string Message(int seq_num) {
  return "8=FIX.4.4\00134=" + std::to_string(seq_num) + "\00158=" + std::string(200, 'm') + "\001";
}

// Message `seq_num` as the store gives it back: the text, "<missing>", or "<error: ...>".
std::string Read(const FIX::FileStore& store, int seq_num) {
  std::vector<std::string> messages;
  try {
    store.get(seq_num, seq_num, messages);
    return messages.empty() ? "<missing>" : messages.front();
  } catch (const FIX::IOException& e) {
    return std::string("<error: ") + e.what() + ">";
  }
}

// True when storing `message` as `seq_num` threw an IOException.
bool WriteFails(FIX::FileStore& store, int seq_num, const std::string& message) {
  try {
    store.set(seq_num, message);
    return false;
  } catch (const FIX::IOException&) {
    return true;
  }
}

}  // namespace

SCENARIO("After a failed write, earlier messages can still be read from the store",
         "[krx][v1][p0-4]") {
  GIVEN("a file store holding messages 1 and 2") {
    const krx::TempDir dir("store41");
    FIX::FileStore store(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
    store.set(1, Message(1));
    store.set(2, Message(2));
    const std::uintmax_t body_before = std::filesystem::file_size(BodyFile(dir));
    WHEN("writing message 3 fails because the body file may not grow") {
      bool failed = false;
      {
        const krx::FileSizeLimit full(body_before + 10);
        failed = WriteFails(store, 3, Message(3));
      }
      THEN("the write reports it, 1 and 2 read back whole, 3 is not there, no half body is left") {
        const std::string one = Read(store, 1);
        const std::string two = Read(store, 2);
        const std::string three = Read(store, 3);
        INFO("1: " << one << "\n2: " << two << "\n3: " << three);
        REQUIRE((failed && one == Message(1) && two == Message(2) && three == "<missing>" &&
                 std::filesystem::file_size(BodyFile(dir)) == body_before));
      }
    }
  }
}

SCENARIO("After a failed write, the store accepts writes once space returns", "[krx][v1][p0-4]") {
  GIVEN("a file store whose write of message 3 failed for lack of space") {
    const krx::TempDir dir("store42");
    FIX::FileStore store(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
    store.set(1, Message(1));
    store.set(2, Message(2));
    {
      const krx::FileSizeLimit full(std::filesystem::file_size(BodyFile(dir)) + 10);
      REQUIRE(WriteFails(store, 3, Message(3)));
    }
    WHEN("space returns and message 3 is written again") {
      const bool failed_again = WriteFails(store, 3, Message(3));
      THEN("messages 1, 2 and 3 read back whole") {
        const std::string three = Read(store, 3);
        INFO("3: " << three);
        REQUIRE((!failed_again && Read(store, 1) == Message(1) && Read(store, 2) == Message(2) &&
                 three == Message(3)));
      }
    }
  }
}

SCENARIO("A failed header write leaves neither the message nor its body behind",
         "[krx][v1][p0-4]") {
  GIVEN("a file store whose header file is larger than its body file") {
    const krx::TempDir dir("store4h");
    FIX::FileStore store(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
    for (int seq = 1000001; seq <= 1000050; ++seq) {
      store.set(seq, "x");
    }
    const std::uintmax_t body_before = std::filesystem::file_size(BodyFile(dir));
    const std::uintmax_t header_before = std::filesystem::file_size(HeaderFile(dir));
    WHEN("the body of the next message fits but its header record does not") {
      bool failed = false;
      {
        const krx::FileSizeLimit full(header_before + 5);
        failed = WriteFails(store, 1000051, "x");
      }
      THEN("the write reports it, the message is not there, and both files are as before") {
        const std::string last = Read(store, 1000051);
        INFO("1000051: " << last << "; body " << std::filesystem::file_size(BodyFile(dir))
                         << " (was " << body_before << ")");
        REQUIRE((failed && last == "<missing>" && Read(store, 1000050) == "x" &&
                 std::filesystem::file_size(BodyFile(dir)) == body_before &&
                 std::filesystem::file_size(HeaderFile(dir)) == header_before));
      }
    }
  }
}
