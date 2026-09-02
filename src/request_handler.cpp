// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/request_handler.hpp>
#include <thread>

namespace copilot
{

struct CopilotRequestHandlerBridge::Exchange
{
    CopilotRequestContext context;
    CopilotHttpRequest request;
    std::shared_ptr<CopilotWebSocketHandler> websocket;
};

CopilotRequestHandlerBridge::CopilotRequestHandlerBridge(
    std::shared_ptr<CopilotRequestHandler> handler,
    SendRpc send_rpc)
    : handler_(std::move(handler)), send_rpc_(std::move(send_rpc))
{
}

CopilotRequestHandlerBridge::~CopilotRequestHandlerBridge()
{
    cancel_all();
}

json CopilotRequestHandlerBridge::handle(
    const std::string& method, const json& params)
{
    if (method == "llmInference.httpRequestStart")
    {
        auto exchange = std::make_shared<Exchange>();
        exchange->context.request_id = params.at("requestId").get<std::string>();
        if (params.contains("sessionId") && params["sessionId"].is_string())
            exchange->context.session_id = params["sessionId"].get<std::string>();
        if (params.contains("agentId") && params["agentId"].is_string())
            exchange->context.agent_id = params["agentId"].get<std::string>();
        if (params.contains("parentAgentId") && params["parentAgentId"].is_string())
            exchange->context.parent_agent_id =
                params["parentAgentId"].get<std::string>();
        if (params.contains("interactionType") && params["interactionType"].is_string())
            exchange->context.interaction_type =
                params["interactionType"].get<std::string>();
        exchange->context.transport = params.value("transport", "http");
        exchange->context.url = params.at("url").get<std::string>();
        exchange->context.headers = params.value(
            "headers", std::map<std::string, std::vector<std::string>>{});
        exchange->context.cancelled = std::make_shared<std::atomic_bool>(false);
        exchange->request.method = params.at("method").get<std::string>();
        exchange->request.url = exchange->context.url;
        exchange->request.headers = exchange->context.headers;

        if (exchange->context.transport == "websocket")
        {
            exchange->websocket = handler_->open_websocket(exchange->context);
            if (!exchange->websocket)
                throw std::runtime_error("WebSocket request was not accepted");
            const auto request_id = exchange->context.request_id;
            const auto send_rpc = send_rpc_;
            exchange->websocket->bind_response_sender(
                [request_id, send_rpc](
                    const std::string& data,
                    bool binary,
                    bool end,
                    const std::optional<std::string>& error)
                {
                    json params{
                        {"requestId", request_id},
                        {"data", data},
                        {"binary", binary},
                        {"end", end},
                    };
                    if (error)
                        params["error"] = json{{"message", *error}};
                    send_rpc("llmInference.httpResponseChunk", params);
                });
            send_rpc_(
                "llmInference.httpResponseStart",
                json{
                    {"requestId", exchange->context.request_id},
                    {"status", 101},
                    {"headers", json::object()},
                });
            exchange->websocket->open(exchange->context);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        pending_[exchange->context.request_id] = exchange;
        return json::object();
    }

    if (method != "llmInference.httpRequestChunk")
        throw std::invalid_argument("unsupported request-handler method: " + method);

    const auto request_id = params.at("requestId").get<std::string>();
    std::shared_ptr<Exchange> exchange;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.find(request_id);
        if (it == pending_.end())
            return json::object();
        exchange = it->second;
    }

    if (params.value("cancel", false))
    {
        exchange->context.cancelled->store(true);
        if (exchange->websocket)
            exchange->websocket->close(
                CopilotWebSocketCloseStatus{
                    .description = params.value("cancelReason", "cancelled"),
                });
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(request_id);
        return json::object();
    }

    const auto data = params.value("data", "");
    const bool binary = params.value("binary", false);
    if (exchange->websocket)
        exchange->websocket->send_request_message(data, binary);
    else
        exchange->request.body += data;

    if (params.value("end", false))
    {
        if (exchange->websocket)
        {
            exchange->websocket->close({});
            std::lock_guard<std::mutex> lock(mutex_);
            pending_.erase(request_id);
        }
        else
        {
            auto self = shared_from_this();
            std::thread(
                [self = std::move(self), exchange]
                {
                    self->complete_http(exchange);
                }).detach();
        }
    }
    return json::object();
}

void CopilotRequestHandlerBridge::complete_http(
    const std::shared_ptr<Exchange>& exchange)
{
    try
    {
        if (exchange->context.is_cancelled())
            return;
        const auto response = handler_->send_request(
            exchange->request, exchange->context);
        if (exchange->context.is_cancelled())
            return;
        json start{
            {"requestId", exchange->context.request_id},
            {"status", response.status},
            {"headers", response.headers},
        };
        if (response.status_text)
            start["statusText"] = *response.status_text;
        send_rpc_("llmInference.httpResponseStart", start);
        if (response.chunks.empty())
        {
            send_rpc_(
                "llmInference.httpResponseChunk",
                json{
                    {"requestId", exchange->context.request_id},
                    {"data", ""},
                    {"end", true},
                });
        }
        else
        {
            for (std::size_t i = 0; i < response.chunks.size(); ++i)
            {
                send_rpc_(
                    "llmInference.httpResponseChunk",
                    json{
                        {"requestId", exchange->context.request_id},
                        {"data", response.chunks[i]},
                        {"binary", response.binary},
                        {"end", i + 1 == response.chunks.size()},
                    });
            }
        }
    }
    catch (const std::exception& error)
    {
        if (!exchange->context.is_cancelled())
            fail(exchange, error.what());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(exchange->context.request_id);
}

void CopilotRequestHandlerBridge::fail(
    const std::shared_ptr<Exchange>& exchange, const std::string& message)
{
    try
    {
        send_rpc_(
            "llmInference.httpResponseChunk",
            json{
                {"requestId", exchange->context.request_id},
                {"data", ""},
                {"end", true},
                {"error", {{"message", message}}},
            });
    }
    catch (...)
    {
    }
}

void CopilotRequestHandlerBridge::cancel_all()
{
    std::unordered_map<std::string, std::shared_ptr<Exchange>> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending.swap(pending_);
    }
    for (const auto& [_, exchange] : pending)
    {
        exchange->context.cancelled->store(true);
        if (exchange->websocket)
            exchange->websocket->close(
                CopilotWebSocketCloseStatus{.description = "client disposed"});
    }
}

std::size_t CopilotRequestHandlerBridge::pending_count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
}

} // namespace copilot
