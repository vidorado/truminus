#include "catch_amalgamated.hpp"
#include "ws_diff.hpp"   // real production change-detection predicates
#include <cmath>

TEST_CASE("multiPowerChanged - sentinel aware", "[ws_diff]") {
    CHECK_FALSE(multiPowerChanged(MULTI_POWER_NA, MULTI_POWER_NA)); // both no-data
    CHECK(multiPowerChanged(MULTI_POWER_NA, 0));                    // no-data -> 0 W
    CHECK(multiPowerChanged(120, MULTI_POWER_NA));                  // value -> no-data
    CHECK_FALSE(multiPowerChanged(100, 104));                       // delta 4 <= 5
    CHECK_FALSE(multiPowerChanged(100, 105));                       // delta 5 not > 5
    CHECK(multiPowerChanged(100, 106));                             // delta 6 > 5
    // no abs() overflow even though INT32_MIN is the sentinel
    CHECK(multiPowerChanged(MULTI_POWER_NA, 100));
}

TEST_CASE("linTempChanged - NaN / sentinel aware", "[ws_diff]") {
    CHECK_FALSE(linTempChanged(NAN, NAN));        // both "no data"
    CHECK_FALSE(linTempChanged(-273.0f, -300.0f)); // both <= -200 = no data
    CHECK(linTempChanged(NAN, 21.0f));            // no-data -> value
    CHECK(linTempChanged(21.0f, NAN));            // value -> no-data
    CHECK_FALSE(linTempChanged(21.00f, 21.03f));  // delta 0.03 < 0.05
    CHECK(linTempChanged(21.0f, 21.1f));          // delta 0.1 > 0.05
}

TEST_CASE("victronChanged - thresholds and validity", "[ws_diff]") {
    VictronData a{}; VictronData b{};
    CHECK_FALSE(victronChanged(a, b));            // both invalid, identical

    a.valid = true;
    CHECK(victronChanged(a, b));                  // validity flip

    b = a;
    CHECK_FALSE(victronChanged(a, b));            // valid, identical

    a.battV = b.battV + 0.04f; CHECK_FALSE(victronChanged(a, b)); a = b;
    a.battV = b.battV + 0.06f; CHECK(victronChanged(a, b));       a = b;
    a.pvW   = b.pvW   + 0.4f;  CHECK_FALSE(victronChanged(a, b)); a = b;
    a.pvW   = b.pvW   + 0.6f;  CHECK(victronChanged(a, b));       a = b;
    a.kWhToday = b.kWhToday + 0.02f; CHECK(victronChanged(a, b)); a = b;
    a.state = b.state + 1;     CHECK(victronChanged(a, b));
}

TEST_CASE("ultimatronChanged", "[ws_diff]") {
    UltimatronData a{}; a.valid = true; UltimatronData b = a;
    CHECK_FALSE(ultimatronChanged(a, b));
    a.soc = b.soc + 1;          CHECK(ultimatronChanged(a, b)); a = b;
    a.battV = b.battV + 0.06f;  CHECK(ultimatronChanged(a, b)); a = b;
    a.battA = b.battA + 0.04f;  CHECK_FALSE(ultimatronChanged(a, b));
}

TEST_CASE("multiplusChanged - NaN battV is ignored", "[ws_diff]") {
    MultiplusData a{}; a.valid = true; a.battV = NAN; MultiplusData b = a;
    CHECK_FALSE(multiplusChanged(a, b));          // NaN battV must not trip the diff
    a.battA = b.battA + 0.2f;  CHECK(multiplusChanged(a, b)); a = b;
    a.battA = b.battA + 0.05f; CHECK_FALSE(multiplusChanged(a, b)); a = b; // 0.1 thresh
    a.soc = b.soc + 1;         CHECK(multiplusChanged(a, b)); a = b;
    a.acInW = MULTI_POWER_NA;  b.acInW = 0; CHECK(multiplusChanged(a, b));
}

TEST_CASE("tankChanged - fresh advert (lastMs) re-emits", "[ws_diff]") {
    TankData a{}; a.valid = true; a.pct = 50; a.lastMs = 1000;
    TankData b = a;
    CHECK_FALSE(tankChanged(a, b));
    a.lastMs = 2000;           CHECK(tankChanged(a, b)); // same pct, fresh advert
    a = b; a.pct = 51;         CHECK(tankChanged(a, b));
    a = b; a.valid = false;    CHECK(tankChanged(a, b));
}
