// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <copilot/types.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace copilot
{

struct CopilotRequestContext
{
    std::string request_id;
    std::optional<std::string> session_id;
    std::optional<std::string> agent_id;
    std::optional<std::string> parent_agent_id;
    std::optional<std::string> interaction_type;
    std::string transport = "http";
    std::string url;
    std::map<std::string, std::vector<std::string>> headers;
    std::shared_ptr<std::atomic_bool> cancelled;

    bool is_cancelled() const noexcept { return cancelled && cancelled->load(); }
};

struct CopilotHttpRequest
{
    std::string method;
    std::string url;
    std::map<std::string, std::vector<std::string>> headers;
    std::string body;
};

struct CopilotHttpResponse
{
    int status = 200;
    std::optional<std::string> status_text;
    std::map<std::string, std::vector<std::string>> headers;
    std::vector<std::string> chunks;
    bool binary = false;
};

struct CopilotWebSocketCloseStatus
{
    std::optional<std::string> description;
    std::optional<std::string> error_code;
    std::optional<std::string> error;
};

class CopilotWebSocketHandler
{
  public:
    using ResponseSender =
        std::function<void(const std::string&, bool, bool, const std::optional<std::string>&)>;

    virtual ~CopilotWebSocketHandler() = default;
    virtual void open(const CopilotRequestContext&) {}
    virtual void send_request_message(const std::string& data, bool binary) = 0;
    virtual void close(const CopilotWebSocketCloseStatus& status)
    {
        if (status.error)
            fail_response(*status.error);
        else
            complete_response();
    }

    void send_response_message(const std::string& data, bool binary = false)
    {
        if (!response_sender_)
            throw std::runtime_error("WebSocket response bridge is not attached");
        response_sender_(data, binary, false, std::nullopt);
    }

    void complete_response()
    {
        if (response_sender_)
            response_sender_("", false, true, std::nullopt);
    }

    void fail_response(const std::string& message)
    {
        if (response_sender_)
            response_sender_("", false, true, message);
    }

    void bind_response_sender(ResponseSender sender)
    {
        response_sender_ = std::move(sender);
    }

  private:
    ResponseSender response_sender_;
};

class CopilotRequestHandler
{
  public:
    virtual ~CopilotRequestHandler() = default;
    virtual CopilotHttpResponse send_request(
        const CopilotHttpRequest& request,
        const CopilotRequestContext& context) = 0;
    virtual std::shared_ptr<CopilotWebSocketHandler> open_websocket(
        const CopilotRequestContext&)
    {
        return {};
    }
};

/// Adapts official httpRequestStart/httpRequestChunk callbacks to high-level handlers.
class CopilotRequestHandlerBridge
    : public std::enable_shared_from_this<CopilotRequestHandlerBridge>
{
  public:
    using SendRpc = std::function<void(const std::string&, const json&)>;

    CopilotRequestHandlerBridge(
        std::shared_ptr<CopilotRequestHandler> handler,
        SendRpc send_rpc);
    ~CopilotRequestHandlerBridge();

    json handle(const std::string& method, const json& params);
    void cancel_all();
    std::size_t pending_count() const;

  private:
    struct Exchange;
    void complete_http(const std::shared_ptr<Exchange>& exchange);
    void fail(const std::shared_ptr<Exchange>& exchange, const std::string& message);

    std::shared_ptr<CopilotRequestHandler> handler_;
    SendRpc send_rpc_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Exchange>> pending_;
};

} // namespace copilot
