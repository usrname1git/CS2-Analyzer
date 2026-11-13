#pragma once

#include "analysis.h"
#include <filesystem>
#include <string>

namespace cs2 {
std::string utf8(const std::wstring& text);
std::string sha256_file(const std::filesystem::path& path);
std::string sha256_text(const std::string& text);
std::string random_suffix();
Json read_json(const std::filesystem::path& path);
void write_text(const std::filesystem::path& path, const std::string& text);
std::string html_escape(const std::string& text);
std::string csv_cell(const Json& value);
void write_csv(const std::filesystem::path& path, const Json& rows,
               const std::vector<std::string>& preferred = {});
void export_raw(const Json& match, const std::filesystem::path& directory);
void write_report(const Report& report, const std::filesystem::path& directory,
                  const std::string& prefix);
}
