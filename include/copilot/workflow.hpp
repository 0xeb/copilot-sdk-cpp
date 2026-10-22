// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#include <copilot/generated/api_types.hpp>
#include <copilot/types.hpp>
#include <future>

namespace copilot
{

class Session;

struct WorkflowHandle
{
    json meta;
    std::function<json(const json& context)> run;
};

WorkflowHandle define_workflow(
    json meta,
    std::function<json(const json& context)> run);

class SessionWorkflowApi
{
  public:
    explicit SessionWorkflowApi(Session& session) : session_(session) {}

    /// Run a registered workflow (`session.workflow.run`). The envelope is
    /// returned for every outcome; inspect `status` before reading `result`.
    std::future<generated::api::WorkflowRunResult> run(
        const std::string& name,
        json options = json::object());
    std::future<generated::api::WorkflowResumeResult> resume(
        const std::string& run_id,
        json options = json::object());
    std::future<generated::api::WorkflowRunResult> get_run(const std::string& run_id);
    std::future<generated::api::WorkflowListRunsResult> list_runs(
        json options = json::object());
    std::future<generated::api::WorkflowRunDetail> get_run_detail(
        const std::string& run_id);
    std::future<generated::api::WorkflowProgressPage> get_run_progress(
        const std::string& run_id,
        json options = json::object());
    std::future<generated::api::WorkflowRunResult> pause(const std::string& run_id);
    std::future<generated::api::WorkflowRunResult> cancel(const std::string& run_id);

    /// Internal tool-originated workflow run (`session.workflow.runFromTool`).
    /// `params` matches the official `WorkflowToolRunRequest` shape.
    std::future<generated::api::WorkflowRunResult> run_from_tool(
        json params = json::object());
    /// Internal tool-originated workflow resume (`session.workflow.resumeFromTool`).
    /// `params` matches the official `WorkflowToolResumeRequest` shape.
    std::future<generated::api::WorkflowResumeResult> resume_from_tool(
        json params = json::object());

  private:
    Session& session_;
};

} // namespace copilot
