#include "io.h"
#include <cmath>
#include <ctime>
#include <iomanip>
#include <map>
#include <sstream>

namespace cs2 {
namespace {
std::string display(const Json& value, int precision = 1) {
    if (value.is_null()) return "N/A";
    if (value.is_string()) return html_escape(value.get<std::string>());
    if (value.is_number_float()) {
        std::ostringstream output;
        output << std::fixed << std::setprecision(precision) << value.get<double>();
        return output.str();
    }
    return html_escape(value.dump());
}

std::string card(const std::string& title, const Json& value, const std::string& unit = "",
                 int precision = 1) {
    return "<div class=\"card\"><strong>" + display(value, precision) +
        (value.is_null() ? "" : unit) + "</strong><span>" + title + "</span></div>";
}

std::string generated_at() {
    const auto now = std::time(nullptr);
    std::tm time{};
    if (gmtime_s(&time, &now) != 0) throw std::runtime_error("Cannot determine report time");
    std::ostringstream output;
    output << std::put_time(&time, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string bar_chart(const std::vector<std::pair<std::string, int>>& values) {
    const int height = (std::max)(90, static_cast<int>(values.size()) * 32 + 30);
    int maximum = 1;
    for (const auto& value : values) maximum = (std::max)(maximum, value.second);
    std::ostringstream chart;
    chart << "<svg viewBox=\"0 0 540 " << height
          << "\" role=\"img\" aria-label=\"Bar chart\">";
    int row = 20;
    for (const auto& value : values) {
        const int width = 300 * value.second / maximum;
        chart << "<text x=\"0\" y=\"" << row + 15 << "\">" << html_escape(value.first)
              << "</text><rect x=\"155\" y=\"" << row << "\" width=\"" << width
              << "\" height=\"20\" rx=\"3\"><title>" << html_escape(value.first) << ": "
              << value.second << "</title></rect><text x=\"" << 165 + width << "\" y=\""
              << row + 15 << "\">" << value.second << "</text>";
        row += 32;
    }
    chart << "</svg>";
    return chart.str();
}

std::string html_report(const Report& report, const Json& summary, const Json& weapons,
                        const std::string& at) {
    std::ostringstream page;
    page << R"(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; base-uri 'none'; form-action 'none'">
<title>CS2 Analysis - )" << html_escape(report.name) << R"(</title>
<style>
:root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:#18191d;color:#eee;font:15px "Segoe UI",sans-serif}
main{max-width:1240px;margin:auto;padding:24px}header{padding:28px;border-radius:12px;background:linear-gradient(130deg,#536bd2,#764a9d);text-align:center}
h1{margin:0 0 12px}h2{font-size:20px;margin:0 0 18px}h3{font-size:16px}p{line-height:1.6}.badge{display:inline-block;padding:8px 14px;border-radius:20px;background:#252c43;margin:8px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:16px;margin:24px 0}.card,section{background:#292b32;border-radius:10px;padding:20px}
.card{border-left:4px solid #8b91ff}.card strong{display:block;color:#a4aaff;font-size:30px}.card span{display:block;color:#ccc;margin-top:8px}
section{margin-bottom:20px}.charts{display:grid;grid-template-columns:1fr 1fr;gap:20px}svg{width:100%;height:auto}svg rect{fill:#8b91ff}svg text{fill:#ddd;font-size:13px}
.scroll{overflow:auto}table{border-collapse:collapse;width:100%;white-space:nowrap}th,td{text-align:left;padding:10px;border-bottom:1px solid #444}
th{color:#b1b8ff}summary{cursor:pointer;font-weight:600;padding:8px 0}.note{border-left:4px solid #dca858;background:#302e29;padding:14px}
.muted,footer{color:#aaa}input{width:100%;background:#18191d;color:#fff;border:1px solid #666;border-radius:6px;padding:10px;margin:8px 0 16px}
footer{text-align:center;padding:20px}@media(max-width:760px){main{padding:12px}.charts{grid-template-columns:1fr}h1{font-size:25px}}
@media print{body{background:white;color:black}.card,section{background:#eee}header{background:#ddd}input{display:none}details{display:block}td,th{color:black}}
</style></head><body><main><header><h1>CS2 Demo Analysis</h1><p>)";
    page << html_escape(report.name) << " | SteamID64: " << html_escape(report.steam_id)
         << " | Platform: " << html_escape(report.platform)
         << " | Supplied ELO: " << (report.elo == 0 ? "not supplied" : std::to_string(report.elo));
    page << R"(</p><span class="badge">CHEATING NOT ASSESSED</span></header><div class="grid">)";
    page << card("Enemy kills", summary["kills"]) << card("Deaths", summary["deaths"])
         << card("Assists", summary["assists"]) << card("K/D", summary["kd"], "", 3)
         << card("Kills per observed round", summary["kpr"], "", 3)
         << card("Headshot kills", summary["headshot_percent"], "%")
         << card("Damage per observed round", summary["adr"])
         << card("Multi-kill rounds / observed rounds", summary["multi_kill_round_percent"], "%")
         << card("Average damage-to-kill interval", summary["average_damage_to_kill_ms"], " ms")
         << card("Timing samples / enemy kills", summary["timing_coverage_percent"], "%")
         << card("Match wins / resolved matches", summary["match_win_percent"], "%")
         << card("Round wins / observed rounds", summary["round_win_percent"], "%")
         << card("Won rounds with kills / rounds with kills", summary["kill_round_win_percent"], "%")
         << card("Opening kills / opening involvements", summary["opening_duel_win_percent"], "%");
    page << "</div><section class=\"note\"><h2>Measurement, not a verdict</h2><p>"
         << summary["matches"] << " analyzed matches; " << summary["rounds"]
         << " player-round economy snapshots; " << summary["timing_samples"] << " timing samples out of "
         << summary["kills"] << " enemy kills. "
         << "Timing measures first matched damage to the kill, at demo tick resolution. "
         << "It is not reaction time, first visibility, shot count, or proof of aim assistance. "
         << "Single-hit kills naturally produce 0 ms. Platform and ELO are supplied metadata, "
         << "not a verified rank or a calibrated comparison group.</p></section>";
    std::vector<std::pair<std::string, int>> weapon_bars;
    for (std::size_t index = 0; index < weapons.size() && index < 8; ++index) {
        weapon_bars.emplace_back(weapons[index]["weapon"].get<std::string>(),
            weapons[index]["kills"].get<int>());
    }
    std::map<int, int> histogram;
    for (const auto& event : report.events) {
        if (!event["damage_to_kill_ms"].is_null()) {
            ++histogram[static_cast<int>(std::floor(event["damage_to_kill_ms"].get<double>() / 100))];
        }
    }
    std::vector<std::pair<std::string, int>> timing_bars;
    for (const auto& value : histogram) {
        timing_bars.emplace_back(std::to_string(value.first * 100) + "-" +
            std::to_string(value.first * 100 + 99) + " ms", value.second);
    }
    page << "<div class=\"charts\"><section><h2>Top weapons by kills</h2>"
         << bar_chart(weapon_bars) << "</section><section><h2>Damage-to-kill distribution</h2>"
         << bar_chart(timing_bars) << "</section></div>";
    page << "<section><h2>Weapon performance</h2><div class=\"scroll\"><table>"
         << "<thead><tr><th>Weapon</th><th>Kills</th><th>HS%</th><th>Timing samples</th>"
         << "<th>Mean damage-to-kill ms</th></tr></thead><tbody>";
    for (const auto& weapon : weapons) {
        page << "<tr><td>" << display(weapon["weapon"]) << "</td><td>" << weapon["kills"]
             << "</td><td>" << display(weapon["headshot_percent"]) << "</td><td>"
             << weapon["timing_samples"] << "</td><td>"
             << display(weapon["average_damage_to_kill_ms"]) << "</td></tr>";
    }
    page << "</tbody></table></div></section><section><h2>Matches</h2>"
         << "<div class=\"scroll\"><table><thead><tr><th>ID</th><th>Demo</th><th>Map</th>"
         << "<th>Source</th><th>Rounds</th><th>K</th><th>D</th><th>A</th><th>KPR</th>"
         << "<th>ADR</th><th>Outcome</th></tr></thead><tbody>";
    for (const auto& match : report.matches) {
        page << "<tr>";
        for (const char* key : {"match_id", "demo_name", "map", "source", "rounds",
                               "kills", "deaths", "assists", "kpr", "adr", "match_outcome"}) {
            page << "<td>" << display(match.at(key), 3) << "</td>";
        }
        page << "</tr>";
    }
    page << "</tbody></table></div></section><section><h2>Review markers</h2><p>"
         << summary["short_damage_sequences"]
         << " firearm kills have at most two positive damage events and a measured interval "
         << "of at most 200 ms. This includes ordinary one-shot kills. These are navigation markers, "
         << "not suspicion scores; damage events are not bullets.</p><details><summary>Show marked events</summary>"
         << "<div class=\"scroll\"><table><thead><tr><th>Match</th><th>Round</th><th>Tick</th>"
         << "<th>Weapon</th><th>Victim</th><th>Damage events</th><th>Interval ms</th></tr></thead><tbody>";
    for (const auto& event : report.events) {
        if (!event.at("short_damage_sequence").get<bool>()) continue;
        page << "<tr>";
        for (const char* key : {"match_id", "roundNumber", "tick", "weaponName",
                               "victimName", "damage_event_count", "damage_to_kill_ms"}) {
            page << "<td>" << display(event.at(key)) << "</td>";
        }
        page << "</tr>";
    }
    page << "</tbody></table></div></details></section><section><h2>Data quality and provenance</h2>";
    if (report.warnings.empty()) page << "<p>No reported import or reconciliation warnings.</p>";
    for (const auto& warning : report.warnings) {
        page << "<p class=\"note\">" << display(warning["match_id"]) << ": "
             << display(warning["message"]) << "</p>";
    }
    page << "<p class=\"muted\">manifest.json records input SHA-256 hashes, decoder provenance where "
         << "supplied, duplicate decisions and output hashes. CSDA scoreboard fields are preserved in "
         << "analysis.json for reconciliation. No external assets or network calls are required.</p></section>"
         << "<section><details><summary>Target-player kill/death/assist events</summary>"
         << "<label for=\"filter\">Filter events</label><input id=\"filter\" type=\"search\" placeholder=\"Match, weapon, victim, tick...\">"
         << "<div class=\"scroll\"><table id=\"events\"><thead><tr><th>Match</th><th>Round</th><th>Tick</th>"
         << "<th>Weapon</th><th>Killer</th><th>Victim</th><th>Target kill</th><th>Target death</th>"
         << "<th>Interval ms</th><th>Timing status</th></tr></thead><tbody>";
    for (const auto& event : report.events) {
        page << "<tr>";
        for (const char* key : {"match_id", "roundNumber", "tick", "weaponName", "killerName",
                               "victimName", "target_kill", "target_death", "damage_to_kill_ms", "timing_reason"}) {
            page << "<td>" << display(event.at(key)) << "</td>";
        }
        page << "</tr>";
    }
    page << R"(</tbody></table></div></details></section><footer>Generated )"
         << at << R"( | CS2 Analyzer 0.1.0 | Offline report</footer></main>
<script>document.getElementById('filter').addEventListener('input',function(){
const query=this.value.toLowerCase();
document.querySelectorAll('#events tbody tr').forEach(row=>{row.hidden=!row.textContent.toLowerCase().includes(query);});
});</script></body></html>)";
    return page.str();
}
}

void write_report(const Report& report, const std::filesystem::path& directory,
                  const std::string& prefix) {
    const auto summary = summarize(report);
    const auto weapons = weapon_statistics(report);
    const auto at = generated_at();
    write_csv(directory / (prefix + "-kills.csv"), report.events,
        {"match_id", "roundNumber", "tick", "weaponName", "killerSteamId", "victimSteamId",
         "target_kill", "target_death", "damage_to_kill_ms", "timing_reason"});
    write_csv(directory / (prefix + "-byfile.csv"), report.matches,
        {"match_id", "demo_name", "map", "source", "kills", "deaths", "assists", "rounds", "kpr", "adr"});
    Json global = summary;
    global["steam_id"] = report.steam_id;
    global["player_name"] = report.name;
    global["supplied_platform"] = report.platform;
    global["supplied_elo"] = report.elo == 0 ? Json(nullptr) : Json(report.elo);
    write_csv(directory / (prefix + "-global.csv"), Json::array({global}));
    write_csv(directory / (prefix + "-rounds.csv"), report.rounds,
        {"match_id", "round", "side", "won", "kills", "overtime_number"});
    write_csv(directory / (prefix + "-weapons.csv"), weapons);
    write_text(directory / "analysis.json", Json{
        {"schema_version", 1}, {"analyzer_version", "0.1.0"}, {"generated_at", at},
        {"metadata", {{"steam_id", report.steam_id}, {"name", report.name},
            {"platform", report.platform}, {"elo", report.elo}, {"rank_verified", false}}},
        {"summary", summary}, {"matches", report.matches}, {"weapons", weapons},
        {"warnings", report.warnings}
    }.dump(2) + "\n");
    std::ostringstream note;
    note << "CS2 Analyzer 0.1.0\nGenerated: " << at << "\nTarget: " << report.name
         << "\nSteamID64: " << report.steam_id << "\nCheating verdict: NOT ASSESSED\n"
         << "Timing: first matched positive damage to kill; not reaction time.\n\n";
    for (auto field = global.begin(); field != global.end(); ++field) {
        note << field.key() << ": " << (field->is_null() ? "N/A" : field->dump()) << "\n";
    }
    write_text(directory / (prefix + "-summary.txt"), note.str());
    write_text(directory / (prefix + "-report.html"), html_report(report, summary, weapons, at));
    Json outputs = Json::array();
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        outputs.push_back({{"path", entry.path().lexically_relative(directory).generic_u8string()},
            {"sha256", sha256_file(entry.path())}, {"bytes", entry.file_size()}});
    }
    write_text(directory / "manifest.json", Json{
        {"schema_version", 1}, {"analyzer_version", "0.1.0"}, {"generated_at", at},
        {"status", "complete"}, {"inputs", report.inputs}, {"outputs", outputs},
        {"timing_method", "same-tick terminal damage; same attacker/victim/weapon/round; 2s inactivity reset; tick resolution"},
        {"round_method", "target player economy snapshots in completed live rounds"},
        {"review_marker", "firearm kill; 1-2 positive damage events; damage-to-kill <=200ms; no cheating inference"}
    }.dump(2) + "\n");
}
}
