#pragma once
#include "io.h"
#include <atomic>
#include <windows.h>

namespace cs2::service {
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
void require(bool ok, const std::string& message);
std::wstring wide(const std::string& text);
std::filesystem::path image_path();
void private_directory(const std::filesystem::path& path);
void validate_tree(const std::filesystem::path& path);
void grant_package(const std::filesystem::path& path, PSID sid, DWORD access, bool directory);
std::uint64_t available_memory();
Json run_isolated(const std::filesystem::path& executable,
                  const std::vector<std::wstring>& arguments,
                  const std::filesystem::path& workspace,
                  std::atomic<bool>& cancel, std::uint64_t memory_bytes,
                  unsigned timeout_seconds = 300, bool protect_match = true);
int worker_probe(const std::filesystem::path& workspace,
                 const std::filesystem::path& private_canary, unsigned short probe_port = 8091);
}
