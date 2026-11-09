cmake_minimum_required(VERSION 3.25)
project(CS2Analyzer VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This release targets Windows.")
endif()

include(FetchContent)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(cs2_analysis src/analysis.cpp src/report.cpp src/io.cpp)
target_include_directories(cs2_analysis PUBLIC src)
target_link_libraries(cs2_analysis PUBLIC nlohmann_json::nlohmann_json bcrypt)
target_compile_features(cs2_analysis PUBLIC cxx_std_17)
target_compile_definitions(cs2_analysis PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(cs2_analysis PUBLIC /W4 /WX /utf-8 /permissive-)
endif()

add_executable(cs2-analyzer src/main.cpp)
target_link_libraries(cs2-analyzer PRIVATE cs2_analysis)

add_library(csda_worker src/worker.cpp)
target_link_libraries(csda_worker PUBLIC cs2_analysis userenv advapi32 ws2_32)
add_executable(csda-service src/service.cpp)
target_link_libraries(csda-service PRIVATE csda_worker winsqlite3 user32 winhttp)

include(CTest)
if(BUILD_TESTING)
    add_executable(worker-tests tests/worker_tests.cpp)
    target_link_libraries(worker-tests PRIVATE csda_worker)
    add_test(NAME worker-containment COMMAND worker-tests)
    add_executable(analysis-tests tests/analysis_tests.cpp)
    target_link_libraries(analysis-tests PRIVATE cs2_analysis)
    add_executable(test-decoder tests/test_decoder.cpp)
    target_link_libraries(test-decoder PRIVATE cs2_analysis)
    add_test(NAME analysis-fixtures COMMAND analysis-tests)
    add_test(NAME cli-and-launcher
        COMMAND pwsh -NoLogo -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/Test-Pipeline.ps1"
        -AnalyzerPath $<TARGET_FILE:cs2-analyzer> -DecoderPath $<TARGET_FILE:test-decoder>)
endif()
