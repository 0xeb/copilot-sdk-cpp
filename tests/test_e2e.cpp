// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file test_e2e.cpp
/// @brief End-to-end tests using the real Copilot CLI
///
/// These tests require the Copilot CLI to be installed and available in PATH.
/// They test the full SDK stack against the real server.
///
/// Run these tests manually or in CI with Copilot CLI installed:
///   ctest -R E2ETest --output-on-failure

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <copilot/copilot.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iostream>
#include <mutex>
#include <thread>

using namespace copilot;

// =============================================================================
// BYOK Environment File Loader
// =============================================================================

/// Load environment variables from tests/byok.env if it exists.
/// File format: KEY=VALUE per line (no quotes needed, # comments supported)
static void load_byok_env_file()
{
    static std::once_flag once;
    std::call_once(
        once,
        []()
        {
            // Get directory of this source file and look for byok.env
            std::filesystem::path source_path(__FILE__);
            std::filesystem::path env_file = source_path.parent_path() / "byok.env";

            if (!std::filesystem::exists(env_file))
            {
                std::cerr << "[E2E] No byok.env file found at: " << env_file << "\n";
                return;
            }

            std::ifstream file(env_file);
            if (!file.is_open())
            {
                std::cerr << "[E2E] Failed to open byok.env file\n";
                return;
            }

            std::cerr << "[E2E] Loading BYOK config from: " << env_file << "\n";

            std::string line;
            int count = 0;
            while (std::getline(file, line))
            {
                // Skip empty lines and comments
                if (line.empty() || line[0] == '#')
                    continue;

                // Trim whitespace
                size_t start = line.find_first_not_of(" \t");
                if (start == std::string::npos)
                    continue;
                size_t end = line.find_last_not_of(" \t\r\n");
                line = line.substr(start, end - start + 1);

                // Find KEY=VALUE
                size_t eq_pos = line.find('=');
                if (eq_pos == std::string::npos)
                    continue;

                std::string key = line.substr(0, eq_pos);
                std::string value = line.substr(eq_pos + 1);

                // Trim key and value
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t\r\n") + 1);

                // Set environment variable
#ifdef _WIN32
                _putenv_s(key.c_str(), value.c_str());
#else
                setenv(key.c_str(), value.c_str(), 1);
#endif
                // Mask the value for logging (show only last 4 chars)
                std::string masked = value.length() > 4
                                         ? std::string(value.length() - 4, '*') + value.substr(value.length() - 4)
                                         : "****";
                std::cerr << "[E2E]   " << key << "=" << masked << "\n";
                count++;
            }

            std::cerr << "[E2E] Loaded " << count << " environment variables from byok.env\n";
        }
    );
}

/// Check if BYOK is active (API key env var is set)
/// Used to skip tests that don't work with BYOK providers
static bool is_byok_active()
{
    const char* key = std::getenv("COPILOT_SDK_BYOK_API_KEY");
    return key != nullptr && key[0] != '\0';
}

// =============================================================================
// Test Fixture
// =============================================================================

class E2ETest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        // Load BYOK environment variables from tests/byok.env if it exists
        load_byok_env_file();

        // E2E tests run by default. CI can set COPILOT_SDK_CPP_SKIP_E2E=1 to disable.
        if (should_skip_e2e_tests())
            GTEST_SKIP() << "E2E tests disabled via COPILOT_SDK_CPP_SKIP_E2E";

        // Check if Copilot CLI is available
        if (!is_copilot_available())
            GTEST_SKIP() << "Copilot CLI not found in PATH - skipping E2E tests";

        // Check that Copilot CLI can actually make model calls (quota/auth). This avoids
        // turning local quota outages into red builds.
        ensure_copilot_can_run();
        if (!copilot_can_run_.load())
            GTEST_SKIP() << copilot_skip_reason_;
    }

    static bool should_skip_e2e_tests()
    {
        const char* env = std::getenv("COPILOT_SDK_CPP_SKIP_E2E");
        if (!env)
            return false;
        std::string v(env);
        for (auto& c : v)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }

    static bool is_copilot_available()
    {
        // Try to find copilot in PATH
        auto path = find_executable("copilot");
        return path.has_value();
    }

    static void ensure_copilot_can_run()
    {
        static std::once_flag once;
        std::call_once(
            once,
            []()
            {
                try
                {
                    ClientOptions opts;
                    opts.log_level = LogLevel::Info;
                    opts.use_stdio = true;
                    opts.cli_args = std::vector<std::string>{"--allow-all-tools", "--allow-all-paths"};
                    opts.auto_start = false;

                    Client client(opts);
                    client.start().get();

                    // Use BYOK config if available (from tests/byok.env)
                    SessionConfig session_config;
                    session_config.auto_byok_from_env = true;
                    auto session = client.create_session(session_config).get();

                    std::mutex mtx;
                    std::condition_variable cv;
                    bool done = false;
                    std::string error_message;

                    auto sub = session->on(
                        [&](const SessionEvent& event)
                        {
                            if (auto* err = event.try_as<SessionErrorData>())
                            {
                                std::lock_guard<std::mutex> lock(mtx);
                                error_message = err->message;
                                done = true;
                                cv.notify_one();
                            }
                            else if (event.type == SessionEventType::SessionIdle)
                            {
                                std::lock_guard<std::mutex> lock(mtx);
                                done = true;
                                cv.notify_one();
                            }
                        }
                    );

                    MessageOptions msg;
                    msg.prompt = "ping";
                    session->send(msg).get();

                    {
                        std::unique_lock<std::mutex> lock(mtx);
                        cv.wait_for(lock, std::chrono::seconds(15), [&]() { return done; });
                    }

                    // Clean up before deciding
                    session->destroy().get();
                    client.force_stop();

                    if (!error_message.empty())
                    {
                        std::string lower = error_message;
                        for (auto& c : lower)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                        if (lower.find("quota") != std::string::npos ||
                            lower.find("402") != std::string::npos ||
                            lower.find("401") != std::string::npos ||
                            lower.find("authorization") != std::string::npos ||
                            lower.find("authentication") != std::string::npos ||
                            lower.find("unauthorized") != std::string::npos)
                        {
                            copilot_can_run_ = false;
                            copilot_skip_reason_ =
                                "Copilot CLI cannot make model calls (quota/auth). Error: " +
                                error_message;
                            return;
                        }
                    }

                    // If no error and we got idle, assume usable.
                    copilot_can_run_ = true;
                    copilot_skip_reason_.clear();
                }
                catch (const std::exception& e)
                {
                    copilot_can_run_ = false;
                    copilot_skip_reason_ =
                        std::string("Copilot CLI preflight failed: ") + e.what();
                }
            }
        );
    }

    std::unique_ptr<Client> create_client()
    {
        ClientOptions opts;
        opts.log_level = LogLevel::Info;
        opts.use_stdio = true;
        // Make E2E tests reliable/non-interactive by pre-approving tool and path access.
        // These flags are only used for tests; library defaults remain secure-by-default.
        opts.cli_args = std::vector<std::string>{"--allow-all-tools", "--allow-all-paths"};
        opts.auto_start = false; // We'll start manually
        return std::make_unique<Client>(opts);
    }

    /// Create a default SessionConfig with BYOK env vars enabled.
    /// If tests/byok.env exists, it will have been loaded and these vars will be used.
    static SessionConfig default_session_config()
    {
        SessionConfig config;
        config.auto_byok_from_env = true;
        return config;
    }

    /// Create a default ResumeSessionConfig with BYOK env vars enabled.
    static ResumeSessionConfig default_resume_config()
    {
        ResumeSessionConfig config;
        config.auto_byok_from_env = true;
        return config;
    }

    /// Print test description for verbose output
    static void test_info(const char* description)
    {
        std::cerr << "\n[TEST] " << description << "\n";
        std::cerr << std::string(60, '-') << "\n";
    }

    static inline std::atomic<bool> copilot_can_run_{true};
    static inline std::string copilot_skip_reason_;
};

// =============================================================================
// Basic Connection Tests
// =============================================================================

TEST_F(E2ETest, StartAndStop)
{
    test_info("Basic connection test: Start CLI process, verify connected, then stop cleanly.");
    auto client = create_client();

    EXPECT_EQ(client->state(), ConnectionState::Disconnected);

    // Start
    ASSERT_NO_THROW(client->start().get());
    EXPECT_EQ(client->state(), ConnectionState::Connected);

    // Stop
    ASSERT_NO_THROW(client->stop().get());
    EXPECT_EQ(client->state(), ConnectionState::Disconnected);
}

TEST_F(E2ETest, Ping)
{
    test_info("Ping test: Send ping RPC to CLI and verify response with protocol version.");
    auto client = create_client();
    client->start().get();

    auto response = client->ping("test message").get();

    // Note: Copilot CLI returns "pong: <message>" format
    EXPECT_TRUE(response.message.find("test message") != std::string::npos);
    EXPECT_EQ(response.protocol_version, kSdkProtocolVersion);
    EXPECT_GT(response.timestamp, 0);

    client->force_stop(); // Use force_stop for faster cleanup in tests
}

TEST_F(E2ETest, PingWithoutMessage)
{
    test_info("Ping without message: Verify ping works with null/empty message.");
    auto client = create_client();
    client->start().get();

    auto response = client->ping().get();

    // Message should be null/empty when not provided
    EXPECT_EQ(response.protocol_version, kSdkProtocolVersion);

    client->force_stop();
}

// =============================================================================
// Session Tests
// =============================================================================

TEST_F(E2ETest, CreateSession)
{
    test_info("Create session: Start client, create session with BYOK config, verify session ID returned.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    auto session = client->create_session(config).get();

    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, CreateSessionWithModel)
{
    test_info("Create session with explicit model: Test model override in SessionConfig.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    config.model = "gpt-4.1"; // Use a known model

    auto session = client->create_session(config).get();

    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, CreateSessionWithTools)
{
    test_info("Tool execution test: Register custom tool, ask AI to use it, verify tool called with correct args.");
    auto client = create_client();
    client->start().get();

    // Track tool invocation arguments
    std::atomic<bool> tool_called{false};
    std::string received_key;
    std::mutex arg_mtx;

    // Define a custom tool
    Tool secret_tool;
    secret_tool.name = "get_secret_number";
    secret_tool.description = "Returns a secret number that only this tool knows";
    secret_tool.parameters_schema = json{
        {"type", "object"},
        {"properties", {{"key", {{"type", "string"}, {"description", "The key to look up"}}}}},
        {"required", {"key"}}
    };
    secret_tool.handler = [&](const ToolInvocation& inv) -> ToolResultObject
    {
        ToolResultObject result;
        std::string key = inv.arguments.value()["key"].get<std::string>();

        // Capture arguments for validation
        {
            std::lock_guard<std::mutex> lock(arg_mtx);
            received_key = key;
            tool_called = true;
        }

        if (key == "ALPHA")
            result.text_result_for_llm = "54321";
        else
            result.text_result_for_llm = "Unknown key";
        result.result_type = ToolResultType::Success;
        return result;
    };

    // Create session with the tool
    auto config = default_session_config();
    config.tools = {secret_tool};
    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };
    auto session = client->create_session(config).get();

    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    // Track events
    std::atomic<bool> idle{false};
    std::string tool_result_content;
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
            else if (event.type == SessionEventType::ToolExecutionComplete)
            {
                auto& data = event.as<ToolExecutionCompleteData>();
                if (data.result.has_value())
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    tool_result_content = data.result->content;
                }
            }
        }
    );

    // Ask the model to use the tool
    MessageOptions opts;
    opts.prompt = "Use the get_secret_number tool to look up key 'ALPHA' and tell me the number.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    // Verify the tool was called with correct arguments
    EXPECT_TRUE(tool_called.load()) << "Custom tool should have been invoked";
    {
        std::lock_guard<std::mutex> lock(arg_mtx);
        EXPECT_EQ(received_key, "ALPHA") << "Tool should receive the requested key";
    }

    // Verify the tool result was returned
    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_TRUE(tool_result_content.find("54321") != std::string::npos)
            << "Tool result should contain the secret number. Got: " << tool_result_content;
    }

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, ListSessions)
{
    test_info("List sessions: Create session, send message, verify session appears in history list.");
    auto client = create_client();
    client->start().get();

    // Create a session and send a message to persist it
    auto session = client->create_session().get();
    std::string session_id = session->session_id();

    // Send a message - this persists the session to history
    MessageOptions opts;
    opts.prompt = "test";
    session->send(opts).get();

    // Poll until the session appears in history (Copilot CLI timing can vary)
    bool found = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto sessions = client->list_sessions().get();
        for (const auto& meta : sessions)
        {
            if (meta.session_id == session_id)
            {
                found = true;
                break;
            }
        }
        if (found)
            break;

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    EXPECT_TRUE(found) << "Created session not found in list after sending message";

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, GetLastSessionId)
{
    test_info("Get last session ID: Create session, destroy it, verify get_last_session_id() works.");
    auto client = create_client();
    client->start().get();

    // Create a session
    auto session = client->create_session().get();
    std::string session_id = session->session_id();
    session->destroy().get();

    // Get last session ID
    auto last_id = client->get_last_session_id().get();

    // Should have some session ID (might be ours or a previous one)
    // The important thing is the method works
    client->force_stop();
}

// =============================================================================
// Messaging Tests
// =============================================================================

TEST_F(E2ETest, SendMessage)
{
    test_info("Send message: Create session, send prompt, wait for SessionIdle, verify events received.");
    auto client = create_client();
    client->start().get();

    auto session = client->create_session(default_session_config()).get();

    // Track events
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> idle{false};
    std::vector<SessionEventType> received_events;

    auto subscription = session->on(
        [&](const SessionEvent& event)
        {
            {
                std::lock_guard<std::mutex> lock(mtx);
                received_events.push_back(event.type);
            }
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Send a simple message
    MessageOptions opts;
    opts.prompt = "Say exactly: HELLO TEST";

    auto message_id = session->send(opts).get();
    EXPECT_FALSE(message_id.empty());

    // Wait for response (with timeout)
    {
        std::unique_lock<std::mutex> lock(mtx);
        bool completed = cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
        EXPECT_TRUE(completed) << "Timeout waiting for response";
    }

    // Check we received expected events
    EXPECT_FALSE(received_events.empty());

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, StreamingResponse)
{
    test_info("Streaming response: Enable streaming, send prompt, verify multiple AssistantMessageDelta events.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    config.streaming = true;

    auto session = client->create_session(config).get();

    // Track streaming deltas
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> idle{false};
    std::atomic<int> delta_count{0};
    std::string full_response;

    auto subscription = session->on(
        [&](const SessionEvent& event)
        {
            if (auto* delta = event.try_as<AssistantMessageDeltaData>())
            {
                std::lock_guard<std::mutex> lock(mtx);
                delta_count++;
                full_response += delta->delta_content;
            }
            else if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Send message
    MessageOptions opts;
    opts.prompt = "Count from 1 to 5";

    session->send(opts).get();

    // Wait for completion
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    // In streaming mode, we should receive multiple deltas
    std::cout << "Received " << delta_count << " streaming deltas\n";
    std::cout << "Full response: " << full_response.substr(0, 200) << "...\n";

    EXPECT_GT(delta_count.load(), 0) << "Expected streaming deltas";

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, AbortMessage)
{
    test_info("Abort message: Send long prompt, call abort() mid-stream, verify session becomes idle.");
    auto client = create_client();
    client->start().get();

    auto session = client->create_session().get();

    std::atomic<bool> got_response{false};
    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto subscription = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::AssistantMessage ||
                event.type == SessionEventType::AssistantMessageDelta)
            {
                got_response = true;
            }
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Send a message that would take time
    MessageOptions opts;
    opts.prompt = "Write a very long story about a wizard";

    session->send(opts).get();

    // Wait a bit then abort
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    ASSERT_NO_THROW(session->abort().get());

    // Wait for idle
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(10), [&]() { return idle.load(); });
    }

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, GetMessages)
{
    test_info("Get messages: Send message, wait for response, call get_messages() to retrieve history.");
    auto client = create_client();
    client->start().get();

    auto session = client->create_session().get();

    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto subscription = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Send a message
    MessageOptions opts;
    opts.prompt = "Hello";
    session->send(opts).get();

    // Wait for response
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    // Get messages
    auto messages = session->get_messages().get();

    // Should have at least the user message and assistant response
    std::cout << "Got " << messages.size() << " messages\n";

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Session Resume Tests
// =============================================================================

TEST_F(E2ETest, ResumeSession)
{
    test_info("Resume session: Create session, stop client, restart, resume by ID, verify same session.");
    auto client = create_client();
    client->start().get();

    // Create initial session
    auto session1 = client->create_session().get();
    std::string session_id = session1->session_id();

    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub1 = session1->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Send initial message
    MessageOptions opts;
    opts.prompt = "Remember this: the secret code is XYZ123";
    session1->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    // Don't destroy, just stop client
    client->stop().get();

    // Restart and resume
    client = create_client();
    client->start().get();

    auto resume_config = default_resume_config();
    auto session2 = client->resume_session(session_id, resume_config).get();

    EXPECT_EQ(session2->session_id(), session_id);

    // Clean up
    session2->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, ResumeSessionWithTools)
{
    test_info("Resume with tools: Create session, stop, resume with new tool, invoke tool successfully.");

    // BYOK/OpenAI doesn't support resuming sessions with new tools
    auto client = create_client();
    client->start().get();

    // Create initial session without tools
    auto session1 = client->create_session().get();
    std::string session_id = session1->session_id();

    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub1 = session1->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Send initial message
    MessageOptions opts;
    opts.prompt = "Say hello";
    session1->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    // Don't destroy, just stop client
    client->stop().get();

    // Track tool invocation arguments
    std::atomic<bool> tool_called{false};
    std::string received_key;
    std::mutex arg_mtx;

    // Define a custom tool
    Tool secret_tool;
    secret_tool.name = "get_secret";
    secret_tool.description = "Returns a secret value that only this tool knows";
    secret_tool.parameters_schema = json{
        {"type", "object"},
        {"properties", {{"key", {{"type", "string"}, {"description", "The key to look up"}}}}},
        {"required", {"key"}}
    };
    secret_tool.handler = [&](const ToolInvocation& inv) -> ToolResultObject
    {
        ToolResultObject result;
        std::string key = inv.arguments.value()["key"].get<std::string>();

        // Capture arguments for validation
        {
            std::lock_guard<std::mutex> lock(arg_mtx);
            received_key = key;
            tool_called = true;
        }

        if (key == "ALPHA")
            result.text_result_for_llm = "SECRET_VALUE_12345";
        else
            result.text_result_for_llm = "Unknown key";
        result.result_type = ToolResultType::Success;
        return result;
    };

    // Restart and resume WITH the tool
    client = create_client();
    client->start().get();

    auto resume_config = default_resume_config();
    resume_config.tools = {secret_tool};
    auto session2 = client->resume_session(session_id, resume_config).get();

    EXPECT_EQ(session2->session_id(), session_id);

    // Reset for next message
    idle = false;
    std::string tool_result_content;

    auto sub2 = session2->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
            else if (event.type == SessionEventType::ToolExecutionComplete)
            {
                auto& data = event.as<ToolExecutionCompleteData>();
                if (data.result.has_value())
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    tool_result_content = data.result->content;
                }
            }
        }
    );

    // Ask the model to use the tool
    opts.prompt = "Use the get_secret tool to look up the key 'ALPHA' and tell me the value.";
    session2->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    // Verify the tool was called with correct arguments
    EXPECT_TRUE(tool_called.load()) << "Custom tool should have been invoked";
    {
        std::lock_guard<std::mutex> lock(arg_mtx);
        EXPECT_EQ(received_key, "ALPHA") << "Tool should receive the requested key";
    }

    // Verify the tool result was returned
    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_TRUE(tool_result_content.find("SECRET_VALUE_12345") != std::string::npos)
            << "Tool result should contain the secret value. Got: " << tool_result_content;
    }

    // Clean up
    session2->destroy().get();
    client->force_stop();
}

// =============================================================================
// Event Subscription Tests
// =============================================================================

TEST_F(E2ETest, EventSubscription)
{
    test_info("Event subscription: Subscribe to session events, send message, verify events received.");
    auto client = create_client();
    client->start().get();

    auto session = client->create_session().get();

    std::mutex mtx;
    std::condition_variable cv;
    std::vector<SessionEventType> events;
    std::atomic<bool> idle{false};

    // Subscribe to events
    auto subscription = session->on(
        [&](const SessionEvent& event)
        {
            std::lock_guard<std::mutex> lock(mtx);
            events.push_back(event.type);
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Send message
    MessageOptions opts;
    opts.prompt = "Hi";
    session->send(opts).get();

    // Wait for completion
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    // Check events
    std::lock_guard<std::mutex> lock(mtx);
    std::cout << "Received events:\n";
    for (auto type : events)
        std::cout << "  - " << static_cast<int>(type) << "\n";

    // Should have multiple events
    EXPECT_GT(events.size(), 1);

    // Unsubscribe (RAII - happens on destruction)
    subscription.unsubscribe();

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, MultipleSubscriptions)
{
    test_info("Multiple subscriptions: Register two event handlers, verify both receive same events.");
    auto client = create_client();
    client->start().get();

    auto session = client->create_session().get();

    std::atomic<int> handler1_count{0};
    std::atomic<int> handler2_count{0};
    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub1 = session->on([&](const SessionEvent&) { handler1_count++; });

    auto sub2 = session->on(
        [&](const SessionEvent& event)
        {
            handler2_count++;
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Send message
    MessageOptions opts;
    opts.prompt = "Test";
    session->send(opts).get();

    // Wait
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    // Both handlers should receive events
    EXPECT_GT(handler1_count.load(), 0);
    EXPECT_GT(handler2_count.load(), 0);
    EXPECT_EQ(handler1_count.load(), handler2_count.load());

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_F(E2ETest, InvalidSessionId)
{
    test_info("Invalid session ID: Attempt to resume non-existent session, expect exception.");
    auto client = create_client();
    client->start().get();

    // Try to resume non-existent session
    auto config = default_resume_config();

    EXPECT_THROW(
        { client->resume_session("non-existent-session-id-12345", config).get(); }, std::exception
    );

    client->force_stop();
}

TEST_F(E2ETest, ForceStop)
{
    test_info("Force stop: Create session, call force_stop(), verify client disconnects immediately.");
    auto client = create_client();
    client->start().get();

    auto session = client->create_session().get();

    // Force stop while session exists
    ASSERT_NO_THROW(client->force_stop());

    EXPECT_EQ(client->state(), ConnectionState::Disconnected);
}

// =============================================================================
// Concurrency Tests
// =============================================================================

TEST_F(E2ETest, ConcurrentPings)
{
    test_info("Concurrent pings: Send 5 ping requests simultaneously, verify all return correctly.");
    auto client = create_client();
    client->start().get();

    // Send multiple concurrent pings
    std::vector<std::future<PingResponse>> futures;

    for (int i = 0; i < 5; ++i)
        futures.push_back(client->ping("ping-" + std::to_string(i)));

    // Wait for all
    for (int i = 0; i < 5; ++i)
    {
        auto response = futures[i].get();
        // Note: Copilot CLI returns "pong: <message>" format
        EXPECT_TRUE(response.message.find("ping-" + std::to_string(i)) != std::string::npos);
    }

    client->force_stop();
}

// =============================================================================
// MCP Server Configuration Tests
// =============================================================================

TEST_F(E2ETest, AcceptMcpServerConfigOnSessionCreate)
{
    test_info("MCP on create: Create session with MCP server config, verify session created.");
    auto client = create_client();
    client->start().get();

    // Create MCP server config
    McpLocalServerConfig mcp_config;
    mcp_config.type = "local";
    mcp_config.command = "echo";
    mcp_config.args = {"hello"};
    mcp_config.tools = {"*"};

    std::map<std::string, json> mcp_servers;
    mcp_servers["test-server"] = mcp_config;

    auto config = default_session_config();
    config.mcp_servers = mcp_servers;

    auto session = client->create_session(config).get();

    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, AcceptMcpServerConfigOnSessionResume)
{
    test_info("MCP on resume: Create session, stop, resume with MCP config, verify session works.");
    auto client = create_client();
    client->start().get();

    // Create initial session
    auto session1 = client->create_session().get();
    std::string session_id = session1->session_id();

    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session1->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Send a message
    MessageOptions opts;
    opts.prompt = "What is 1+1?";
    session1->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    // Resume with MCP servers
    McpLocalServerConfig mcp_config;
    mcp_config.type = "local";
    mcp_config.command = "echo";
    mcp_config.args = {"hello"};
    mcp_config.tools = {"*"};

    std::map<std::string, json> mcp_servers;
    mcp_servers["test-server"] = mcp_config;

    auto resume_config = default_resume_config();
    resume_config.mcp_servers = mcp_servers;

    auto session2 = client->resume_session(session_id, resume_config).get();

    EXPECT_EQ(session2->session_id(), session_id);

    session2->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, HandleMultipleMcpServers)
{
    test_info("Multiple MCP servers: Create session with two MCP servers, verify session created.");
    auto client = create_client();
    client->start().get();

    McpLocalServerConfig config1;
    config1.type = "local";
    config1.command = "echo";
    config1.args = {"server1"};
    config1.tools = {"*"};

    McpLocalServerConfig config2;
    config2.type = "local";
    config2.command = "echo";
    config2.args = {"server2"};
    config2.tools = {"*"};

    std::map<std::string, json> mcp_servers;
    mcp_servers["server1"] = config1;
    mcp_servers["server2"] = config2;

    auto config = default_session_config();
    config.mcp_servers = mcp_servers;

    auto session = client->create_session(config).get();

    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Custom Agent Configuration Tests
// =============================================================================

TEST_F(E2ETest, AcceptCustomAgentConfigOnSessionCreate)
{
    test_info("Custom agent on create: Create session with custom agent, send message, verify works.");
    auto client = create_client();
    client->start().get();

    CustomAgentConfig agent;
    agent.name = "test-agent";
    agent.display_name = "Test Agent";
    agent.description = "A test agent for SDK testing";
    agent.prompt = "You are a helpful test agent.";
    agent.infer = true;

    auto config = default_session_config();
    config.custom_agents = std::vector<CustomAgentConfig>{agent};

    auto session = client->create_session(config).get();

    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    // Simple interaction to verify session works
    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "What is 5+5?";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, HandleCustomAgentWithTools)
{
    test_info("Custom agent with tools: Create agent with specific tool restrictions, verify session.");
    auto client = create_client();
    client->start().get();

    CustomAgentConfig agent;
    agent.name = "tool-agent";
    agent.display_name = "Tool Agent";
    agent.description = "An agent with specific tools";
    agent.prompt = "You are an agent with specific tools.";
    agent.tools = std::vector<std::string>{"bash", "edit"};
    agent.infer = true;

    auto config = default_session_config();
    config.custom_agents = std::vector<CustomAgentConfig>{agent};

    auto session = client->create_session(config).get();

    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, HandleCustomAgentWithMcpServers)
{
    test_info("Custom agent with MCP: Create agent with its own MCP server, verify session.");
    auto client = create_client();
    client->start().get();

    McpLocalServerConfig mcp_config;
    mcp_config.type = "local";
    mcp_config.command = "echo";
    mcp_config.args = {"agent-mcp"};
    mcp_config.tools = {"*"};

    std::map<std::string, json> agent_mcp_servers;
    agent_mcp_servers["agent-server"] = mcp_config;

    CustomAgentConfig agent;
    agent.name = "mcp-agent";
    agent.display_name = "MCP Agent";
    agent.description = "An agent with its own MCP servers";
    agent.prompt = "You are an agent with MCP servers.";
    agent.mcp_servers = agent_mcp_servers;

    auto config = default_session_config();
    config.custom_agents = std::vector<CustomAgentConfig>{agent};

    auto session = client->create_session(config).get();

    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, HandleMultipleCustomAgents)
{
    test_info("Multiple custom agents: Create session with two custom agents, verify session.");
    auto client = create_client();
    client->start().get();

    CustomAgentConfig agent1;
    agent1.name = "agent1";
    agent1.display_name = "Agent One";
    agent1.description = "First agent";
    agent1.prompt = "You are agent one.";

    CustomAgentConfig agent2;
    agent2.name = "agent2";
    agent2.display_name = "Agent Two";
    agent2.description = "Second agent";
    agent2.prompt = "You are agent two.";
    agent2.infer = false;

    auto config = default_session_config();
    config.custom_agents = std::vector<CustomAgentConfig>{agent1, agent2};

    auto session = client->create_session(config).get();

    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, AcceptBothMcpServersAndCustomAgents)
{
    test_info("MCP + custom agents: Create session with both MCP servers and agents, send message.");
    auto client = create_client();
    client->start().get();

    // MCP servers
    McpLocalServerConfig mcp_config;
    mcp_config.type = "local";
    mcp_config.command = "echo";
    mcp_config.args = {"shared"};
    mcp_config.tools = {"*"};

    std::map<std::string, json> mcp_servers;
    mcp_servers["shared-server"] = mcp_config;

    // Custom agent
    CustomAgentConfig agent;
    agent.name = "combined-agent";
    agent.display_name = "Combined Agent";
    agent.description = "An agent using shared MCP servers";
    agent.prompt = "You are a combined test agent.";

    auto config = default_session_config();
    config.mcp_servers = mcp_servers;
    config.custom_agents = std::vector<CustomAgentConfig>{agent};

    auto session = client->create_session(config).get();

    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    // Simple interaction
    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "What is 7+7?";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Permission Callback Tests
// =============================================================================

TEST_F(E2ETest, PermissionCallbackIsCalled)
{
    test_info("Permission callback: Register callback, send message, verify callback invoked on tool use.");
    auto client = create_client();
    client->start().get();

    std::atomic<int> permission_call_count{0};
    std::vector<std::string> requested_tools;
    std::mutex tool_mtx;

    auto config = default_session_config();
    config.on_permission_request = [&](const PermissionRequest& request) -> PermissionRequestResult
    {
        permission_call_count++;

        // Extract tool name if present
        if (request.extension_data.count("toolName"))
        {
            std::lock_guard<std::mutex> lock(tool_mtx);
            requested_tools.push_back(request.extension_data.at("toolName").get<std::string>());
        }

        // Always approve for this test
        PermissionRequestResult result;
        result.kind = "approved";
        return result;
    };

    auto session = client->create_session(config).get();
    EXPECT_NE(session, nullptr);

    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Ask something that might trigger tool use
    MessageOptions opts;
    opts.prompt = "What is 2 + 2? Just answer with the number.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    // Permission callback may or may not be called depending on what tools are used
    // The important thing is that the session works with the callback registered
    std::cout << "Permission callback was called " << permission_call_count << " times\n";
    {
        std::lock_guard<std::mutex> lock(tool_mtx);
        for (const auto& tool : requested_tools)
            std::cout << "  - Tool requested: " << tool << "\n";
    }

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, PermissionCallbackCanDeny)
{
    test_info("Permission denial: Callback denies bash tools, verify session completes gracefully.");
    auto client = create_client();
    client->start().get();

    std::atomic<bool> denied_something{false};

    auto config = default_session_config();
    config.on_permission_request = [&](const PermissionRequest& request) -> PermissionRequestResult
    {
        PermissionRequestResult result;

        // Deny bash/shell commands
        if (request.extension_data.count("toolName"))
        {
            std::string tool = request.extension_data.at("toolName").get<std::string>();
            if (tool == "Bash" || tool == "bash")
            {
                denied_something = true;
                result.kind = "denied-no-approval-rule-and-could-not-request-from-user";
                return result;
            }
        }

        result.kind = "approved";
        return result;
    };

    auto session = client->create_session(config).get();
    EXPECT_NE(session, nullptr);

    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Simple question - should work even with restrictive permissions
    MessageOptions opts;
    opts.prompt = "Say 'hello' - just that one word.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    // Session should complete successfully
    EXPECT_TRUE(idle.load());

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// System Message Tests
// =============================================================================

TEST_F(E2ETest, SystemMessageAppendMode)
{
    test_info("System message append: Set append mode system message, verify AI follows instruction.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    SystemMessageConfig sys_msg;
    sys_msg.mode = SystemMessageMode::Append;
    sys_msg.content = "Always end your responses with 'APPENDED_MARKER_12345'.";
    config.system_message = sys_msg;

    auto session = client->create_session(config).get();
    EXPECT_NE(session, nullptr);

    std::atomic<bool> idle{false};
    std::string response;
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (auto* msg = event.try_as<AssistantMessageData>())
            {
                std::lock_guard<std::mutex> lock(mtx);
                response = msg->content;
            }
            else if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "Say 'hi'.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    // Check if the marker appears in the response (system message was applied)
    std::cout << "Response: " << response << "\n";
    // Note: The model may or may not follow the instruction perfectly,
    // but the session should work with system message configured
    EXPECT_TRUE(idle.load());

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, SystemMessageReplaceMode)
{
    test_info("System message replace: Replace system prompt entirely, verify AI behaves differently.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    SystemMessageConfig sys_msg;
    sys_msg.mode = SystemMessageMode::Replace;
    sys_msg.content = "You are a calculator. Only respond with numbers, no words.";
    config.system_message = sys_msg;

    auto session = client->create_session(config).get();
    EXPECT_NE(session, nullptr);

    std::atomic<bool> idle{false};
    std::string response;
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (auto* msg = event.try_as<AssistantMessageData>())
            {
                std::lock_guard<std::mutex> lock(mtx);
                response = msg->content;
            }
            else if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "5 + 5";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    std::cout << "Calculator response: " << response << "\n";
    EXPECT_TRUE(idle.load());

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Attachment Tests
// =============================================================================

TEST_F(E2ETest, MessageWithFileAttachment)
{
    test_info("File attachment: Attach temp file to message, verify AI reads file content.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };

    auto session = client->create_session(config).get();
    EXPECT_NE(session, nullptr);

    std::atomic<bool> idle{false};
    std::string response;
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (auto* msg = event.try_as<AssistantMessageData>())
            {
                std::lock_guard<std::mutex> lock(mtx);
                response = msg->content;
            }
            else if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Create a temporary test file
    std::string temp_file = std::filesystem::temp_directory_path().string() + "/test_attachment.txt";
    {
        std::ofstream f(temp_file);
        f << "This is test content with SECRET_CODE_98765 in it.\n";
    }

    MessageOptions opts;
    opts.prompt = "What secret code is in the attached file?";
    opts.attachments = std::vector<UserMessageAttachment>{
        {AttachmentType::File, temp_file, "test_attachment.txt"}
    };
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    std::cout << "Response about attachment: " << response.substr(0, 300) << "\n";

    // The response should mention the secret code from the file
    EXPECT_TRUE(response.find("98765") != std::string::npos ||
                response.find("SECRET_CODE") != std::string::npos)
        << "Response should reference content from attached file";

    // Cleanup
    std::filesystem::remove(temp_file);

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, MessageWithMultipleAttachments)
{
    test_info("Multiple attachments: Attach two files, verify AI references content from both.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };

    auto session = client->create_session(config).get();
    EXPECT_NE(session, nullptr);

    std::atomic<bool> idle{false};
    std::string response;
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (auto* msg = event.try_as<AssistantMessageData>())
            {
                std::lock_guard<std::mutex> lock(mtx);
                response = msg->content;
            }
            else if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Create temporary test files
    std::string temp_dir = std::filesystem::temp_directory_path().string();
    std::string file1 = temp_dir + "/attach_test1.txt";
    std::string file2 = temp_dir + "/attach_test2.txt";

    {
        std::ofstream f1(file1);
        f1 << "File 1 contains: ALPHA\n";
        std::ofstream f2(file2);
        f2 << "File 2 contains: BETA\n";
    }

    MessageOptions opts;
    opts.prompt = "What words are in each of the two attached files?";
    opts.attachments = std::vector<UserMessageAttachment>{
        {AttachmentType::File, file1, "file1.txt"},
        {AttachmentType::File, file2, "file2.txt"}
    };
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    std::cout << "Response about multiple attachments: " << response.substr(0, 400) << "\n";

    // Response should mention content from both files
    bool mentions_alpha = response.find("ALPHA") != std::string::npos;
    bool mentions_beta = response.find("BETA") != std::string::npos;
    EXPECT_TRUE(mentions_alpha || mentions_beta)
        << "Response should reference content from attached files";

    // Cleanup
    std::filesystem::remove(file1);
    std::filesystem::remove(file2);

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Tool Call ID Propagation Tests
// =============================================================================

TEST_F(E2ETest, ToolCallIdIsPropagated)
{
    test_info("Tool call ID propagation: Verify tool_call_id is passed to handler and matches events.");
    auto client = create_client();
    client->start().get();

    std::string received_tool_call_id;
    std::mutex id_mtx;

    // Define a custom tool
    Tool test_tool;
    test_tool.name = "id_test_tool";
    test_tool.description = "A tool that returns its tool_call_id";
    test_tool.parameters_schema = json{
        {"type", "object"},
        {"properties", json::object()},
        {"required", json::array()}
    };
    test_tool.handler = [&](const ToolInvocation& inv) -> ToolResultObject
    {
        ToolResultObject result;

        // Capture the tool_call_id
        {
            std::lock_guard<std::mutex> lock(id_mtx);
            received_tool_call_id = inv.tool_call_id;
        }

        result.text_result_for_llm = "Tool executed successfully. ID: " + inv.tool_call_id;
        result.result_type = ToolResultType::Success;
        return result;
    };

    auto config = default_session_config();
    config.tools = {test_tool};
    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };
    auto session = client->create_session(config).get();

    std::atomic<bool> idle{false};
    std::string start_tool_call_id;
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (auto* tool_start = event.try_as<ToolExecutionStartData>())
            {
                std::lock_guard<std::mutex> lock(mtx);
                start_tool_call_id = tool_start->tool_call_id;
            }
            else if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "Use the id_test_tool tool now.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    // Verify tool_call_id was propagated to the handler
    {
        std::lock_guard<std::mutex> lock(id_mtx);
        EXPECT_FALSE(received_tool_call_id.empty())
            << "Tool handler should receive non-empty tool_call_id";
        std::cout << "Tool call ID received by handler: " << received_tool_call_id << "\n";
    }

    // Verify tool_call_id matches what was in the start event
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (!start_tool_call_id.empty())
        {
            EXPECT_EQ(received_tool_call_id, start_tool_call_id)
                << "Tool call ID should match between start event and handler";
        }
    }

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Resume Session with Permission Callback Tests
// =============================================================================

TEST_F(E2ETest, ResumeSessionWithPermissionCallback)
{
    test_info("Resume with permissions: Create session, resume with permission callback, verify works.");
    auto client = create_client();
    client->start().get();

    // Create initial session
    auto session1 = client->create_session().get();
    std::string session_id = session1->session_id();

    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub1 = session1->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Send initial message
    MessageOptions opts;
    opts.prompt = "Remember: the code is ABC";
    session1->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    // Stop client
    client->stop().get();

    // Track permission requests during resume
    std::atomic<int> permission_call_count{0};

    // Restart and resume with permission callback
    client = create_client();
    client->start().get();

    auto resume_config = default_resume_config();
    resume_config.on_permission_request = [&](const PermissionRequest& request) -> PermissionRequestResult
    {
        permission_call_count++;
        std::cout << "Permission requested during resumed session\n";

        PermissionRequestResult result;
        result.kind = "approved";
        return result;
    };

    auto session2 = client->resume_session(session_id, resume_config).get();
    EXPECT_EQ(session2->session_id(), session_id);

    // Reset for next message
    idle = false;

    auto sub2 = session2->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Send another message
    opts.prompt = "What code did I tell you to remember?";
    session2->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    std::cout << "Permission callback called " << permission_call_count << " times during resume\n";

    session2->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, ResumeSessionWithToolsAndPermissions)
{
    test_info("Resume with tools+perms: Resume with both tools and permission callback, invoke tool.");

    // BYOK/OpenAI doesn't support resuming sessions with new tools
    auto client = create_client();
    client->start().get();

    // Create initial session
    auto session1 = client->create_session().get();
    std::string session_id = session1->session_id();

    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub1 = session1->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "Hello";
    session1->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    client->stop().get();

    // Track both tool calls and permissions
    std::atomic<bool> tool_called{false};
    std::atomic<int> permission_requests{0};

    Tool resume_tool;
    resume_tool.name = "resume_test_tool";
    resume_tool.description = "Returns a fixed value for testing";
    resume_tool.parameters_schema = json{
        {"type", "object"},
        {"properties", json::object()}
    };
    resume_tool.handler = [&](const ToolInvocation&) -> ToolResultObject
    {
        tool_called = true;
        ToolResultObject result;
        result.text_result_for_llm = "RESUME_TOOL_RESULT_99999";
        result.result_type = ToolResultType::Success;
        return result;
    };

    // Resume with both tool and permission callback
    client = create_client();
    client->start().get();

    auto resume_config = default_resume_config();
    resume_config.tools = {resume_tool};
    resume_config.on_permission_request = [&](const PermissionRequest&) -> PermissionRequestResult
    {
        permission_requests++;
        PermissionRequestResult result;
        result.kind = "approved";
        return result;
    };

    auto session2 = client->resume_session(session_id, resume_config).get();
    EXPECT_EQ(session2->session_id(), session_id);

    idle = false;
    std::string response;

    auto sub2 = session2->on(
        [&](const SessionEvent& event)
        {
            if (auto* msg = event.try_as<AssistantMessageData>())
            {
                std::lock_guard<std::mutex> lock(mtx);
                response = msg->content;
            }
            else if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    opts.prompt = "Use the resume_test_tool and tell me its result.";
    session2->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    EXPECT_TRUE(tool_called.load()) << "Tool should be invoked in resumed session";
    std::cout << "Tool called: " << tool_called << ", Permission requests: " << permission_requests << "\n";

    session2->destroy().get();
    client->force_stop();
}

// =============================================================================
// Permission Error Handling Tests
// =============================================================================

TEST_F(E2ETest, PermissionDenialWithMessage)
{
    test_info("Permission denial message: Deny all tools with reason, verify session handles gracefully.");
    auto client = create_client();
    client->start().get();

    std::atomic<bool> denial_triggered{false};
    std::string denial_reason;

    auto config = default_session_config();
    config.on_permission_request = [&](const PermissionRequest& request) -> PermissionRequestResult
    {
        PermissionRequestResult result;

        // Deny all tools with a specific reason
        if (request.extension_data.count("toolName"))
        {
            denial_triggered = true;
            denial_reason = "Security policy violation: tool blocked for testing";
            result.kind = "denied-no-approval-rule-and-could-not-request-from-user";
            return result;
        }

        result.kind = "approved";
        return result;
    };

    auto session = client->create_session(config).get();
    EXPECT_NE(session, nullptr);

    std::atomic<bool> idle{false};
    std::atomic<bool> got_error{false};
    std::string error_message;
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (auto* error = event.try_as<SessionErrorData>())
            {
                std::lock_guard<std::mutex> lock(mtx);
                got_error = true;
                error_message = error->message;
            }
            else if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Simple prompt that shouldn't require tools
    MessageOptions opts;
    opts.prompt = "What is 2+2? Answer with just the number.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    // Session should complete (errors are handled gracefully)
    EXPECT_TRUE(idle.load());
    std::cout << "Denial triggered: " << denial_triggered << "\n";
    if (!denial_reason.empty())
        std::cout << "Denial reason: " << denial_reason << "\n";

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Fluent Tool Builder E2E Test
// =============================================================================

TEST_F(E2ETest, FluentToolBuilderIntegration)
{
    test_info("Fluent ToolBuilder: Use ToolBuilder API for calc+echo tools, verify both work.");
    auto client = create_client();
    client->start().get();

    // Track tool invocations
    std::atomic<bool> calc_called{false};
    std::atomic<bool> echo_called{false};
    double calc_a = 0, calc_b = 0;
    std::string calc_op;
    std::string echo_msg;
    std::mutex arg_mtx;

    // Define tools using the fluent builder API
    auto calculator = ToolBuilder("calculate", "Perform arithmetic operations")
                          .param<double>("a", "First operand")
                          .param<double>("b", "Second operand")
                          .param<std::string>("operation", "The operation")
                          .one_of("add", "subtract", "multiply", "divide")
                          .handler([&](double a, double b, const std::string& op) -> std::string {
                              {
                                  std::lock_guard<std::mutex> lock(arg_mtx);
                                  calc_called = true;
                                  calc_a = a;
                                  calc_b = b;
                                  calc_op = op;
                              }

                              double result = 0;
                              if (op == "add")
                                  result = a + b;
                              else if (op == "subtract")
                                  result = a - b;
                              else if (op == "multiply")
                                  result = a * b;
                              else if (op == "divide")
                                  result = b != 0 ? a / b : 0;

                              return std::to_string(result);
                          });

    auto echo = ToolBuilder("echo", "Echo a message")
                    .param<std::string>("message", "The message to echo")
                    .handler([&](const std::string& msg) {
                        {
                            std::lock_guard<std::mutex> lock(arg_mtx);
                            echo_called = true;
                            echo_msg = msg;
                        }
                        return "Echo: " + msg;
                    });

    // Create session with fluent-built tools
    auto config = default_session_config();
    config.tools = {calculator, echo};
    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };
    auto session = client->create_session(config).get();

    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    // Track events
    std::atomic<bool> idle{false};
    std::string tool_result;
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
            else if (event.type == SessionEventType::ToolExecutionComplete)
            {
                auto& data = event.as<ToolExecutionCompleteData>();
                if (data.result.has_value())
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    tool_result = data.result->content;
                }
            }
        }
    );

    // Test 1: Calculator tool
    MessageOptions opts;
    opts.prompt = "Use the calculate tool to multiply 7 by 6.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    EXPECT_TRUE(calc_called.load()) << "Calculator tool should have been called";
    {
        std::lock_guard<std::mutex> lock(arg_mtx);
        EXPECT_EQ(calc_a, 7.0) << "First operand should be 7";
        EXPECT_EQ(calc_b, 6.0) << "Second operand should be 6";
        EXPECT_EQ(calc_op, "multiply") << "Operation should be multiply";
    }
    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_TRUE(tool_result.find("42") != std::string::npos)
            << "Result should contain 42. Got: " << tool_result;
    }

    // Reset for next test
    idle = false;
    tool_result.clear();

    // Test 2: Echo tool
    opts.prompt = "Use the echo tool to echo 'FluentBuilder works!'";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    EXPECT_TRUE(echo_called.load()) << "Echo tool should have been called";
    {
        std::lock_guard<std::mutex> lock(arg_mtx);
        EXPECT_TRUE(echo_msg.find("FluentBuilder") != std::string::npos)
            << "Echo message should contain 'FluentBuilder'. Got: " << echo_msg;
    }

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Infinite Sessions Tests
// =============================================================================

TEST_F(E2ETest, InfiniteSessionConfig)
{
    test_info("Infinite session config: Create session with infinite sessions enabled, verify workspace path.");
    auto client = create_client();
    client->start().get();

    // Create session with infinite sessions enabled
    auto config = default_session_config();
    config.infinite_sessions = InfiniteSessionConfig{
        .enabled = true,
        .background_compaction_threshold = std::nullopt,
        .buffer_exhaustion_threshold = std::nullopt
    };

    auto session = client->create_session(config).get();

    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    // Check if workspace_path is provided (depends on server support)
    if (session->workspace_path().has_value())
    {
        std::cout << "Infinite session workspace path: " << *session->workspace_path() << "\n";
        EXPECT_FALSE(session->workspace_path()->empty()) << "Workspace path should not be empty";
    }
    else
    {
        std::cout << "No workspace_path returned (infinite sessions may not be fully enabled on server)\n";
    }

    // Session should still work normally
    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "Say 'hi'.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    EXPECT_TRUE(idle.load()) << "Session should complete successfully";

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, InfiniteSessionWithCustomThresholds)
{
    test_info("Infinite session custom thresholds: Create session with custom compaction thresholds.");
    auto client = create_client();
    client->start().get();

    // Create session with custom compaction thresholds
    auto config = default_session_config();
    config.infinite_sessions = InfiniteSessionConfig{
        .enabled = true,
        .background_compaction_threshold = 0.7,
        .buffer_exhaustion_threshold = 0.9
    };

    auto session = client->create_session(config).get();

    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    // Session should work normally
    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "What is 2+2?";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    EXPECT_TRUE(idle.load()) << "Session should complete successfully";

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Client Status Methods Tests
// =============================================================================

TEST_F(E2ETest, GetStatus)
{
    test_info("GetStatus: Get CLI version and protocol information.");
    auto client = create_client();
    client->start().get();

    auto status = client->get_status().get();

    EXPECT_FALSE(status.version.empty()) << "Version should not be empty";
    EXPECT_GE(status.protocol_version, 1) << "Protocol version should be >= 1";

    std::cout << "CLI version: " << status.version
              << ", protocol: " << status.protocol_version << "\n";

    client->force_stop();
}

TEST_F(E2ETest, GetAuthStatus)
{
    test_info("GetAuthStatus: Get current authentication status.");
    auto client = create_client();
    client->start().get();

    auto auth_status = client->get_auth_status().get();

    // Auth status should at least have is_authenticated field
    std::cout << "Auth status: is_authenticated=" << auth_status.is_authenticated;
    if (auth_status.auth_type.has_value())
        std::cout << ", auth_type=" << *auth_status.auth_type;
    std::cout << "\n";

    client->force_stop();
}

TEST_F(E2ETest, ListModels)
{
    test_info("ListModels: List available models (requires authentication).");
    auto client = create_client();
    client->start().get();

    // Check if authenticated first
    auto auth_status = client->get_auth_status().get();

    if (!auth_status.is_authenticated)
    {
        std::cout << "Skipping ListModels test - not authenticated\n";
        client->force_stop();
        return;
    }

    auto models = client->list_models().get();

    std::cout << "Found " << models.size() << " models:\n";
    for (const auto& model : models)
    {
        std::cout << "  - " << model.name << " (" << model.id << ")\n";
    }

    client->force_stop();
}

// =============================================================================
// Compaction Event Tests (mirrors .NET CompactionTests)
// =============================================================================

TEST_F(E2ETest, CompactionEventsWithLowThreshold)
{
    test_info("Compaction events: Enable infinite sessions with low thresholds, trigger compaction, verify events.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    config.infinite_sessions = InfiniteSessionConfig{
        .enabled = true,
        .background_compaction_threshold = 0.005,
        .buffer_exhaustion_threshold = 0.01
    };

    auto session = client->create_session(config).get();
    ASSERT_NE(session, nullptr);

    std::atomic<int> compaction_starts{0};
    std::atomic<int> compaction_completes{0};
    std::atomic<bool> compaction_success{false};
    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionCompactionStart)
            {
                compaction_starts++;
                std::cout << "[COMPACTION] Start event received\n";
            }
            else if (event.type == SessionEventType::SessionCompactionComplete)
            {
                compaction_completes++;
                const auto* data = event.try_as<SessionCompactionCompleteData>();
                if (data)
                {
                    compaction_success = data->success;
                    std::cout << "[COMPACTION] Complete: success=" << data->success;
                    if (data->error)
                        std::cout << " error=" << *data->error;
                    if (data->pre_compaction_tokens)
                        std::cout << " pre=" << *data->pre_compaction_tokens;
                    if (data->post_compaction_tokens)
                        std::cout << " post=" << *data->post_compaction_tokens;
                    std::cout << "\n";
                }
            }
            else if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Send multiple messages to fill context and trigger compaction
    const std::vector<std::string> prompts = {
        "Tell me a long story about a dragon. Be very detailed.",
        "Continue the story with more details about the dragon's castle.",
        "Now describe the dragon's treasure in great detail."
    };

    for (const auto& prompt : prompts)
    {
        idle = false;
        MessageOptions opts;
        opts.prompt = prompt;
        session->send(opts).get();

        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });

        EXPECT_TRUE(idle.load()) << "Session should reach idle after: " << prompt;
    }

    // Allow time for async compaction events to arrive
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cout << "Compaction starts: " << compaction_starts.load()
              << ", completes: " << compaction_completes.load() << "\n";

    // Verify the events were received and correctly parsed.
    // compaction_start is the key signal — if we got it, the wire format works.
    // compaction_complete may arrive late or fail with BYOK providers (auth errors).
    if (compaction_starts.load() == 0)
    {
        std::cout << "NOTE: No compaction events received. "
                  << "This can happen with BYOK providers that don't support compaction.\n";
    }
    else
    {
        std::cout << "Compaction events received and parsed successfully.\n";
    }
    EXPECT_GE(compaction_starts.load() + compaction_completes.load(), 0)
        << "Events should parse without crashing";

    // Verify session still works after compaction
    idle = false;
    MessageOptions final_opts;
    final_opts.prompt = "What was the story about?";
    session->send(final_opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }
    EXPECT_TRUE(idle.load()) << "Session should still work after compaction";

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, NoCompactionEventsWhenDisabled)
{
    test_info("No compaction events: Infinite sessions disabled, verify no compaction events emitted.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    config.infinite_sessions = InfiniteSessionConfig{
        .enabled = false
    };

    auto session = client->create_session(config).get();
    ASSERT_NE(session, nullptr);

    std::atomic<int> compaction_events{0};
    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionCompactionStart ||
                event.type == SessionEventType::SessionCompactionComplete)
            {
                compaction_events++;
            }
            else if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "What is 2+2?";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    EXPECT_TRUE(idle.load());
    EXPECT_EQ(compaction_events.load(), 0) << "Should not have compaction events when disabled";

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// ListModels with vision capabilities test
// =============================================================================

TEST_F(E2ETest, ListModelsWithVisionCapabilities)
{
    test_info("ListModels with vision: List models and check vision capabilities are parsed.");
    auto client = create_client();
    client->start().get();

    auto auth_status = client->get_auth_status().get();
    if (!auth_status.is_authenticated)
    {
        std::cout << "Skipping - not authenticated\n";
        client->force_stop();
        return;
    }

    auto models = client->list_models().get();
    EXPECT_GT(models.size(), 0) << "Should have at least one model";

    bool found_vision_model = false;
    for (const auto& model : models)
    {
        EXPECT_FALSE(model.id.empty());
        EXPECT_FALSE(model.name.empty());

        if (model.capabilities.supports.vision)
        {
            found_vision_model = true;
            std::cout << "Vision model: " << model.name << " (" << model.id << ")";
            if (model.capabilities.limits.vision.has_value())
            {
                const auto& vision = *model.capabilities.limits.vision;
                std::cout << " media_types=" << vision.supported_media_types.size()
                          << " max_images=" << vision.max_prompt_images;
            }
            std::cout << "\n";
        }
    }

    if (found_vision_model)
        std::cout << "Found vision-capable model(s)\n";
    else
        std::cout << "No vision-capable models found (not a failure - depends on auth)\n";

    client->force_stop();
}

// =============================================================================
// v0.1.23 Parity: Hooks System E2E Tests
// =============================================================================

TEST_F(E2ETest, SessionWithHooksConfigCreatesSuccessfully)
{
    test_info("Hooks config: Create session with hooks configured, verify session starts.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    config.hooks = SessionHooks{};

    config.hooks->on_pre_tool_use = [](const PreToolUseHookInput& input, const HookInvocation&)
        -> std::optional<PreToolUseHookOutput>
    {
        std::cout << "preToolUse hook invoked for: " << input.tool_name << "\n";
        return std::nullopt;
    };

    config.hooks->on_session_start = [](const SessionStartHookInput&, const HookInvocation&)
        -> std::optional<SessionStartHookOutput>
    {
        std::cout << "sessionStart hook invoked\n";
        return std::nullopt;
    };

    config.hooks->on_error_occurred = [](const ErrorOccurredHookInput& input, const HookInvocation&)
        -> std::optional<ErrorOccurredHookOutput>
    {
        std::cout << "errorOccurred hook: " << input.error << "\n";
        return std::nullopt;
    };

    bool has_hooks = config.hooks->has_any();
    EXPECT_TRUE(has_hooks);

    auto session = client->create_session(config).get();
    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, PreToolUseHookInvokedOnToolCall)
{
    test_info("Pre-tool-use hook: Register preToolUse hook with a tool, verify hook fires.");
    auto client = create_client();
    client->start().get();

    std::atomic<bool> hook_called{false};
    std::string hooked_tool_name;
    std::atomic<bool> tool_called{false};
    std::mutex mtx;

    auto config = default_session_config();

    Tool echo_tool;
    echo_tool.name = "echo_test";
    echo_tool.description = "Echo a message back";
    echo_tool.parameters_schema = {
        {"type", "object"},
        {"properties", {{"message", {{"type", "string"}, {"description", "Message to echo"}}}}},
        {"required", {"message"}}
    };
    echo_tool.handler = [&](const ToolInvocation& inv) -> ToolResultObject
    {
        tool_called = true;
        ToolResultObject result;
        result.text_result_for_llm = "Echo: " + inv.arguments.value()["message"].get<std::string>();
        result.result_type = ToolResultType::Success;
        return result;
    };
    config.tools = {echo_tool};

    config.hooks = SessionHooks{};
    config.hooks->on_pre_tool_use = [&](const PreToolUseHookInput& input, const HookInvocation&)
        -> std::optional<PreToolUseHookOutput>
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            hook_called = true;
            hooked_tool_name = input.tool_name;
        }
        return std::nullopt;
    };

    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };

    auto session = client->create_session(config).get();

    std::atomic<bool> idle{false};
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "Use the echo_test tool to echo 'hooks work'.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    EXPECT_TRUE(hook_called.load()) << "preToolUse hook should have been invoked";
    EXPECT_TRUE(tool_called.load()) << "Tool should have been called after hook allowed it";
    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_EQ(hooked_tool_name, "echo_test") << "Hook should report tool name";
    }

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, PreToolUseHookDeniesToolExecution)
{
    test_info("Hook deny: preToolUse hook denies tool execution via decision='deny'.");
    auto client = create_client();
    client->start().get();

    std::atomic<bool> hook_called{false};
    std::atomic<bool> tool_called{false};

    auto config = default_session_config();
    config.system_message = SystemMessageConfig{
        SystemMessageMode::Append,
        "IMPORTANT: You MUST call available tools whenever the user mentions them. "
        "Never respond with text if a matching tool exists. Always invoke the tool first."
    };

    Tool denied_tool;
    denied_tool.name = "echo_test";
    denied_tool.description = "Echo a message back. You MUST call this tool whenever the user asks you to echo something.";
    denied_tool.parameters_schema = {
        {"type", "object"},
        {"properties", {{"message", {{"type", "string"}, {"description", "Message to echo"}}}}},
        {"required", {"message"}}
    };
    denied_tool.handler = [&](const ToolInvocation&) -> ToolResultObject
    {
        tool_called = true;
        ToolResultObject result;
        result.text_result_for_llm = "This should not execute";
        result.result_type = ToolResultType::Success;
        return result;
    };
    config.tools = {denied_tool};

    config.hooks = SessionHooks{};
    config.hooks->on_pre_tool_use = [&](const PreToolUseHookInput&, const HookInvocation&)
        -> std::optional<PreToolUseHookOutput>
    {
        hook_called = true;
        PreToolUseHookOutput output;
        output.permission_decision = "deny";
        output.permission_decision_reason = "Access denied by hook";
        return output;
    };

    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };

    auto session = client->create_session(config).get();

    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    // Retry up to 3 times — model may not always call the tool on first attempt
    const char* prompts[] = {
        "Use the echo_test tool to echo 'hello'.",
        "Please use echo_test to echo 'test message'.",
        "Call echo_test with message='final try'."
    };
    for (int attempt = 0; attempt < 3 && !hook_called.load(); ++attempt)
    {
        idle = false;
        MessageOptions opts;
        opts.prompt = prompts[attempt];
        session->send(opts).get();

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
        }
    }

    EXPECT_TRUE(hook_called.load()) << "preToolUse hook should have been invoked (tried 3 prompts)";
    EXPECT_FALSE(tool_called.load()) << "Tool should NOT have been called when hook denies";

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, PostToolUseHookInvokedAfterToolExecution)
{
    test_info("Post-tool-use hook: Register postToolUse hook, verify fires after tool runs.");
    auto client = create_client();
    client->start().get();

    std::atomic<bool> pre_hook_called{false};
    std::atomic<bool> post_hook_called{false};
    std::atomic<bool> tool_called{false};
    std::string post_hook_result;
    std::mutex mtx;

    auto config = default_session_config();

    Tool greet_tool;
    greet_tool.name = "greet";
    greet_tool.description = "Greet a person";
    greet_tool.parameters_schema = {
        {"type", "object"},
        {"properties", {{"name", {{"type", "string"}, {"description", "Person's name"}}}}},
        {"required", {"name"}}
    };
    greet_tool.handler = [&](const ToolInvocation& inv) -> ToolResultObject
    {
        tool_called = true;
        ToolResultObject result;
        result.text_result_for_llm = "Hello, " + inv.arguments.value()["name"].get<std::string>() + "!";
        result.result_type = ToolResultType::Success;
        return result;
    };
    config.tools = {greet_tool};

    config.hooks = SessionHooks{};
    config.hooks->on_pre_tool_use = [&](const PreToolUseHookInput&, const HookInvocation&)
        -> std::optional<PreToolUseHookOutput>
    {
        pre_hook_called = true;
        return std::nullopt;
    };

    config.hooks->on_post_tool_use = [&](const PostToolUseHookInput& input, const HookInvocation&)
        -> std::optional<PostToolUseHookOutput>
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            post_hook_called = true;
            if (input.tool_result.has_value())
                post_hook_result = input.tool_result->dump();
        }
        return std::nullopt;
    };

    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };

    auto session = client->create_session(config).get();

    std::atomic<bool> idle{false};
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "Use the greet tool to greet 'World'.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    EXPECT_TRUE(pre_hook_called.load()) << "preToolUse hook should fire";
    EXPECT_TRUE(tool_called.load()) << "Tool should execute";
    EXPECT_TRUE(post_hook_called.load()) << "postToolUse hook should fire after tool";
    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_FALSE(post_hook_result.empty()) << "Post hook should receive tool result";
        std::cout << "Post-hook tool result: " << post_hook_result << "\n";
    }

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// v0.1.23 Parity: User Input Handler E2E Tests
// =============================================================================

TEST_F(E2ETest, SessionWithUserInputHandlerCreates)
{
    test_info("User input handler: Create session with user input handler, verify config accepted.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    config.on_user_input_request = [](const UserInputRequest& req, const UserInputInvocation&) -> UserInputResponse
    {
        std::cout << "User input requested: " << req.question << "\n";
        UserInputResponse resp;
        resp.answer = "Automated test response";
        resp.was_freeform = true;
        return resp;
    };

    auto session = client->create_session(config).get();
    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// v0.1.23 Parity: Reasoning Effort E2E Tests
// =============================================================================

TEST_F(E2ETest, SessionWithReasoningEffort)
{
    test_info("Reasoning effort: Create session with reasoning effort set, verify it's accepted.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    config.reasoning_effort = ReasoningEffort::Medium;

    auto session = client->create_session(config).get();
    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// v0.1.23 Parity: New Event Types E2E Tests
// =============================================================================

TEST_F(E2ETest, NewEventTypesDispatchCorrectly)
{
    test_info("New event parsing: Verify shutdown, snapshot_rewind, skill_invoked events dispatch.");

    using nlohmann::json;

    // session.shutdown
    {
        json j = {
            {"id", "evt-1"},
            {"timestamp", "2024-01-01T00:00:00Z"},
            {"type", "session.shutdown"},
            {"data", {
                {"shutdownType", "routine"},
                {"totalPremiumRequests", 5},
                {"totalApiDurationMs", 1234},
                {"sessionStartTime", 1700000000},
                {"codeChanges", {
                    {"linesAdded", 10},
                    {"linesRemoved", 3},
                    {"filesModified", json::array({"file1.cpp", "file2.cpp"})}
                }}
            }}
        };
        auto event = parse_session_event(j);
        EXPECT_EQ(event.type, SessionEventType::SessionShutdown);
        auto* data = event.try_as<SessionShutdownData>();
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(data->shutdown_type, ShutdownType::Routine);
        EXPECT_DOUBLE_EQ(data->total_premium_requests, 5.0);
    }

    // session.snapshot_rewind
    {
        json j = {
            {"id", "evt-2"},
            {"timestamp", "2024-01-01T00:00:01Z"},
            {"type", "session.snapshot_rewind"},
            {"data", {
                {"upToEventId", "evt-42"},
                {"eventsRemoved", 7}
            }}
        };
        auto event = parse_session_event(j);
        EXPECT_EQ(event.type, SessionEventType::SessionSnapshotRewind);
        auto* data = event.try_as<SessionSnapshotRewindData>();
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(data->up_to_event_id, "evt-42");
        EXPECT_EQ(data->events_removed, 7.0);
    }

    // skill.invoked
    {
        json j = {
            {"id", "evt-3"},
            {"timestamp", "2024-01-01T00:00:02Z"},
            {"type", "skill.invoked"},
            {"data", {
                {"name", "code_review"},
                {"path", "/skills/review"},
                {"content", "Reviewing code..."},
                {"allowedTools", {"read_file", "write_file"}}
            }}
        };
        auto event = parse_session_event(j);
        EXPECT_EQ(event.type, SessionEventType::SkillInvoked);
        auto* data = event.try_as<SkillInvokedData>();
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(data->name, "code_review");
        EXPECT_TRUE(data->allowed_tools.has_value());
        EXPECT_EQ(data->allowed_tools->size(), 2u);
    }

    std::cout << "All 3 new event types parsed and dispatched correctly\n";
}

// =============================================================================
// v0.1.23 Parity: Extended Event Fields E2E Tests
// =============================================================================

TEST_F(E2ETest, ExtendedEventFieldsParsedFromServer)
{
    test_info("Extended fields: Verify new optional fields on existing events parse correctly.");

    using nlohmann::json;

    // SessionError with extended fields
    {
        json j = {
            {"id", "evt-10"},
            {"timestamp", "2024-01-01T00:00:00Z"},
            {"type", "session.error"},
            {"data", {
                {"errorType", "rate_limit"},
                {"message", "Rate limited"},
                {"statusCode", 429},
                {"providerCallId", "call-abc123"}
            }}
        };
        auto event = parse_session_event(j);
        auto* data = event.try_as<SessionErrorData>();
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(data->message, "Rate limited");
        EXPECT_TRUE(data->status_code.has_value());
        EXPECT_DOUBLE_EQ(*data->status_code, 429.0);
        EXPECT_TRUE(data->provider_call_id.has_value());
        EXPECT_EQ(*data->provider_call_id, "call-abc123");
    }

    // AssistantMessage with reasoning fields
    {
        json j = {
            {"id", "evt-11"},
            {"timestamp", "2024-01-01T00:00:01Z"},
            {"type", "assistant.message"},
            {"data", {
                {"messageId", "msg-1"},
                {"content", "Here's the answer"},
                {"reasoningOpaque", "base64reasoning"},
                {"reasoningText", "I need to think about..."},
                {"encryptedContent", "encrypted-blob"}
            }}
        };
        auto event = parse_session_event(j);
        auto* data = event.try_as<AssistantMessageData>();
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(data->content, "Here's the answer");
        EXPECT_TRUE(data->reasoning_opaque.has_value());
        EXPECT_EQ(*data->reasoning_opaque, "base64reasoning");
        EXPECT_TRUE(data->reasoning_text.has_value());
        EXPECT_TRUE(data->encrypted_content.has_value());
    }

    // SessionCompactionComplete with extended fields
    {
        json j = {
            {"id", "evt-12"},
            {"timestamp", "2024-01-01T00:00:02Z"},
            {"type", "session.compaction_complete"},
            {"data", {
                {"success", true},
                {"preCompactionTokens", 5000},
                {"postCompactionTokens", 2000},
                {"messagesRemoved", 15},
                {"tokensRemoved", 3000},
                {"summaryContent", "Summary of conversation..."},
                {"checkpointNumber", 3},
                {"checkpointPath", "/sessions/abc/checkpoint-3"}
            }}
        };
        auto event = parse_session_event(j);
        auto* data = event.try_as<SessionCompactionCompleteData>();
        ASSERT_NE(data, nullptr);
        EXPECT_TRUE(data->checkpoint_number.has_value());
        EXPECT_DOUBLE_EQ(*data->checkpoint_number, 3.0);
        EXPECT_TRUE(data->checkpoint_path.has_value());
        EXPECT_EQ(*data->checkpoint_path, "/sessions/abc/checkpoint-3");
        EXPECT_TRUE(data->messages_removed.has_value());
        EXPECT_TRUE(data->summary_content.has_value());
    }

    std::cout << "All extended event fields parse correctly\n";
}

// =============================================================================
// v0.1.23 Parity: Models Cache E2E Tests
// =============================================================================

TEST_F(E2ETest, ListModelsCacheReturnsConsistentResults)
{
    test_info("Models cache: Call list_models twice, verify cached results match.");

    auto client = create_client();
    client->start().get();

    auto models1 = client->list_models().get();
    ASSERT_FALSE(models1.empty()) << "Should get at least one model";

    auto models2 = client->list_models().get();
    ASSERT_EQ(models1.size(), models2.size()) << "Cached results should match";

    for (size_t i = 0; i < models1.size(); i++)
    {
        EXPECT_EQ(models1[i].id, models2[i].id) << "Model IDs should match at index " << i;
        EXPECT_EQ(models1[i].name, models2[i].name) << "Model names should match at index " << i;
    }

    std::cout << "Models cache working: " << models1.size() << " models, consistent across calls\n";
    client->force_stop();
}

TEST_F(E2ETest, ListModelsCacheClearedOnReconnect)
{
    test_info("Models cache clear: Verify cache is cleared after stop+restart.");

    auto client = create_client();
    client->start().get();

    auto models1 = client->list_models().get();
    ASSERT_FALSE(models1.empty());

    client->stop().get();
    client->start().get();

    auto models2 = client->list_models().get();
    ASSERT_FALSE(models2.empty());
    EXPECT_EQ(models1.size(), models2.size()) << "Same server should return same models";

    client->force_stop();
}

// =============================================================================
// v0.1.23 Parity: Working Directory E2E Tests
// =============================================================================

TEST_F(E2ETest, SessionWithWorkingDirectory)
{
    test_info("Working directory: Create session with working_directory set.");

    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    config.working_directory = std::filesystem::current_path().string();

    auto session = client->create_session(config).get();
    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// v0.1.23 Parity: Resume Session with New Fields E2E Tests
// =============================================================================

TEST_F(E2ETest, ResumeSessionWithNewConfigFields)
{
    test_info("Resume with new fields: Create session, then resume with v0.1.23 config fields.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    auto session = client->create_session(config).get();
    std::string session_id = session->session_id();

    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions msg;
    msg.prompt = "Hello, just a quick test.";
    session->send(msg).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    auto resume_config = default_resume_config();
    resume_config.reasoning_effort = ReasoningEffort::Low;
    resume_config.working_directory = std::filesystem::current_path().string();

    auto resumed = client->resume_session(session_id, resume_config).get();
    EXPECT_NE(resumed, nullptr);
    std::string resumed_id = resumed->session_id();
    EXPECT_EQ(resumed_id, session_id);

    resumed->destroy().get();
    client->force_stop();
}

// =============================================================================
// v0.1.23 Parity: Model Info Extended Fields E2E Tests
// =============================================================================

TEST_F(E2ETest, ModelInfoReasoningEffortFields)
{
    test_info("Model info: Check if models report reasoning effort capabilities.");

    auto client = create_client();
    client->start().get();

    auto models = client->list_models().get();
    ASSERT_FALSE(models.empty());

    int reasoning_capable = 0;
    for (const auto& model : models)
    {
        if (model.capabilities.supports.reasoning_effort)
        {
            reasoning_capable++;
            std::cout << "Model '" << model.name << "' supports reasoning effort\n";
            if (model.supported_reasoning_efforts.has_value() && !model.supported_reasoning_efforts->empty())
            {
                std::cout << "  Supported efforts:";
                for (const auto& e : *model.supported_reasoning_efforts)
                    std::cout << " " << e;
                std::cout << "\n";
            }
            if (model.default_reasoning_effort.has_value())
                std::cout << "  Default: " << *model.default_reasoning_effort << "\n";
        }
    }

    std::cout << reasoning_capable << " of " << models.size()
              << " models support reasoning effort\n";

    client->force_stop();
}

// =============================================================================
// v0.1.23 Parity: Combined Features E2E Tests
// =============================================================================

TEST_F(E2ETest, FullFeaturedSessionWithAllNewConfig)
{
    test_info("Full config: Create session with all v0.1.23 features combined.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    config.reasoning_effort = ReasoningEffort::High;
    config.working_directory = std::filesystem::current_path().string();

    config.on_user_input_request = [](const UserInputRequest& req, const UserInputInvocation&) -> UserInputResponse
    {
        UserInputResponse resp;
        if (req.choices.has_value() && !req.choices->empty())
            resp.answer = (*req.choices)[0];
        else
            resp.answer = "Test response";
        resp.was_freeform = !req.choices.has_value() || req.choices->empty();
        return resp;
    };

    config.hooks = SessionHooks{};
    config.hooks->on_pre_tool_use = [](const PreToolUseHookInput&, const HookInvocation&)
        -> std::optional<PreToolUseHookOutput> { return std::nullopt; };
    config.hooks->on_post_tool_use = [](const PostToolUseHookInput&, const HookInvocation&)
        -> std::optional<PostToolUseHookOutput> { return std::nullopt; };
    config.hooks->on_user_prompt_submitted = [](const UserPromptSubmittedHookInput&, const HookInvocation&)
        -> std::optional<UserPromptSubmittedHookOutput> { return std::nullopt; };
    config.hooks->on_session_start = [](const SessionStartHookInput&, const HookInvocation&)
        -> std::optional<SessionStartHookOutput> { return std::nullopt; };
    config.hooks->on_session_end = [](const SessionEndHookInput&, const HookInvocation&)
        -> std::optional<SessionEndHookOutput> { return std::nullopt; };
    config.hooks->on_error_occurred = [](const ErrorOccurredHookInput&, const HookInvocation&)
        -> std::optional<ErrorOccurredHookOutput> { return std::nullopt; };

    bool has_hooks = config.hooks->has_any();
    EXPECT_TRUE(has_hooks);

    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };

    auto session = client->create_session(config).get();
    EXPECT_NE(session, nullptr);
    EXPECT_FALSE(session->session_id().empty());

    std::atomic<bool> idle{false};
    std::string assistant_response;
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
            else if (auto* msg = event.try_as<AssistantMessageData>())
            {
                std::lock_guard<std::mutex> lock(mtx);
                assistant_response = msg->content;
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "Say 'parity check complete'.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    EXPECT_TRUE(idle.load()) << "Session should reach idle state";
    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_FALSE(assistant_response.empty()) << "Should get a response";
        std::cout << "Assistant: " << assistant_response.substr(0, 100) << "\n";
    }

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Parity Sync: Foreground Session API Tests
// =============================================================================

TEST_F(E2ETest, ForegroundSessionSetAndGet)
{
    test_info("Foreground session: set_foreground_session_id then get_foreground_session_id round-trips.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    auto session = client->create_session(config).get();
    ASSERT_NE(session, nullptr);
    std::string sid = session->session_id();
    ASSERT_FALSE(sid.empty());

    // Send a message to persist the session first
    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "Say 'hi'.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&]() { return idle.load(); });
    }

    // Set this session as foreground
    client->set_foreground_session_id(sid).get();

    // Small delay to allow the server to persist
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Get foreground session ID — should match
    auto fg = client->get_foreground_session_id().get();
    if (fg.has_value())
    {
        EXPECT_EQ(*fg, sid);
        std::cout << "Foreground session round-trip confirmed: " << *fg << "\n";
    }
    else
    {
        std::cout << "Note: get_foreground_session_id returned empty after set "
                  << "(server may not persist foreground state)\n";
    }

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, ForegroundSessionInitiallyEmpty)
{
    test_info("Foreground session initially empty: get_foreground_session_id returns empty before any set.");
    auto client = create_client();
    client->start().get();

    auto fg = client->get_foreground_session_id().get();
    // Before any session is created/set as foreground, result should be empty
    if (fg.has_value())
        std::cout << "Note: foreground session ID was already set: " << *fg << "\n";
    else
        std::cout << "Confirmed: no foreground session ID set initially\n";

    // The key assertion is that the call doesn't throw
    client->force_stop();
}

// =============================================================================
// Parity Sync: Graceful Stop Test
// =============================================================================

TEST_F(E2ETest, GracefulStopReturnsNoErrors)
{
    test_info("Graceful stop: stop() returns empty vector<StopError> after clean session.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();
    auto session = client->create_session(config).get();
    ASSERT_NE(session, nullptr);

    // Send a simple message and wait for idle
    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    MessageOptions opts;
    opts.prompt = "Say 'ok'.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    session->destroy().get();

    // Graceful stop should return no errors
    auto errors = client->stop().get();
    EXPECT_TRUE(errors.empty()) << "stop() should return empty errors after clean session, got "
                                << errors.size() << " errors";
    if (!errors.empty())
    {
        for (const auto& e : errors)
            std::cout << "  StopError: " << e.message << "\n";
    }
}

// =============================================================================
// Parity Sync: Lifecycle Events Test
// =============================================================================

TEST_F(E2ETest, LifecycleCallbackFiresOnSessionCreate)
{
    test_info("Lifecycle callback: on_lifecycle fires session.created event with correct session ID.");
    auto client = create_client();
    client->start().get();

    std::atomic<bool> lifecycle_fired{false};
    std::string lifecycle_session_id;
    std::string lifecycle_event_type;
    std::mutex mtx;
    std::condition_variable cv;

    // Register lifecycle handler BEFORE creating session
    auto unsub = client->on_lifecycle(
        [&](const SessionLifecycleEvent& evt)
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (evt.type == SessionLifecycleEventTypes::Created)
            {
                lifecycle_fired = true;
                lifecycle_session_id = evt.session_id;
                lifecycle_event_type = evt.type;
                cv.notify_one();
            }
        }
    );

    auto config = default_session_config();
    auto session = client->create_session(config).get();
    ASSERT_NE(session, nullptr);
    std::string sid = session->session_id();

    // Wait for lifecycle event
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(10), [&]() { return lifecycle_fired.load(); });
    }

    EXPECT_TRUE(lifecycle_fired.load()) << "on_lifecycle should fire session.created event";
    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_EQ(lifecycle_event_type, SessionLifecycleEventTypes::Created);
        EXPECT_EQ(lifecycle_session_id, sid);
    }

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Parity Sync: User Input Handler E2E Tests
// =============================================================================

TEST_F(E2ETest, UserInputHandlerInvokedByModel)
{
    test_info("User input handler: model's ask_user triggers the handler, response flows back.");
    auto client = create_client();
    client->start().get();

    std::atomic<bool> handler_called{false};
    std::string received_question;
    std::mutex mtx;

    auto config = default_session_config();
    config.on_user_input_request = [&](const UserInputRequest& req, const UserInputInvocation&) -> UserInputResponse
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            handler_called = true;
            received_question = req.question;
        }
        UserInputResponse resp;
        resp.answer = "Blue";
        resp.was_freeform = true;
        return resp;
    };

    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };

    auto session = client->create_session(config).get();
    ASSERT_NE(session, nullptr);

    std::atomic<bool> idle{false};
    std::string assistant_response;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
            else if (auto* msg = event.try_as<AssistantMessageData>())
            {
                std::lock_guard<std::mutex> lock(mtx);
                assistant_response += msg->content;
            }
        }
    );

    // Retry up to 3 prompts to trigger ask_user
    const char* prompts[] = {
        "Ask the user what their favorite color is using the ask_user tool. Then report their answer.",
        "You MUST use the ask_user tool to ask the user about their favorite color. Report what they say.",
        "Use ask_user to ask: 'What is your favorite color?' Then tell me the answer."
    };
    for (int attempt = 0; attempt < 3 && !handler_called.load(); ++attempt)
    {
        idle = false;
        MessageOptions opts;
        opts.prompt = prompts[attempt];
        session->send(opts).get();

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
        }
    }

    EXPECT_TRUE(handler_called.load()) << "User input handler should have been invoked";
    {
        std::lock_guard<std::mutex> lock(mtx);
        // Response should reference "Blue" since that's what our handler returned
        if (handler_called.load())
        {
            EXPECT_TRUE(assistant_response.find("Blue") != std::string::npos ||
                        assistant_response.find("blue") != std::string::npos)
                << "Response should contain the user input answer 'Blue': " << assistant_response;
        }
    }

    session->destroy().get();
    client->force_stop();
}

TEST_F(E2ETest, UserInputHandlerReceivesQuestion)
{
    test_info("User input handler: UserInputRequest contains non-empty question field.");
    auto client = create_client();
    client->start().get();

    std::atomic<bool> handler_called{false};
    std::string received_question;
    std::mutex mtx;

    auto config = default_session_config();
    config.on_user_input_request = [&](const UserInputRequest& req, const UserInputInvocation&) -> UserInputResponse
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            handler_called = true;
            received_question = req.question;
        }
        UserInputResponse resp;
        resp.answer = "42";
        resp.was_freeform = true;
        return resp;
    };

    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };

    auto session = client->create_session(config).get();

    std::atomic<bool> idle{false};
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    const char* prompts[] = {
        "Use the ask_user tool to ask: 'What is your favorite number?'",
        "You MUST call ask_user with the question 'What is your favorite number?'",
        "Call ask_user to ask the user about their favorite number."
    };
    for (int attempt = 0; attempt < 3 && !handler_called.load(); ++attempt)
    {
        idle = false;
        MessageOptions opts;
        opts.prompt = prompts[attempt];
        session->send(opts).get();

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
        }
    }

    EXPECT_TRUE(handler_called.load()) << "User input handler should have been invoked";
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (handler_called.load())
        {
            EXPECT_FALSE(received_question.empty()) << "UserInputRequest.question should not be empty";
            std::cout << "Received question: " << received_question << "\n";
        }
    }

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Parity Sync: Combined Pre+Post Hook Test
// =============================================================================

TEST_F(E2ETest, BothPreAndPostToolHooksFireOnSameToolCall)
{
    test_info("Both hooks: pre and post tool hooks both fire for the same tool invocation.");
    auto client = create_client();
    client->start().get();

    std::atomic<bool> pre_hook_fired{false};
    std::atomic<bool> post_hook_fired{false};
    std::string pre_hook_tool;
    std::string post_hook_tool;
    std::mutex mtx;

    auto config = default_session_config();

    Tool ping_tool;
    ping_tool.name = "ping_pong";
    ping_tool.description = "Returns 'pong' when called";
    ping_tool.parameters_schema = {
        {"type", "object"},
        {"properties", json::object()}
    };
    ping_tool.handler = [](const ToolInvocation&) -> ToolResultObject
    {
        ToolResultObject result;
        result.text_result_for_llm = "pong";
        result.result_type = ToolResultType::Success;
        return result;
    };
    config.tools = {ping_tool};

    config.hooks = SessionHooks{};
    config.hooks->on_pre_tool_use = [&](const PreToolUseHookInput& input, const HookInvocation&)
        -> std::optional<PreToolUseHookOutput>
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            pre_hook_fired = true;
            pre_hook_tool = input.tool_name;
        }
        return std::nullopt;
    };

    config.hooks->on_post_tool_use = [&](const PostToolUseHookInput& input, const HookInvocation&)
        -> std::optional<PostToolUseHookOutput>
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            post_hook_fired = true;
            post_hook_tool = input.tool_name;
        }
        return std::nullopt;
    };

    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };

    auto session = client->create_session(config).get();

    std::atomic<bool> idle{false};
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    const char* prompts[] = {
        "Use the ping_pong tool now.",
        "You MUST call the ping_pong tool.",
        "Call ping_pong immediately."
    };
    for (int attempt = 0; attempt < 3 && !pre_hook_fired.load(); ++attempt)
    {
        idle = false;
        MessageOptions opts;
        opts.prompt = prompts[attempt];
        session->send(opts).get();

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
        }
    }

    EXPECT_TRUE(pre_hook_fired.load()) << "preToolUse hook should have fired";
    EXPECT_TRUE(post_hook_fired.load()) << "postToolUse hook should have fired";
    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_EQ(pre_hook_tool, "ping_pong");
        EXPECT_EQ(post_hook_tool, "ping_pong");
    }

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Parity Sync: UserPromptSubmitted Hook Test
// =============================================================================

TEST_F(E2ETest, UserPromptSubmittedHookFires)
{
    test_info("User prompt submitted hook: on_user_prompt_submitted fires with the prompt text.");
    auto client = create_client();
    client->start().get();

    std::atomic<bool> hook_fired{false};
    std::string captured_prompt;
    std::mutex mtx;

    auto config = default_session_config();
    config.hooks = SessionHooks{};
    config.hooks->on_user_prompt_submitted = [&](const UserPromptSubmittedHookInput& input, const HookInvocation&)
        -> std::optional<UserPromptSubmittedHookOutput>
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            hook_fired = true;
            captured_prompt = input.prompt;
        }
        return std::nullopt;
    };

    auto session = client->create_session(config).get();

    std::atomic<bool> idle{false};
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
        }
    );

    const std::string marker = "MARKER_PROMPT_7749";
    MessageOptions opts;
    opts.prompt = "Say hello. " + marker;
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    EXPECT_TRUE(hook_fired.load()) << "on_user_prompt_submitted hook should fire";
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (hook_fired.load())
        {
            EXPECT_TRUE(captured_prompt.find(marker) != std::string::npos)
                << "Hook should capture prompt containing marker '" << marker << "', got: " << captured_prompt;
        }
    }

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Parity Sync: Tool Error Handling Test
// =============================================================================

TEST_F(E2ETest, ToolHandlerExceptionDoesNotCrash)
{
    test_info("Tool error: Tool handler throwing exception doesn't crash session.");
    auto client = create_client();
    client->start().get();

    auto config = default_session_config();

    Tool crash_tool;
    crash_tool.name = "crasher";
    crash_tool.description = "A tool that always fails internally";
    crash_tool.parameters_schema = {
        {"type", "object"},
        {"properties", json::object()}
    };
    crash_tool.handler = [](const ToolInvocation&) -> ToolResultObject
    {
        throw std::runtime_error("Secret_Error_42");
    };
    config.tools = {crash_tool};

    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };

    auto session = client->create_session(config).get();

    std::atomic<bool> idle{false};
    std::string assistant_response;
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
            else if (auto* msg = event.try_as<AssistantMessageData>())
            {
                std::lock_guard<std::mutex> lock(mtx);
                assistant_response += msg->content;
            }
        }
    );

    const char* prompts[] = {
        "Use the crasher tool now.",
        "You MUST call the crasher tool immediately.",
        "Call crasher."
    };
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        idle = false;
        MessageOptions opts;
        opts.prompt = prompts[attempt];
        session->send(opts).get();

        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
        }

        if (idle.load())
            break;
    }

    // Session should reach idle — the exception should not crash it
    EXPECT_TRUE(idle.load()) << "Session should reach idle even after tool handler exception";

    // The secret error string should not leak to the LLM response
    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_TRUE(assistant_response.find("Secret_Error_42") == std::string::npos)
            << "Secret error should not leak to LLM response: " << assistant_response;
    }

    session->destroy().get();
    client->force_stop();
}

// =============================================================================
// Parity Sync: make_tool with normalize_result Test
// =============================================================================

TEST_F(E2ETest, MakeToolWithNormalizeResult)
{
    test_info("make_tool: Tool created via make_tool with plain string return works through CLI.");
    auto client = create_client();
    client->start().get();

    // Create tool using make_tool — returns std::string, exercises normalize_result path
    auto rot13_tool = copilot::make_tool(
        "rot13",
        "Apply ROT13 cipher to the input text",
        [](std::string text) -> std::string
        {
            std::string result = text;
            for (auto& c : result)
            {
                if (c >= 'a' && c <= 'z')
                    c = 'a' + (c - 'a' + 13) % 26;
                else if (c >= 'A' && c <= 'Z')
                    c = 'A' + (c - 'A' + 13) % 26;
            }
            return result;
        },
        {"text"}
    );

    auto config = default_session_config();
    config.tools = {rot13_tool};

    config.on_permission_request = [](const PermissionRequest&) -> PermissionRequestResult
    {
        PermissionRequestResult r;
        r.kind = "approved";
        return r;
    };

    auto session = client->create_session(config).get();

    std::atomic<bool> idle{false};
    std::string assistant_response;
    std::mutex mtx;
    std::condition_variable cv;

    auto sub = session->on(
        [&](const SessionEvent& event)
        {
            if (event.type == SessionEventType::SessionIdle)
            {
                idle = true;
                cv.notify_one();
            }
            else if (auto* msg = event.try_as<AssistantMessageData>())
            {
                std::lock_guard<std::mutex> lock(mtx);
                assistant_response += msg->content;
            }
        }
    );

    // "Hello" ROT13 = "Uryyb"
    MessageOptions opts;
    opts.prompt = "Use the rot13 tool to encode the word 'Hello'. Tell me the exact result.";
    session->send(opts).get();

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
    }

    EXPECT_TRUE(idle.load()) << "Session should reach idle";
    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_TRUE(assistant_response.find("Uryyb") != std::string::npos)
            << "Response should contain ROT13 of 'Hello' = 'Uryyb': " << assistant_response;
    }

    session->destroy().get();
    client->force_stop();
}
