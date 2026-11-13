#include "fixture.h"
#include "io.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void near(const cs2::Json& value, double expected, const char* message) {
    expect(value.is_number() && std::abs(value.get<double>() - expected) < 0.0001, message);
}

template<class Function>
void rejects(Function function, const char* message) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    expect(failed, message);
}

cs2::Json analyze(const cs2::Json& value, cs2::Report& report) {
    auto stats = cs2::analyze_match(value, fixture::target, "m1", report);
    report.matches.push_back(stats);
    return stats;
}
}

int main() {
    try {
        cs2::Report report;
        const auto result = analyze(fixture::match(), report);
        expect(result["kills"] == 2 && result["deaths"] == 1 && result["rounds"] == 3, "K/D/round counts");
        near(result["kpr"], 2.0 / 3.0, "Zero-kill rounds must remain in KPR");
        near(result["multi_kill_round_percent"], 100.0 / 3.0, "Multi-kill denominator");
        near(result["headshot_percent"], 50, "Headshot denominator");
        near(result["opening_duel_win_percent"], 50, "Opening duel denominator");
        expect(result["rounds_won"] == 2 && result["match_outcome"] == "win", "Side-switch-aware result");
        expect(result["rounds_with_kills"] == 1 && result["rounds_with_kills_won"] == 1, "Kill-round wins");
        near(report.events[0]["damage_to_kill_ms"], 156.25, "Tickrate-derived interval");
        near(report.events[1]["damage_to_kill_ms"], 0, "One-shot kill has zero damage interval");
        expect(result["timing_samples"] == 2 && result["short_damage_sequences"] == 2, "Measured samples");
        expect(report.warnings.empty(), "Fixture scoreboard must reconcile");
        auto equivalent = fixture::match();
        equivalent["players"][fixture::target]["color"] = 3;
        equivalent["source"] = "esportal";
        equivalent["damages"][0]["weaponUniqueId"] = "random-id";
        std::reverse(equivalent["playerEconomies"].begin(), equivalent["playerEconomies"].end());
        expect(cs2::canonical_core(equivalent) == cs2::canonical_core(fixture::match()),
            "Run-dependent IDs, color and economy order do not create conflicting demos");

        auto second = fixture::match();
        second["checksum"] = "synthetic-match-2";
        report.matches.push_back(cs2::analyze_match(second, fixture::target, "m2", report));
        const auto combined = cs2::summarize(report);
        expect(combined["rounds"] == 6 && report.rounds.size() == 6, "Round numbers are match-scoped");
        expect(combined["cheating_verdict"] == "not_assessed", "No automatic verdict");
        near(combined["multi_kill_round_percent"], 100.0 / 3.0, "Global rate uses sums, not averages");
        const auto weapons = cs2::weapon_statistics(report);
        expect(weapons[0]["kills"] == 4 && weapons[0]["timing_samples"] == 4, "Weapon reconciliation");

        auto unavailable = fixture::match();
        unavailable["playerEconomies"] = cs2::Json::array();
        cs2::Report incomplete;
        const auto missing_rounds = analyze(unavailable, incomplete);
        expect(missing_rounds["kpr"].is_null() && missing_rounds["adr"].is_null(), "Unknown denominator is not zero");
        expect(!incomplete.warnings.empty(), "Missing participation produces warning");
        unavailable = fixture::match();
        unavailable["playerEconomies"].erase(2);
        unavailable["shots"].push_back({{"roundNumber", 3}, {"tick", 250},
            {"playerSteamId", fixture::target_number}, {"isPlayerControllingBot", false}});
        cs2::Report missing_shot_round;
        expect(analyze(unavailable, missing_shot_round)["kpr"].is_null(),
            "Missing participation must include shot-only rounds");
        unavailable = fixture::match();
        unavailable["tickrate"] = 0;
        cs2::Report no_rate;
        const auto no_timing = analyze(unavailable, no_rate);
        expect(no_timing["timing_samples"] == 0 && no_timing["average_damage_to_kill_ms"].is_null(), "Missing tickrate");
        unavailable = fixture::match();
        unavailable["damages"][1]["tick"] = 19;
        cs2::Report no_terminal;
        expect(analyze(unavailable, no_terminal)["timing_samples"] == 1, "Do not invent terminal hit");
        unavailable = fixture::match();
        unavailable["damages"][0]["tick"] = 0;
        unavailable["damages"][1]["tick"] = 200;
        unavailable["kills"][0]["tick"] = 200;
        cs2::Report idle;
        analyze(unavailable, idle);
        for (const auto& event : idle.events) {
            if (event["tick"] == 200) near(event["damage_to_kill_ms"], 0, "Reset stale damage after 2s inactivity");
        }
        unavailable = fixture::match();
        unavailable["kills"][0]["weaponType"] = "grenade";
        cs2::Report utility;
        expect(analyze(unavailable, utility)["timing_samples"] == 1, "Do not treat grenade timing as gun timing");
        unavailable = fixture::match();
        unavailable["kills"][0]["isKillerControllingBot"] = true;
        cs2::Report bot;
        expect(analyze(unavailable, bot)["kills"] == 1, "Exclude controlled-bot kills");
        unavailable = fixture::match();
        unavailable["kills"][0]["victimSide"] = 2;
        cs2::Report teamkill;
        expect(analyze(unavailable, teamkill)["kills"] == 1, "Exclude team kills");
        unavailable = fixture::match();
        unavailable["kills"].push_back(fixture::kill(3, 300, fixture::target_number, fixture::target_number, 3, 3));
        unavailable["kills"].back()["weaponName"] = "World";
        cs2::Report disconnect;
        expect(analyze(unavailable, disconnect)["deaths"] == 1, "CSDA world-disconnect convention");
        unavailable = fixture::match();
        unavailable["players"][fixture::target]["killCount"] = 99;
        cs2::Report mismatch;
        analyze(unavailable, mismatch);
        expect(!mismatch.warnings.empty(), "Scoreboard mismatch must be visible");
        unavailable = fixture::match();
        unavailable["players"] = cs2::Json::object();
        cs2::Report absent;
        expect(cs2::analyze_match(unavailable, fixture::target, "m1", absent).is_null(), "Target absent");

        rejects([] {
            auto invalid = fixture::match();
            invalid["rounds"].push_back(invalid["rounds"][0]);
            cs2::Report temporary;
            analyze(invalid, temporary);
        }, "Duplicate round rejected");
        rejects([] {
            auto invalid = fixture::match();
            invalid["kills"][0]["killerSteamId"] = 76561198000000000.0;
            cs2::Report temporary;
            analyze(invalid, temporary);
        }, "Floating SteamID rejected");
        rejects([] {
            auto invalid = fixture::match();
            invalid["kills"][0]["roundNumber"] = 999;
            cs2::Report temporary;
            analyze(invalid, temporary);
        }, "Unknown round rejected");
        rejects([] { cs2::parse_steam_id("18446744073709551616"); }, "Overflow SteamID rejected");
        expect(cs2::parse_steam_id("18446744073709551615") == UINT64_MAX, "64-bit SteamID exactness");
        expect(cs2::html_escape("<script>\"&'") == "&lt;script&gt;&quot;&amp;&#39;", "HTML escaping");
        expect(cs2::csv_cell("=SUM(1,2)") == "\"'=SUM(1,2)\"", "CSV formula protection");
        expect(cs2::csv_cell("\t@SUM(1)") == "\"'\t@SUM(1)\"", "CSV whitespace formula protection");
        expect(cs2::csv_cell(cs2::Json(UINT64_MAX)) == "\"18446744073709551615\"", "CSV exact integer");
        expect(cs2::sha256_text("abc") ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 known vector");
        std::cout << "Analysis fixtures passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fixture failure: " << error.what() << "\n";
        return 1;
    }
}
