#include <winsock2.h>
#include "worker.h"
#include <iostream>
namespace fs = std::filesystem;
int wmain(int argc, wchar_t** argv) {
    using namespace cs2::service;
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--hold") { Sleep(30000); return 0; }
        if (argc == 2 && std::wstring(argv[1]) == L"--memory") {
            auto* allocation = VirtualAlloc(nullptr, 96ULL * 1024 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            const auto error = GetLastError();
            if (allocation) VirtualFree(allocation, 0, MEM_RELEASE);
            require(!allocation && (error == ERROR_COMMITMENT_LIMIT || error == ERROR_NOT_ENOUGH_MEMORY),
                "Memory allocation was not denied by the bounded Job");
            return 0;
        }
        if (argc == 5 && std::wstring(argv[1]) == L"--probe") {
            try {
                size_t consumed = 0;
                const auto port = std::stoul(argv[4], &consumed);
                require(consumed == std::wcslen(argv[4]) && port > 0 && port <= 65535, "Invalid probe port");
                return worker_probe(argv[2], argv[3], static_cast<unsigned short>(port));
            }
            catch (const std::exception& error) {
                cs2::write_text(fs::path(argv[2]) / "probe-error.json",
                    cs2::Json{{"error", error.what()}}.dump());
                return 1;
            }
        }
        const auto root = fs::temp_directory_path() / ("CSDA-Containment-" + cs2::random_suffix());
        private_directory(root);
        const auto canary = root / "private-canary.txt";
        cs2::write_text(canary, "synthetic owner-only canary");
        WSADATA sockets{};
        require(WSAStartup(MAKEWORD(2, 2), &sockets) == 0, "Test socket initialization failed");
        const auto listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(listener != INVALID_SOCKET, "Test listener creation failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
            listen(listener, 8) == 0, "Containment test needs its own reachable loopback listener");
        int address_size = sizeof(address);
        require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size) == 0,
            "Cannot read the fixture listener's assigned port");
        const auto control = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        require(control != INVALID_SOCKET &&
            connect(control, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
            "Uncontained positive control could not reach the test listener");
        const auto accepted = accept(listener, nullptr, nullptr);
        require(accepted != INVALID_SOCKET, "Positive-control connection was not accepted");
        closesocket(accepted);
        closesocket(control);
        const auto job = root / "job";
        std::atomic<bool> cancel{false};
        const auto result = run_isolated(image_path(), {L"--probe", job.wstring(), canary.wstring(),
            std::to_wstring(ntohs(address.sin_port))},
            job, cancel, 1024ULL * 1024 * 1024, 20);
        closesocket(listener);
        WSACleanup();
        std::cout << result.dump() << "\n";
        if (result.at("exit_code") != 0) {
            if (fs::exists(job / "probe-error.json")) std::cerr << cs2::read_json(job / "probe-error.json").dump() << "\n";
            throw std::runtime_error("Worker containment probe failed; fixture retained at " + root.u8string());
        }
        const auto proof = cs2::read_json(job / "isolation.json");
        require(proof.at("private_read_denied") == true && proof.at("network_denied") == true,
            "Containment proof incomplete");
        const auto timed = run_isolated(image_path(), {L"--hold"}, root / "timeout", cancel, 1024ULL * 1024 * 1024, 1);
        require(timed.at("error") == "Worker timed out" && timed.at("elapsed_ms").get<unsigned long long>() < 5000,
            "Timeout did not terminate/reap the exact worker");
        cancel = true;
        const auto cancelled = run_isolated(image_path(), {L"--hold"}, root / "cancel", cancel, 1024ULL * 1024 * 1024, 10);
        require(cancelled.at("error") == "Cancelled", "Cancellation was not enforced");
        cancel = false;
        auto* allocation = VirtualAlloc(nullptr, 96ULL * 1024 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        require(allocation != nullptr, "Uncontained memory positive control failed");
        VirtualFree(allocation, 0, MEM_RELEASE);
        const auto memory = run_isolated(image_path(), {L"--memory"}, root / "memory", cancel, 64ULL * 1024 * 1024, 10);
        require(memory.at("exit_code") == 0, "Job memory cap qualification failed");
        fs::remove_all(root);
        std::cout << "PASS: real AppContainer, non-elevated token, read-only binary directory, private-file/network denial, credential-free environment, timeout/cancel reap, bounded Job memory and accounting.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
