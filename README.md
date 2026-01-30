# copilot-sdk-cpp

C++ SDK for interacting with the Copilot CLI agent runtime (JSON-RPC over stdio or TCP).

[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-blue)]()
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)]()
[![License](https://img.shields.io/badge/license-MIT-green)]()

## Build

Requirements: CMake 3.20+ and a C++20 compiler.

```sh
cmake -S . -B build
cmake --build build --config Release
```

## Tests

Unit tests:

```sh
ctest --test-dir build -C Release --output-on-failure
```

E2E tests (real Copilot CLI):
- Require `copilot` to be installed and authenticated.
- To disable E2E tests in CI/non-interactive runs, set `COPILOT_SDK_CPP_SKIP_E2E=1`.

Snapshot conformance tests (optional):
- Require Python 3 and the upstream `copilot-sdk` snapshots directory.
- Install Python deps: `python -m pip install --user -r tests/snapshot_tests/requirements.txt`
- Enable with `-DCOPILOT_BUILD_SNAPSHOT_TESTS=ON` and set `-DCOPILOT_SDK_CPP_SNAPSHOT_DIR=...` if auto-detection fails.
- Run: `cmake --build build --target run_snapshot_tests --config Release`

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
copilot::SessionConfig config;
config.tools = {calc, search};
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

config.tools = {echo, add, greet};
```

See `examples/tools.cpp` and `examples/resume_with_tools.cpp` for complete examples.

## BYOK (Bring Your Own Key)

Use your own API key instead of GitHub Copilot authentication.

### Method 1: Explicit Configuration

```cpp
copilot::ProviderConfig provider;
provider.api_key = "sk-your-api-key";
provider.base_url = "https://api.openai.com/v1";
provider.type = "openai";

copilot::SessionConfig config;
config.provider = provider;
config.model = "gpt-4";
auto session = client.create_session(config).get();
```

### Method 2: Environment Variables

Set environment variables:

```bash
export COPILOT_SDK_BYOK_API_KEY=sk-your-api-key
export COPILOT_SDK_BYOK_BASE_URL=https://api.openai.com/v1   # Optional, defaults to OpenAI
export COPILOT_SDK_BYOK_PROVIDER_TYPE=openai                 # Optional, defaults to "openai"
export COPILOT_SDK_BYOK_MODEL=gpt-4                          # Optional
```

Then enable auto-loading in your code:

```cpp
copilot::SessionConfig config;
config.auto_byok_from_env = true;  // Load from COPILOT_SDK_BYOK_* env vars
auto session = client.create_session(config).get();
```

**Precedence** (for each field):
1. Explicit value in `SessionConfig` (highest priority)
2. Environment variable (if `auto_byok_from_env = true`)
3. Default Copilot behavior (lowest priority)

**Note:** `auto_byok_from_env` defaults to `false` for backwards compatibility. Existing code will not be affected by setting these environment variables.

### Checking Environment Configuration

```cpp
// Check if BYOK env vars are configured
if (copilot::ProviderConfig::is_env_configured()) {
    // COPILOT_SDK_BYOK_API_KEY is set
}

// Load provider config from env (returns nullopt if not configured)
if (auto provider = copilot::ProviderConfig::from_env()) {
    // Use *provider
}

// Load model from env
if (auto model = copilot::ProviderConfig::model_from_env()) {
    // Use *model
}
```

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
