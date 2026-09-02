// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <copilot/client.hpp>
#include <copilot/factory.hpp>

namespace copilot
{

struct JoinSessionConfig
{
    ResumeSessionConfig session;
    std::vector<std::string> requested_environment_variables;
    std::vector<std::shared_ptr<FactoryHandle>> factories;
};

struct ExtensionSession
{
    std::shared_ptr<Client> client;
    std::shared_ptr<Session> session;

    Session* operator->() const noexcept { return session.get(); }
    Session& operator*() const noexcept { return *session; }
};

std::future<ExtensionSession> join_session(JoinSessionConfig config = {});

} // namespace copilot
