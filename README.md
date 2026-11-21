# copilot-sdk-cpp

C++ SDK for interacting with the Copilot CLI agent runtime (JSON-RPC over stdio or TCP).

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-blue)]()
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)]()
[![License](https://img.shields.io/badge/license-MIT-green)]()

## Version 0.4.5

Adds experimental session management (peer sessions): sessions in separate
processes can discover each other by name and exchange messages via
`Session::management()` and `Session::name()`. See [Session management](#session-management-peer-sessions-experimental).

### 0.4.4

Targets Copilot CLI 1.0.115-1 and SDK protocol version 4 only. This is a clean
break that mirrors the upstream SDK v2 API: client options and session options are
grouped into nested option structs, runtime selection moved to `RuntimeConnection`
factories, and BYOK uses named providers. There are no deprecated aliases; see
[Migrating from 0.4.3](#migrating-from-043).

New in 0.4.4: protocol 4 events, `Session::get_raw_events`,
`Session::set_fusion_tier`, `MessageSource::automation`, host network callbacks
(`ClientOptions::network`), Microsoft Entra registration and token providers
(`ClientOptions::entra`), per-session SessionFs providers with SQLite support
(`state.create_session_fs_provider`), `Session::log`, `Session::ui()`, typed hooks,
MCP server configs and MCP OAuth results, system-message section transforms, and
session-creation handlers. See `CHANGELOG.md`.

### Compatibility

Each release accepts only the protocol range below; `Client::start()` throws
`ProtocolVersionMismatch` (with the runtime's protocol, CLI version and the spawned
executable path) for any other runtime. Copilot CLI 1.0.113 and later speak protocol 4.

| copilot-sdk-cpp | Protocol accepted | Schemas generated from | Tested with Copilot CLI |
|-----------------|-------------------|------------------------|-------------------------|
| 0.4.5           | 4                 | 1.0.115-1              | 1.0.115-1               |
| 0.4.4           | 4                 | 1.0.115-1              | 1.0.115-1               |
| 0.4.3           | 2–3               | 1.0.102-1              | 1.0.102-1               |
| 0.4.2           | 2–3               | 1.0.102-1              | 1.0.102-1               |
| 0.4.1           | 2–3               | 1.0.84-6               | 1.0.84-6                |

The CLI updates itself unless started with `--no-auto-update`; the SDK passes that flag
(and `--headless`) when it spawns the runtime, so a spawned CLI stays on the version
you installed.

The headers and library compile warning-free with MSVC `/std:c++20 /permissive- /W4 /WX`;
configure with `-DCOPILOT_STRICT_WARNINGS=ON` to build the library that way.

### 0.4.3

Targeted Copilot CLI 1.0.102-1 and SDK protocol version 3.

Fixes MSVC compilation of JSON-RPC responses with nlohmann/json 3.12.0 and
vcpkg's optional-conversion backport, preserving explicit JSON null results.

- Correlate message waiters with the originating request, including sub-agent,
  abort, and autopilot events; receive structured JSON with `response_schema`
  and `send_and_wait_structured`.
- Replace session tools at runtime with `Session::set_tools`; use tool namespaces
  and per-invocation agent/request identifiers.
- Supply session skill providers, installation confirmation handlers, and extension
  launch providers. Handle MCP authentication token updates or runtime refresh.
- Configure diagnostics, agent/hook directories, sandboxing, image generation,
  transcript recovery, and the expanded provider/agent options.
- Cancel incoming JSON-RPC requests and run request callbacks independently of the
  reader. Retire session callbacks on teardown and shut down owned runtimes gracefully.
- Use generated binary SessionFs request/result types and report partial writes
  with `SessionFsError::write_changed`.

Known limitations: the in-process macOS UI-thread dispatcher is not provided.

## Build

Requirements: CMake 3.20+ and a C++20 compiler.

```sh
cmake -S . -B build
cmake --build build --config Release
```

## Official protocol baseline and code generation

`schemas/official-baseline.json` pins the official SDK commit, protocol version,
schema hashes, and the complete generated surface: 1,899 API definitions, 679
session-event definitions, 619 RPC methods, and 163 event variants. Generated
wire-preserving types live under `include/copilot/generated/`; the existing
handwritten public API remains the ergonomic layer.

```sh
# Regenerate from the committed baseline
cmake --build build --target copilot_codegen

# Fail if committed generated headers are stale
cmake --build build --target copilot_codegen_check
```

To refresh from a checked-out official SDK and the matching CLI package schemas:

```sh
python tools/generate_protocol_types.py --refresh \
  --official-sdk-root ../copilot-sdk \
  --api-schema <path-to-api.schema.json> \
  --session-events-schema <path-to-session-events.schema.json>
```

## Quick Start

```cpp
#include <copilot/copilot.hpp>

copilot::ClientOptions options;                          // in-process runtime by default
options.observability.log_level = copilot::LogLevel::Info;
copilot::Client client(options);
client.start().get();                                    // optional: create_session connects

copilot::CreateSessionOptions session_options;
session_options.model = "gpt-4";
session_options.events.streaming = true;
session_options.permissions.on_request = [](const copilot::PermissionRequest&) {
    return copilot::PermissionRequestResult{.kind = "approve-once"};
};
auto session = client.create_session(std::move(session_options)).get();

auto subscription = session->on([](const copilot::SessionEvent& event) {
    if (auto* delta = event.try_as<copilot::AssistantMessageDeltaData>())
        std::cout << delta->delta_content << std::flush;
});

copilot::MessageOptions message;
message.prompt = "Hello!";
session->send(message).get();
```

See `examples/` for complete programs (streaming, tools, hooks, permissions, BYOK,
MCP servers, custom agents, cloud sessions, and more).

## Runtime connections

`ClientOptions::runtime` selects how the runtime is hosted. When unset it is
`RuntimeConnection::for_default()`: in-process, unless
`COPILOT_SDK_DEFAULT_CONNECTION=stdio`.

| Factory | Runtime |
|---------|---------|
| `RuntimeConnection::for_in_process(base_directory, idle_timeout, cli_entrypoint, library_path)` | Loaded into this process |
| `RuntimeConnection::for_spawned_stdio(path, args, environment, working_directory, base_directory, idle_timeout)` | Child process over stdio |
| `RuntimeConnection::for_spawned_tcp(port, connection_token, path, args, ...)` | Child process as a loopback TCP server |
| `RuntimeConnection::for_tcp(host, port, connection_token)` | Attach to an already-running server |
| `RuntimeConnection::for_parent_process()` | Attach to the parent process's stdio (extensions) |

Process settings (path, args, environment, working directory) exist only on spawned
connections. Attached connections reject runtime-only options such as
`github.token`, `observability.telemetry`, or a base directory.

Other client settings are grouped: `observability` (`log_level`, `telemetry`,
`on_get_trace_context`), `github` (`token`, `use_logged_in_user`, `on_telemetry`),
`models` (`on_list_models`, `request_handler`), `plugins.builtin_directories`,
`extensions.launch_provider`, and `installations.confirmation_handler`. `mode`,
`session_fs`, and `client_info` stay at the root.

## Session options

`CreateSessionOptions` / `ResumeSessionOptions` keep `model`, `client_name`,
`system_message`, `working_directory`, `config_directory`, and
`enable_config_discovery` at the root (`session_id`, `name`, and `cloud` are
create-only). Everything else lives in a group:

| Group | Common fields |
|-------|---------------|
| `tools` | `definitions`, `available_tools`, `excluded_tools`, `large_output`, `search` |
| `permissions` | `on_request`, `on_request_with_context`, `sandbox`, `mode` |
| `events` | `streaming`, `include_sub_agent_streaming_events`, `on_event` |
| `inference` | `reasoning_effort`, `providers`, `models`, `default_provider`, `capabilities` |
| `interaction` | `on_user_input_request`, `on_elicitation_request`, `on_exit_plan_mode_request`, `commands` |
| `agents` | `definitions`, `default_agent`, `selected`, `directories` |
| `mcp` | `servers`, `disabled_servers`, `on_auth_request` |
| `hooks` | `handlers` (`SessionHooks`), `directories`, `enable_repository_hooks` |
| `state` | `infinite_sessions`, `memory` |
| `instructions` | `directories`, `organization_text`, `skip_custom_instructions` |
| `skills`, `github`, `filesystem`, `execution`, `git`, `policy`, `experiments`, `canvases`, `extensions`, `plugins` | See `include/copilot/types.hpp` |

### Cloud sessions

```cpp
copilot::CreateSessionOptions options;
options.permissions.on_request = [](const copilot::PermissionRequest&) {
    return copilot::PermissionRequestResult{.kind = "approve-once"};
};
options.cloud = copilot::CloudSessionOptions{
    .repository = copilot::CloudSessionRepository{.owner = "github", .name = "copilot-sdk"}
};
auto session = client.create_session(std::move(options)).get();
```

The Mission Control URL arrives in a `session.info` event. See
`examples/remote_session.cpp`.

## Current public API surface

`ClientOptions` supports SDK modes plus in-process, spawned stdio, spawned TCP, and
attached TCP runtime connections. Session create/resume requests expose the current
official configuration fields through nested option groups.
`Session::invoke`/`invoke_typed` provide low-level access, with typed convenience
wrappers for MCP, authentication, agents, skills, tasks, commands, history,
usage, and remote (Mission Control) operations.

Advanced parity includes:

- `RuntimeConnection::for_in_process()` using the official
  `copilot_runtime_*` native ABI (`prebuilds/<platform>/runtime.node`).
- `CopilotRequestHandler` and WebSocket handlers for streamed model-layer I/O.
- Automatic MCP OAuth event handling and pending-request responses.
- `create_canvas`, `define_workflow`, `Session::workflow()`, and `join_session`
  helpers for canvas, workflow, and extension consumers.
- `Client::start_ahp_host(AhpHostOptions)` to host published sessions over AHP
  (`AhpHost::publish_session`, `unpublish_session`, `list_sessions`, `dispose`).
- Experimental `Client::decisions()` for yes/no and structured questions answered
  by Copilot decision models; `Client::runtime_port()` for TCP runtimes.
- `Session::open_canvases()` and `Session::permission_mode_result()` snapshots.

## Session management (peer sessions, experimental)

Sessions can find and message each other, including sessions owned by other
programs on the same machine that share the Copilot config directory. The feature
is off by default and each session opts in. The API may change upstream.

```cpp
CreateSessionOptions options;
options.name = "build-agent";          // peers address each other by name
auto session = client.create_session(options).get();

SessionManagementConfigureOptions enable;
enable.enabled = true;                 // scope defaults to "all"; InProcess hides it from other processes
session->management().configure(enable).get();

// Live, opted-in peers only (the caller is excluded). Optional exact-cwd filter.
for (const auto& peer : session->management().list().get())
    if (peer.name == "test-agent")
        session->management().message(peer.session_id, "Build is green.").get();

// Receiving side: peer text arrives as a system.notification and wakes an idle session.
auto sub = session->on([](const SessionEvent& event) {
    if (auto peer = peer_message_from_event(event))
        std::cout << peer->sender_name << ": " << peer->content << "\n";
});
```

- `ManagedSessionInfo` carries `session_id`, `name`, `cwd`, `state` (idle/busy),
  `model`, `parent_session_id`, and `session_state_path`. Use them to pick a peer
  out of many live sessions.
- `message` returns `accepted` once the text is delivered or queued. It is not a reply.
- `create` asks the host to start a child session (register a handler with
  `set_session_creation_handler`). `close` ends a session this session created.
- `Session::name()` gets, sets, or auto-sets the session name.
- `Client::list_sessions` returns persisted history, including dead sessions,
  with `client_name`/`last_client_name`. `management().list` shows live sessions only.

## Custom Tools

Custom tools are provided when creating or resuming a session. The SDK auto-generates JSON schemas from C++ types.

### Fluent Builder (Recommended)

```cpp
#include <copilot/tool_builder.hpp>

// Fluent builder with full control over parameters
auto calc = copilot::ToolBuilder("calculator", "Perform math calculations")
    .param<std::string>("expression", "Math expression to evaluate")
    .handler([](std::string expression) {
        // Evaluate expression...
        return "42";
    });

// With enum constraints and default values
auto search = copilot::ToolBuilder("search", "Search the web")
    .param<std::string>("query", "Search query")
    .param<int>("limit", "Max results").default_value(10)
    .param<std::string>("sort", "Sort order").one_of("relevance", "date")
    .handler([](std::string query, int limit, std::string sort) {
        return "Results for: " + query;
    });

// Use in session
copilot::CreateSessionOptions config;
config.tools.definitions = {calc, search};
auto session = client.create_session(config).get();
```

### Quick One-Liner with `make_tool`

```cpp
// Simple tools with auto-generated schema
auto echo = copilot::make_tool(
    "echo", "Echo a message",
    [](std::string message) { return message; },
    {"message"}
);

auto add = copilot::make_tool(
    "add", "Add two numbers",
    [](double a, double b) { return std::to_string(a + b); },
    {"first", "second"}
);

// Optional parameters (use std::optional)
auto greet = copilot::make_tool(
    "greet", "Greet someone",
    [](std::string name, std::optional<std::string> title) {
        return title ? "Hello, " + *title + " " + name + "!"
                     : "Hello, " + name + "!";
    },
    {"name", "title"}
);

config.tools.definitions = {echo, add, greet};
```

See `examples/tools.cpp` and `examples/resume_with_tools.cpp` for complete examples.

## BYOK (Bring Your Own Key)

Use your own API key instead of GitHub Copilot authentication. Providers are named
connections in `inference.providers`; select one with `inference.default_provider`
(or a canonical `provider/model-id` model).

### Explicit configuration

```cpp
copilot::NamedProviderConfig provider;
provider.name = "openai";
provider.type = "openai";
provider.base_url = "https://api.openai.com/v1";
provider.api_key = "sk-your-api-key";

copilot::CreateSessionOptions config;
config.model = "gpt-4";
config.inference.providers = std::vector<copilot::NamedProviderConfig>{provider};
config.inference.default_provider = "openai";
auto session = client.create_session(config).get();
```

Use `inference.models` (`ProviderModelConfig{.id, .provider, ...}`) to register
model definitions with limits, capabilities, or a different `wire_model`. Adding a
provider does not disable Copilot API models; set
`github.capi = CapiSessionOptions{.enabled = false}` for a BYOK-only catalog.

### Environment variables

```bash
export COPILOT_SDK_BYOK_API_KEY=sk-your-api-key
export COPILOT_SDK_BYOK_BASE_URL=https://api.openai.com/v1   # Optional, defaults to OpenAI
export COPILOT_SDK_BYOK_PROVIDER_TYPE=openai                 # Optional, defaults to "openai"
export COPILOT_SDK_BYOK_MODEL=gpt-4                          # Optional
```

`ProviderConfig` is only an environment helper. `from_env()` returns a
`NamedProviderConfig` named `"byok"` (or `nullopt` when no API key is set):

```cpp
copilot::CreateSessionOptions config;
if (auto provider = copilot::ProviderConfig::from_env()) {
    config.inference.providers = std::vector<copilot::NamedProviderConfig>{*provider};
    config.inference.default_provider = provider->name;  // "byok"
}
if (auto model = copilot::ProviderConfig::model_from_env())
    config.model = *model;

// copilot::ProviderConfig::is_env_configured() checks COPILOT_SDK_BYOK_API_KEY only.
```

See `examples/byok.cpp`.

## Migrating from 0.4.3

0.4.4 requires SDK protocol 4 and removes the old flat API without aliases.

- **Renamed types:** `SessionConfig` → `CreateSessionOptions`,
  `ResumeSessionConfig` → `ResumeSessionOptions`, `JoinSessionConfig` →
  `JoinSessionOptions` (wraps a `ResumeSessionOptions session`),
  `ClientMode::CopilotCli` → `ClientMode::CodingAgent`.
- **Runtime:** `ClientOptions::connection`, `cli_path`, `cli_args`, `cwd`,
  `environment`, `use_stdio`, `port`, `cli_url`, and `tcp_connection_token` are
  replaced by `ClientOptions::runtime` built with a factory:
  `for_spawned_stdio(path, args, ...)`, `for_spawned_tcp(port, token, ...)`,
  `for_tcp(host, port, token)` (was `cli_url`), or `for_in_process(...)`.
  `copilot_home` is now `runtime->base_directory`.
- **Client groups:** `log_level`/`telemetry` → `observability.*`; `github_token`,
  `use_logged_in_user` → `github.token`, `github.use_logged_in_user`;
  `Client::set_on_list_models(h)` → `ClientOptions::models.on_list_models = h`.
- **Session groups:** flat fields moved into groups, for example
  `tools` → `tools.definitions`, `on_permission_request` → `permissions.on_request`,
  `streaming` → `events.streaming`, `mcp_servers` → `mcp.servers`,
  `custom_agents` → `agents.definitions`, `hooks` → `hooks.handlers`,
  `reasoning_effort` → `inference.reasoning_effort`,
  `on_user_input_request` → `interaction.on_user_input_request`,
  `infinite_sessions` → `state.infinite_sessions`,
  `instruction_directories` → `instructions.directories`.
- **BYOK:** `SessionConfig::provider` (`ProviderConfig`) →
  `inference.providers` (`NamedProviderConfig` with a `name`) plus
  `inference.default_provider = "<name>"`.
- **Removed:** `auto_start` (create/resume and queries connect on demand),
  `auto_restart`, remote sessions (`remote_session`, `RemoteSessionMode`,
  `ClientOptions::remote`; use `CreateSessionOptions::cloud` or
  `Session::enable_remote()`), `auto_byok_from_env` (use
  `ProviderConfig::from_env()`), and the generic `ClientOptions::request_handler`.
  `github_token_provider` moved to the session as `github.token_provider`.
  `Client::rpc_client()` was removed; use `rpc_client_shared()`.
  `Session::get_messages()` was renamed to `Session::get_events()`.

## Install / Package

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=./install
cmake --build build --config Release
cmake --install build --config Release
```

## Related Projects

| Project | Language | Description |
|---------|----------|-------------|
| [claude-agent-sdk-cpp](https://github.com/0xeb/claude-agent-sdk-cpp) | C++ | C++ port of the Claude Agent SDK |
| [claude-agent-sdk-dotnet](https://github.com/0xeb/claude-agent-sdk-dotnet) | C# | .NET port of the Claude Agent SDK |
| [fastmcpp](https://github.com/0xeb/fastmcpp) | C++ | C++ port of FastMCP for building MCP servers |

## Projects Using This SDK

| Project | Description |
|---------|-------------|
| [windbg_copilot](https://github.com/0xeb/windbg_copilot) | WinDbg extension for AI-assisted debugging |
| [lldb_copilot](https://github.com/0xeb/lldb_copilot) | LLDB plugin for AI-assisted debugging |
| [libagents](https://github.com/0xeb/libagents) | Unified C++ library for AI agent providers |

Want to add your project? Open a PR!

## Author

Elias Bachaalany ([@0xeb](https://github.com/0xeb))

Pair-programmed with Claude Code and Codex.

## License

Copyright 2025 Elias Bachaalany

Licensed under the MIT License. See `LICENSE` for details.

This is a C++ port of [copilot-sdk](https://github.com/github/copilot-sdk) by GitHub.
