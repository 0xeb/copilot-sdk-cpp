// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <chrono>
#include <condition_variable>
#include <copilot/jsonrpc.hpp>
#include <gtest/gtest.h>
#include <mutex>
#include <queue>
#include <thread>

using namespace copilot;

// =============================================================================
// Pipe Transport for Testing
// =============================================================================

/// A pair of connected transports for bidirectional testing
class PipeTransport : public ITransport
{
  public:
    PipeTransport() = default;

    /// Connect two transports together
    static std::pair<std::unique_ptr<PipeTransport>, std::unique_ptr<PipeTransport>> create_pair()
    {
        auto a = std::make_unique<PipeTransport>();
        auto b = std::make_unique<PipeTransport>();
        a->peer_ = b.get();
        b->peer_ = a.get();
        return {std::move(a), std::move(b)};
    }

    size_t read(char* buffer, size_t size) override
    {
        std::unique_lock<std::mutex> lock(mutex_);

        // Wait for data, close, or peer close
        cv_.wait(lock, [this] { return !read_buffer_.empty() || !open_ || peer_closed_; });

        if ((!open_ || peer_closed_) && read_buffer_.empty())
            return 0; // EOF

        size_t bytes_read = 0;
        while (bytes_read < size && !read_buffer_.empty())
        {
            buffer[bytes_read++] = read_buffer_.front();
            read_buffer_.pop();
        }
        return bytes_read;
    }

    void write(const char* data, size_t size) override
    {
        if (!open_)
            throw ConnectionClosedError();

        if (peer_ && !peer_->peer_closed_)
        {
            std::lock_guard<std::mutex> lock(peer_->mutex_);
            for (size_t i = 0; i < size; ++i)
                peer_->read_buffer_.push(data[i]);
            peer_->cv_.notify_one();
        }
    }

    void close() override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = false;
        }
        cv_.notify_all();

        // Mark peer as having lost its peer (EOF on next read)
        if (peer_)
        {
            std::lock_guard<std::mutex> lock(peer_->mutex_);
            peer_->peer_closed_ = true;
            peer_->cv_.notify_all();
        }
    }

    bool is_open() const override
    {
        return open_;
    }

  private:
    std::queue<char> read_buffer_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_ = true;
    bool peer_closed_ = false;
    PipeTransport* peer_ = nullptr;
};

// =============================================================================
// JSON-RPC Message Type Tests
// =============================================================================

TEST(JsonRpcMessageTest, RequestSerialization)
{
    JsonRpcRequest req{"test.method", json{{"key", "value"}}, JsonRpcId{int64_t(1)}};

    auto j = req.to_json();
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["method"], "test.method");
    EXPECT_EQ(j["params"]["key"], "value");
    EXPECT_EQ(j["id"], 1);
}

TEST(JsonRpcMessageTest, RequestWithoutParams)
{
    JsonRpcRequest req{"ping", nullptr, JsonRpcId{int64_t(1)}};

    auto j = req.to_json();
    EXPECT_EQ(j["method"], "ping");
    EXPECT_FALSE(j.contains("params"));
}

TEST(JsonRpcMessageTest, NotificationSerialization)
{
    JsonRpcRequest notif{"session.event", json{{"sessionId", "s1"}}, std::nullopt};

    auto j = notif.to_json();
    EXPECT_EQ(j["method"], "session.event");
    EXPECT_FALSE(j.contains("id"));
    EXPECT_TRUE(notif.is_notification());
}

TEST(JsonRpcMessageTest, RequestDeserialization)
{
    json j = {
        {"jsonrpc", "2.0"}, {"method", "tool.call"}, {"params", {{"name", "read_file"}}}, {"id", 42}
    };

    auto req = JsonRpcRequest::from_json(j);
    EXPECT_EQ(req.method, "tool.call");
    EXPECT_EQ(req.params["name"], "read_file");
    EXPECT_TRUE(req.id.has_value());
    EXPECT_EQ(std::get<int64_t>(*req.id), 42);
}

TEST(JsonRpcMessageTest, ResponseSerialization)
{
    JsonRpcResponse resp{JsonRpcId{int64_t(1)}, json{{"status", "ok"}}, std::nullopt};

    auto j = resp.to_json();
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["id"], 1);
    EXPECT_EQ(j["result"]["status"], "ok");
    EXPECT_FALSE(j.contains("error"));
}

TEST(JsonRpcMessageTest, ErrorResponseSerialization)
{
    JsonRpcResponse resp{
        JsonRpcId{int64_t(1)}, std::nullopt, JsonRpcErrorObject{-32601, "Method not found", nullptr}
    };

    auto j = resp.to_json();
    EXPECT_EQ(j["jsonrpc"], "2.0");
    EXPECT_EQ(j["id"], 1);
    EXPECT_FALSE(j.contains("result"));
    EXPECT_EQ(j["error"]["code"], -32601);
    EXPECT_EQ(j["error"]["message"], "Method not found");
}

TEST(JsonRpcMessageTest, ResponseDeserialization)
{
    json j = {{"jsonrpc", "2.0"}, {"result", {{"message", "pong"}}}, {"id", 1}};

    auto resp = JsonRpcResponse::from_json(j);
    EXPECT_EQ(std::get<int64_t>(resp.id), 1);
    EXPECT_TRUE(resp.result.has_value());
    EXPECT_EQ((*resp.result)["message"], "pong");
    EXPECT_FALSE(resp.is_error());
}

TEST(JsonRpcMessageTest, ErrorResponseDeserialization)
{
    json j = {
        {"jsonrpc", "2.0"}, {"error", {{"code", -32600}, {"message", "Invalid Request"}}}, {"id", 1}
    };

    auto resp = JsonRpcResponse::from_json(j);
    EXPECT_TRUE(resp.is_error());
    EXPECT_EQ(resp.error->code, -32600);
    EXPECT_EQ(resp.error->message, "Invalid Request");
}

TEST(JsonRpcMessageTest, StringIdSupport)
{
    JsonRpcRequest req{"test", nullptr, JsonRpcId{std::string("req-abc-123")}};

    auto j = req.to_json();
    EXPECT_EQ(j["id"], "req-abc-123");

    auto parsed = JsonRpcRequest::from_json(j);
    EXPECT_EQ(std::get<std::string>(*parsed.id), "req-abc-123");
}

// =============================================================================
// JSON-RPC Client Tests
// =============================================================================

TEST(JsonRpcClientTest, BasicRequestResponse)
{
    auto [client_transport, server_transport] = PipeTransport::create_pair();

    JsonRpcClient client(std::move(client_transport));
    MessageFramer server_framer(*server_transport);

    client.start();

    // Send request in background
    auto future = client.invoke("ping", json{{"message", "hello"}});

    // Server receives and responds
    std::thread server_thread(
        [&]
        {
            auto msg = server_framer.read_message();
            auto req = json::parse(msg);
            EXPECT_EQ(req["method"], "ping");
            EXPECT_EQ(req["params"]["message"], "hello");

            json response = {
                {"jsonrpc", "2.0"}, {"result", {{"message", "pong"}}}, {"id", req["id"]}
            };
            server_framer.write_message(response.dump());
        }
    );

    auto result = future.get();
    EXPECT_EQ(result["message"], "pong");

    server_thread.join();
    client.stop();
}

TEST(JsonRpcClientTest, ErrorResponse)
{
    auto [client_transport, server_transport] = PipeTransport::create_pair();

    JsonRpcClient client(std::move(client_transport));
    MessageFramer server_framer(*server_transport);

    client.start();

    auto future = client.invoke("unknown.method");

    std::thread server_thread(
        [&]
        {
            auto msg = server_framer.read_message();
            auto req = json::parse(msg);

            json response = {
                {"jsonrpc", "2.0"},
                {"error", {{"code", -32601}, {"message", "Method not found"}}},
                {"id", req["id"]}
            };
            server_framer.write_message(response.dump());
        }
    );

    EXPECT_THROW(
        {
            try
            {
                future.get();
            }
            catch (const JsonRpcError& e)
            {
                EXPECT_EQ(e.code(), JsonRpcErrorCode::MethodNotFound);
                throw;
            }
        },
        JsonRpcError
    );

    server_thread.join();
    client.stop();
}

TEST(JsonRpcClientTest, Notification)
{
    auto [client_transport, server_transport] = PipeTransport::create_pair();

    JsonRpcClient client(std::move(client_transport));
    MessageFramer server_framer(*server_transport);

    client.start();

    // Send notification
    client.notify("log", json{{"level", "info"}, {"message", "test"}});

    // Server receives
    auto msg = server_framer.read_message();
    auto notif = json::parse(msg);

    EXPECT_EQ(notif["method"], "log");
    EXPECT_EQ(notif["params"]["level"], "info");
    EXPECT_FALSE(notif.contains("id"));

    client.stop();
}

TEST(JsonRpcClientTest, NotificationHandler)
{
    auto [client_transport, server_transport] = PipeTransport::create_pair();

    JsonRpcClient client(std::move(client_transport));
    MessageFramer server_framer(*server_transport);

    std::string received_method;
    json received_params;
    std::mutex mutex;
    std::condition_variable cv;
    bool received = false;

    client.set_notification_handler(
        [&](const std::string& method, const json& params)
        {
            std::lock_guard<std::mutex> lock(mutex);
            received_method = method;
            received_params = params;
            received = true;
            cv.notify_one();
        }
    );

    client.start();

    // Server sends notification
    json notif = {
        {"jsonrpc", "2.0"},
        {"method", "session.event"},
        {"params", {{"sessionId", "s123"}, {"type", "message"}}}
    };
    server_framer.write_message(notif.dump());

    // Wait for handler
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::seconds(1), [&] { return received; });
    }

    EXPECT_TRUE(received);
    EXPECT_EQ(received_method, "session.event");
    EXPECT_EQ(received_params["sessionId"], "s123");

    client.stop();
}

TEST(JsonRpcClientTest, RequestHandler)
{
    auto [client_transport, server_transport] = PipeTransport::create_pair();

    JsonRpcClient client(std::move(client_transport));
    MessageFramer server_framer(*server_transport);

    // Client handles incoming requests (server-to-client calls)
    client.set_request_handler(
        [](const std::string& method, const json& params) -> json
        {
            if (method == "tool.call")
                return json{{"result", "tool executed: " + params["name"].get<std::string>()}};
            throw JsonRpcError(JsonRpcErrorCode::MethodNotFound, "Unknown method");
        }
    );

    client.start();

    // Server sends request to client
    json request = {
        {"jsonrpc", "2.0"}, {"method", "tool.call"}, {"params", {{"name", "read_file"}}}, {"id", 99}
    };
    server_framer.write_message(request.dump());

    // Server receives response
    auto response_str = server_framer.read_message();
    auto response = json::parse(response_str);

    EXPECT_EQ(response["id"], 99);
    EXPECT_EQ(response["result"]["result"], "tool executed: read_file");

    client.stop();
}

TEST(JsonRpcClientTest, RequestHandlerError)
{
    auto [client_transport, server_transport] = PipeTransport::create_pair();

    JsonRpcClient client(std::move(client_transport));
    MessageFramer server_framer(*server_transport);

    client.set_request_handler(
        [](const std::string& method, const json&) -> json
        { throw JsonRpcError(JsonRpcErrorCode::InvalidParams, "Missing required param"); }
    );

    client.start();

    json request = {{"jsonrpc", "2.0"}, {"method", "test"}, {"params", {}}, {"id", 1}};
    server_framer.write_message(request.dump());

    auto response_str = server_framer.read_message();
    auto response = json::parse(response_str);

    EXPECT_EQ(response["id"], 1);
    EXPECT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"]["code"], static_cast<int>(JsonRpcErrorCode::InvalidParams));

    client.stop();
}

TEST(JsonRpcClientTest, MultipleRequests)
{
    auto [client_transport, server_transport] = PipeTransport::create_pair();

    JsonRpcClient client(std::move(client_transport));
    MessageFramer server_framer(*server_transport);

    client.start();

    // Send multiple requests concurrently
    auto future1 = client.invoke("method1", json{{"n", 1}});
    auto future2 = client.invoke("method2", json{{"n", 2}});
    auto future3 = client.invoke("method3", json{{"n", 3}});

    // Server responds in reverse order
    std::thread server_thread(
        [&]
        {
            std::vector<json> requests;
            for (int i = 0; i < 3; ++i)
            {
                auto msg = server_framer.read_message();
                requests.push_back(json::parse(msg));
            }

            // Respond in reverse order
            for (auto it = requests.rbegin(); it != requests.rend(); ++it)
            {
                json response = {
                    {"jsonrpc", "2.0"},
                    {"result", {{"echo", (*it)["params"]["n"]}}},
                    {"id", (*it)["id"]}
                };
                server_framer.write_message(response.dump());
            }
        }
    );

    // Results should match regardless of response order
    EXPECT_EQ(future1.get()["echo"], 1);
    EXPECT_EQ(future2.get()["echo"], 2);
    EXPECT_EQ(future3.get()["echo"], 3);

    server_thread.join();
    client.stop();
}

TEST(JsonRpcClientTest, ConnectionClosed)
{
    // Test that client.stop() properly fails pending requests
    auto [client_transport, server_transport] = PipeTransport::create_pair();

    JsonRpcClient client(std::move(client_transport));
    client.start();

    auto future = client.invoke("test", nullptr, std::chrono::milliseconds{5000});

    // Ensure request is pending
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Stop client without responding - this should fail pending requests
    client.stop();

    // The future should throw because client was stopped
    bool threw_error = false;
    try
    {
        future.get();
    }
    catch (const JsonRpcError& e)
    {
        threw_error = true;
        EXPECT_EQ(e.code(), JsonRpcErrorCode::ConnectionClosed);
    }
    catch (const std::exception&)
    {
        threw_error = true;
        // Accept any exception - the important thing is it doesn't hang
    }

    EXPECT_TRUE(threw_error);
}

TEST(JsonRpcClientTest, StopWithPendingRequests)
{
    auto [client_transport, server_transport] = PipeTransport::create_pair();

    JsonRpcClient client(std::move(client_transport));
    client.start();

    auto future = client.invoke("slow.method");

    // Stop without responding
    client.stop();

    EXPECT_THROW(future.get(), JsonRpcError);
}

// =============================================================================
// Error Type Tests
// =============================================================================

TEST(JsonRpcErrorTest, ErrorCodeValues)
{
    EXPECT_EQ(static_cast<int>(JsonRpcErrorCode::ParseError), -32700);
    EXPECT_EQ(static_cast<int>(JsonRpcErrorCode::InvalidRequest), -32600);
    EXPECT_EQ(static_cast<int>(JsonRpcErrorCode::MethodNotFound), -32601);
    EXPECT_EQ(static_cast<int>(JsonRpcErrorCode::InvalidParams), -32602);
    EXPECT_EQ(static_cast<int>(JsonRpcErrorCode::InternalError), -32603);
}

TEST(JsonRpcErrorTest, ErrorWithData)
{
    JsonRpcError error(JsonRpcErrorCode::InvalidParams, "Bad params", json{{"field", "name"}});

    EXPECT_EQ(error.code(), JsonRpcErrorCode::InvalidParams);
    EXPECT_STREQ(error.what(), "Bad params");
    EXPECT_EQ(error.data()["field"], "name");
}

// =============================================================================
// Large Payload Tests
// =============================================================================

TEST(JsonRpc, LargePayload64KBBoundary)
{
    auto [client_transport, server_transport] = PipeTransport::create_pair();
    JsonRpcClient client(std::move(client_transport));
    MessageFramer server_framer(*server_transport);

    client.start();

    // Create payload exactly at 64KB boundary
    std::string large_data(64 * 1024, 'A');
    auto future = client.invoke("large.method", json{{"data", large_data}});

    std::thread server_thread(
        [&]
        {
            auto msg = server_framer.read_message();
            auto req = json::parse(msg);
            EXPECT_EQ(req["method"], "large.method");
            EXPECT_EQ(req["params"]["data"].get<std::string>().size(), 64 * 1024);

            json response = {
                {"jsonrpc", "2.0"}, {"result", {{"ok", true}}}, {"id", req["id"]}
            };
            server_framer.write_message(response.dump());
        }
    );

    auto result = future.get();
    EXPECT_EQ(result["ok"], true);

    server_thread.join();
    client.stop();
}

TEST(JsonRpc, LargePayload70KB)
{
    auto [client_transport, server_transport] = PipeTransport::create_pair();
    JsonRpcClient client(std::move(client_transport));
    MessageFramer server_framer(*server_transport);

    client.start();

    std::string large_data(70 * 1024, 'B');
    auto future = client.invoke("large.method", json{{"data", large_data}});

    std::thread server_thread(
        [&]
        {
            auto msg = server_framer.read_message();
            auto req = json::parse(msg);
            EXPECT_EQ(req["params"]["data"].get<std::string>().size(), 70 * 1024);

            json response = {
                {"jsonrpc", "2.0"}, {"result", {{"ok", true}}}, {"id", req["id"]}
            };
            server_framer.write_message(response.dump());
        }
    );

    auto result = future.get();
    EXPECT_EQ(result["ok"], true);

    server_thread.join();
    client.stop();
}

TEST(JsonRpc, LargePayload100KB)
{
    auto [client_transport, server_transport] = PipeTransport::create_pair();
    JsonRpcClient client(std::move(client_transport));
    MessageFramer server_framer(*server_transport);

    client.start();

    std::string large_data(100 * 1024, 'C');
    auto future = client.invoke("large.method", json{{"data", large_data}});

    std::thread server_thread(
        [&]
        {
            auto msg = server_framer.read_message();
            auto req = json::parse(msg);
            EXPECT_EQ(req["params"]["data"].get<std::string>().size(), 100 * 1024);

            json response = {
                {"jsonrpc", "2.0"}, {"result", {{"size", 100 * 1024}}}, {"id", req["id"]}
            };
            server_framer.write_message(response.dump());
        }
    );

    auto result = future.get();
    EXPECT_EQ(result["size"], 100 * 1024);

    server_thread.join();
    client.stop();
}

TEST(JsonRpc, EOFOnPartialData)
{
    auto [client_transport, server_transport] = PipeTransport::create_pair();
    JsonRpcClient client(std::move(client_transport));

    client.start();

    auto future = client.invoke("test.method");

    // Close server transport immediately to simulate EOF on partial data
    server_transport->close();

    // The future should eventually fail or throw
    EXPECT_THROW(future.get(), std::exception);

    client.stop();
}
