// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/extension.hpp>
#include <cstdlib>

namespace copilot
{

// NODEJS REFERENCE: nodejs/src/extension.ts joinSession
std::future<ExtensionSession> join_session(JoinSessionOptions options)
{
    return std::async(
        std::launch::async,
        [options = std::move(options)]() mutable
        {
            const auto session_id = detail::non_empty_env_value("SESSION_ID");
            if (!session_id)
                throw std::runtime_error(
                    "joinSession() is intended for extensions running as child processes of the "
                    "Copilot CLI."
                );

            auto& session_options = options.session;
            session_options.extensions.sdk_path.reset();
            session_options.skills.provider.reset();
            session_options.tools.image_generation.reset();
            // Only the session's owning connection may set extension sources.
            session_options.extension_sources.reset();
            // NODEJS REFERENCE: extension.ts joinSession (delete resumeOptions.extensionPaths)
            session_options.extension_paths.reset();
            // Only the session's owning connection may declare a host-policy pre-check.
            session_options.permissions.host_policy_pre_check.reset();
            if (!session_options.suppress_resume_event)
                session_options.suppress_resume_event = true;
            if (!options.requested_environment_variables.empty())
                session_options.requested_environment_variables =
                    std::move(options.requested_environment_variables);
            if (!options.workflows.empty())
                session_options.workflow_objects = std::move(options.workflows);

            // Without a caller handler upstream installs a "no-result" default and tells the
            // runtime not to route permission requests here.
            const bool has_permission_handler =
                static_cast<bool>(session_options.permissions.on_request) ||
                static_cast<bool>(session_options.permissions.on_request_with_context);
            if (!has_permission_handler)
                session_options.permissions.on_request = [](const PermissionRequest&)
                { return PermissionRequestResult::no_result(); };

            ClientOptions client_options;
            client_options.runtime = RuntimeConnection::for_parent_process();
            auto client = std::make_shared<Client>(std::move(client_options));
            client->start().get();
            auto session = client
                               ->resume_session(
                                   *session_id, std::move(session_options), has_permission_handler
                               )
                               .get();
            return ExtensionSession{std::move(client), std::move(session)};
        }
    );
}

} // namespace copilot
