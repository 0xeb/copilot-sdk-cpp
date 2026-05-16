cmake_minimum_required(VERSION 3.20)
project(copilot_sdk_cpp VERSION 0.1.0 LANGUAGES CXX)

# Enable folder organization in IDEs
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Standard install dirs (lib/bin/include)
include(GNUInstallDirs)

# C++20 Standard (for std::jthread, std::stop_token, concepts, etc.)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(COPILOT_BUILD_TESTS "Build tests" ON)
option(COPILOT_BUILD_EXAMPLES "Build examples" ON)
option(COPILOT_BUILD_SNAPSHOT_TESTS "Build snapshot conformance tests (requires upstream snapshots + Python)" OFF)
option(COPILOT_WITH_FASTMCPP "Build in-process MCP examples with fastmcpp" OFF)

# Find dependencies
find_package(nlohmann_json CONFIG QUIET)

set(copilot_sdk_cpp_bundled_nlohmann_json OFF)
if(NOT TARGET nlohmann_json::nlohmann_json)
    # Vendor nlohmann/json headers via FetchContent without adding its CMake project.
    # This keeps our install/export set self-contained while still allowing consumers
    # to override with their own nlohmann_json package.
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_GetProperties(nlohmann_json)
    if(NOT nlohmann_json_POPULATED)
        if(POLICY CMP0169)
            cmake_policy(SET CMP0169 OLD)
        endif()
        FetchContent_Populate(nlohmann_json)
    endif()

    add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
    set_target_properties(nlohmann_json::nlohmann_json PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES
            "$<BUILD_INTERFACE:${nlohmann_json_SOURCE_DIR}/include>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )

    set(copilot_sdk_cpp_bundled_nlohmann_json ON)
endif()

# Library target
add_library(copilot_sdk_cpp
    # Headers (for IDE visibility)
    include/copilot/copilot.hpp
    include/copilot/types.hpp
    include/copilot/events.hpp
    include/copilot/transport.hpp
    include/copilot/transport_stdio.hpp
    include/copilot/transport_tcp.hpp
    include/copilot/jsonrpc.hpp
    include/copilot/process.hpp
    include/copilot/client.hpp
    include/copilot/session.hpp
    include/copilot/rpc_methods.hpp
    include/copilot/rpc_types.hpp
    # Sources
    src/types.cpp
    src/events.cpp
    src/transport.cpp
    src/jsonrpc.cpp
    src/process_win32.cpp
    src/process_posix.cpp
    src/client.cpp
    src/session.cpp
)
add_library(copilot::copilot_sdk_cpp ALIAS copilot_sdk_cpp)

target_include_directories(copilot_sdk_cpp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(copilot_sdk_cpp
    PUBLIC
        nlohmann_json::nlohmann_json
)

# Platform-specific settings
if(WIN32)
    target_compile_definitions(copilot_sdk_cpp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    # Needed for Winsock APIs when building with MinGW/GCC (pragma comment only works on MSVC)
    target_link_libraries(copilot_sdk_cpp PUBLIC ws2_32)
endif()

# Set target properties
set_target_properties(copilot_sdk_cpp PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    FOLDER "Libraries"
)

# Tests
if(COPILOT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
if(COPILOT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# In-process MCP examples (requires fastmcpp)
if(COPILOT_WITH_FASTMCPP)
    # Try to find installed fastmcpp first
    find_package(fastmcpp QUIET)

    if(NOT TARGET fastmcpp_core)
        message(STATUS "fastmcpp not found locally, fetching from GitHub...")
        include(FetchContent)
        set(COPILOT_FASTMCPP_GIT_TAG
            "97d9bc1f07bb591a76acdfd04461e6b56c50b20a"
            CACHE STRING
            "Git tag/commit for fastmcpp when fetched"
        )
        FetchContent_Declare(
            fastmcpp
            GIT_REPOSITORY https://github.com/0xeb/fastmcpp.git
            GIT_TAG ${COPILOT_FASTMCPP_GIT_TAG}
        )
        # Disable fastmcpp tests/examples for faster build
        set(FASTMCPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(FASTMCPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(fastmcpp)
    else()
        message(STATUS "Using locally installed fastmcpp")
    endif()

    add_subdirectory(examples/mcp)
endif()

# Install rules (only when built as top-level project)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    install(TARGETS copilot_sdk_cpp
        EXPORT copilot_sdk_cpp_targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    if(copilot_sdk_cpp_bundled_nlohmann_json)
        install(DIRECTORY ${nlohmann_json_SOURCE_DIR}/include/nlohmann
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()

    # CMake package config (find_package(copilot_sdk_cpp))
    set(copilot_sdk_cpp_install_cmakedir "${CMAKE_INSTALL_LIBDIR}/cmake/copilot_sdk_cpp")

    install(EXPORT copilot_sdk_cpp_targets
        FILE copilot_sdk_cppTargets.cmake
        NAMESPACE copilot::
        DESTINATION ${copilot_sdk_cpp_install_cmakedir}
    )

    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/copilot_sdk_cppConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/copilot_sdk_cppConfig.cmake
        INSTALL_DESTINATION ${copilot_sdk_cpp_install_cmakedir}
    )

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/copilot_sdk_cppConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/copilot_sdk_cppConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/copilot_sdk_cppConfigVersion.cmake
        DESTINATION ${copilot_sdk_cpp_install_cmakedir}
    )
endif()
