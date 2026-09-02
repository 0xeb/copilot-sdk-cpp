// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <copilot/types.hpp>
#include <functional>
#include <map>

namespace copilot
{

class CanvasError : public std::runtime_error
{
  public:
    CanvasError(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code))
    {
    }

    const std::string& code() const noexcept { return code_; }

  private:
    std::string code_;
};

struct CanvasAction
{
    std::string name;
    std::optional<std::string> description;
    std::optional<json> input_schema;
    std::function<json(const json&)> handler;
};

struct CanvasOptions
{
    std::string id;
    std::string display_name;
    std::string description;
    std::optional<json> input_schema;
    std::vector<CanvasAction> actions;
    std::function<json(const json&)> open;
    std::function<void(const json&)> on_close;
};

class Canvas
{
  public:
    explicit Canvas(CanvasOptions options);

    const std::string& id() const noexcept { return id_; }
    const json& declaration() const noexcept { return declaration_; }
    json handle_open(const json& request) const;
    void handle_close(const json& request) const;
    json handle_action(const std::string& action, const json& request) const;

  private:
    std::string id_;
    json declaration_;
    std::function<json(const json&)> open_;
    std::function<void(const json&)> close_;
    std::map<std::string, std::function<json(const json&)>> actions_;
};

inline std::shared_ptr<Canvas> create_canvas(CanvasOptions options)
{
    return std::make_shared<Canvas>(std::move(options));
}

} // namespace copilot
