// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <array>
#include <copilot/client.hpp>
#include <copilot/decisions.hpp>
#include <copilot/jsonrpc.hpp>
#include <copilot/rpc_methods.hpp>

namespace copilot
{

namespace api = generated::api;

std::future<std::vector<api::DecisionModelInfo>>
CopilotDecisions::list_models(std::optional<api::ModelsListRequest> request) const
{
    // NODEJS REFERENCE: nodejs/src/decisions.ts listModels
    json params = request ? request->raw() : json::object();
    if (!params.is_object())
        params = json::object();
    return std::async(
        std::launch::async,
        [client = client_, params = std::move(params)]() mutable
        {
            const json list = client->invoke(rpc::methods::kModelsList, std::move(params)).get();
            std::vector<api::DecisionModelInfo> models;
            if (list.is_object())
            {
                const auto it = list.find("decisionModels");
                if (it != list.end() && it->is_array())
                    for (const auto& model : *it)
                        models.emplace_back(model);
            }
            return models;
        }
    );
}

std::future<DecisionsAskResultWithNames> CopilotDecisions::ask(api::DecisionsAskRequest request) const
{
    // NODEJS REFERENCE: nodejs/src/decisions.ts ask
    return std::async(
        std::launch::async,
        [client = client_, params = request.raw()]() mutable
        {
            json raw = client->invoke(rpc::methods::kDecisionsAsk, std::move(params)).get();
            DecisionsAskResultWithNames out;
            if (raw.is_object())
            {
                const auto it = raw.find("answers");
                if (it != raw.end() && it->is_array())
                    for (const auto& answer : *it)
                        if (answer.is_object() && answer.contains("name") &&
                            answer["name"].is_string())
                            out.by_name.insert_or_assign(
                                answer["name"].get<std::string>(), api::DecisionAnswer(answer)
                            );
            }
            out.result = api::DecisionsAskResult(std::move(raw));
            return out;
        }
    );
}

std::future<YesNoResult> CopilotDecisions::yes_no(
    std::string question, std::variant<std::string, api::DecisionContext> context,
    YesNoOptions options
) const
{
    // NODEJS REFERENCE: nodejs/src/decisions.ts yesNo
    json request = json::object();
    if (const auto* text = std::get_if<std::string>(&context))
        request["context"] = json{{"kind", "text"}, {"text", *text}};
    else
        request["context"] = std::get<api::DecisionContext>(context).raw();
    request["questions"] = json::array(
        {json{{"kind", "boolean"}, {"name", "answer"}, {"instructions", std::move(question)}}}
    );
    if (options.selection_id)
        request["selectionId"] = *options.selection_id;
    if (options.github_token)
        request["gitHubToken"] = *options.github_token;
    if (options.model)
        request["model"] = *options.model;
    if (options.timeout_ms)
        request["timeoutMs"] = *options.timeout_ms;
    if (options.purpose)
        request["purpose"] = *options.purpose;

    return std::async(
        std::launch::async,
        [self = *this, request = std::move(request), threshold = options.threshold]() mutable
        {
            const auto asked = self.ask(api::DecisionsAskRequest(std::move(request))).get();
            const json& raw = asked.result.raw();
            YesNoResult out;
            if (!raw.is_object() || !raw.contains("answers") || !raw["answers"].is_array() ||
                raw["answers"].empty())
                return out;
            const json& answer = raw["answers"][0];
            if (!answer.is_object() || answer.value("kind", std::string()) != "boolean" ||
                !answer.contains("probability") || !answer["probability"].is_number())
                return out;
            out.refused = false;
            out.probability = answer["probability"].get<double>();
            out.yes = out.probability >= threshold;
            return out;
        }
    );
}

std::optional<api::DecisionErrorData> decision_error_data(const std::exception& error)
{
    // NODEJS REFERENCE: nodejs/src/decisions.ts decisionErrorData + DECISION_ERROR_KINDS
    static constexpr std::array<std::string_view, 10> kDecisionErrorKinds{
        "notEnabled",
        "noModel",
        "unauthenticated",
        "invalidRequest",
        "tooLarge",
        "upstreamProtocol",
        "rateLimited",
        "timeout",
        "unavailable",
        "overloaded",
    };
    const auto* rpc_error = dynamic_cast<const JsonRpcError*>(&error);
    if (!rpc_error)
        return std::nullopt;
    const json& data = rpc_error->data();
    if (!data.is_object())
        return std::nullopt;
    const auto it = data.find("kind");
    // Other RPCs also put a string `kind` in error data, so only the decision kinds count.
    if (it == data.end() || !it->is_string())
        return std::nullopt;
    const auto& kind = it->get_ref<const std::string&>();
    for (const auto known : kDecisionErrorKinds)
        if (kind == known)
            return api::DecisionErrorData(data);
    return std::nullopt;
}

std::optional<api::DecisionErrorData> decision_error_data(std::exception_ptr error)
{
    if (!error)
        return std::nullopt;
    try
    {
        std::rethrow_exception(error);
    }
    catch (const std::exception& e)
    {
        return decision_error_data(e);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

std::optional<std::string> decision_error_kind(const std::exception& error)
{
    const auto data = decision_error_data(error);
    if (!data)
        return std::nullopt;
    return data->raw().at("kind").get<std::string>();
}

std::optional<std::string> decision_error_kind(std::exception_ptr error)
{
    const auto data = decision_error_data(std::move(error));
    if (!data)
        return std::nullopt;
    return data->raw().at("kind").get<std::string>();
}

} // namespace copilot
