// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file remote_session.cpp
/// @brief Compile-only demo for the v0.1.49 `remoteSession` field on
/// `SessionConfig` (Mission Control integration, upstream nodejs PR #1295).
///
/// Demonstrates the three `RemoteSessionMode` values and shows how the
/// resulting `session.create` payload differs. No live Copilot CLI is
/// required: the example uses the public `build_session_create_request`
/// helper to render each request payload for inspection.
///
/// Remote-session modes (matches upstream nodejs):
///   * `Off`    — explicitly disable remote steering for this session.
///   * `Export` — export this session so it shows up in Mission Control
///                without accepting remote commands.
///   * `On`     — enable full remote steering from GitHub web / mobile.

#include <copilot/copilot.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace
{

const char* mode_name(copilot::RemoteSessionMode mode)
{
    using copilot::RemoteSessionMode;
    switch (mode)
    {
    case RemoteSessionMode::Off: return "off";
    case RemoteSessionMode::Export: return "export";
    case RemoteSessionMode::On: return "on";
    }
    return "?";
}

} // namespace

int main()
{
    using namespace copilot;

    const std::vector<RemoteSessionMode> modes = {
        RemoteSessionMode::Off,
        RemoteSessionMode::Export,
        RemoteSessionMode::On,
    };

    for (auto mode : modes)
    {
        SessionConfig cfg;
        cfg.client_name = "remote-session-demo";
        cfg.remote_session = mode;
        cfg.enable_session_telemetry = (mode != RemoteSessionMode::Off);

        json request = build_session_create_request(cfg);

        std::cout << "[mode=" << mode_name(mode) << "] session.create payload:\n";
        std::cout << request.dump(2) << "\n\n";

        if (!request.contains("remoteSession"))
        {
            std::cerr << "remoteSession field missing for mode " << mode_name(mode) << "\n";
            return 1;
        }
    }

    return 0;
}
