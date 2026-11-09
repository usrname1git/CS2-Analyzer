#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace cs2 {
using Json = nlohmann::json;

struct Report {
    std::string steam_id;
    std::string name;
    std::string platform;
    int elo = 0;
    std::vector<Json> matches;
    std::vector<Json> events;
    std::vector<Json> rounds;
    std::vector<Json> warnings;
    std::vector<Json> inputs;
};

std::uint64_t parse_steam_id(const std::string& text);
Json analyze_match(const Json& match, const std::string& steam_id,
                   const std::string& match_id, Report& report);
Json summarize(const Report& report);
Json weapon_statistics(const Report& report);
Json canonical_core(const Json& match);
}
