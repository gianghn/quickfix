// Rows 2.1-2.4 of docs/V1_BDD_CHECKLIST.md (P0-1): a data field's length comes from the wire, so a
// forged one must be rejected as an invalid message instead of moving the parser past the buffer.
// Out-of-bounds reads show as wrong results here and as reports under AddressSanitizer (Linux CI).
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>

#include <catch_amalgamated.hpp>

#include "DataDictionary.h"
#include "Exceptions.h"
#include "FixFields.h"
#include "FixInput.h"
#include "Message.h"

namespace {

std::string Fix44() {
  return std::string(KRX_SPEC_DIR) + "/FIX44.xml";
}

// A FIX.4.4 News whose RawData is `data` and whose RawDataLength says `data_length`.
std::string NewsWithDataLength(int data_length, const std::string& data = "abc") {
  FIX::Message news;
  news.getHeader().setField(FIX::BeginString("FIX.4.4"));
  news.getHeader().setField(FIX::MsgType("B"));  // News
  news.setField(FIX::Headline("forged"));
  news.setField(FIX::RawDataLength(data_length));
  news.setField(FIX::RawData(data));
  return news.toString();
}

void RequireRejected(int data_length) {
  const FIX::DataDictionary dictionary(Fix44());
  const std::string wire = NewsWithDataLength(data_length);
  FIX::Message parsed;
  INFO(wire);
  REQUIRE_THROWS_AS(parsed.setString(wire, false, &dictionary, &dictionary), FIX::InvalidMessage);
}

}  // namespace

SCENARIO("A data length pointing past the end of the frame is rejected",
         "[krx][v1][security][p0-1]") {
  GIVEN("a News whose RawData is 3 bytes but whose RawDataLength says 99999") {
    THEN("parsing it throws InvalidMessage and the process carries on") {
      RequireRejected(99999);
    }
  }
}

SCENARIO("A negative data length is rejected", "[krx][v1][security][p0-1]") {
  GIVEN("a News whose RawDataLength is -1") {
    THEN("parsing it throws InvalidMessage") {
      RequireRejected(-1);
    }
  }
}

SCENARIO("A data length of two billion is rejected", "[krx][v1][security][p0-1]") {
  GIVEN("a News whose RawDataLength is 2000000000") {
    THEN("parsing it throws InvalidMessage") {
      RequireRejected(2000000000);
    }
  }
}

SCENARIO("A data length that ends inside the value is rejected", "[krx][v1][security][p0-1]") {
  GIVEN("a News whose RawData is 3 bytes but whose RawDataLength says 2") {
    THEN("parsing it throws InvalidMessage, since the byte after the data is not SOH") {
      RequireRejected(2);
    }
  }
}

SCENARIO("A correct data length still parses", "[krx][v1][security][p0-1]") {
  GIVEN("a News whose RawDataLength matches its 3-byte RawData") {
    const FIX::DataDictionary dictionary(Fix44());
    const std::string wire = NewsWithDataLength(3);
    WHEN("it is parsed") {
      FIX::Message parsed;
      parsed.setString(wire, false, &dictionary, &dictionary);
      THEN("the data comes back whole") {
        const std::string& data = parsed.getField(FIX::FIELD::RawData);
        INFO(data);
        REQUIRE((data == "abc"));
      }
    }
  }
}

SCENARIO("An empty data field with length 0 still parses", "[krx][v1][security][p0-1]") {
  GIVEN("a News whose RawDataLength is 0 and whose RawData is empty") {
    const FIX::DataDictionary dictionary(Fix44());
    const std::string wire = NewsWithDataLength(0, "");
    WHEN("it is parsed") {
      FIX::Message parsed;
      parsed.setString(wire, false, &dictionary, &dictionary);
      THEN("the data comes back empty") {
        const std::string& data = parsed.getField(FIX::FIELD::RawData);
        INFO(data);
        REQUIRE(data.empty());
      }
    }
  }
}

SCENARIO("The fuzzer's recorded crash input no longer crashes the parser",
         "[krx][v1][security][p0-1]") {
  GIVEN("the input libFuzzer saved for P0-1 (test/krx/corpus)") {
    std::ifstream file(std::string(KRX_CORPUS_DIR) + "/p0-1-forged-data-length.bin",
                       std::ios::binary);
    REQUIRE(file.good());
    const std::string input{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const krx::FixDictionaries dictionaries(KRX_SPEC_DIR);
    WHEN("it is fed to the parser as the fuzzer did") {
      std::size_t rejected = 0;
      REQUIRE_NOTHROW(rejected = krx::FeedFixInput(dictionaries, input));
      THEN("the engine rejects it with a FIX exception") {
        INFO(rejected);
        REQUIRE((rejected == 1));
      }
    }
  }
}
