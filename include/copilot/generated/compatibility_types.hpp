// Generated compatibility types retained from the first parity tranche. DO NOT EDIT.

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace copilot::generated
{

struct PingRequest
{
    std::optional<std::string> message{};
    bool operator==(const PingRequest&) const = default;
};

inline void to_json(nlohmann::json& j, const PingRequest& value)
{
    j = nlohmann::json::object();
    if (value.message)
        j["message"] = *value.message;
}

inline void from_json(const nlohmann::json& j, PingRequest& value)
{
    if (j.contains("message") && !j.at("message").is_null())
        value.message = j.at("message").get<std::string>();
    else
        value.message.reset();
}

struct PingResult
{
    std::string message{};
    std::string timestamp{};
    std::int64_t protocol_version{};
    bool operator==(const PingResult&) const = default;
};

inline void to_json(nlohmann::json& j, const PingResult& value)
{
    j = nlohmann::json{
        {"message", value.message},
        {"timestamp", value.timestamp},
        {"protocolVersion", value.protocol_version},
    };
}

inline void from_json(const nlohmann::json& j, PingResult& value)
{
    j.at("message").get_to(value.message);
    j.at("timestamp").get_to(value.timestamp);
    j.at("protocolVersion").get_to(value.protocol_version);
}

struct SessionIdleData
{
    std::optional<bool> aborted{};
    bool operator==(const SessionIdleData&) const = default;
};

inline void to_json(nlohmann::json& j, const SessionIdleData& value)
{
    j = nlohmann::json::object();
    if (value.aborted)
        j["aborted"] = *value.aborted;
}

inline void from_json(const nlohmann::json& j, SessionIdleData& value)
{
    if (j.contains("aborted") && !j.at("aborted").is_null())
        value.aborted = j.at("aborted").get<bool>();
    else
        value.aborted.reset();
}

} // namespace copilot::generated
