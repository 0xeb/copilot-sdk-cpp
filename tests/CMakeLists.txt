# Tests for copilot-sdk-cpp

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Put GTest/GMock targets in Tests folder for IDE organization
set_target_properties(gtest PROPERTIES FOLDER "Tests/GoogleTest")
set_target_properties(gtest_main PROPERTIES FOLDER "Tests/GoogleTest")
set_target_properties(gmock PROPERTIES FOLDER "Tests/GoogleTest")
set_target_properties(gmock_main PROPERTIES FOLDER "Tests/GoogleTest")

# Test for types and events
add_executable(test_types
    test_types.cpp
)

target_link_libraries(test_types
    PRIVATE
        copilot_sdk_cpp
        GTest::gtest_main
)

set_target_properties(test_types PROPERTIES FOLDER "Tests")

# Test for transport layer
add_executable(test_transport
    test_transport.cpp
)

target_link_libraries(test_transport
    PRIVATE
        copilot_sdk_cpp
        GTest::gtest_main
)

set_target_properties(test_transport PROPERTIES FOLDER "Tests")

# Test for JSON-RPC layer
add_executable(test_jsonrpc
    test_jsonrpc.cpp
)

target_link_libraries(test_jsonrpc
    PRIVATE
        copilot_sdk_cpp
        GTest::gtest_main
)

set_target_properties(test_jsonrpc PROPERTIES FOLDER "Tests")

# Test for process management
add_executable(test_process
    test_process.cpp
)

target_link_libraries(test_process
    PRIVATE
        copilot_sdk_cpp
        GTest::gtest_main
)

set_target_properties(test_process PROPERTIES FOLDER "Tests")

# Test for client and session
add_executable(test_client_session
    test_client_session.cpp
)

target_link_libraries(test_client_session
    PRIVATE
        copilot_sdk_cpp
        GTest::gtest_main
)

set_target_properties(test_client_session PROPERTIES FOLDER "Tests")

# E2E tests with real Copilot CLI (requires copilot CLI installed)
add_executable(test_e2e
    test_e2e.cpp
)

target_link_libraries(test_e2e
    PRIVATE
        copilot_sdk_cpp
        GTest::gtest_main
)

set_target_properties(test_e2e PROPERTIES FOLDER "Tests")

# Test for fluent tool builder API
add_executable(test_tool_builder
    test_tool_builder.cpp
)

target_link_libraries(test_tool_builder
    PRIVATE
        copilot_sdk_cpp
        GTest::gtest_main
)

set_target_properties(test_tool_builder PROPERTIES FOLDER "Tests")

# Test for RPC method catalog and typed parameter/result structs
add_executable(test_rpc_methods
    test_rpc_methods.cpp
)

target_link_libraries(test_rpc_methods
    PRIVATE
        copilot_sdk_cpp
        GTest::gtest_main
)

set_target_properties(test_rpc_methods PROPERTIES FOLDER "Tests")

# Focused tests for v0.1.49 parity additions (augments other suites)
add_executable(test_v0149_features
    test_v0149_features.cpp
)

target_link_libraries(test_v0149_features
    PRIVATE
        copilot_sdk_cpp
        GTest::gtest_main
)

set_target_properties(test_v0149_features PROPERTIES FOLDER "Tests")

# Offline conformance suite (in-process JSON-RPC peer; closes E2E gap)
add_executable(test_conformance
    test_conformance.cpp
)

target_link_libraries(test_conformance
    PRIVATE
        copilot_sdk_cpp
        GTest::gtest_main
)

set_target_properties(test_conformance PROPERTIES FOLDER "Tests")

include(GoogleTest)
gtest_discover_tests(test_types)
gtest_discover_tests(test_transport)
gtest_discover_tests(test_jsonrpc)
gtest_discover_tests(test_process)
gtest_discover_tests(test_client_session)
gtest_discover_tests(test_e2e)
gtest_discover_tests(test_tool_builder)
gtest_discover_tests(test_rpc_methods)
gtest_discover_tests(test_v0149_features)
gtest_discover_tests(test_conformance)

# Snapshot conformance tests (optional, requires upstream snapshots + Python)
if(COPILOT_BUILD_SNAPSHOT_TESTS)
    add_subdirectory(snapshot_tests)
endif()
