// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/workflow.hpp>
#include <copilot/session.hpp>

namespace copilot
{

WorkflowHandle define_workflow(
    json meta,
    std::function<json(const json& context)> run)
{
    if (!meta.is_object() || !meta.contains("name") ||
        !meta["name"].is_string() || meta["name"].get<std::string>().empty())
    {
        throw std::invalid_argument("workflow metadata requires a non-empty name");
    }
    if (!run)
        throw std::invalid_argument("workflow run handler is required");
    return WorkflowHandle{std::move(meta), std::move(run)};
}

std::future<generated::api::WorkflowRunResult> SessionWorkflowApi::run(
    const std::string& name, json options)
{
    options["name"] = name;
    return session_.invoke_typed<generated::api::WorkflowRunResult>(
        "session.workflow.run", std::move(options));
}

std::future<generated::api::WorkflowResumeResult> SessionWorkflowApi::resume(
    const std::string& run_id, json options)
{
    options["runId"] = run_id;
    return session_.invoke_typed<generated::api::WorkflowResumeResult>(
        "session.workflow.resume", std::move(options));
}

std::future<generated::api::WorkflowRunResult> SessionWorkflowApi::get_run(
    const std::string& run_id)
{
    return session_.invoke_typed<generated::api::WorkflowRunResult>(
        "session.workflow.getRun", json{{"runId", run_id}});
}

std::future<generated::api::WorkflowListRunsResult> SessionWorkflowApi::list_runs(
    json options)
{
    return session_.invoke_typed<generated::api::WorkflowListRunsResult>(
        "session.workflow.listRuns", std::move(options));
}

std::future<generated::api::WorkflowRunDetail> SessionWorkflowApi::get_run_detail(
    const std::string& run_id)
{
    return session_.invoke_typed<generated::api::WorkflowRunDetail>(
        "session.workflow.getRunDetail", json{{"runId", run_id}});
}

std::future<generated::api::WorkflowProgressPage>
SessionWorkflowApi::get_run_progress(
    const std::string& run_id, json options)
{
    options["runId"] = run_id;
    return session_.invoke_typed<generated::api::WorkflowProgressPage>(
        "session.workflow.getRunProgress", std::move(options));
}

std::future<generated::api::WorkflowRunResult> SessionWorkflowApi::pause(
    const std::string& run_id)
{
    return session_.invoke_typed<generated::api::WorkflowRunResult>(
        "session.workflow.pause", json{{"runId", run_id}});
}

std::future<generated::api::WorkflowRunResult> SessionWorkflowApi::cancel(
    const std::string& run_id)
{
    return session_.invoke_typed<generated::api::WorkflowRunResult>(
        "session.workflow.cancel", json{{"runId", run_id}});
}

std::future<generated::api::WorkflowRunResult> SessionWorkflowApi::run_from_tool(
    json params)
{
    return session_.invoke_typed<generated::api::WorkflowRunResult>(
        "session.workflow.runFromTool", std::move(params));
}

std::future<generated::api::WorkflowResumeResult>
SessionWorkflowApi::resume_from_tool(json params)
{
    return session_.invoke_typed<generated::api::WorkflowResumeResult>(
        "session.workflow.resumeFromTool", std::move(params));
}

} // namespace copilot
