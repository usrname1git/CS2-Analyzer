#include "io.h"
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace {
struct Options {
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path output;
    std::filesystem::path provenance;
    std::string steam_id;
    std::string name = "Player";
    std::string platform = "Unknown";
    std::string prefix = "CS2-Analysis";
    int elo = 0;
};

Options parse(int argc, wchar_t** argv) {
    Options options;
    std::set<std::string> seen;
    for (int index = 1; index < argc; ++index) {
        const auto key = cs2::utf8(argv[index]);
        if (key == "--help" || key == "--version") continue;
        if (index + 1 >= argc) throw std::runtime_error("Missing value for " + key);
        const auto value = cs2::utf8(argv[++index]);
        if (key != "--input" && !seen.insert(key).second) {
            throw std::runtime_error("Duplicate option: " + key);
        }
        if (key == "--input") options.inputs.push_back(std::filesystem::u8path(value));
        else if (key == "--output") options.output = std::filesystem::u8path(value);
        else if (key == "--provenance") options.provenance = std::filesystem::u8path(value);
        else if (key == "--steam-id") options.steam_id = value;
        else if (key == "--name") options.name = value;
        else if (key == "--platform") options.platform = value;
        else if (key == "--prefix") options.prefix = value;
        else if (key == "--elo") {
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
                throw std::runtime_error("ELO must be a non-negative integer");
            }
            options.elo = std::stoi(value);
        } else throw std::runtime_error("Unknown option: " + key);
    }
    cs2::parse_steam_id(options.steam_id);
    if (options.inputs.empty() || options.output.empty()) {
        throw std::runtime_error("--input, --output and --steam-id are required");
    }
    if (options.prefix.empty() || options.prefix.size() > 100 ||
        options.prefix.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
        throw std::runtime_error("Prefix must contain only ASCII letters, digits, '-' or '_'");
    }
    return options;
}
}

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path staging;
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--version") {
            std::cout << "CS2 Analyzer 0.1.0\n";
            return 0;
        }
        if (argc == 2 && std::wstring(argv[1]) == L"--help") {
            std::cout << "cs2-analyzer --input CSDA.json [--input other.json] --steam-id ID --output NEW_DIRECTORY\n"
                      << "  [--name NAME] [--platform LABEL] [--elo NUMBER] [--prefix SAFE_NAME]\n"
                      << "  [--provenance WRAPPER_INPUT_RECEIPTS.json]\n"
                      << "Exit 0: complete report; exit 1: explicit failure, no completed report.\n";
            return 0;
        }
        const auto options = parse(argc, argv);
        const auto output = std::filesystem::absolute(options.output).lexically_normal();
        if (std::filesystem::exists(output)) {
            throw std::runtime_error("Output directory already exists; choose a new run directory");
        }
        std::filesystem::create_directories(output.parent_path());
        staging = output.parent_path() / (output.filename().wstring() + L".pending-" +
            std::filesystem::u8path(cs2::random_suffix()).wstring());
        if (!std::filesystem::create_directory(staging)) {
            throw std::runtime_error("Cannot create private staging directory");
        }
        cs2::Report report;
        report.steam_id = options.steam_id;
        report.name = options.name;
        report.platform = options.platform;
        report.elo = options.elo;
        cs2::Json provenance = cs2::Json::array();
        if (!options.provenance.empty()) {
            provenance = cs2::read_json(options.provenance);
            if (!provenance.is_array()) throw std::runtime_error("Provenance must be an array");
        }
        std::map<std::string, std::pair<std::string, std::string>> checksums;
        for (const auto& input : options.inputs) {
            const auto path = std::filesystem::absolute(input).lexically_normal();
            const auto before = cs2::sha256_file(path);
            const auto match = cs2::read_json(path);
            if (before != cs2::sha256_file(path)) {
                throw std::runtime_error("Input changed during reading: " + path.u8string());
            }
            if (!match.is_object() || !match.contains("checksum") || !match["checksum"].is_string()) {
                throw std::runtime_error("Not a CSDA match export: " + path.u8string());
            }
            const auto checksum = match["checksum"].get<std::string>();
            const auto core = cs2::canonical_core(match);
            const auto fingerprint = cs2::sha256_text(core.dump());
            cs2::Json receipt{{"path", path.u8string()}, {"sha256", before},
                {"checksum", checksum}, {"core_sha256", fingerprint}};
            receipt["imported_source"] = match.at("source");
            for (const auto& item : provenance) {
                if (!item.is_object() || !item.contains("json_path") || !item["json_path"].is_string()) {
                    throw std::runtime_error("Invalid provenance receipt");
                }
                if (std::filesystem::absolute(std::filesystem::u8path(
                    item["json_path"].get<std::string>())).lexically_normal() == path) {
                    if (!item.contains("json_sha256") || item["json_sha256"] != before) {
                        throw std::runtime_error("Provenance hash does not match imported JSON");
                    }
                    receipt["decoder_provenance"] = item;
                }
            }
            if (checksums.count(checksum)) {
                const auto& previous = checksums.at(checksum);
                if (fingerprint != previous.first) {
                    throw std::runtime_error("Conflicting exports share a CSDA checksum: " + checksum);
                }
                receipt["status"] = "duplicate";
                receipt["duplicate_of"] = previous.second;
                report.inputs.push_back(receipt);
                std::cerr << "Skipping duplicate demo: " << path.filename().u8string() << "\n";
                continue;
            }
            const auto match_id = "m" + std::to_string(checksums.size() + 1);
            checksums[checksum] = {fingerprint, match_id};
            auto stats = cs2::analyze_match(match, options.steam_id, match_id, report);
            receipt["match_id"] = match_id;
            if (stats.is_null()) {
                receipt["status"] = "target_absent";
                report.inputs.push_back(receipt);
                std::cerr << "Target absent in: " << path.filename().u8string() << "\n";
                continue;
            }
            receipt["status"] = "analyzed";
            report.inputs.push_back(receipt);
            report.matches.push_back(std::move(stats));
            cs2::export_raw(match, staging / "raw" / match_id);
        }
        if (report.matches.empty()) throw std::runtime_error("Target not found in any input match");
        for (const auto& warning : report.warnings) {
            std::cerr << "Warning " << warning.at("match_id").get<std::string>() << ": "
                      << warning.at("message").get<std::string>() << "\n";
        }
        cs2::write_report(report, staging, options.prefix);
        std::filesystem::rename(staging, output);
        staging.clear();
        std::cout << "Report: " << (output / (options.prefix + "-report.html")).u8string() << "\n";
        std::cout << "Matches: " << report.matches.size() << "; cheating verdict: NOT ASSESSED\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Analysis failed: " << error.what() << "\n";
        if (!staging.empty()) {
            std::cerr << "Incomplete staging data retained (not a completed report): "
                      << staging.u8string() << "\n";
        }
        return 1;
    }
}
