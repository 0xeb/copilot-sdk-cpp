// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file instruction_directories.cpp
/// @brief Compile-only demo for the v0.1.49 `instructionDirectories` field on
/// `SessionConfig` and `ResumeSessionConfig`.
///
/// Builds a `SessionConfig` populated with per-session instruction directories
/// and dumps the resulting `session.create` request payload that the SDK would
/// send to the Copilot CLI. No network or CLI is required: this example
/// exercises the public `build_session_create_request` helper so the API is
/// exercised at build time and the JSON envelope is human-inspectable.
///
/// Background: instructionDirectories (upstream nodejs PR #1190) lets a host
/// application supplement the global instruction set with additional
/// directories scoped to a single session — useful for ephemeral or
/// workspace-specific guidance without mutating the user's CLI config.

#include <copilot/copilot.hpp>

#include <iostream>

int main()
{
    using namespace copilot;

    SessionConfig cfg;
    cfg.client_name = "instruction-dirs-demo";
    cfg.instruction_directories = std::vector<std::string>{
        "/etc/copilot/instructions",
        "./workspace/.copilot/instructions",
    };
    cfg.enable_config_discovery = true;
    cfg.streaming = false;

    json request = build_session_create_request(cfg);

    std::cout << "session.create request payload:\n";
    std::cout << request.dump(2) << "\n";

    // Sanity-check the fields actually round-tripped through the builder.
    if (!request.contains("instructionDirectories") ||
        !request["instructionDirectories"].is_array() ||
        request["instructionDirectories"].size() != 2)
    {
        std::cerr << "instructionDirectories field missing or malformed\n";
        return 1;
    }
    if (request.value("clientName", std::string{}) != "instruction-dirs-demo")
    {
        std::cerr << "clientName missing\n";
        return 1;
    }
    return 0;
}
