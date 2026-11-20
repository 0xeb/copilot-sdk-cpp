// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file remote_session.cpp
/// @brief Cloud session demo: run a session on GitHub-hosted compute through Mission
/// Control via `CreateSessionOptions::cloud`.
///
/// The 0.4.3 `remote_session` / `RemoteSessionMode` option was removed in 0.4.4 (as in
/// upstream). Cloud sessions are the replacement for Mission Control integration: they are
/// inherently remote, and the runtime reports the Mission Control URL in a `session.info`
/// event.
///
/// By default this is a dry run that renders the `session.create` payload with the public
/// `build_session_create_request` helper, so no CLI is needed. Pass
/// `--live <owner>/<name>[@branch]` to actually create a cloud session (requires Copilot
/// cloud-agent entitlement).

#include <copilot/copilot.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

namespace
{

copilot::CreateSessionOptions make_options(copilot::CloudSessionRepository repository)
{
    copilot::CreateSessionOptions options;
    options.client_name = "cloud-session-demo";
    options.events.streaming = true;
    options.permissions.on_request = [](const copilot::PermissionRequest&)
    { return copilot::PermissionRequestResult{.kind = "approve-once"}; };
    options.cloud = copilot::CloudSessionOptions{.repository = std::move(repository)};
    return options;
}

std::optional<copilot::CloudSessionRepository> parse_repository(const std::string& spec)
{
    const auto slash = spec.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 == spec.size())
        return std::nullopt;
    copilot::CloudSessionRepository repository;
    repository.owner = spec.substr(0, slash);
    std::string rest = spec.substr(slash + 1);
    if (const auto at = rest.find('@'); at != std::string::npos)
    {
        repository.branch = rest.substr(at + 1);
        rest = rest.substr(0, at);
    }
    repository.name = rest;
    return repository;
}

int dry_run()
{
    const copilot::CloudSessionRepository repositories[] = {
        {.owner = "github", .name = "copilot-sdk"},
        {.owner = "github", .name = "copilot-sdk", .branch = "main"},
    };
    for (const auto& repository : repositories)
    {
        copilot::json request = copilot::build_session_create_request(make_options(repository));
        std::cout << "session.create payload (cloud):\n" << request.dump(2) << "\n\n";

        if (!request.contains("cloud") || !request["cloud"].contains("repository") ||
            request["cloud"]["repository"].value("owner", std::string{}) != repository.owner)
        {
            std::cerr << "cloud.repository field missing or malformed\n";
            return 1;
        }
    }
    return 0;
}

int live(copilot::CloudSessionRepository repository)
{
    copilot::Client client;
    client.start().get();

    std::cout << "Creating cloud session (waits for the remote worker to connect)...\n";
    auto session = client.create_session(make_options(std::move(repository))).get();
    std::cout << "Cloud session: " << session->session_id() << "\n";

    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> idle{false};
    auto subscription = session->on(
        [&](const copilot::SessionEvent& event)
        {
            if (auto* info = event.try_as<copilot::SessionInfoData>())
                std::cout << "[session.info/" << info->info_type << "] " << info->message << "\n";
            else if (auto* delta = event.try_as<copilot::AssistantMessageDeltaData>())
                std::cout << delta->delta_content << std::flush;
            else if (auto* error = event.try_as<copilot::SessionErrorData>())
                std::cerr << "\nError: " << error->message << "\n";
            if (event.type == copilot::SessionEventType::SessionIdle ||
                event.type == copilot::SessionEventType::SessionError)
            {
                std::lock_guard<std::mutex> lock(mtx);
                idle = true;
                cv.notify_one();
            }
        }
    );

    copilot::MessageOptions message;
    message.prompt = "Summarize the README";
    session->send(message).get();
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv.wait_for(lock, std::chrono::minutes(5), [&] { return idle.load(); });
    }
    std::cout << "\n";

    session->destroy().get();
    client.stop().get();
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    try
    {
        if (argc >= 3 && std::string(argv[1]) == "--live")
        {
            auto repository = parse_repository(argv[2]);
            if (!repository)
            {
                std::cerr << "Usage: " << argv[0] << " [--live <owner>/<name>[@branch]]\n";
                return 1;
            }
            return live(std::move(*repository));
        }
        return dry_run();
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
