#include "io.h"
#include <windows.h>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    try {
        std::filesystem::path input;
        std::filesystem::path output;
        for (int index = 1; index < argc; ++index) {
            const auto option = cs2::utf8(argv[index]);
            if (option.rfind("-demo-path=", 0) == 0) input = std::filesystem::u8path(option.substr(11));
            if (option.rfind("-output=", 0) == 0) output = std::filesystem::u8path(option.substr(8));
        }
        const auto data = cs2::read_json(input);
        if (data.contains("test_sleep_ms")) Sleep(data.at("test_sleep_ms").get<DWORD>());
        if (data.contains("test_exit")) return data.at("test_exit").get<int>();
        cs2::write_text(output, data.dump());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 9;
    }
}
