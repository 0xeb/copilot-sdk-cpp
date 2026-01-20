# Standalone repro for ResumeSessionWithTools issue
# This is a completely independent project - does NOT include the SDK as subdirectory
#
# Prerequisites:
#   Build the SDK first:
#     cd ../..
#     cmake -B build -G "Visual Studio 17 2022" -A x64
#     cmake --build build --config Release
#
# Build this repro:
#   cd tests/repro
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Release
#
# Run:
#   build\Release\repro_resume_with_tools.exe

cmake_minimum_required(VERSION 3.20)
project(repro_resume_with_tools)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Paths to pre-built SDK
set(SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
set(SDK_INCLUDE "${SDK_ROOT}/include")
set(SDK_LIB_DIR "${SDK_ROOT}/build/Release")
set(SDK_BUILD_DIR "${SDK_ROOT}/build")

# Check SDK was built
if(NOT EXISTS "${SDK_LIB_DIR}/copilot_sdk_cpp.lib")
    message(FATAL_ERROR
        "SDK not built. Please build the SDK first:\n"
        "  cd ${SDK_ROOT}\n"
        "  cmake -B build && cmake --build build --config Release")
endif()

# Fetch nlohmann/json (same as SDK does)
include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_executable(repro_resume_with_tools repro_resume_with_tools.cpp)

target_include_directories(repro_resume_with_tools PRIVATE "${SDK_INCLUDE}")
target_link_libraries(repro_resume_with_tools PRIVATE nlohmann_json::nlohmann_json)
target_link_libraries(repro_resume_with_tools PRIVATE "${SDK_LIB_DIR}/copilot_sdk_cpp.lib")

# Windows socket library
if(WIN32)
    target_link_libraries(repro_resume_with_tools PRIVATE ws2_32)
endif()
