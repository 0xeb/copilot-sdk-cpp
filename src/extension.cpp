// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/extension.hpp>
#include <cstdlib>

namespace copilot
{

std::future<ExtensionSession> join_session(JoinSessionConfig config)
{
    return std::async(
        std::launch::async,
        [config = std::move(config)]() mutable
        {
            const char* session_id = std::getenv("SESSION_ID");
            if (!session_id || !*session_id)
                throw std::runtime_error(
                    "join_session is only available in a Copilot extension child process");

            config.session.disable_resume = true;
            config.session.requested_environment_variables =
                std::move(config.requested_environment_variables);
            if (!config.factories.empty())
            {
                config.session.factories = std::vector<json>{};
                for (const auto& factory : config.factories)
                {
                    if (factory)
                        config.session.factories->push_back(factory->meta);
                }
                config.session.factory_objects = config.factories;
            }

            ClientOptions options;
            options.connection = RuntimeConnection::for_parent_process();
            auto client = std::make_shared<Client>(std::move(options));
            client->start().get();
            auto session =
                client->resume_session(session_id, std::move(config.session)).get();
            return ExtensionSession{std::move(client), std::move(session)};
        });
}

} // namespace copilot
