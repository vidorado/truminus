#include "catch_amalgamated.hpp"
#include "ws_command.hpp"   // real production parser

// Characterization tests for parseWsCommand(): they pin down the exact mapping
// that lives inline in main.cpp's onWsCommand(), so the upcoming extraction of
// the dispatch is provably behavior-preserving.

TEST_CASE("null id or value yields None", "[ws_command]") {
    CHECK(parseWsCommand(nullptr, "1").kind == WsCmdKind::None);
    CHECK(parseWsCommand("/heating", nullptr).kind == WsCmdKind::None);
    CHECK(parseWsCommand(nullptr, nullptr).kind == WsCmdKind::None);
}

TEST_CASE("leading slash is optional", "[ws_command]") {
    CHECK(parseWsCommand("/heating", "1").kind == WsCmdKind::Heating);
    CHECK(parseWsCommand("heating", "1").kind  == WsCmdKind::Heating);
}

TEST_CASE("heating truthiness", "[ws_command]") {
    CHECK(parseWsCommand("heating", "1").boolVal    == true);
    CHECK(parseWsCommand("heating", "true").boolVal == true);
    CHECK(parseWsCommand("heating", "TRUE").boolVal == true);  // case-insensitive
    CHECK(parseWsCommand("heating", "True").boolVal == true);
    CHECK(parseWsCommand("heating", "0").boolVal    == false);
    CHECK(parseWsCommand("heating", "false").boolVal== false);
    CHECK(parseWsCommand("heating", "yes").boolVal  == false);  // only 1/true count
    CHECK(parseWsCommand("heating", "1").valid      == true);
}

TEST_CASE("fan maps via mode_controller and validates", "[ws_command]") {
    auto off = parseWsCommand("fan", "off");
    CHECK(off.kind == WsCmdKind::Fan);
    CHECK(off.valid == true);
    CHECK(off.intVal == 0);

    CHECK(parseWsCommand("fan", "eco").intVal  == 1);
    CHECK(parseWsCommand("fan", "high").intVal == 2);
    CHECK(parseWsCommand("fan", "1").intVal    == 3);   // level 1 -> 3
    CHECK(parseWsCommand("fan", "10").intVal   == 12);  // level 10 -> 12

    auto bad = parseWsCommand("fan", "banana");
    CHECK(bad.kind == WsCmdKind::Fan);
    CHECK(bad.valid == false);   // unknown value rejected, dispatcher skips it

    CHECK(parseWsCommand("fan", "11").valid == false);  // out of 1..10 range
}

TEST_CASE("boiler maps via mode_controller and validates", "[ws_command]") {
    CHECK(parseWsCommand("boiler", "off").intVal   == 0);
    CHECK(parseWsCommand("boiler", "eco").intVal   == 1);
    CHECK(parseWsCommand("boiler", "high").intVal  == 2);
    CHECK(parseWsCommand("boiler", "boost").intVal == 3);
    CHECK(parseWsCommand("boiler", "boost").valid  == true);

    auto bad = parseWsCommand("boiler", "nope");
    CHECK(bad.kind == WsCmdKind::Boiler);
    CHECK(bad.valid == false);
}

TEST_CASE("temp parses float best-effort", "[ws_command]") {
    auto t = parseWsCommand("temp", "21.5");
    CHECK(t.kind == WsCmdKind::Temp);
    CHECK(t.valid == true);
    CHECK(t.floatVal == Catch::Approx(21.5f));

    CHECK(parseWsCommand("temp", "18").floatVal == Catch::Approx(18.0f));
    // garbage parses to 0.0 (matches legacy strtof behavior) but is still "valid"
    CHECK(parseWsCommand("temp", "abc").floatVal == Catch::Approx(0.0f));
    CHECK(parseWsCommand("temp", "abc").valid == true);
}

TEST_CASE("energy_idx parses int best-effort", "[ws_command]") {
    CHECK(parseWsCommand("energy_idx", "0").intVal == 0);
    CHECK(parseWsCommand("energy_idx", "4").intVal == 4);
    CHECK(parseWsCommand("energy_idx", "2").kind == WsCmdKind::EnergyIdx);
    CHECK(parseWsCommand("energy_idx", "2").valid == true);
}

TEST_CASE("OTA action commands carry no value", "[ws_command]") {
    CHECK(parseWsCommand("ota_check", "").kind   == WsCmdKind::OtaCheck);
    CHECK(parseWsCommand("ota_install", "").kind == WsCmdKind::OtaInstall);
    CHECK(parseWsCommand("ota_cancel", "").kind  == WsCmdKind::OtaCancel);
    CHECK(parseWsCommand("ota_check", "").valid  == true);
}

TEST_CASE("unrecognised id is Unknown", "[ws_command]") {
    CHECK(parseWsCommand("bogus", "x").kind == WsCmdKind::Unknown);
    CHECK(parseWsCommand("/also_bogus", "1").kind == WsCmdKind::Unknown);
    CHECK(parseWsCommand("bogus", "x").valid == false);
}
