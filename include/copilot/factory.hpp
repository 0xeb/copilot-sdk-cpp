// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <copilot/generated/api_types.hpp>
#include <copilot/types.hpp>
#include <future>

namespace copilot
{

class Session;

struct FactoryHandle
{
    json meta;
    std::function<json(const json& context)> run;
};

FactoryHandle define_factory(
    json meta,
    std::function<json(const json& context)> run);

class SessionFactoryApi
{
  public:
    explicit SessionFactoryApi(Session& session) : session_(session) {}

    std::future<generated::api::FactoryExecuteResult> run(
        const std::string& name,
        json options = json::object());
    std::future<generated::api::FactoryResumeResult> resume(
        const std::string& run_id,
        json options = json::object());
    std::future<generated::api::FactoryRunResult> get_run(const std::string& run_id);
    std::future<generated::api::FactoryListRunsResult> list_runs(
        json options = json::object());
    std::future<generated::api::FactoryRunDetail> get_run_detail(
        const std::string& run_id);
    std::future<generated::api::FactoryProgressPage> get_run_progress(
        const std::string& run_id,
        json options = json::object());
    std::future<generated::api::FactoryRunResult> cancel(const std::string& run_id);

    /// Internal tool-originated factory run (`session.factory.runFromTool`).
    /// `params` matches the official `FactoryToolRunRequest` shape.
    std::future<generated::api::FactoryRunResult> run_from_tool(
        json params = json::object());
    /// Internal tool-originated factory resume (`session.factory.resumeFromTool`).
    /// `params` matches the official `FactoryToolResumeRequest` shape.
    std::future<generated::api::FactoryResumeResult> resume_from_tool(
        json params = json::object());

  private:
    Session& session_;
};

} // namespace copilot
