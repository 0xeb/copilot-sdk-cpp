// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

/// @file decisions.hpp
/// @brief Convenience wrapper over the experimental `decisions.*` RPCs.

#include <copilot/generated/api_types.hpp>
#include <cstdint>
#include <exception>
#include <future>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace copilot
{

class Client;

/// Options for CopilotDecisions::yes_no.
// NODEJS REFERENCE: nodejs/src/decisions.ts YesNoOptions
struct YesNoOptions
{
    /// Account to ask as; see `DecisionsAskRequest.selectionId`.
    std::optional<std::string> selection_id;
    /// GitHub token to ask as; see `DecisionsAskRequest.gitHubToken`.
    std::optional<std::string> github_token;
    /// Decision model id; the runtime default when omitted.
    std::optional<std::string> model;
    /// Probability at or above which the answer counts as yes.
    double threshold = 0.5;
    /// Deadline in milliseconds; see `DecisionsAskRequest.timeoutMs`.
    std::optional<std::int64_t> timeout_ms;
    /// Static telemetry identifier of the calling feature.
    std::optional<std::string> purpose;
};

/// Result of CopilotDecisions::yes_no. A refusal is never reported as "no":
/// when `refused` is true, `yes` and `probability` carry no meaning.
// NODEJS REFERENCE: nodejs/src/decisions.ts YesNoResult
struct YesNoResult
{
    bool refused = true;
    bool yes = false;
    double probability = 0.0;
};

/// A decisions result with its answers also indexed by question name.
// NODEJS REFERENCE: nodejs/src/decisions.ts DecisionsAskResultWithNames
struct DecisionsAskResultWithNames
{
    generated::api::DecisionsAskResult result;
    std::map<std::string, generated::api::DecisionAnswer> by_name;
};

/// One-off yes/no, pick-one and rating questions answered by Copilot decision
/// models. Requires the runtime's `DECISIONS_API` feature flag.
/// @experimental
// NODEJS REFERENCE: nodejs/src/decisions.ts CopilotDecisions
class CopilotDecisions
{
  public:
    explicit CopilotDecisions(Client& client) : client_(&client) {}

    /// The decision models the account can call: `models.list`'s `decisionModels`,
    /// or an empty list when the runtime does not report them (the feature is off).
    std::future<std::vector<generated::api::DecisionModelInfo>>
    list_models(std::optional<generated::api::ModelsListRequest> request = std::nullopt) const;

    /// Asks every question in `request` and returns the answers, also indexed by name.
    std::future<DecisionsAskResultWithNames> ask(generated::api::DecisionsAskRequest request) const;

    /// Asks one yes/no question about `context`, which may be plain text or a structured context.
    std::future<YesNoResult> yes_no(
        std::string question,
        std::variant<std::string, generated::api::DecisionContext> context,
        YesNoOptions options = {}
    ) const;

  private:
    Client* client_;
};

/// The typed failure behind a rejected `decisions.*` call, or nullopt when the
/// error did not come from one.
// NODEJS REFERENCE: nodejs/src/decisions.ts decisionErrorData
std::optional<generated::api::DecisionErrorData> decision_error_data(const std::exception& error);
std::optional<generated::api::DecisionErrorData> decision_error_data(std::exception_ptr error);

/// Shorthand for `decision_error_data(error)->kind`.
// NODEJS REFERENCE: nodejs/src/decisions.ts decisionErrorKind
std::optional<std::string> decision_error_kind(const std::exception& error);
std::optional<std::string> decision_error_kind(std::exception_ptr error);

} // namespace copilot
