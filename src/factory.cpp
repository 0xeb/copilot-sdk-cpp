// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/factory.hpp>
#include <copilot/session.hpp>

namespace copilot
{

FactoryHandle define_factory(
    json meta,
    std::function<json(const json& context)> run)
{
    if (!meta.is_object() || !meta.contains("name") ||
        !meta["name"].is_string() || meta["name"].get<std::string>().empty())
    {
        throw std::invalid_argument("factory metadata requires a non-empty name");
    }
    if (!run)
        throw std::invalid_argument("factory run handler is required");
    return FactoryHandle{std::move(meta), std::move(run)};
}

std::future<generated::api::FactoryExecuteResult> SessionFactoryApi::run(
    const std::string& name, json options)
{
    options["name"] = name;
    return session_.invoke_typed<generated::api::FactoryExecuteResult>(
        "session.factory.run", std::move(options));
}

std::future<generated::api::FactoryResumeResult> SessionFactoryApi::resume(
    const std::string& run_id, json options)
{
    options["runId"] = run_id;
    return session_.invoke_typed<generated::api::FactoryResumeResult>(
        "session.factory.resume", std::move(options));
}

std::future<generated::api::FactoryRunResult> SessionFactoryApi::get_run(
    const std::string& run_id)
{
    return session_.invoke_typed<generated::api::FactoryRunResult>(
        "session.factory.getRun", json{{"runId", run_id}});
}

std::future<generated::api::FactoryListRunsResult> SessionFactoryApi::list_runs(
    json options)
{
    return session_.invoke_typed<generated::api::FactoryListRunsResult>(
        "session.factory.listRuns", std::move(options));
}

std::future<generated::api::FactoryRunDetail> SessionFactoryApi::get_run_detail(
    const std::string& run_id)
{
    return session_.invoke_typed<generated::api::FactoryRunDetail>(
        "session.factory.getRunDetail", json{{"runId", run_id}});
}

std::future<generated::api::FactoryProgressPage>
SessionFactoryApi::get_run_progress(
    const std::string& run_id, json options)
{
    options["runId"] = run_id;
    return session_.invoke_typed<generated::api::FactoryProgressPage>(
        "session.factory.getRunProgress", std::move(options));
}

std::future<generated::api::FactoryRunResult> SessionFactoryApi::cancel(
    const std::string& run_id)
{
    return session_.invoke_typed<generated::api::FactoryRunResult>(
        "session.factory.cancel", json{{"runId", run_id}});
}

std::future<generated::api::FactoryRunResult> SessionFactoryApi::run_from_tool(
    json params)
{
    return session_.invoke_typed<generated::api::FactoryRunResult>(
        "session.factory.runFromTool", std::move(params));
}

std::future<generated::api::FactoryResumeResult>
SessionFactoryApi::resume_from_tool(json params)
{
    return session_.invoke_typed<generated::api::FactoryResumeResult>(
        "session.factory.resumeFromTool", std::move(params));
}

} // namespace copilot
