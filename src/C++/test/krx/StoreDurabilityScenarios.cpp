// Phase 4 of docs/V1_BDD_CHECKLIST.md: the message store survives disk errors and crashes.
// Rows 4.1 and 4.2 (P0-4): a write that fails (here: the file may not grow, as on a full disk)
// leaves the store as it was before the write: earlier messages still read back, the failed one is
// not there, and the store takes writes again once space returns.
// Row 4.3 (P0-4, D7): a session whose store cannot write fails closed: the send fails, an error is
// logged and the session disconnects.
// Row 4.4 (P0-4): a restart loads only records whose message is whole in the body file.
// Row 4.6 (P2-4): a stored message comes back with every byte, NULs included.
// Row 4.5 (P0-5): with FileStoreSync=Y, body, header and seqnums are synced before send returns.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <catch_amalgamated.hpp>

#include "Exceptions.h"
#include "FaultyFile.h"
#include "FileStore.h"
#include "FixFields.h"
#include "Message.h"
#include "SessionID.h"
#include "SessionPair.h"
#include "SessionSettings.h"
#include "Utility.h"

namespace {

constexpr std::chrono::seconds kWait{5};

// A logged-on session pair whose session stores its messages in `dir`.
krx::PairOptions OnFileStore(const krx::TempDir& dir) {
  krx::PairOptions options;
  options.file_store_dir = dir.Path().string();
  return options;
}

void LogOn(krx::SessionPair& pair) {
  pair.Write(pair.Logon(1));
  REQUIRE(pair.WaitLoggedOn(kWait));
  REQUIRE((krx::MsgTypeOf(pair.ReadFrame(kWait)) == "A"));
}

// How many times `store` (a FileStore) has forced its files to disk.
std::uint64_t SyncsOf(const FIX::MessageStore* store) {
  const auto* file_store = dynamic_cast<const FIX::FileStore*>(store);
  return file_store == nullptr ? 0 : file_store->getSyncCount();
}

// An order from the gateway (a BusinessMessageReject stands in for it, as in the socket rows).
FIX::Message Order() {
  FIX::Message order;
  order.getHeader().setField(FIX::MsgType("j"));
  order.setField(FIX::RefMsgType("D"));
  order.setField(FIX::BusinessRejectReason(0));
  order.setField(FIX::Text(std::string(200, 'o')));
  return order;
}

FIX::SessionID StoreSession() {
  return {"FIX.4.4", "KRXGW", "KRXEX"};
}

// The store's body file (the messages themselves) in `dir`.
std::filesystem::path BodyFile(const krx::TempDir& dir, const std::string& counterparty = "KRXEX") {
  return dir.Path() / ("FIX.4.4-KRXGW-" + counterparty + ".body");
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

SCENARIO("A restart after a failed write loads only consistent records", "[krx][v1][p0-4]") {
  GIVEN("a store that held messages 1 to 3 when the machine stopped with 3's body cut short") {
    const krx::TempDir dir("store44");
    {
      FIX::FileStore store(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
      store.set(1, Message(1));
      store.set(2, Message(2));
      store.set(3, Message(3));
    }
    std::filesystem::resize_file(BodyFile(dir), std::filesystem::file_size(BodyFile(dir)) - 50);
    WHEN("the store is opened again") {
      const FIX::FileStore store(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
      std::vector<std::string> range;
      bool range_failed = false;
      try {
        store.get(1, 3, range);
      } catch (const FIX::IOException&) {
        range_failed = true;
      }
      THEN("1 and 2 read back whole, 3 is not there, and reading 1 to 3 gives 1 and 2") {
        const std::string three = Read(store, 3);
        INFO("3: " << three << "; range read " << (range_failed ? "failed" : "worked") << ", "
                   << range.size() << " messages");
        REQUIRE((Read(store, 1) == Message(1) && Read(store, 2) == Message(2) &&
                 three == "<missing>" && !range_failed && range.size() == 2));
      }
    }
  }
}

SCENARIO("A store reopened after a clean stop keeps its one message", "[krx][v1][p0-4]") {
  GIVEN("a store that held only message 1 (its body is the whole body file) and stopped cleanly") {
    const krx::TempDir dir("store44b");
    {
      FIX::FileStore store(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
      store.set(1, Message(1));
    }
    WHEN("the store is opened again") {
      const FIX::FileStore store(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
      THEN("message 1 reads back whole") {
        const std::string one = Read(store, 1);
        INFO("1: " << one);
        REQUIRE((one == Message(1)));
      }
    }
  }
}

SCENARIO("A message with an embedded NUL comes back from the store whole", "[krx][v1][p2-4]") {
  GIVEN("a file store") {
    const krx::TempDir dir("store46");
    FIX::FileStore store(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
    WHEN("a message whose data field holds a NUL byte is stored") {
      using namespace std::string_literals;
      const std::string message = "8=FIX.4.4\00195=3\00196=a\0b\00110=000\001"s;
      store.set(1, message);
      THEN("it reads back with every byte") {
        const std::string back = Read(store, 1);
        INFO(back.size() << " of " << message.size() << " bytes back");
        REQUIRE((back == message));
      }
    }
  }
}

SCENARIO("With store sync on, a stored message and its seqnum are on disk before send() returns",
         "[krx][v1][p0-5]") {
  GIVEN("a logged-on session whose file store syncs to disk") {
    const krx::TempDir dir("store45");
    krx::PairOptions options = OnFileStore(dir);
    options.file_store_sync = true;
    krx::SessionPair pair("KRXSY45", options);
    LogOn(pair);
    WHEN("the gateway sends an order") {
      const std::uint64_t before = SyncsOf(pair.Store());
      const bool sent = pair.Send(Order());
      const std::uint64_t after = SyncsOf(pair.Store());
      THEN("body, header and seqnums are synced by the time send returns") {
        INFO(before << " file syncs before the send, " << after << " after");
        REQUIRE((sent && after - before >= 3));
      }
    }
  }
}

SCENARIO("With store sync off, the store does not force anything to disk", "[krx][v1][p0-5]") {
  GIVEN("a logged-on session on a file store with sync off (the default)") {
    const krx::TempDir dir("store45off");
    krx::SessionPair pair("KRXSN45", OnFileStore(dir));
    LogOn(pair);
    WHEN("the gateway sends an order") {
      const bool sent = pair.Send(Order());
      THEN("the store never synced") {
        const std::uint64_t syncs = SyncsOf(pair.Store());
        INFO(syncs << " syncs");
        REQUIRE((sent && syncs == 0));
      }
    }
  }
}

SCENARIO("FileStoreSync=Y in the session settings turns store sync on", "[krx][v1][p0-5]") {
  GIVEN("session settings with FileStorePath and FileStoreSync=Y") {
    const krx::TempDir dir("store45cfg");
    std::istringstream text(
        "[DEFAULT]\nConnectionType=acceptor\nFileStorePath=" + dir.Path().string() +
        "\nFileStoreSync=Y\n[SESSION]\nBeginString=FIX.4.4\n"
        "SenderCompID=KRXGW\nTargetCompID=KRXEX\n");
    const FIX::SessionSettings settings(text);
    FIX::FileStoreFactory factory(settings);
    WHEN("the factory's store keeps a message and its seqnum") {
      FIX::MessageStore* store = factory.create(FIX::UtcTimeStamp::now(), StoreSession());
      store->set(1, Message(1));
      store->incrNextSenderMsgSeqNum();
      const std::uint64_t syncs = SyncsOf(store);
      factory.destroy(store);
      THEN("it synced them to disk") {
        INFO(syncs << " syncs");
        REQUIRE((syncs > 0));
      }
    }
  }
}

SCENARIO("A failed write stops order flow and disconnects the session with an error logged",
         "[krx][v1][p0-4]") {
  GIVEN("a logged-on session that stores its messages in files") {
    const krx::TempDir dir("store43");
    krx::SessionPair pair("KRXSF43", OnFileStore(dir));
    LogOn(pair);
    WHEN("the gateway sends an order while the store's files may not grow") {
      bool sent = true;
      {
        const krx::FileSizeLimit full(std::filesystem::file_size(BodyFile(dir, "KRXSF43")) + 10);
        sent = pair.Send(Order());
      }
      const bool closed = pair.WaitClosed(kWait);
      THEN("the send fails, a store error is logged, and the session disconnects") {
        INFO(pair.Events().Dump());
        REQUIRE((!sent && pair.Events().Contains("Store write failed") && closed));
      }
    }
  }
}
