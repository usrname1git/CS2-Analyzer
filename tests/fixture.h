#pragma once
#include "analysis.h"

namespace fixture {
inline constexpr const char* target = "76561198000000001";
inline constexpr std::uint64_t target_number = 76561198000000001ULL;
inline constexpr std::uint64_t opponent = 76561198000000002ULL;

inline cs2::Json kill(int round, int tick, std::uint64_t killer, std::uint64_t victim,
                     int side, int other_side, bool headshot = false) {
    return {{"roundNumber", round}, {"tick", tick}, {"weaponName", "AK-47"},
        {"weaponType", "rifle"}, {"killerSteamId", killer}, {"victimSteamId", victim},
        {"killerSide", side}, {"victimSide", other_side}, {"killerName", "Player"},
        {"victimName", "Opponent"}, {"assisterSteamId", 0}, {"assisterSide", 0},
        {"isHeadshot", headshot}, {"isKillerControllingBot", false},
        {"isVictimControllingBot", false}, {"isAssisterControllingBot", false},
        {"isTradeKill", false}, {"isTradeDeath", false}};
}

inline cs2::Json damage(int round, int tick, std::uint64_t victim, int amount) {
    return {{"roundNumber", round}, {"tick", tick}, {"attackerSteamId", target_number},
        {"victimSteamId", victim}, {"attackerSide", 2}, {"victimSide", 3},
        {"healthDamage", amount}, {"weaponType", "rifle"}, {"weaponName", "AK-47"},
        {"isAttackerControllingBot", false}};
}

inline cs2::Json match() {
    using cs2::Json;
    return {{"checksum", "synthetic-match-1"}, {"game", "CS2"}, {"mapName", "de_fixture"},
        {"source", "faceit"}, {"demoFileName", "synthetic.dem"}, {"tickrate", 64},
        {"winner", {{"letter", "A"}}},
        {"players", {{target, {{"steamId", target_number}, {"name", "Player"},
            {"killCount", 2}, {"deathCount", 1}, {"assistCount", 0}, {"headshotCount", 1},
            {"healthDamage", 200}, {"kast", 66.6667}}}}},
        {"rounds", Json::array({
            {{"number", 1}, {"teamASide", 2}, {"teamBSide", 3}, {"winnerSide", 2}, {"overtimeNumber", 0}},
            {{"number", 2}, {"teamASide", 3}, {"teamBSide", 2}, {"winnerSide", 2}, {"overtimeNumber", 0}},
            {{"number", 3}, {"teamASide", 3}, {"teamBSide", 2}, {"winnerSide", 3}, {"overtimeNumber", 1}}
        })},
        {"playerEconomies", Json::array({
            {{"steamId", target_number}, {"roundNumber", 1}, {"playerSide", 2}},
            {{"steamId", target_number}, {"roundNumber", 2}, {"playerSide", 3}},
            {{"steamId", target_number}, {"roundNumber", 3}, {"playerSide", 3}}
        })},
        {"kills", Json::array({kill(1, 20, target_number, opponent, 2, 3),
            kill(1, 30, target_number, opponent + 1, 2, 3, true),
            kill(2, 100, opponent, target_number, 2, 3)})},
        {"damages", Json::array({damage(1, 10, opponent, 40), damage(1, 20, opponent, 60),
            damage(1, 30, opponent + 1, 100)})},
        {"shots", Json::array()}, {"bombsPlanted", Json::array()}};
}
}
