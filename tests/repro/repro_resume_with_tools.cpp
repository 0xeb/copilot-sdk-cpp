// Repro: ResumeSessionWithTools fails with BYOK (OpenAI) but passes with Copilot
//
// Issue: When using BYOK with OpenAI, resuming a session with new tools doesn't work.
// The AI never invokes the tool registered during resume.
//
// With Copilot auth: PASS (20s)
// With BYOK/OpenAI: FAIL (tool never called)
//
// Build:
//   cl /EHsc /std:c++20 /I../../include repro_resume_with_tools.cpp /link /LIBPATH:../../build/Release copilot_sdk_cpp.lib
//
// Or use CMake (see CMakeLists.txt)

#include <copilot/copilot.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

using namespace copilot;

// Load byok.env if present
void load_byok_env()
{
    std::filesystem::path env_file = std::filesystem::path(__FILE__).parent_path() / "byok.env";
    if (!std::filesystem::exists(env_file))
    {
        std::cout << "[REPRO] No byok.env found, using Copilot auth\n";
        return;
    }

    std::ifstream file(env_file);
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;
        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
#ifdef _WIN32
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 1);
#endif
    }
    std::cout << "[REPRO] Loaded BYOK config from byok.env\n";
}

int main()
{
    load_byok_env();

    std::cout << "\n=== Repro: ResumeSessionWithTools ===\n\n";

    // Create client
    ClientOptions opts;
    auto client = std::make_unique<Client>(opts);
    client->start().get();
    std::cout << "[1] Client started\n";

    // Create session config with BYOK if available
    SessionConfig config;
    config.auto_byok_from_env = true;

    // Create initial session (no tools)
    auto session1 = client->create_session(config).get();
    std::string session_id = session1->session_id();
    std::cout << "[2] Created session: " << session_id << "\n";

    // Wait for first message
    std::atomic<bool> idle{false};
    std::mutex mtx;
    std::condition_variable cv;

    session1->on([&](const SessionEvent& event) {
        if (event.type == SessionEventType::SessionIdle)
        {
            idle = true;
            cv.notify_one();
        }
    });

    MessageOptions msg_opts;
    msg_opts.prompt = "Say hello";
    session1->send(msg_opts).get();
    std::cout << "[3] Sent initial message\n";

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(30), [&] { return idle.load(); });
    }
    std::cout << "[4] Session idle after first message\n";

    // Stop client (don't destroy session)
    client->stop().get();
    std::cout << "[5] Client stopped\n";

    // Restart client
    client = std::make_unique<Client>(opts);
    client->start().get();
    std::cout << "[6] Client restarted\n";

    // Define tool for resume
    std::atomic<bool> tool_called{false};
    std::string received_key;

    Tool secret_tool;
    secret_tool.name = "get_secret";
    secret_tool.description = "Returns a secret value";
    secret_tool.parameters_schema = json{
        {"type", "object"},
        {"properties", {{"key", {{"type", "string"}}}}},
        {"required", {"key"}}};
    secret_tool.handler = [&](const ToolInvocation& inv) -> ToolResultObject {
        tool_called = true;
        received_key = inv.arguments.value()["key"].get<std::string>();
        std::cout << "[!] Tool invoked with key: " << received_key << "\n";

        ToolResultObject result;
        result.text_result_for_llm = "SECRET_VALUE_12345";
        result.result_type = "success";
        return result;
    };

    // Resume session WITH the new tool
    ResumeSessionConfig resume_config;
    resume_config.auto_byok_from_env = true;
    resume_config.tools = {secret_tool};

    auto session2 = client->resume_session(session_id, resume_config).get();
    std::cout << "[7] Resumed session with tool\n";

    // Reset for second wait
    idle = false;

    session2->on([&](const SessionEvent& event) {
        if (event.type == SessionEventType::SessionIdle)
        {
            idle = true;
            cv.notify_one();
        }
    });

    // Ask AI to use the tool
    msg_opts.prompt = "Use the get_secret tool with key 'ALPHA' and tell me the result.";
    session2->send(msg_opts).get();
    std::cout << "[8] Sent message asking to use tool\n";

    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::seconds(60), [&] { return idle.load(); });
    }

    // Check results
    std::cout << "\n=== Results ===\n";
    std::cout << "Tool called: " << (tool_called ? "YES" : "NO") << "\n";
    std::cout << "Received key: " << (received_key.empty() ? "(empty)" : received_key) << "\n";

    if (tool_called && received_key == "ALPHA")
    {
        std::cout << "\n[PASS] Tool was invoked correctly!\n";
        return 0;
    }
    else
    {
        std::cout << "\n[FAIL] Tool was NOT invoked - this is the BYOK/OpenAI limitation\n";
        return 1;
    }
}
