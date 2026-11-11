// Row 1.6 of docs/V1_BDD_CHECKLIST.md (K-4): a typed message hands out its real header and trailer.
// Casting them to the version's Header/Trailer types was undefined behaviour (UBSan's vptr check).
#include <type_traits>
#include <utility>

#include <catch_amalgamated.hpp>

#include "FixFields.h"
#include "Message.h"
#include "fix44/Logon.h"

SCENARIO("A typed message's header is never treated as a type it is not", "[krx][v1][k-4]") {
  GIVEN("a typed FIX.4.4 Logon") {
    STATIC_REQUIRE(
        std::is_same_v<decltype(std::declval<FIX44::Logon&>().getHeader()), FIX::Header&>);
    STATIC_REQUIRE(
        std::is_same_v<decltype(std::declval<FIX44::Logon&>().getTrailer()), FIX::Trailer&>);
    FIX44::Logon logon;
    WHEN("its header and trailer are set through the typed accessors") {
      logon.getHeader().set(FIX::SenderCompID("EXCH"));
      logon.getTrailer().set(FIX::CheckSum(7));
      THEN("they read back the same") {
        FIX::SenderCompID sender;
        FIX::CheckSum check_sum;
        REQUIRE((logon.getHeader().get(sender).getValue() == "EXCH" &&
                 logon.getTrailer().get(check_sum).getValue() == 7));
      }
    }
  }
}
