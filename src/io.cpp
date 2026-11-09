#include "io.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace cs2 {
namespace {
void check_status(NTSTATUS status, const char* operation) {
    if (status < 0) {
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(status));
    }
}

std::string hex_bytes(const unsigned char* data, std::size_t size) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < size; ++index) {
        out << std::setw(2) << static_cast<unsigned>(data[index]);
    }
    return out.str();
}

class Sha256 {
    struct Algorithm {
        BCRYPT_ALG_HANDLE handle = nullptr;
        ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    } algorithm_;
    struct Hash {
        BCRYPT_HASH_HANDLE handle = nullptr;
        ~Hash() { if (handle) BCryptDestroyHash(handle); }
    } hash_;
public:
    Sha256() {
        check_status(BCryptOpenAlgorithmProvider(&algorithm_.handle, BCRYPT_SHA256_ALGORITHM,
            nullptr, 0), "Opening SHA-256 provider");
        check_status(BCryptCreateHash(algorithm_.handle, &hash_.handle, nullptr, 0,
            nullptr, 0, 0), "Creating SHA-256 hash");
    }
    void update(const unsigned char* data, std::size_t size) {
        for (std::size_t offset = 0; offset < size;) {
            const auto length = static_cast<ULONG>((std::min)(size - offset,
                static_cast<std::size_t>(65536)));
            check_status(BCryptHashData(hash_.handle, const_cast<PUCHAR>(data + offset),
                length, 0), "Hashing bytes");
            offset += length;
        }
    }
    std::string finish() {
        std::array<unsigned char, 32> digest{};
        check_status(BCryptFinishHash(hash_.handle, digest.data(),
            static_cast<ULONG>(digest.size()), 0), "Finishing SHA-256 hash");
        return hex_bytes(digest.data(), digest.size());
    }
};
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0) throw std::runtime_error("Invalid Unicode command-line argument");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), result.data(), size, nullptr, nullptr) != size) {
        throw std::runtime_error("Unable to encode command-line argument");
    }
    return result;
}

std::string sha256_file(const std::filesystem::path& path) {
    Sha256 hash;
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read file for hashing: " + path.u8string());
    std::array<unsigned char, 65536> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        if (input.gcount() > 0) {
            hash.update(buffer.data(), static_cast<std::size_t>(input.gcount()));
        }
    }
    if (!input.eof()) throw std::runtime_error("File read failed while hashing: " + path.u8string());
    return hash.finish();
}

std::string sha256_text(const std::string& text) {
    Sha256 hash;
    hash.update(reinterpret_cast<const unsigned char*>(text.data()), text.size());
    return hash.finish();
}

std::string random_suffix() {
    std::array<unsigned char, 8> bytes{};
    check_status(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG), "Generating run identifier");
    return hex_bytes(bytes.data(), bytes.size());
}

Json read_json(const std::filesystem::path& path) {
    constexpr std::uintmax_t max_bytes = 512ULL * 1024ULL * 1024ULL;
    if (std::filesystem::file_size(path) > max_bytes) {
        throw std::runtime_error("JSON exceeds the 512 MiB per-file limit: " + path.u8string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read JSON: " + path.u8string());
    Json value = Json::parse(input);
    if (input.bad()) throw std::runtime_error("JSON read failed: " + path.u8string());
    return value;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create output: " + path.u8string());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    if (!output) throw std::runtime_error("Writing output failed: " + path.u8string());
}

std::string html_escape(const std::string& text) {
    std::string result;
    for (char character : text) {
        switch (character) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&#39;"; break;
        default: result += character;
        }
    }
    return result;
}

std::string csv_cell(const Json& value) {
    if (value.is_null()) return {};
    std::string text = value.is_string() ? value.get<std::string>() : value.dump();
    if (value.is_string()) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first != std::string::npos &&
            (text[first] == '=' || text[first] == '+' || text[first] == '-' || text[first] == '@')) {
            text.insert(text.begin(), '\'');
        }
        if (!text.empty() && (text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
            text.insert(text.begin(), '\'');
        }
    }
    std::string quoted = "\"";
    for (char character : text) {
        if (character == '"') quoted += '"';
        quoted += character;
    }
    quoted += '"';
    return quoted;
}

void write_csv(const std::filesystem::path& path, const Json& rows,
               const std::vector<std::string>& preferred) {
    if (!rows.is_array()) throw std::runtime_error("CSV rows must be an array");
    std::vector<std::string> columns = preferred;
    std::set<std::string> seen(columns.begin(), columns.end());
    std::set<std::string> remaining;
    for (const auto& row : rows) {
        if (!row.is_object()) throw std::runtime_error("CSV rows must contain objects");
        for (auto field = row.begin(); field != row.end(); ++field) {
            if (!seen.count(field.key())) remaining.insert(field.key());
        }
    }
    columns.insert(columns.end(), remaining.begin(), remaining.end());
    std::ostringstream output;
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (index) output << ',';
        output << csv_cell(columns[index]);
    }
    output << "\r\n";
    for (const auto& row : rows) {
        for (std::size_t index = 0; index < columns.size(); ++index) {
            if (index) output << ',';
            const auto found = row.find(columns[index]);
            if (found != row.end()) output << csv_cell(*found);
        }
        output << "\r\n";
    }
    write_text(path, output.str());
}

void export_raw(const Json& match, const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    for (auto field = match.begin(); field != match.end(); ++field) {
        if (!field->is_array()) continue;
        const auto& key = field.key();
        if (key.empty() || key.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos) {
            throw std::runtime_error("Unsafe CSDA table name");
        }
        write_csv(directory / (key + ".csv"), *field);
    }
    Json players = Json::array();
    for (const auto& player : match.at("players")) players.push_back(player);
    write_csv(directory / "players.csv", players);
    Json metadata = match;
    for (auto field = match.begin(); field != match.end(); ++field) {
        if (field->is_array() || field.key() == "players") metadata.erase(field.key());
    }
    write_text(directory / "match.json", metadata.dump(2) + "\n");
}
}
