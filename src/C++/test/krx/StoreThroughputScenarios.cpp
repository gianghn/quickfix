// Phase 15 of docs/V1_BDD_CHECKLIST.md: the file store with fewer system calls per message.
// Rows 15.1-15.3 (item 15.1) guard a change that must not change behaviour: messages written at
// offsets the store keeps in memory read back whole, also right after they are written and after a
// failed write, and the seqnums file keeps its format.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
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
