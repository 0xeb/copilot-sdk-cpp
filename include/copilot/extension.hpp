// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <copilot/client.hpp>
#include <copilot/workflow.hpp>

namespace copilot
{

/// Options for join_session(). The extension SDK path, skill provider and image-generation
/// consent belong to the session owner and are stripped before the resume.
// NODEJS REFERENCE: nodejs/src/extension.ts JoinSessionOptions
struct JoinSessionOptions
{
    ResumeSessionOptions session;
    /// Sensitive environment variables the extension needs; granted values are set in
    /// this process before join_session() resolves.
    std::vector<std::string> requested_environment_variables;
    std::vector<std::shared_ptr<WorkflowHandle>> workflows;
};

struct ExtensionSession
{
    std::shared_ptr<Client> client;
    std::shared_ptr<Session> session;

    Session* operator->() const noexcept { return session.get(); }
    Session& operator*() const noexcept { return *session; }
};

std::future<ExtensionSession> join_session(JoinSessionOptions options = {});

} // namespace copilot
