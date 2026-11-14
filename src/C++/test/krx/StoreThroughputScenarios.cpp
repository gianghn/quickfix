// Phase 15 of docs/V1_BDD_CHECKLIST.md: the file store with fewer system calls per message.
// Rows 15.1-15.3 (item 15.1) guard a change that must not change behaviour: messages written at
// offsets the store keeps in memory read back whole, also right after they are written and after a
// failed write, and the seqnums file keeps its format.
// Rows 15.12-15.14 (item 15.6, Q18): with store sync off, FileStoreFlushInterval makes a background
// thread flush the store to disk; a failed flush fails the next write; destroying or resetting the
// store stops the thread safely.
// Rows 15.15-15.17 (item 15.7, Q18): with store sync on, a batch of orders is stored, synced once,
// then written in order; a failed sync sends none; the result says how many went out.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <catch_amalgamated.hpp>

#include "Exceptions.h"
#include "FaultyFile.h"
#include "FileStore.h"
#include "FixFields.h"
#include "SessionID.h"
#include "SessionPair.h"
#include "SessionSettings.h"
#include "Utility.h"

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr int kFlushMs = 5;
// Generous: sanitized CI runs are slow. The thread itself waits one interval.
constexpr milliseconds kFlushedWithin{500};

FIX::SessionID StoreSession() {
  return {"FIX.4.4", "KRXGW", "KRXEX"};
}

std::filesystem::path StoreFile(const krx::TempDir& dir, const std::string& suffix) {
  return dir.Path() / ("FIX.4.4-KRXGW-KRXEX." + suffix);
}

// Message `seq_num`, its length varying with the number so offsets do not line up by chance.
std::string Message(int seq_num) {
  return "8=FIX.4.4\00134=" + std::to_string(seq_num) +
         "\00158=" + std::string(static_cast<std::size_t>(50 + ((seq_num % 7) * 31)), 'm') + "\001";
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

// The first message number in [first, last] that `store` does not give back as Message(n), or 0.
int FirstWrong(const FIX::FileStore& store, int first, int last) {
  for (int seq = first; seq <= last; ++seq) {
    if (Read(store, seq) != Message(seq)) {
      return seq;
    }
  }
  return 0;
}

// Stores messages 1 to `count` in a new store in `dir`, reading each back once stored and all of
// them every tenth, as a resend during order flow does; then sets the next target seqnum to 57.
// The first message that did not read back right, or 0.
int StoreWhileReading(const krx::TempDir& dir, int count) {
  FIX::FileStore store(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
  for (int seq = 1; seq <= count; ++seq) {
    store.set(seq, Message(seq));
    store.incrNextSenderMsgSeqNum();
    const int wrong = Read(store, seq) == Message(seq) ? 0 : seq;
    const int wrong_earlier = seq % 10 == 0 ? FirstWrong(store, 1, seq) : 0;
    if (wrong != 0 || wrong_earlier != 0) {
      return wrong != 0 ? wrong : wrong_earlier;
    }
  }
  store.setNextTargetMsgSeqNum(57);
  return 0;
}

// How many times `store` (a FileStore) has forced a file to disk.
std::uint64_t SyncsOf(const FIX::MessageStore* store) {
  const auto* file_store = dynamic_cast<const FIX::FileStore*>(store);
  return file_store == nullptr ? 0 : file_store->getSyncCount();
}

// Waits up to kFlushedWithin for `store` to have synced at least once; then lets the flush that
// was running end (three intervals) so the count is settled. True when it synced.
bool FlushedSoon(const FIX::MessageStore* store) {
  const auto start = Clock::now();
  while (SyncsOf(store) == 0 && Clock::now() - start < kFlushedWithin) {
    std::this_thread::sleep_for(milliseconds(1));
  }
  std::this_thread::sleep_for(milliseconds(3 * kFlushMs));
  return SyncsOf(store) > 0;
}

// A store whose every sync fails, as a disk that returns EIO would (row 15.13). It stops its
// flusher itself, before its own part is destroyed.
class FailingFlushStore : public FIX::FileStore {
 public:
  explicit FailingFlushStore(const krx::TempDir& dir)
      : FIX::FileStore(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession(), false,
                       kFlushMs) {}

  ~FailingFlushStore() override { stopFlusher(); }

  FailingFlushStore(const FailingFlushStore&) = delete;
  FailingFlushStore& operator=(const FailingFlushStore&) = delete;
  FailingFlushStore(FailingFlushStore&&) = delete;
  FailingFlushStore& operator=(FailingFlushStore&&) = delete;

 protected:
  bool syncFile(int /*fd*/) override { return false; }
};

// Stores messages from `first` on in `store` until one write throws or kFlushedWithin passes. The
// error's text, or "" when every write worked.
std::string FirstWriteError(FIX::FileStore& store, int first) {
  const auto start = Clock::now();
  for (int seq = first; Clock::now() - start < kFlushedWithin; ++seq) {
    try {
      store.set(seq, Message(seq));
    } catch (const FIX::IOException& e) {
      return e.what();
    }
    std::this_thread::sleep_for(milliseconds(1));
  }
  return "";
}

constexpr std::chrono::seconds kWait{5};

void LogOn(krx::SessionPair& pair) {
  pair.Write(pair.Logon(1));
  REQUIRE(pair.WaitLoggedOn(kWait));
  REQUIRE((krx::MsgTypeOf(pair.ReadFrame(kWait)) == "A"));
}

// `count` orders from the gateway (BusinessMessageRejects stand in for them, as in the store rows),
// each with a Text of `text_size` bytes.
std::vector<FIX::Message> Orders(int count, std::size_t text_size = 200) {
  std::vector<FIX::Message> orders(static_cast<std::size_t>(count));
  for (FIX::Message& order : orders) {
    order.getHeader().setField(FIX::MsgType("j"));
    order.setField(FIX::RefMsgType("D"));
    order.setField(FIX::BusinessRejectReason(0));
    order.setField(FIX::Text(std::string(text_size, 'o')));
  }
  return orders;
}

// The MsgSeqNum (34) of a raw frame, or 0.
int SeqNumOf(const std::string& frame) {
  const std::size_t tag = frame.find("\00134=");
  return tag == std::string::npos ? 0 : std::stoi(frame.substr(tag + 4));
}

// The frames the session sent, until none comes for `quiet`: the sequence numbers of the orders
// (35=j) among them, in the order they arrived.
std::vector<int> OrdersRead(krx::SessionPair& pair, milliseconds quiet) {
  std::vector<int> seq_nums;
  for (std::string frame = pair.ReadFrame(quiet); !frame.empty(); frame = pair.ReadFrame(quiet)) {
    if (krx::MsgTypeOf(frame) == "j") {
      seq_nums.push_back(SeqNumOf(frame));
    }
  }
  return seq_nums;
}

// True when `seq_nums` is `first`, `first` + 1, ... with `count` numbers.
bool Consecutive(const std::vector<int>& seq_nums, int first, std::size_t count) {
  if (seq_nums.size() != count) {
    return false;
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (seq_nums[i] != first + static_cast<int>(i)) {
      return false;
    }
  }
  return true;
}

std::string Contents(const std::filesystem::path& path) {
  const std::ifstream file(path, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

}  // namespace

SCENARIO("Messages stored while earlier ones are read back all read back whole after a restart",
         "[krx][v1][p15]") {
  GIVEN("a file store taking 300 messages, each read back once stored, all of them every tenth") {
    const krx::TempDir dir("store151");
    const int wrong_while_writing = StoreWhileReading(dir, 300);
    WHEN("the store is opened again") {
      const FIX::FileStore reopened(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
      THEN("every message reads back whole and the next seqnums are 301 and 57") {
        const int wrong_after = FirstWrong(reopened, 1, 300);
        INFO("first wrong while writing: " << wrong_while_writing
                                           << ", after restart: " << wrong_after);
        REQUIRE((wrong_while_writing == 0 && wrong_after == 0 &&
                 reopened.getNextSenderMsgSeqNum() == 301 &&
                 reopened.getNextTargetMsgSeqNum() == 57));
      }
    }
  }
}

SCENARIO("After a failed write, the next message is stored right after the last good one",
         "[krx][v1][p15][p0-4]") {
  GIVEN("a file store holding messages 1 and 2 whose write of message 3 failed for lack of space") {
    const krx::TempDir dir("store152");
    std::uintmax_t body_good = 0;
    bool failed = false;
    {
      FIX::FileStore store(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
      store.set(1, Message(1));
      store.set(2, Message(2));
      body_good = std::filesystem::file_size(StoreFile(dir, "body"));
      {
        const krx::FileSizeLimit full(body_good + 10);
        try {
          store.set(3, Message(3));
        } catch (const FIX::IOException&) {
          failed = true;
        }
      }
      WHEN("message 4 is stored once space returns, and the store is opened again") {
        store.set(4, Message(4));
        const std::uintmax_t body_after = std::filesystem::file_size(StoreFile(dir, "body"));
        const FIX::FileStore reopened(FIX::UtcTimeStamp::now(), dir.Path().string(),
                                      StoreSession());
        THEN("message 4 starts where message 2 ended, and 1, 2 and 4 read back whole") {
          const std::string four = Read(reopened, 4);
          INFO("body " << body_after << " (good " << body_good << "), 4: " << four);
          REQUIRE((failed && body_after == body_good + Message(4).size() &&
                   Read(reopened, 1) == Message(1) && Read(reopened, 2) == Message(2) &&
                   Read(reopened, 3) == "<missing>" && four == Message(4)));
        }
      }
    }
  }
}

SCENARIO("After every update the seqnums file holds the last seqnums in its existing format",
         "[krx][v1][p15]") {
  GIVEN("a file store") {
    const krx::TempDir dir("store153");
    FIX::FileStore store(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
    WHEN("sender goes to 12345, target to 67, then sender down to 5 and target up by one") {
      store.setNextSenderMsgSeqNum(12345);
      store.setNextTargetMsgSeqNum(67);
      const std::string raised = Contents(StoreFile(dir, "seqnums"));
      store.setNextSenderMsgSeqNum(5);
      store.incrNextTargetMsgSeqNum();
      const std::string lowered = Contents(StoreFile(dir, "seqnums"));
      THEN("the file reads \"0000012345 : 0000000067\", then \"0000000005 : 0000000068\"") {
        INFO("raised: '" << raised << "', lowered: '" << lowered << "'");
        REQUIRE((raised == "0000012345 : 0000000067" && lowered == "0000000005 : 0000000068"));
      }
    }
  }
}

SCENARIO("With a flush interval and store sync off, a stored message is flushed soon after",
         "[krx][v1][p15][flush]") {
  GIVEN("session settings with FileStoreSync=N and FileStoreFlushInterval=5") {
    const krx::TempDir dir("store1512");
    std::istringstream text(
        "[DEFAULT]\nConnectionType=acceptor\nFileStorePath=" + dir.Path().string() +
        "\nFileStoreSync=N\nFileStoreFlushInterval=5\n[SESSION]\nBeginString=FIX.4.4\n"
        "SenderCompID=KRXGW\nTargetCompID=KRXEX\n");
    const FIX::SessionSettings settings(text);
    FIX::FileStoreFactory factory(settings);
    WHEN("the factory's store keeps a message, then nothing more for ten intervals") {
      FIX::MessageStore* store = factory.create(FIX::UtcTimeStamp::now(), StoreSession());
      store->set(1, Message(1));
      store->incrNextSenderMsgSeqNum();
      const bool flushed = FlushedSoon(store);
      const std::uint64_t settled = SyncsOf(store);
      std::this_thread::sleep_for(milliseconds(10 * kFlushMs));
      const std::uint64_t idle = SyncsOf(store);
      factory.destroy(store);
      THEN("it was flushed to disk, and the idle store flushed nothing more") {
        INFO("syncs once flushed: " << settled << ", after idling: " << idle);
        REQUIRE((flushed && idle == settled));
      }
    }
  }
}

SCENARIO("After a background flush fails, the next store write fails with the flush's error",
         "[krx][v1][p15][flush]") {
  GIVEN("a store with a flush interval whose syncs fail") {
    const krx::TempDir dir("store1513");
    FailingFlushStore store(dir);
    WHEN("it keeps storing messages") {
      const std::string error = FirstWriteError(store, 1);
      THEN("a write soon fails, and its error names the failed flush") {
        INFO("error: '" << error << "'");
        REQUIRE(error.contains("flush"));
      }
    }
  }
}

SCENARIO("A store destroyed or reset while its flusher runs opens again with what it kept",
         "[krx][v1][p15][flush]") {
  GIVEN("a store flushing every millisecond") {
    const krx::TempDir dir("store1514");
    auto store = std::make_unique<FIX::FileStore>(FIX::UtcTimeStamp::now(), dir.Path().string(),
                                                  StoreSession(), false, 1);
    WHEN("it keeps 200 messages, is reset, keeps 100 new ones, and is destroyed at once") {
      for (int seq = 1; seq <= 200; ++seq) {
        store->set(seq, Message(seq));
      }
      store->reset(FIX::UtcTimeStamp::now());
      for (int seq = 1; seq <= 100; ++seq) {
        store->set(seq, Message(seq));
      }
      const auto start = Clock::now();
      store.reset();
      const auto took = std::chrono::duration_cast<milliseconds>(Clock::now() - start);
      const FIX::FileStore reopened(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
      THEN("destroying it was prompt, and the store opens again with the 100 new messages") {
        const int wrong = FirstWrong(reopened, 1, 100);
        INFO("destroy took " << took.count() << " ms; first wrong: " << wrong);
        REQUIRE((took < kFlushedWithin && wrong == 0 && Read(reopened, 101) == "<missing>"));
      }
    }
  }
}

SCENARIO("With store sync, a batch of orders costs one sync and reaches the exchange in order",
         "[krx][v1][p15][batch]") {
  GIVEN("a logged-on session whose file store syncs to disk") {
    const krx::TempDir dir("store1515");
    krx::PairOptions options;
    options.file_store_dir = dir.Path().string();
    options.file_store_sync = true;
    krx::SessionPair pair("KRXB15", options);
    LogOn(pair);
    WHEN("the gateway sends five orders as one batch") {
      std::vector<FIX::Message> orders = Orders(5);
      const std::uint64_t syncs_before = SyncsOf(pair.Store());
      const std::size_t sent = pair.SendBatch(orders);
      const std::uint64_t syncs = SyncsOf(pair.Store()) - syncs_before;
      const std::vector<int> read = OrdersRead(pair, milliseconds(200));
      THEN("all five went out after one sync, whole and with consecutive sequence numbers") {
        INFO("sent " << sent << ", syncs " << syncs << ", orders read " << read.size());
        REQUIRE((sent == 5 && syncs == 1 && Consecutive(read, 2, 5)));
      }
    }
  }
}

SCENARIO("With store sync, a batch whose sync fails sends none of its orders and disconnects",
         "[krx][v1][p15][batch]") {
  GIVEN("a logged-on session whose store's syncs start failing") {
    const krx::TempDir dir("store1516");
    krx::PairOptions options;
    options.file_store_dir = dir.Path().string();
    options.sync_fails = true;
    krx::SessionPair pair("KRXB16", options);
    LogOn(pair);
    auto* store = dynamic_cast<krx::SyncFailingFileStore*>(pair.Store());
    REQUIRE((store != nullptr));
    store->FailSyncs();
    WHEN("the gateway sends three orders as one batch") {
      std::vector<FIX::Message> orders = Orders(3);
      const std::size_t sent = pair.SendBatch(orders);
      const std::vector<int> read = OrdersRead(pair, milliseconds(200));
      THEN("none went out, an error is logged, and the session disconnects") {
        const bool logged = pair.WaitForEvent("Store write failed", kWait);
        const bool closed = pair.WaitClosed(kWait);
        INFO("sent " << sent << ", orders read " << read.size() << "\n" << pair.Events().Dump());
        REQUIRE((sent == 0 && read.empty() && logged && closed));
      }
    }
  }
}

SCENARIO("When the connection fails mid-batch, the result says which orders went out",
         "[krx][v1][p15][batch]") {
  GIVEN("a logged-on session with a 300 ms send timeout, the exchange not reading") {
    const krx::TempDir dir("store1517");
    krx::PairOptions options;
    options.file_store_dir = dir.Path().string();
    options.file_store_sync = true;
    options.send_timeout_ms = 300;
    krx::SessionPair pair("KRXB17", options);
    LogOn(pair);
    WHEN("the gateway sends a batch of 5,000 orders of 1 KB, more than the socket holds") {
      std::vector<FIX::Message> orders = Orders(5000, 1000);
      const std::size_t sent = pair.SendBatch(orders);
      const std::vector<int> read = OrdersRead(pair, milliseconds(500));
      THEN("some but not all went out, and the exchange got exactly those, in order") {
        INFO("sent " << sent << ", orders read " << read.size());
        REQUIRE((sent > 0 && sent < 5000 && Consecutive(read, 2, sent)));
      }
    }
  }
}

SCENARIO("A store with a long flush interval stops its flusher at once when destroyed",
         "[krx][v1][p15][flush]") {
  GIVEN("a store flushing every 5 seconds, its flusher started by a write and now waiting") {
    const krx::TempDir dir("store1514long");
    auto store = std::make_unique<FIX::FileStore>(FIX::UtcTimeStamp::now(), dir.Path().string(),
                                                  StoreSession(), false, 5000);
    store->set(1, Message(1));
    std::this_thread::sleep_for(milliseconds(100));
    WHEN("it is destroyed") {
      const auto start = Clock::now();
      store.reset();
      const auto took = std::chrono::duration_cast<milliseconds>(Clock::now() - start);
      THEN("destroying it did not wait for the next flush") {
        INFO("destroy took " << took.count() << " ms");
        REQUIRE((took < kFlushedWithin));
      }
    }
  }
}

SCENARIO("Seqnums at the edges of the format are written as printf's \"%10.10d\" writes them",
         "[krx][v1][p15]") {
  GIVEN("a file store") {
    const krx::TempDir dir("store153edge");
    FIX::FileStore store(FIX::UtcTimeStamp::now(), dir.Path().string(), StoreSession());
    WHEN("the seqnums are set to 0 and -5, then to the largest and smallest int") {
      store.setNextSenderMsgSeqNum(0);
      store.setNextTargetMsgSeqNum(-5);
      const std::string small = Contents(StoreFile(dir, "seqnums"));
      store.setNextSenderMsgSeqNum(std::numeric_limits<int>::max());
      store.setNextTargetMsgSeqNum(std::numeric_limits<int>::min());
      const std::string limits = Contents(StoreFile(dir, "seqnums"));
      THEN("the file reads \"0000000000 : -0000000005\", then \"2147483647 : -2147483648\"") {
        INFO("small: '" << small << "', limits: '" << limits << "'");
        REQUIRE((small == "0000000000 : -0000000005" && limits == "2147483647 : -2147483648"));
      }
    }
  }
}
