# Changelog

## 0.4.3

- Fix MSVC compilation of JSON-RPC responses when using nlohmann/json 3.12.0
  with the optional-conversion backport distributed by vcpkg. Preserve explicit
  JSON null results when constructing and parsing responses.
- Clarify the 0.4.2 session lifecycle behavior: `Session::on` throws
  `std::runtime_error` when event dispatch has stopped, including after client
  shutdown. Create or resume a session before subscribing again.

## 0.4.2

- Update protocol coverage for Copilot CLI 1.0.102-1.
- Add request-correlated and structured message waiting, live tool replacement,
  skill providers, installation confirmation, and extension launch callbacks.
- Expand session, agent, provider, MCP authentication, and binary filesystem APIs.
- Fix hook error-object parsing, reentrant JSON-RPC callbacks, request cancellation,
  session admission races, and runtime shutdown and process-tree cleanup.
