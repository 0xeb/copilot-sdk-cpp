// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file snapshot_replay.cpp
/// @brief Replay executable for snapshot-based conformance testing
///
/// This executable:
/// 1. Reads a JSON test config from stdin
/// 2. Runs a deterministic in-process JSON-RPC server
/// 3. Creates a Copilot session against it
/// 4. Sends prompts and captures tool invocations
/// 5. Outputs a JSON transcript to stdout
///
/// Used by snapshot_runner.py to validate SDK behavior against upstream snapshots.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <copilot/copilot.hpp>
#include <copilot/transport_tcp.hpp>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace copilot;
using json = nlohmann::json;

// =============================================================================
// Transcript Recording
// =============================================================================

struct ToolCall
{
    std::string id;
    std::string name;
    json arguments;
    std::string result;
};

struct TranscriptEntry
{
    std::string role;
    std::string content;
    std::vector<ToolCall> tool_calls;
};

std::vector<TranscriptEntry> g_transcript;
std::mutex g_transcript_mutex;

// =============================================================================
// Dynamic Tool Creation
// =============================================================================

Tool create_tool_from_config(const json& tool_config,
                             std::vector<ToolCall>& captured_calls,
                             std::mutex& calls_mutex)
{
    std::string name = tool_config["name"].get<std::string>();
    std::string description = tool_config.value("description", "Test tool");
    json params_schema = tool_config.value("parameters_schema", json{{"type", "object"}});
    std::string fixed_result = tool_config.value("result", "OK");

    Tool tool;
    tool.name = name;
    tool.description = description;
    tool.parameters_schema = params_schema;
    tool.handler = [name, fixed_result, &captured_calls, &calls_mutex](
                       const ToolInvocation& inv) -> ToolResultObject {
        ToolResultObject result;

        // Capture the tool call
        {
            std::lock_guard<std::mutex> lock(calls_mutex);
            ToolCall call;
            call.id = inv.tool_call_id;
            call.name = name;
            call.arguments = inv.arguments.value_or(json::object());
            call.result = fixed_result;
            captured_calls.push_back(call);
        }

        result.text_result_for_llm = fixed_result;
        result.result_type = ToolResultType::Success;
        return result;
    };

    return tool;
}

// =============================================================================
// Deterministic JSON-RPC server (minimal subset for snapshot tests)
// =============================================================================

struct ExpectedToolCall
{
    std::string id;
    std::string name;
    json arguments;
};

struct ExpectedTurn
{
    std::string prompt;
    std::vector<ExpectedToolCall> tool_calls;
    std::vector<std::string> assistant_messages;
};

static std::string now_timestamp_utc()
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto secs = time_point_cast<seconds>(now);
    auto t = system_clock::to_time_t(secs);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(
        buf,
        sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02dZ",
        tm.tm_year + 1900,
        tm.tm_mon + 1,
        tm.tm_mday,
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec
    );
    return buf;
}

static json make_session_event(const std::string& type, const json& data)
{
    json event;
    event["id"] = "evt_" + std::to_string(std::rand());
    event["timestamp"] = now_timestamp_utc();
    event["type"] = type;
    event["data"] = data;
    return event;
}

class SnapshotRpcServer
{
  public:
    SnapshotRpcServer(std::vector<ExpectedTurn> turns, int protocol_version)
        : turns_(std::move(turns)), protocol_version_(protocol_version)
    {
    }

    int start()
    {
        TcpTransport::Socket listen_sock = TcpTransport::kInvalidSocket;

#ifdef _WIN32
        WinsockInitializer::instance();
        listen_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock == INVALID_SOCKET)
            throw std::runtime_error("socket() failed");
#else
        listen_sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_sock < 0)
            throw std::runtime_error("socket() failed");
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);

        int yes = 1;
        setsockopt(
            listen_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&yes), sizeof(yes)
        );

        if (::bind(listen_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            close_socket(listen_sock);
            throw std::runtime_error("bind() failed");
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listen_sock, reinterpret_cast<sockaddr*>(&bound), &len) != 0)
        {
            close_socket(listen_sock);
            throw std::runtime_error("getsockname() failed");
        }

        int port = ntohs(bound.sin_port);

        if (::listen(listen_sock, 1) != 0)
        {
            close_socket(listen_sock);
            throw std::runtime_error("listen() failed");
        }

        listen_socket_ = listen_sock;
        server_thread_ = std::thread([this]() { this->run(); });
        return port;
    }

    void stop()
    {
        stop_requested_ = true;
        if (listen_socket_ != TcpTransport::kInvalidSocket)
        {
            close_socket(listen_socket_);
            listen_socket_ = TcpTransport::kInvalidSocket;
        }
        if (server_thread_.joinable())
            server_thread_.join();
    }

  private:
    TcpTransport::Socket listen_socket_ = TcpTransport::kInvalidSocket;
    std::optional<std::string> session_id_;
    std::vector<ExpectedTurn> turns_;
    size_t next_turn_ = 0;
    std::optional<size_t> last_turn_index_;
    int protocol_version_ = 1;
    std::atomic<bool> stop_requested_{false};
    std::thread server_thread_;

    static void close_socket(TcpTransport::Socket sock)
    {
#ifdef _WIN32
        closesocket(sock);
#else
        ::close(sock);
#endif
    }

    void run()
    {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        TcpTransport::Socket client_sock = ::accept(
            listen_socket_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len
        );
        if (client_sock == TcpTransport::kInvalidSocket)
            return;

        TcpTransport transport(client_sock);
        MessageFramer framer(transport);

        int next_server_request_id = 1000;

        auto send_response = [&](const json& id, const json& result)
        {
            json resp;
            resp["jsonrpc"] = "2.0";
            resp["id"] = id;
            resp["result"] = result;
            framer.write_message(resp.dump());
        };

        auto send_notification = [&](const std::string& method, const json& params)
        {
            json msg;
            msg["jsonrpc"] = "2.0";
            msg["method"] = method;
            msg["params"] = params;
            framer.write_message(msg.dump());
        };

        auto handle_client_request = [&](const std::string& method, const json& params) -> json
        {
            if (method == "ping")
            {
                json result;
                result["message"] = "pong";
                result["timestamp"] = static_cast<int64_t>(std::time(nullptr));
                result["protocolVersion"] = protocol_version_;
                return result;
            }
            if (method == "session.create")
            {
                session_id_ = "sess_" + std::to_string(std::rand());
                return json{{"sessionId", *session_id_}};
            }
            if (method == "session.send")
            {
                (void)params;
                if (next_turn_ < turns_.size())
                {
                    last_turn_index_ = next_turn_;
                    ++next_turn_;
                }
                else
                {
                    last_turn_index_.reset();
                }
                return json{{"messageId", "msg_" + std::to_string(std::rand())}};
            }
            if (method == "session.destroy")
            {
                return json::object();
            }
            if (method == "session.resume")
            {
                if (!session_id_.has_value())
                    session_id_ = params.value("sessionId", "sess_" + std::to_string(std::rand()));
                return json{{"sessionId", *session_id_}};
            }
            if (method == "session.list")
            {
                return json{{"sessions", json::array()}};
            }
            if (method == "session.getLastId")
            {
                return json{{"sessionId", session_id_.value_or("")}};
            }

            return json::object();
        };

        auto send_request_and_wait = [&](const std::string& method, const json& params) -> json
        {
            int id = next_server_request_id++;
            json req;
            req["jsonrpc"] = "2.0";
            req["id"] = id;
            req["method"] = method;
            req["params"] = params;
            framer.write_message(req.dump());

            while (transport.is_open() && !stop_requested_)
            {
                json incoming = json::parse(framer.read_message());

                if (incoming.contains("id") && incoming["id"] == id && incoming.contains("result"))
                    return incoming["result"];

                if (incoming.contains("method") && incoming.contains("id"))
                {
                    const std::string m = incoming["method"].get<std::string>();
                    const json p = incoming.value("params", json::object());
                    json result = handle_client_request(m, p);
                    send_response(incoming["id"], result);
                }
            }
            return json::object();
        };

        while (transport.is_open() && !stop_requested_)
        {
            json msg;
            try
            {
                msg = json::parse(framer.read_message());
            }
            catch (...)
            {
                break;
            }

            if (!msg.contains("method") || !msg.contains("id"))
                continue;

            const std::string method = msg["method"].get<std::string>();
            const json params = msg.value("params", json::object());

            json result = handle_client_request(method, params);
            send_response(msg["id"], result);

            if (method == "session.send" && session_id_.has_value() && last_turn_index_.has_value())
            {
                const auto& turn = turns_.at(*last_turn_index_);

                for (const auto& tc : turn.tool_calls)
                {
                    json call_params;
                    call_params["sessionId"] = *session_id_;
                    call_params["toolCallId"] =
                        tc.id.empty() ? ("toolcall_" + std::to_string(std::rand())) : tc.id;
                    call_params["toolName"] = tc.name;
                    call_params["arguments"] = tc.arguments;
                    (void)send_request_and_wait("tool.call", call_params);
                }

                for (const auto& content : turn.assistant_messages)
                {
                    json data;
                    data["messageId"] = "msg_" + std::to_string(std::rand());
                    data["content"] = content;
                    send_notification(
                        "session.event",
                        json{{"sessionId", *session_id_},
                             {"event", make_session_event("assistant.message", data)}}
                    );
                }

                send_notification(
                    "session.event",
                    json{{"sessionId", *session_id_},
                         {"event", make_session_event("session.idle", json::object())}}
                );
            }
        }
    }
};

// =============================================================================
// Main
// =============================================================================

int main(int argc, char* argv[])
{
    try
    {
        std::srand(static_cast<unsigned>(std::time(nullptr)));

        // Read JSON config from file (passed as argument)
        if (argc < 2)
        {
            json error_output;
            error_output["error"] = "Usage: snapshot_replay <config.json>";
            std::cout << error_output.dump() << std::endl;
            return 1;
        }

        std::ifstream config_file(argv[1]);
        if (!config_file.is_open())
        {
            json error_output;
            error_output["error"] = std::string("Cannot open config file: ") + argv[1];
            std::cout << error_output.dump() << std::endl;
            return 1;
        }

        std::stringstream buffer;
        buffer << config_file.rdbuf();
        std::string input = buffer.str();

        if (input.empty())
        {
            json error_output;
            error_output["error"] = "Empty config file";
            std::cout << error_output.dump() << std::endl;
            return 1;
        }

        json config = json::parse(input);

        // Extract test configuration
        std::vector<ExpectedTurn> turns;
        if (config.contains("turns") && config["turns"].is_array())
        {
            for (const auto& t : config["turns"])
            {
                ExpectedTurn turn;
                turn.prompt = t.value("prompt", "");
                if (t.contains("tool_calls") && t["tool_calls"].is_array())
                {
                    for (const auto& tc : t["tool_calls"])
                    {
                        ExpectedToolCall call;
                        call.id = tc.value("id", "");
                        call.name = tc.value("name", "");
                        call.arguments = tc.value("arguments", json::object());
                        turn.tool_calls.push_back(call);
                    }
                }
                if (t.contains("assistant_messages") && t["assistant_messages"].is_array())
                {
                    for (const auto& m : t["assistant_messages"])
                        if (m.is_string())
                            turn.assistant_messages.push_back(m.get<std::string>());
                }
                turns.push_back(std::move(turn));
            }
        }
        else if (config.contains("prompts") && config["prompts"].is_array())
        {
            for (const auto& p : config["prompts"])
                turns.push_back(ExpectedTurn{p.get<std::string>(), {}, {}});
        }

        std::vector<ToolCall> captured_calls;
        std::mutex calls_mutex;

        // Create tools from config
        std::vector<Tool> tools;
        if (config.contains("tools"))
        {
            for (const auto& tc : config["tools"])
                tools.push_back(create_tool_from_config(tc, captured_calls, calls_mutex));
        }

        // Start deterministic in-process server and connect the SDK to it
        SnapshotRpcServer server(turns, kSdkProtocolVersion);
        int port = server.start();

        ClientOptions opts;
        opts.log_level = LogLevel::Info;
        opts.use_stdio = false;
        opts.cli_url = std::to_string(port);
        opts.auto_start = false;
        Client client(opts);
        client.start().get();

        // Create session with tools
        SessionConfig session_config;
        session_config.tools = tools;

        // Handle system message config
        if (config.contains("system_message"))
        {
            SystemMessageConfig sys_msg;
            auto& sm = config["system_message"];
            sys_msg.content = sm.value("content", "");
            std::string mode = sm.value("mode", "append");
            sys_msg.mode = (mode == "replace") ? SystemMessageMode::Replace : SystemMessageMode::Append;
            session_config.system_message = sys_msg;
        }

        auto session = client.create_session(session_config).get();

        // Event tracking
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> idle{false};
        std::vector<std::string> assistant_messages;

        auto sub = session->on(
            [&](const SessionEvent& event)
            {
                if (auto* msg = event.try_as<AssistantMessageData>())
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    assistant_messages.push_back(msg->content);
                }
                else if (event.type == SessionEventType::SessionIdle)
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    idle = true;
                    cv.notify_one();
                }
            });

        // Send prompts and collect responses
        json output;
        output["session_id"] = session->session_id();
        output["turns"] = json::array();

        for (const auto& turn_cfg : turns)
        {
            idle = false;
            assistant_messages.clear();

            MessageOptions msg_opts;
            msg_opts.prompt = turn_cfg.prompt;
            session->send(msg_opts).get();

            // Wait for idle with timeout
            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait_for(lock, std::chrono::seconds(60), [&]() { return idle.load(); });
            }

            // Record this turn
            json turn;
            turn["prompt"] = turn_cfg.prompt;
            turn["assistant_messages"] = assistant_messages;

            // Capture tool calls made during this turn
            {
                std::lock_guard<std::mutex> lock(calls_mutex);
                turn["tool_calls"] = json::array();
                for (const auto& tc : captured_calls)
                {
                    json call;
                    call["id"] = tc.id;
                    call["name"] = tc.name;
                    call["arguments"] = tc.arguments;
                    call["result"] = tc.result;
                    turn["tool_calls"].push_back(call);
                }
                captured_calls.clear(); // Clear for next turn
            }

            output["turns"].push_back(turn);
        }

        // Cleanup
        session->destroy().get();
        client.stop().get();
        server.stop();

        // Output JSON transcript
        std::cout << output.dump(2) << std::endl;
        return 0;
    }
    catch (const std::exception& e)
    {
        json error_output;
        error_output["error"] = e.what();
        std::cout << error_output.dump() << std::endl;
        return 1;
    }
}
