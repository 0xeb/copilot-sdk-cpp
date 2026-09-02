// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/canvas.hpp>

namespace copilot
{

Canvas::Canvas(CanvasOptions options)
    : id_(std::move(options.id)),
      open_(std::move(options.open)),
      close_(std::move(options.on_close))
{
    if (id_.empty())
        throw std::invalid_argument("canvas id cannot be empty");
    if (!open_)
        throw std::invalid_argument("canvas open handler is required");

    declaration_ = json{
        {"id", id_},
        {"displayName", options.display_name},
        {"description", options.description},
    };
    if (options.input_schema)
        declaration_["inputSchema"] = *options.input_schema;

    if (!options.actions.empty())
    {
        declaration_["actions"] = json::array();
        for (auto& action : options.actions)
        {
            if (action.name.rfind("canvas.", 0) == 0)
                throw std::invalid_argument("canvas action names cannot start with 'canvas.'");
            json wire{{"name", action.name}};
            if (action.description)
                wire["description"] = *action.description;
            if (action.input_schema)
                wire["inputSchema"] = *action.input_schema;
            declaration_["actions"].push_back(std::move(wire));
            actions_[action.name] = std::move(action.handler);
        }
    }
}

json Canvas::handle_open(const json& request) const
{
    return open_(request);
}

void Canvas::handle_close(const json& request) const
{
    if (close_)
        close_(request);
}

json Canvas::handle_action(const std::string& action, const json& request) const
{
    const auto it = actions_.find(action);
    if (it == actions_.end() || !it->second)
        throw CanvasError(
            "canvas_action_no_handler",
            "No handler implemented for canvas action " + action);
    return it->second(request);
}

} // namespace copilot
