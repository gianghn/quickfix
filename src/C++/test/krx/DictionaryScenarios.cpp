// Row 1.4 of docs/V1_BDD_CHECKLIST.md (K-3): assigning a data dictionary replaces its groups. The
// leak half is proven by LeakSanitizer (Linux CI); the stale-group half shows on every platform.
#include <string>

#include <catch_amalgamated.hpp>

#include "DataDictionary.h"
#include "FixFields.h"

namespace {

std::string Fix44() {
  return std::string(KRX_SPEC_DIR) + "/FIX44.xml";
}

}  // namespace

SCENARIO("Assigning one data dictionary over another frees what it held", "[krx][v1][k-3]") {
  GIVEN("a FIX.4.4 dictionary, whose NewOrderSingle has a parties group") {
    FIX::DataDictionary dictionary(Fix44());
    WHEN("an empty dictionary is assigned over it") {
      dictionary = FIX::DataDictionary();
      THEN("the old parties group is gone") {
        REQUIRE_FALSE(dictionary.isGroup("D", FIX::FIELD::NoPartyIDs));
      }
    }
  }
}

SCENARIO("A data dictionary assigned to itself keeps its groups", "[krx][v1][k-3]") {
  GIVEN("a FIX.4.4 dictionary") {
    FIX::DataDictionary dictionary(Fix44());
    const FIX::DataDictionary& same = dictionary;
    WHEN("it is assigned to itself") {
      dictionary = same;
      THEN("its parties group still describes the party fields") {
        int delimiter = 0;
        const FIX::DataDictionary* parties = nullptr;
        REQUIRE((dictionary.getGroup("D", FIX::FIELD::NoPartyIDs, delimiter, parties) &&
                 delimiter == FIX::FIELD::PartyID && parties->isField(FIX::FIELD::PartyRole)));
      }
    }
  }
}
