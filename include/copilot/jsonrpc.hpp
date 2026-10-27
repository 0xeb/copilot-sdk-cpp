// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <copilot/cancellation.hpp>
#include <copilot/transport.hpp>
#include <copilot/types.hpp>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace copilot
{

// =============================================================================
// JSON-RPC 2.0 Exceptions
// =============================================================================

/// JSON-RPC error codes (standard and custom)
enum class JsonRpcErrorCode : int
{
    // Standard JSON-RPC 2.0 errors
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,

    // Server errors (-32000 to -32099)
    ServerError = -32000,

    // Custom errors
    RequestCancelled = -32800,
    ConnectionClosed = -32801,
    Timeout = -32802,
};

/// Exception for JSON-RPC errors
class JsonRpcError : public std::runtime_error
{
  public:
    /// A null `data` means "no data" here; use with_data() to carry an explicit
    /// `data: null`.
    JsonRpcError(JsonRpcErrorCode code, const std::string& message, const json& data = nullptr)
        : std::runtime_error(message), code_(code), data_(data), has_data_(!data.is_null())
    {
    }

    /// Error whose `data` member is preserved exactly: nullopt = absent,
    /// json(nullptr) = present and null.
    /// C# REFERENCE: dotnet/src/JsonRpc.cs (4001c1da7, structured error data)
    static JsonRpcError
    with_data(JsonRpcErrorCode code, const std::string& message, std::optional<json> data)
    {
        JsonRpcError error(code, message);
        error.has_data_ = data.has_value();
        error.data_ = data.value_or(json(nullptr));
        return error;
    }

    JsonRpcErrorCode code() const
    {
        return code_;
    }
    /// The `data` member; null when absent (see has_data()).
    const json& data() const
    {
        return data_;
    }
    /// True when the wire error carried a `data` member, even `data: null`.
    bool has_data() const noexcept
    {
        return has_data_;
    }
    std::optional<json> data_if_present() const
    {
        return has_data_ ? std::optional<json>(data_) : std::nullopt;
    }

  private:
    JsonRpcErrorCode code_;
    json data_;
    bool has_data_ = false;
};

// =============================================================================
// JSON-RPC 2.0 Message Types
// =============================================================================

/// JSON-RPC request ID (can be string or integer)
using JsonRpcId = std::variant<std::string, int64_t>;

/// Convert JsonRpcId to JSON
inline json id_to_json(const JsonRpcId& id)
{
    return std::visit([](const auto& v) -> json { return v; }, id);
}

/// Parse JsonRpcId from JSON
inline JsonRpcId id_from_json(const json& j)
{
    if (j.is_string())
        return j.get<std::string>();
    else if (j.is_number_integer())
        return j.get<int64_t>();
    throw std::runtime_error("Invalid JSON-RPC id type");
}

/// JSON-RPC 2.0 Request
struct JsonRpcRequest
{
    std::string method;
    json params;
    std::optional<JsonRpcId> id; // nullopt for notifications

    json to_json() const
    {
        json j = {{"jsonrpc", "2.0"}, {"method", method}};
        if (!params.is_null())
            j["params"] = params;
        if (id)
            j["id"] = id_to_json(*id);
        return j;
    }

    static JsonRpcRequest from_json(const json& j)
    {
        JsonRpcRequest req;
        req.method = j.at("method").get<std::string>();
        if (j.contains("params"))
            req.params = j.at("params");
        if (j.contains("id") && !j.at("id").is_null())
            req.id = id_from_json(j.at("id"));
        return req;
    }

    bool is_notification() const
    {
        return !id.has_value();
    }
};

/// JSON-RPC 2.0 Error object
struct JsonRpcErrorObject
{
    int code;
    std::string message;
    /// nullopt = no `data` member; json(nullptr) = `"data": null`.
    std::optional<json> data;

    json to_json() const
    {
        json j = {{"code", code}, {"message", message}};
        if (data)
            j["data"] = *data;
        return j;
    }

    static JsonRpcErrorObject from_json(const json& j)
    {
        JsonRpcErrorObject err;
        err.code = j.at("code").get<int>();
        err.message = j.at("message").get<std::string>();
        if (j.contains("data"))
            err.data = j.at("data");
        return err;
    }
};

/// JSON-RPC 2.0 Response
struct JsonRpcResponse
{
    JsonRpcId id;
    std::optional<json> result;
    std::optional<JsonRpcErrorObject> error;

    json to_json() const
    {
        json j = {{"jsonrpc", "2.0"}, {"id", id_to_json(id)}};
        if (result)
            j["result"] = *result;
        if (error)
            j["error"] = error->to_json();
        return j;
    }

    static JsonRpcResponse from_json(const json& j)
    {
        JsonRpcResponse resp;
        if (j.contains("id") && !j.at("id").is_null())
            resp.id = id_from_json(j.at("id"));
        if (j.contains("result"))
            resp.result.emplace(j.at("result"));
        if (j.contains("error"))
            resp.error = JsonRpcErrorObject::from_json(j.at("error"));
        return resp;
    }

    bool is_error() const
    {
        return error.has_value();
    }
};

// =============================================================================
// Pending Request Tracking
// =============================================================================

/// Holds state for a pending request awaiting response
struct PendingRequest
{
    std::promise<json> promise;
    std::function<void(const json&)> on_success;
    std::chrono::steady_clock::time_point deadline;

    PendingRequest(std::chrono::milliseconds timeout = std::chrono::milliseconds{0})
        : deadline(
              timeout.count() > 0 ? std::chrono::steady_clock::now() + timeout
                                  : std::chrono::steady_clock::time_point::max()
          )
    {
    }
};

// =============================================================================
// JSON-RPC Client
// =============================================================================

/// Handler for incoming notifications
using NotificationHandler = std::function<void(const std::string& method, const json& params)>;

/// Handler for incoming requests (returns response result or throws)
using RequestHandler = std::function<json(const std::string& method, const json& params)>;

/// Handler for incoming requests that can observe cancellation. The token is
/// signalled by a matching `$/cancelRequest` or when the connection closes.
using CancellableRequestHandler = std::function<
    json(const std::string& method, const json& params, const CancellationToken& cancellation)>;

/// JSON-RPC 2.0 client with bidirectional communication
///
/// Features:
/// - Send requests and await responses (with timeout)
/// - Send notifications (fire-and-forget)
/// - Handle incoming notifications via callback (in order, on the read thread)
/// - Handle incoming requests (server-to-client calls) via callback, each on its
///   own worker thread so a handler may itself call invoke() and wait
/// - Incoming `$/cancelRequest` support
/// - Background read loop with automatic dispatch
class JsonRpcClient
{
  public:
    /// Construct client with transport and message framer
    /// @param transport The underlying transport (takes ownership)
    explicit JsonRpcClient(std::unique_ptr<ITransport> transport)
        : wire_(std::make_shared<Wire>(std::move(transport))),
          incoming_(std::make_shared<IncomingRegistry>()), next_id_(1), running_(false)
    {
    }

    ~JsonRpcClient()
    {
        stop();
    }

    // Non-copyable, non-movable (due to mutex/thread)
    JsonRpcClient(const JsonRpcClient&) = delete;
    JsonRpcClient& operator=(const JsonRpcClient&) = delete;
    JsonRpcClient(JsonRpcClient&&) = delete;
    JsonRpcClient& operator=(JsonRpcClient&&) = delete;

    /// Start the background read loop
    void start()
    {
        if (running_.exchange(true))
            return; // Already running

        read_thread_ = std::thread([this] { read_loop(); });
        timeout_thread_ = std::thread([this] { timeout_loop(); });
    }

    /// Stop the client and close connection
    void stop()
    {
        bool was_running = running_.exchange(false);

        pending_cv_.notify_all();

        // Close transport to unblock read
        if (was_running && wire_->transport)
            wire_->transport->close();

        // Wait for read thread
        if (read_thread_.joinable())
        {
            if (read_thread_.get_id() == std::this_thread::get_id())
                read_thread_.detach();
            else
                read_thread_.join();
        }

        if (timeout_thread_.joinable())
            timeout_thread_.join();

        // Fail all pending requests
        fail_all_pending(JsonRpcErrorCode::ConnectionClosed, "Connection closed");
        notify_closed();
        join_workers();
    }

    void signal_connection_closed()
    {
        fail_all_pending(JsonRpcErrorCode::ConnectionClosed, "Runtime process exited");
        notify_closed();
        if (wire_->transport)
            wire_->transport->close();
    }

    /// Check if client is running
    bool is_running() const
    {
        return running_;
    }

    /// Set handler for incoming notifications
    void set_notification_handler(NotificationHandler handler)
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        notification_handler_ = std::move(handler);
    }

    /// Set handler for incoming requests
    void set_request_handler(RequestHandler handler)
    {
        if (!handler)
        {
            set_request_handler(CancellableRequestHandler{});
            return;
        }
        set_request_handler(CancellableRequestHandler(
            [handler = std::move(handler
             )](const std::string& method, const json& params, const CancellationToken&)
            { return handler(method, params); }
        ));
    }

    /// Set a cancellation-aware handler for incoming requests
    void set_request_handler(CancellableRequestHandler handler)
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        request_handler_ = std::move(handler);
    }

    /// Called once when the connection closes (read EOF or stop()), after
    /// pending outgoing requests have failed and incoming tokens are cancelled.
    void set_on_closed(std::function<void()> callback)
    {
        std::lock_guard<std::mutex> lock(handlers_mutex_);
        on_closed_ = std::move(callback);
    }

    /// Signalled when the connection closes.
    CancellationToken connection_closed_token() const
    {
        return incoming_->connection_closed.token();
    }

    /// Send a request and await response
    /// @param method The method name
    /// @param params The parameters (can be object or array)
    /// @param timeout Request timeout (0 = no timeout)
    /// @return Future that resolves to the result. Once the connection has
    ///         failed the future is already failed with ConnectionClosed and
    ///         nothing is written.
    std::future<json> invoke(
        const std::string& method,
        const json& params = nullptr,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{30000}
    )
    {
        return invoke_with_id(method, params, timeout).second;
    }

    /// Run completion on the read thread before subsequent inbound messages are dispatched.
    std::future<json> invoke_with_completion(
        const std::string& method, const json& params, std::function<void(const json&)> completion
    )
    {
        return invoke_with_id(
                   method, params, std::chrono::milliseconds(30000), std::move(completion)
        )
            .second;
    }

    /// Send a request and wait for response synchronously
    template <typename T = json>
    T invoke_sync(
        const std::string& method,
        const json& params = nullptr,
        std::chrono::milliseconds timeout = std::chrono::milliseconds{30000}
    )
    {
        auto [id, future] = invoke_with_id(method, params, timeout);

        if (timeout.count() > 0)
        {
            auto status = future.wait_for(timeout);
            if (status == std::future_status::timeout)
            {
                fail_pending_request(id, JsonRpcErrorCode::Timeout, "Request timed out");
                throw JsonRpcError(JsonRpcErrorCode::Timeout, "Request timed out");
            }
        }

        auto result = future.get();

        if constexpr (std::is_same_v<T, json>)
            return result;
        else
            return result.get<T>();
    }

    /// Send a notification (no response expected)
    void notify(const std::string& method, const json& params = nullptr)
    {
        JsonRpcRequest request{method, params, std::nullopt};
        send_message(request.to_json());
    }

    /// Send a response to an incoming request
    void send_response(const JsonRpcId& id, const json& result)
    {
        wire_->send_response(id, result);
    }

    /// Send an error response to an incoming request (a null `data` is omitted)
    void send_error_response(
        const JsonRpcId& id, int code, const std::string& message, const json& data = nullptr
    )
    {
        wire_->send_error(
            id, code, message, data.is_null() ? std::nullopt : std::optional<json>(data)
        );
    }

  private:
    // State shared with request workers. Workers capture these by shared_ptr and
    // never touch the JsonRpcClient itself, so a handler that ends up destroying
    // the client (e.g. Client::stop() from inside a tool) cannot leave a worker
    // writing through a dangling `this`.
    struct Wire
    {
        explicit Wire(std::unique_ptr<ITransport> t) : transport(std::move(t)), framer(*transport)
        {
        }

        std::unique_ptr<ITransport> transport;
        MessageFramer framer;
        std::mutex write_mutex;

        void send(const json& message)
        {
            std::lock_guard<std::mutex> lock(write_mutex);
            framer.write_message(message.dump());
        }

        void send_response(const JsonRpcId& id, const json& result)
        {
            send(JsonRpcResponse{id, std::optional<json>{result}, std::nullopt}.to_json());
        }

        void send_error(
            const JsonRpcId& id,
            int code,
            const std::string& message,
            std::optional<json> data = std::nullopt
        )
        {
            send(JsonRpcResponse{
                id, std::nullopt, JsonRpcErrorObject{code, message, std::move(data)}
            }.to_json());
        }

        /// Best effort: the peer may already be gone.
        template <class F>
        void try_send(F&& f) noexcept
        {
            try
            {
                f();
            }
            catch (...)
            {
            }
        }
    };

    // C# REFERENCE: dotnet/src/JsonRpc.cs IncomingRequestCancellation
    struct IncomingCancel
    {
        int64_t id = 0;
        CancellationSource source; // handler token: request cancel OR connection closed
        std::atomic<bool> request_cancelled{false};
        std::atomic<bool> responded{false};

        /// Claim the single response slot for this request.
        bool claim() noexcept
        {
            return !responded.exchange(true);
        }
    };

    struct IncomingRegistry
    {
        std::mutex mutex;
        std::map<int64_t, std::shared_ptr<IncomingCancel>> requests;
        CancellationSource connection_closed;

        void retire(const std::shared_ptr<IncomingCancel>& request)
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = requests.find(request->id);
            if (it != requests.end() && it->second == request)
                requests.erase(it);
        }
    };

    struct Worker
    {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    static constexpr int kRequestCancelledCode =
        static_cast<int>(JsonRpcErrorCode::RequestCancelled);

    /// Numeric request ids only; string ids and values outside int64 do not
    /// participate in cancellation.
    static std::optional<int64_t> numeric_request_id(const json& id)
    {
        if (id.is_number_unsigned())
        {
            const auto value = id.get<uint64_t>();
            if (value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                return std::nullopt;
            return static_cast<int64_t>(value);
        }
        if (id.is_number_integer())
            return id.get<int64_t>();
        return std::nullopt;
    }

    /// Internal invoke that returns both request ID and future for cleanup purposes
    std::pair<int64_t, std::future<json>> invoke_with_id(
        const std::string& method,
        const json& params,
        std::chrono::milliseconds timeout,
        std::function<void(const json&)> completion = {}
    )
    {
        auto id = next_id_++;

        auto pending = std::make_shared<PendingRequest>(timeout);
        pending->on_success = std::move(completion);
        auto future = pending->promise.get_future();

        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            // Sticky terminal error: once the connection has failed, fail fast
            // instead of writing and waiting out the timeout.
            // C# REFERENCE: dotnet/src/JsonRpc.cs InvokeAsync (_terminalError, 9515b5e85)
            if (terminal_)
            {
                pending->promise.set_exception(
                    std::make_exception_ptr(JsonRpcError(*terminal_, "Connection closed"))
                );
                return {id, std::move(future)};
            }
            pending_requests_[id] = pending;
        }
        pending_cv_.notify_all();

        JsonRpcRequest request{method, params, JsonRpcId{id}};

        try
        {
            send_message(request.to_json());
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_requests_.erase(id);
            throw;
        }

        return {id, std::move(future)};
    }

    void timeout_loop()
    {
        using clock = std::chrono::steady_clock;

        while (running_)
        {
            std::vector<std::shared_ptr<PendingRequest>> expired;
            auto now = clock::now();
            auto next_deadline = clock::time_point::max();

            {
                std::unique_lock<std::mutex> lock(pending_mutex_);
                for (auto it = pending_requests_.begin(); it != pending_requests_.end();)
                {
                    const auto& pending = it->second;
                    if (pending->deadline <= now)
                    {
                        expired.push_back(pending);
                        it = pending_requests_.erase(it);
                    }
                    else
                    {
                        next_deadline = std::min(next_deadline, pending->deadline);
                        ++it;
                    }
                }

                if (expired.empty())
                {
                    if (!running_)
                        break;
                    if (next_deadline == clock::time_point::max())
                    {
                        pending_cv_.wait_for(
                            lock,
                            std::chrono::milliseconds(250),
                            [this] { return !running_.load(); }
                        );
                    }
                    else
                    {
                        pending_cv_.wait_until(
                            lock, next_deadline, [this] { return !running_.load(); }
                        );
                    }
                    continue;
                }
            }

            for (auto& pending : expired)
            {
                try
                {
                    pending->promise.set_exception(std::make_exception_ptr(
                        JsonRpcError(JsonRpcErrorCode::Timeout, "Request timed out")
                    ));
                }
                catch (...)
                {
                    // Ignore if already fulfilled
                }
            }
        }
    }

    void fail_pending_request(int64_t id, JsonRpcErrorCode code, const std::string& message)
    {
        std::shared_ptr<PendingRequest> pending;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.find(id);
            if (it == pending_requests_.end())
                return;
            pending = it->second;
            pending_requests_.erase(it);
        }
        try
        {
            pending->promise.set_exception(std::make_exception_ptr(JsonRpcError(code, message)));
        }
        catch (...)
        {
            // Ignore if already fulfilled
        }
    }

    void send_message(const json& message)
    {
        wire_->send(message);
    }

    void read_loop()
    {
        while (running_)
        {
            try
            {
                auto message_str = wire_->framer.read_message();
                auto message = json::parse(message_str);
                dispatch_message(message);
            }
            catch (const ConnectionClosedError&)
            {
                running_ = false;
                fail_all_pending(JsonRpcErrorCode::ConnectionClosed, "Connection closed");
                notify_closed();
                break;
            }
            catch (const json::exception& e)
            {
                // JSON parse error - log and continue
                // In a real implementation, would use a logger
                continue;
            }
            catch (const std::exception& e)
            {
                // Other errors - continue if still running
                if (!running_)
                    break;
            }
        }
    }

    void dispatch_message(const json& message)
    {
        // Check if it's a response (has id and result/error, no method)
        if (message.contains("id") && !message.at("id").is_null() &&
            (message.contains("result") || message.contains("error")) &&
            !message.contains("method"))
        {
            handle_response(message);
            return;
        }

        // Check if it's a request or notification (has method)
        if (message.contains("method"))
        {
            auto request = JsonRpcRequest::from_json(message);
            // Intercepted before any routing, whether or not it carries an id.
            if (request.method == "$/cancelRequest")
            {
                handle_cancel_request(request.params);
                return;
            }
            if (request.is_notification())
                handle_notification(request);
            else
                handle_request(request, message.at("id"));
            return;
        }

        // Unknown message format - ignore
    }

    void handle_response(const json& message)
    {
        auto response = JsonRpcResponse::from_json(message);

        // Find pending request by ID
        int64_t id = 0;
        if (auto* int_id = std::get_if<int64_t>(&response.id))
        {
            id = *int_id;
        }
        else
        {
            // String IDs not used for our outgoing requests
            return;
        }

        std::shared_ptr<PendingRequest> pending;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            auto it = pending_requests_.find(id);
            if (it == pending_requests_.end())
                return; // Unknown request ID
            pending = it->second;
            pending_requests_.erase(it);
        }

        // Resolve or reject the promise
        if (response.is_error())
        {
            auto& err = *response.error;
            pending->promise.set_exception(std::make_exception_ptr(JsonRpcError::with_data(
                static_cast<JsonRpcErrorCode>(err.code), err.message, err.data
            )));
        }
        else
        {
            try
            {
                const auto result = response.result.value_or(nullptr);
                if (pending->on_success)
                    pending->on_success(result);
                pending->promise.set_value(result);
            }
            catch (...)
            {
                pending->promise.set_exception(std::current_exception());
            }
        }
    }

    void handle_notification(const JsonRpcRequest& request)
    {
        NotificationHandler handler;
        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            handler = notification_handler_;
        }

        if (handler)
        {
            try
            {
                handler(request.method, request.params);
            }
            catch (...)
            {
                // Notification handlers should not throw
            }
        }
    }

    // C# REFERENCE: dotnet/src/JsonRpc.cs HandleCancelRequest
    void handle_cancel_request(const json& params)
    {
        if (!params.is_object() || !params.contains("id"))
            return;
        const auto id = numeric_request_id(params.at("id"));
        if (!id)
            return;

        std::shared_ptr<IncomingCancel> request;
        {
            std::lock_guard<std::mutex> lock(incoming_->mutex);
            const auto it = incoming_->requests.find(*id);
            if (it == incoming_->requests.end())
                return;
            request = it->second;
        }

        request->request_cancelled = true;
        // Signal the handler before replying (d12079020).
        request->source.cancel();
        if (request->claim())
        {
            wire_->try_send(
                [&]
                { wire_->send_error(JsonRpcId{*id}, kRequestCancelledCode, "Request cancelled"); }
            );
        }
    }

    // C# REFERENCE: dotnet/src/JsonRpc.cs HandleIncomingMethodAsync
    void handle_request(const JsonRpcRequest& request, const json& raw_id)
    {
        CancellableRequestHandler handler;
        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            handler = request_handler_;
        }

        if (!handler)
        {
            // No handler - respond with method not found
            wire_->try_send(
                [&]
                {
                    wire_->send_error(
                        *request.id,
                        static_cast<int>(JsonRpcErrorCode::MethodNotFound),
                        "Method not found: " + request.method
                    );
                }
            );
            return;
        }

        // Register for cancellation (numeric ids only; a duplicate id gets no
        // registration and only observes connection close).
        std::shared_ptr<IncomingCancel> cancel;
        CancellationToken token = incoming_->connection_closed.token();
        if (const auto numeric = numeric_request_id(raw_id))
        {
            auto candidate = std::make_shared<IncomingCancel>();
            candidate->id = *numeric;
            std::lock_guard<std::mutex> lock(incoming_->mutex);
            if (incoming_->connection_closed.is_cancelled())
                candidate->source.cancel();
            if (incoming_->requests.emplace(*numeric, candidate).second)
            {
                cancel = candidate;
                token = candidate->source.token();
            }
        }

        // Each request runs on its own worker so a handler can call invoke() and
        // wait for the reply, which only this read thread can deliver.
        spawn_worker(
            [wire = wire_,
             incoming = incoming_,
             handler = std::move(handler),
             request,
             cancel,
             token]()
            {
                const JsonRpcId id = *request.id;
                auto cancelled = [&] { return cancel && cancel->request_cancelled.load(); };
                auto reply_cancelled = [&]
                {
                    if (cancel->claim())
                        wire->try_send(
                            [&]
                            { wire->send_error(id, kRequestCancelledCode, "Request cancelled"); }
                        );
                };
                auto claim = [&] { return !cancel || cancel->claim(); };
                auto reply_error = [&](int code,
                                       const std::string& message,
                                       std::optional<json> data = std::nullopt)
                {
                    if (cancelled())
                        reply_cancelled();
                    else if (claim())
                        wire->try_send([&] { wire->send_error(id, code, message, data); });
                };

                try
                {
                    auto result = handler(request.method, request.params, token);
                    if (cancelled())
                        reply_cancelled(); // late result is dropped
                    else if (claim())
                        wire->try_send([&] { wire->send_response(id, result); });
                }
                catch (const JsonRpcError& e)
                {
                    reply_error(static_cast<int>(e.code()), e.what(), e.data_if_present());
                }
                catch (const std::exception& e)
                {
                    reply_error(static_cast<int>(JsonRpcErrorCode::InternalError), e.what());
                }
                catch (...)
                {
                    reply_error(static_cast<int>(JsonRpcErrorCode::InternalError), "Unknown error");
                }
                if (cancel)
                    incoming->retire(cancel);
            }
        );
    }

    void spawn_worker(std::function<void()> work)
    {
        std::vector<std::thread> finished;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            for (auto it = workers_.begin(); it != workers_.end();)
            {
                if (it->done->load())
                {
                    finished.push_back(std::move(it->thread));
                    it = workers_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            auto done = std::make_shared<std::atomic<bool>>(false);
            workers_.push_back(Worker{
                std::thread(
                    [work = std::move(work), done]()
                    {
                        work();
                        done->store(true);
                    }
                ),
                done
            });
        }
        for (auto& thread : finished)
            if (thread.joinable())
                thread.join();
    }

    void join_workers()
    {
        std::vector<Worker> workers;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            workers.swap(workers_);
        }
        for (auto& worker : workers)
        {
            if (!worker.thread.joinable())
                continue;
            if (worker.thread.get_id() == std::this_thread::get_id())
                worker.thread.detach(); // stop() called from inside a handler
            else
                worker.thread.join();
        }
    }

    /// Cancel every incoming handler token and fire on_closed, once.
    void notify_closed()
    {
        std::vector<std::shared_ptr<IncomingCancel>> requests;
        {
            std::lock_guard<std::mutex> lock(incoming_->mutex);
            if (incoming_->connection_closed.is_cancelled())
                return;
            incoming_->connection_closed.cancel();
            for (auto& [_, request] : incoming_->requests)
                requests.push_back(request);
        }
        for (auto& request : requests)
            request->source.cancel();

        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(handlers_mutex_);
            callback = on_closed_;
        }
        if (callback)
        {
            try
            {
                callback();
            }
            catch (...)
            {
            }
        }
    }

    void fail_all_pending(JsonRpcErrorCode code, const std::string& message)
    {
        std::vector<std::shared_ptr<PendingRequest>> to_fail;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            if (!terminal_)
                terminal_ = code;
            to_fail.reserve(pending_requests_.size());
            for (auto& [id, pending] : pending_requests_)
                to_fail.push_back(pending);
            pending_requests_.clear();
        }
        for (auto& pending : to_fail)
        {
            try
            {
                pending->promise.set_exception(std::make_exception_ptr(JsonRpcError(code, message))
                );
            }
            catch (...)
            {
                // Ignore if already fulfilled
            }
        }
    }

    std::shared_ptr<Wire> wire_;
    std::shared_ptr<IncomingRegistry> incoming_;
    std::atomic<int64_t> next_id_;
    std::atomic<bool> running_;

    std::thread read_thread_;
    std::thread timeout_thread_;

    std::mutex pending_mutex_;
    std::condition_variable pending_cv_;
    std::map<int64_t, std::shared_ptr<PendingRequest>> pending_requests_;
    std::optional<JsonRpcErrorCode> terminal_;

    std::mutex handlers_mutex_;
    NotificationHandler notification_handler_;
    CancellableRequestHandler request_handler_;
    std::function<void()> on_closed_;

    std::mutex workers_mutex_;
    std::vector<Worker> workers_;
};

} // namespace copilot
