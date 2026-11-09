#include "analysis.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace cs2 {
namespace {
const Json& required(const Json& value, const char* key) {
    if (!value.is_object() || !value.contains(key)) {
        throw std::runtime_error(std::string("Missing CSDA field: ") + key);
    }
    return value.at(key);
}

int number(const Json& value, const char* key) {
    const auto& field = required(value, key);
    if (!field.is_number_integer() || field.get<double>() < 0 ||
        field.get<double>() > std::numeric_limits<int>::max()) {
        throw std::runtime_error(std::string("Invalid non-negative integer: ") + key);
    }
    return field.get<int>();
}

std::string text(const Json& value, const char* key) {
    const auto& field = required(value, key);
    if (!field.is_string()) throw std::runtime_error(std::string("Invalid string: ") + key);
    return field.get<std::string>();
}

bool flag(const Json& value, const char* key) {
    if (!value.contains(key)) return false;
    if (!value.at(key).is_boolean()) {
        throw std::runtime_error(std::string("Invalid boolean: ") + key);
    }
    return value.at(key).get<bool>();
}

std::string id(const Json& value, const char* key) {
    const auto& field = required(value, key);
    if (field.is_string()) {
        const auto result = field.get<std::string>();
        if (result == "0") return result;
        parse_steam_id(result);
        return result;
    }
    if (field.is_number_unsigned()) return std::to_string(field.get<std::uint64_t>());
    if (field.is_number_integer() && field.get<std::int64_t>() >= 0) {
        return std::to_string(field.get<std::int64_t>());
    }
    throw std::runtime_error(std::string("SteamID must be an exact integer or decimal string: ") + key);
}

const Json& table(const Json& value, const char* key) {
    const auto& result = required(value, key);
    if (!result.is_array()) throw std::runtime_error(std::string("Invalid CSDA event table: ") + key);
    for (const auto& row : result) {
        if (!row.is_object()) throw std::runtime_error(std::string("Invalid row in: ") + key);
    }
    return result;
}

bool combat_side(int side) { return side == 2 || side == 3; }

bool enemy_kill(const Json& kill) {
    const auto killer = id(kill, "killerSteamId");
    const auto victim = id(kill, "victimSteamId");
    const int side = number(kill, "killerSide");
    const int other_side = number(kill, "victimSide");
    return killer != "0" && victim != "0" && killer != victim &&
        combat_side(side) && combat_side(other_side) && side != other_side;
}

bool firearm(const Json& kill) {
    static const std::set<std::string> types{
        "pistol", "smg", "shotgun", "rifle", "sniper", "machine_gun"};
    return types.count(text(kill, "weaponType")) != 0;
}

Json ratio(std::int64_t numerator, std::int64_t denominator, double scale = 1.0) {
    if (denominator == 0) return nullptr;
    return scale * static_cast<double>(numerator) / static_cast<double>(denominator);
}

void warning(Report& report, const std::string& match_id, const std::string& message) {
    report.warnings.push_back({{"match_id", match_id}, {"message", message}});
}

Json timing(const Json& kill, const std::vector<const Json*>& damages, double tickrate) {
    Json result{{"damage_to_kill_ms", nullptr}, {"damage_event_count", nullptr},
                {"first_damage_tick", nullptr}, {"short_damage_sequence", false},
                {"timing_reason", "not_a_firearm_kill"}};
    if (!firearm(kill)) return result;
    if (tickrate <= 0) {
        result["timing_reason"] = "tickrate_unavailable";
        return result;
    }
    const int kill_tick = number(kill, "tick");
    const auto weapon = text(kill, "weaponName");
    const auto victim = id(kill, "victimSteamId");
    const int round = number(kill, "roundNumber");
    int first = kill_tick;
    int last = kill_tick;
    int count = 0;
    for (auto entry = damages.rbegin(); entry != damages.rend(); ++entry) {
        const auto& damage = **entry;
        const int tick = number(damage, "tick");
        if (tick > kill_tick || number(damage, "roundNumber") != round ||
            id(damage, "victimSteamId") != victim) continue;
        if (count == 0 && tick != kill_tick) {
            result["timing_reason"] = "no_same_tick_terminal_damage";
            return result;
        }
        if (text(damage, "weaponName") != weapon ||
            static_cast<double>(last - tick) / tickrate > 2.0) break;
        first = tick;
        last = tick;
        ++count;
    }
    if (count == 0) {
        result["timing_reason"] = "no_same_tick_terminal_damage";
        return result;
    }
    const double elapsed = 1000.0 * static_cast<double>(kill_tick - first) / tickrate;
    result["damage_to_kill_ms"] = elapsed;
    result["damage_event_count"] = count;
    result["first_damage_tick"] = first;
    result["short_damage_sequence"] = count <= 2 && elapsed <= 200.0;
    result["timing_reason"] = "measured_at_demo_tick_resolution";
    return result;
}
}

std::uint64_t parse_steam_id(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos ||
        value.front() == '0') {
        throw std::runtime_error("SteamID64 must be a nonzero decimal integer without leading zeros");
    }
    std::size_t consumed = 0;
    const auto result = std::stoull(value, &consumed);
    if (consumed != value.size()) throw std::runtime_error("Invalid SteamID64");
    return result;
}

Json analyze_match(const Json& match, const std::string& steam_id,
                   const std::string& match_id, Report& report) {
    parse_steam_id(steam_id);
    if (text(match, "game") != "CS2") throw std::runtime_error("Only CS2 demos are supported");
    if (text(match, "checksum").empty()) throw std::runtime_error("CSDA checksum is empty");
    const auto& players = required(match, "players");
    if (!players.is_object()) throw std::runtime_error("CSDA players must be a SteamID-keyed object");
    if (!players.contains(steam_id)) return nullptr;
    const auto& player = players.at(steam_id);
    if (id(player, "steamId") != steam_id) throw std::runtime_error("Player key and SteamID disagree");
    const auto& round_table = table(match, "rounds");
    const auto& kill_table = table(match, "kills");
    const auto& damage_table = table(match, "damages");
    const auto& shots = table(match, "shots");
    const auto& economies = table(match, "playerEconomies");
    std::map<int, const Json*> live_rounds;
    for (const auto& round : round_table) {
        const int round_number = number(round, "number");
        if (round_number == 0 || !live_rounds.emplace(round_number, &round).second) {
            throw std::runtime_error("Duplicate or zero round number in CSDA export");
        }
        if (!combat_side(number(round, "teamASide")) ||
            !combat_side(number(round, "teamBSide")) ||
            number(round, "teamASide") == number(round, "teamBSide") ||
            !combat_side(number(round, "winnerSide"))) {
            throw std::runtime_error("Invalid round side or winner");
        }
    }
    if (live_rounds.empty()) throw std::runtime_error("No completed live rounds in CSDA export");
    double tickrate = 0;
    const auto& rate = required(match, "tickrate");
    if (!rate.is_number() || !std::isfinite(rate.get<double>()) || rate.get<double>() < 0) {
        throw std::runtime_error("Invalid demo tickrate");
    }
    tickrate = rate.get<double>();
    if (tickrate == 0) warning(report, match_id, "Tickrate unavailable; timing is not measured.");

    std::map<int, int> player_rounds;
    std::set<std::string> player_teams;
    for (const auto& economy : economies) {
        if (id(economy, "steamId") != steam_id) continue;
        const int round_number = number(economy, "roundNumber");
        const int side = number(economy, "playerSide");
        if (!live_rounds.count(round_number) || !combat_side(side) ||
            !player_rounds.emplace(round_number, side).second) {
            throw std::runtime_error("Invalid or duplicate target-player economy snapshot");
        }
        const auto& round = *live_rounds.at(round_number);
        player_teams.insert(side == number(round, "teamASide") ? "A" : "B");
    }
    Json stats{
        {"match_id", match_id}, {"checksum", text(match, "checksum")},
        {"map", text(match, "mapName")}, {"source", text(match, "source")},
        {"player_name", text(player, "name")}, {"steam_id", steam_id},
        {"demo_name", text(match, "demoFileName")}, {"tickrate", tickrate},
        {"kills", 0}, {"deaths", 0}, {"assists", 0}, {"headshots", 0},
        {"damage", 0}, {"utility_damage", 0}, {"opening_kills", 0}, {"opening_deaths", 0},
        {"trade_kills", 0}, {"trade_deaths", 0}, {"rounds", player_rounds.size()},
        {"rounds_with_kills", 0}, {"rounds_with_kills_won", 0},
        {"multi_kill_rounds", 0}, {"rounds_won", 0},
        {"timing_samples", 0}, {"short_damage_sequences", 0},
        {"timing_sum_ms", 0.0}, {"round_denominator_complete", !player_rounds.empty()},
        {"match_outcome", "unresolved"}, {"csda_scoreboard", Json::object()}
    };
    for (const char* key : {"killCount", "deathCount", "assistCount", "headshotCount",
                           "healthDamage", "kast", "hltvRating", "hltvRating2"}) {
        if (player.contains(key)) stats["csda_scoreboard"][key] = player.at(key);
    }
    const auto add = [&stats](const char* key, std::int64_t amount = 1) {
        stats[key] = stats.at(key).get<std::int64_t>() + amount;
    };
    bool complete = !player_rounds.empty();
    for (const auto& shot : shots) {
        const int round = number(shot, "roundNumber");
        number(shot, "tick");
        if (!live_rounds.count(round)) throw std::runtime_error("Shot references an unknown round");
        if (id(shot, "playerSteamId") == steam_id &&
            !flag(shot, "isPlayerControllingBot") && !player_rounds.count(round)) {
            complete = false;
        }
    }
    std::vector<const Json*> damages;
    for (const auto& damage : damage_table) {
        const int round = number(damage, "roundNumber");
        number(damage, "tick");
        if (!live_rounds.count(round)) throw std::runtime_error("Damage references an unknown round");
        if (id(damage, "attackerSteamId") != steam_id ||
            id(damage, "victimSteamId") == steam_id ||
            id(damage, "victimSteamId") == "0" ||
            flag(damage, "isAttackerControllingBot") ||
            !combat_side(number(damage, "attackerSide")) ||
            !combat_side(number(damage, "victimSide")) ||
            number(damage, "attackerSide") == number(damage, "victimSide")) continue;
        if (!player_rounds.count(round)) complete = false;
        const int amount = number(damage, "healthDamage");
        add("damage", amount);
        if (text(damage, "weaponType") == "grenade") add("utility_damage", amount);
        if (amount > 0) damages.push_back(&damage);
    }
    std::stable_sort(damages.begin(), damages.end(), [](const Json* left, const Json* right) {
        return number(*left, "tick") < number(*right, "tick");
    });
    std::vector<const Json*> kills;
    for (const auto& kill : kill_table) {
        if (!live_rounds.count(number(kill, "roundNumber"))) {
            throw std::runtime_error("Kill references an unknown round");
        }
        number(kill, "tick");
        kills.push_back(&kill);
    }
    std::stable_sort(kills.begin(), kills.end(), [](const Json* left, const Json* right) {
        return number(*left, "tick") < number(*right, "tick");
    });
    std::map<int, int> round_kills;
    std::set<int> opening_seen;
    for (const auto* entry : kills) {
        const auto& kill = *entry;
        const int round = number(kill, "roundNumber");
        const auto killer = id(kill, "killerSteamId");
        const auto victim = id(kill, "victimSteamId");
        const bool enemy = enemy_kill(kill);
        const bool target_kill = killer == steam_id && enemy &&
            !flag(kill, "isKillerControllingBot");
        const bool disconnect = killer == victim && text(kill, "weaponName") == "World";
        const bool target_death = victim == steam_id && !disconnect &&
            !flag(kill, "isVictimControllingBot");
        const bool target_assist = id(kill, "assisterSteamId") == steam_id &&
            !flag(kill, "isAssisterControllingBot") &&
            number(kill, "assisterSide") != number(kill, "victimSide");
        if (enemy && !flag(kill, "isKillerControllingBot") &&
            !flag(kill, "isVictimControllingBot") && opening_seen.insert(round).second) {
            if (target_kill) add("opening_kills");
            if (target_death) add("opening_deaths");
        }
        if (!target_kill && !target_death && !target_assist) continue;
        if (!player_rounds.count(round)) complete = false;
        if (target_kill) {
            add("kills");
            ++round_kills[round];
            if (flag(kill, "isHeadshot")) add("headshots");
            if (flag(kill, "isTradeKill")) add("trade_kills");
        }
        if (target_death) {
            add("deaths");
            if (enemy && flag(kill, "isTradeDeath")) add("trade_deaths");
        }
        if (target_assist) add("assists");
        Json row = kill;
        row["match_id"] = match_id;
        row["target_kill"] = target_kill;
        row["target_death"] = target_death;
        row["target_assist"] = target_assist;
        for (const char* key : {"killerSteamId", "victimSteamId", "assisterSteamId"}) {
            row[key] = id(kill, key);
        }
        const Json measurement = target_kill ? timing(kill, damages, tickrate) : Json{
            {"damage_to_kill_ms", nullptr}, {"damage_event_count", nullptr},
            {"first_damage_tick", nullptr}, {"short_damage_sequence", false},
            {"timing_reason", "not_a_target_enemy_kill"}};
        row.update(measurement);
        if (!measurement.at("damage_to_kill_ms").is_null()) {
            add("timing_samples");
            stats["timing_sum_ms"] = stats.at("timing_sum_ms").get<double>() +
                measurement.at("damage_to_kill_ms").get<double>();
            if (measurement.at("short_damage_sequence").get<bool>()) add("short_damage_sequences");
        }
        report.events.push_back(std::move(row));
    }
    for (const auto& entry : player_rounds) {
        const auto& round = *live_rounds.at(entry.first);
        const bool won = entry.second == number(round, "winnerSide");
        const int count = round_kills[entry.first];
        if (won) add("rounds_won");
        if (count > 0) {
            add("rounds_with_kills");
            if (won) add("rounds_with_kills_won");
        }
        if (count >= 2) add("multi_kill_rounds");
        report.rounds.push_back({{"match_id", match_id}, {"round", entry.first},
            {"side", entry.second}, {"won", won}, {"kills", count},
            {"overtime_number", number(round, "overtimeNumber")}});
    }
    if (!complete) {
        warning(report, match_id,
            "Player economy snapshots do not cover recorded activity; round-based rates are unavailable.");
    }
    stats["round_denominator_complete"] = complete;
    stats["kd"] = ratio(stats["kills"], stats["deaths"]);
    stats["headshot_percent"] = ratio(stats["headshots"], stats["kills"], 100);
    stats["kpr"] = complete ? ratio(stats["kills"], stats["rounds"]) : Json(nullptr);
    stats["adr"] = complete ? ratio(stats["damage"], stats["rounds"]) : Json(nullptr);
    stats["multi_kill_round_percent"] = complete ?
        ratio(stats["multi_kill_rounds"], stats["rounds"], 100) : Json(nullptr);
    stats["opening_duel_win_percent"] = ratio(stats["opening_kills"],
        stats["opening_kills"].get<int>() + stats["opening_deaths"].get<int>(), 100);
    stats["round_win_percent"] = complete ?
        ratio(stats["rounds_won"], stats["rounds"], 100) : Json(nullptr);
    stats["kill_round_win_percent"] = complete ?
        ratio(stats["rounds_with_kills_won"], stats["rounds_with_kills"], 100) : Json(nullptr);
    stats["average_damage_to_kill_ms"] = stats["timing_samples"].get<int>() == 0 ?
        Json(nullptr) : Json(stats["timing_sum_ms"].get<double>() / stats["timing_samples"].get<int>());
    stats["timing_coverage_percent"] = ratio(stats["timing_samples"], stats["kills"], 100);
    if (complete && player_teams.size() == 1 && match.contains("winner") &&
        match["winner"].is_object()) {
        const auto winner = text(match["winner"], "letter");
        if (winner != "A" && winner != "B") throw std::runtime_error("Invalid match winner letter");
        stats["match_outcome"] = winner == *player_teams.begin() ? "win" : "loss";
    }
    for (const auto& pair : std::vector<std::pair<const char*, const char*>>{
        {"kills", "killCount"}, {"deaths", "deathCount"}, {"assists", "assistCount"},
        {"headshots", "headshotCount"}, {"damage", "healthDamage"}}) {
        if (player.contains(pair.second) && stats.at(pair.first) != player.at(pair.second)) {
            warning(report, match_id, std::string("Event-derived ") + pair.first +
                " differs from CSDA scoreboard: " + stats.at(pair.first).dump() +
                " versus " + player.at(pair.second).dump() + ".");
        }
    }
    return stats;
}

Json summarize(const Report& report) {
    Json result{{"matches", report.matches.size()}, {"resolved_matches", 0}, {"matches_won", 0},
        {"round_denominator_complete", true}, {"timing_sum_ms", 0.0}};
    const std::vector<std::string> counts{"kills", "deaths", "assists", "headshots", "damage",
        "utility_damage", "opening_kills", "opening_deaths", "trade_kills", "trade_deaths",
        "rounds", "rounds_with_kills", "rounds_with_kills_won", "multi_kill_rounds",
        "rounds_won", "timing_samples", "short_damage_sequences"};
    for (const auto& key : counts) result[key] = 0;
    for (const auto& match : report.matches) {
        for (const auto& key : counts) {
            result[key] = result[key].get<std::int64_t>() + match.at(key).get<std::int64_t>();
        }
        result["timing_sum_ms"] = result["timing_sum_ms"].get<double>() +
            match.at("timing_sum_ms").get<double>();
        if (!match.at("round_denominator_complete").get<bool>()) {
            result["round_denominator_complete"] = false;
        }
        if (match.at("match_outcome") != "unresolved") {
            result["resolved_matches"] = result["resolved_matches"].get<int>() + 1;
            if (match.at("match_outcome") == "win") {
                result["matches_won"] = result["matches_won"].get<int>() + 1;
            }
        }
    }
    const bool complete = result["round_denominator_complete"].get<bool>();
    result["kd"] = ratio(result["kills"], result["deaths"]);
    result["headshot_percent"] = ratio(result["headshots"], result["kills"], 100);
    result["kpr"] = complete ? ratio(result["kills"], result["rounds"]) : Json(nullptr);
    result["adr"] = complete ? ratio(result["damage"], result["rounds"]) : Json(nullptr);
    result["multi_kill_round_percent"] = complete ?
        ratio(result["multi_kill_rounds"], result["rounds"], 100) : Json(nullptr);
    result["round_win_percent"] = complete ?
        ratio(result["rounds_won"], result["rounds"], 100) : Json(nullptr);
    result["kill_round_win_percent"] = complete ?
        ratio(result["rounds_with_kills_won"], result["rounds_with_kills"], 100) : Json(nullptr);
    result["match_win_percent"] = ratio(result["matches_won"], result["resolved_matches"], 100);
    result["opening_duel_win_percent"] = ratio(result["opening_kills"],
        result["opening_kills"].get<std::int64_t>() + result["opening_deaths"].get<std::int64_t>(), 100);
    result["timing_coverage_percent"] = ratio(result["timing_samples"], result["kills"], 100);
    const auto samples = result["timing_samples"].get<std::int64_t>();
    result["average_damage_to_kill_ms"] = samples == 0 ? Json(nullptr) :
        Json(result["timing_sum_ms"].get<double>() / static_cast<double>(samples));
    result["cheating_verdict"] = "not_assessed";
    return result;
}

Json weapon_statistics(const Report& report) {
    std::map<std::string, Json> weapons;
    for (const auto& event : report.events) {
        if (!event.at("target_kill").get<bool>()) continue;
        const auto name = event.at("weaponName").get<std::string>();
        if (!weapons.count(name)) {
            weapons[name] = {{"weapon", name}, {"kills", 0}, {"headshots", 0},
                {"timing_samples", 0}, {"timing_sum_ms", 0.0}};
        }
        auto& value = weapons[name];
        value["kills"] = value["kills"].get<int>() + 1;
        if (flag(event, "isHeadshot")) value["headshots"] = value["headshots"].get<int>() + 1;
        if (!event["damage_to_kill_ms"].is_null()) {
            value["timing_samples"] = value["timing_samples"].get<int>() + 1;
            value["timing_sum_ms"] = value["timing_sum_ms"].get<double>() +
                event["damage_to_kill_ms"].get<double>();
        }
    }
    std::vector<Json> sorted;
    for (auto& item : weapons) {
        auto& weapon = item.second;
        weapon["headshot_percent"] = ratio(weapon["headshots"], weapon["kills"], 100);
        weapon["average_damage_to_kill_ms"] = weapon["timing_samples"].get<int>() == 0 ?
            Json(nullptr) : Json(weapon["timing_sum_ms"].get<double>() / weapon["timing_samples"].get<int>());
        sorted.push_back(std::move(weapon));
    }
    std::sort(sorted.begin(), sorted.end(), [](const Json& left, const Json& right) {
        if (left.at("kills") == right.at("kills")) return left.at("weapon") < right.at("weapon");
        return left.at("kills") > right.at("kills");
    });
    return sorted;
}

Json canonical_core(const Json& match) {
    Json result;
    for (const char* key : {"game", "mapName", "tickrate", "rounds",
                           "kills", "damages", "shots", "playerEconomies", "players", "winner"}) {
        result[key] = match.at(key);
    }
    for (auto& player : result.at("players")) player.erase("color");
    for (auto& damage : result.at("damages")) damage.erase("weaponUniqueId");
    for (auto& shot : result.at("shots")) {
        shot.erase("weaponId");
        shot.erase("projectileId");
    }
    for (const char* key : {"shots", "playerEconomies"}) {
        auto& rows = result.at(key);
        std::sort(rows.begin(), rows.end());
    }
    return result;
}
}
