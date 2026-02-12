// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file test_tool_builder.cpp
/// @brief Tests for the fluent tool builder API

#include <gtest/gtest.h>

#include <copilot/tool_builder.hpp>

using namespace copilot;

// =============================================================================
// Schema Generation Tests
// =============================================================================

TEST(ToolBuilderTest, SchemaTypeTraits)
{
    // Basic types - use STREQ for C-string comparison
    EXPECT_STREQ(detail::schema_type<int>::type_name, "integer");
    EXPECT_STREQ(detail::schema_type<long>::type_name, "integer");
    EXPECT_STREQ(detail::schema_type<double>::type_name, "number");
    EXPECT_STREQ(detail::schema_type<float>::type_name, "number");
    EXPECT_STREQ(detail::schema_type<bool>::type_name, "boolean");
    EXPECT_STREQ(detail::schema_type<std::string>::type_name, "string");

    // Array type
    auto arr_schema = detail::schema_type<std::vector<int>>::schema();
    EXPECT_EQ(arr_schema["type"], "array");
    EXPECT_EQ(arr_schema["items"]["type"], "integer");
}

TEST(ToolBuilderTest, SingleParamTool)
{
    auto echo = ToolBuilder("echo", "Echo back input")
                    .param<std::string>("message", "Message to echo")
                    .handler([](const std::string& msg) { return msg; });

    EXPECT_EQ(echo.name, "echo");
    EXPECT_EQ(echo.description, "Echo back input");

    // Check schema
    EXPECT_EQ(echo.parameters_schema["type"], "object");
    EXPECT_TRUE(echo.parameters_schema["properties"].contains("message"));
    EXPECT_EQ(echo.parameters_schema["properties"]["message"]["type"], "string");
    EXPECT_EQ(echo.parameters_schema["properties"]["message"]["description"], "Message to echo");

    // Required should contain "message"
    auto& required = echo.parameters_schema["required"];
    EXPECT_EQ(required.size(), 1);
    EXPECT_EQ(required[0], "message");
}

TEST(ToolBuilderTest, MultiParamTool)
{
    auto calc = ToolBuilder("calc", "Calculator")
                    .param<double>("a", "First number")
                    .param<double>("b", "Second number")
                    .param<std::string>("op", "Operation")
                    .handler([](double a, double b, const std::string& op) {
                        if (op == "add")
                            return std::to_string(a + b);
                        if (op == "sub")
                            return std::to_string(a - b);
                        return std::string("unknown");
                    });

    EXPECT_EQ(calc.name, "calc");

    // Check all params present
    auto& props = calc.parameters_schema["properties"];
    EXPECT_TRUE(props.contains("a"));
    EXPECT_TRUE(props.contains("b"));
    EXPECT_TRUE(props.contains("op"));

    EXPECT_EQ(props["a"]["type"], "number");
    EXPECT_EQ(props["b"]["type"], "number");
    EXPECT_EQ(props["op"]["type"], "string");

    // All should be required
    auto& required = calc.parameters_schema["required"];
    EXPECT_EQ(required.size(), 3);
}

TEST(ToolBuilderTest, EnumConstraint)
{
    auto mood = ToolBuilder("set_mood", "Set mood")
                    .param<std::string>("mood", "Mood to set")
                    .one_of("happy", "sad", "neutral")
                    .handler([](const std::string& m) { return "Mood: " + m; });

    auto& mood_prop = mood.parameters_schema["properties"]["mood"];
    EXPECT_TRUE(mood_prop.contains("enum"));

    auto& enum_vals = mood_prop["enum"];
    EXPECT_EQ(enum_vals.size(), 3);
    EXPECT_EQ(enum_vals[0], "happy");
    EXPECT_EQ(enum_vals[1], "sad");
    EXPECT_EQ(enum_vals[2], "neutral");
}

TEST(ToolBuilderTest, DefaultValue)
{
    auto fetch = ToolBuilder("fetch", "Fetch URL")
                     .param<std::string>("url", "URL to fetch")
                     .param<int>("timeout", "Timeout in seconds")
                     .default_value(30)
                     .handler([](const std::string& url, int timeout) {
                         return url + " (timeout=" + std::to_string(timeout) + ")";
                     });

    // URL should be required
    auto& required = fetch.parameters_schema["required"];
    bool url_required = false;
    bool timeout_required = false;
    for (const auto& r : required)
    {
        if (r == "url")
            url_required = true;
        if (r == "timeout")
            timeout_required = true;
    }
    EXPECT_TRUE(url_required);
    EXPECT_FALSE(timeout_required); // timeout has default, so not required
}

// =============================================================================
// Handler Invocation Tests
// =============================================================================

TEST(ToolBuilderTest, HandlerInvocationSuccess)
{
    auto add = ToolBuilder("add", "Add numbers")
                   .param<double>("a", "First")
                   .param<double>("b", "Second")
                   .handler([](double a, double b) { return std::to_string(a + b); });

    ToolInvocation inv;
    inv.session_id = "test";
    inv.tool_call_id = "call1";
    inv.tool_name = "add";
    inv.arguments = json{{"a", 10.0}, {"b", 32.0}};

    auto result = add.handler(inv);
    EXPECT_EQ(result.result_type, ToolResultType::Success);
    EXPECT_EQ(result.text_result_for_llm, "42.000000");
}

TEST(ToolBuilderTest, HandlerInvocationWithStrings)
{
    auto greet = ToolBuilder("greet", "Greet someone")
                     .param<std::string>("name", "Name")
                     .handler([](const std::string& name) { return "Hello, " + name + "!"; });

    ToolInvocation inv;
    inv.session_id = "test";
    inv.tool_call_id = "call1";
    inv.tool_name = "greet";
    inv.arguments = json{{"name", "World"}};

    auto result = greet.handler(inv);
    EXPECT_EQ(result.result_type, ToolResultType::Success);
    EXPECT_EQ(result.text_result_for_llm, "Hello, World!");
}

TEST(ToolBuilderTest, HandlerWithDefaultValue)
{
    auto fetch = ToolBuilder("fetch", "Fetch")
                     .param<std::string>("url", "URL")
                     .param<int>("timeout", "Timeout")
                     .default_value(30)
                     .handler([](const std::string& url, int timeout) {
                         return url + ":" + std::to_string(timeout);
                     });

    // Without timeout provided
    ToolInvocation inv1;
    inv1.arguments = json{{"url", "http://example.com"}};
    auto result1 = fetch.handler(inv1);
    EXPECT_EQ(result1.text_result_for_llm, "http://example.com:30");

    // With timeout provided
    ToolInvocation inv2;
    inv2.arguments = json{{"url", "http://example.com"}, {"timeout", 60}};
    auto result2 = fetch.handler(inv2);
    EXPECT_EQ(result2.text_result_for_llm, "http://example.com:60");
}

TEST(ToolBuilderTest, HandlerErrorHandling)
{
    auto div = ToolBuilder("divide", "Divide")
                   .param<double>("a", "Numerator")
                   .param<double>("b", "Denominator")
                   .handler([](double a, double b) -> std::string {
                       if (b == 0)
                           throw std::runtime_error("Division by zero");
                       return std::to_string(a / b);
                   });

    ToolInvocation inv;
    inv.arguments = json{{"a", 10.0}, {"b", 0.0}};

    auto result = div.handler(inv);
    EXPECT_EQ(result.result_type, ToolResultType::Failure);
    EXPECT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "Division by zero");
}

TEST(ToolBuilderTest, MissingRequiredArg)
{
    auto greet = ToolBuilder("greet", "Greet")
                     .param<std::string>("name", "Name")
                     .handler([](const std::string& name) { return "Hi " + name; });

    ToolInvocation inv;
    inv.arguments = json::object(); // Missing "name"

    auto result = greet.handler(inv);
    EXPECT_EQ(result.result_type, ToolResultType::Failure);
    EXPECT_TRUE(result.error.has_value());
}

// =============================================================================
// Struct-based Builder Tests (Option B)
// =============================================================================

struct SearchArgs
{
    std::string query;
    int limit;

    NLOHMANN_DEFINE_TYPE_INTRUSIVE(SearchArgs, query, limit)
};

TEST(ToolBuilderTest, StructBasedBuilder)
{
    auto search = ToolBuilder::create<SearchArgs>("search", "Search documents")
                      .describe(&SearchArgs::query, "query", "Search query text")
                      .describe(&SearchArgs::limit, "limit", "Maximum results")
                      .handler([](const SearchArgs& args) {
                          return "Searching: " + args.query + " (limit=" + std::to_string(args.limit) + ")";
                      });

    EXPECT_EQ(search.name, "search");
    EXPECT_EQ(search.description, "Search documents");

    // Check schema
    auto& props = search.parameters_schema["properties"];
    EXPECT_TRUE(props.contains("query"));
    EXPECT_TRUE(props.contains("limit"));
}

TEST(ToolBuilderTest, StructBasedHandlerInvocation)
{
    auto search = ToolBuilder::create<SearchArgs>("search", "Search")
                      .describe(&SearchArgs::query, "query", "Query")
                      .describe(&SearchArgs::limit, "limit", "Limit")
                      .handler([](const SearchArgs& args) {
                          return args.query + ":" + std::to_string(args.limit);
                      });

    ToolInvocation inv;
    inv.arguments = json{{"query", "test"}, {"limit", 5}};

    auto result = search.handler(inv);
    EXPECT_EQ(result.result_type, ToolResultType::Success);
    EXPECT_EQ(result.text_result_for_llm, "test:5");
}

// =============================================================================
// Convenience Function Tests
// =============================================================================

TEST(ToolBuilderTest, ConvenienceFunction)
{
    auto echo = copilot::tool("echo", "Echo")
                    .param<std::string>("msg", "Message")
                    .handler([](const std::string& m) { return m; });

    EXPECT_EQ(echo.name, "echo");
}

// =============================================================================
// Integration with SessionConfig
// =============================================================================

TEST(ToolBuilderTest, SessionConfigIntegration)
{
    auto tool1 = ToolBuilder("tool1", "First tool")
                     .param<std::string>("x", "X param")
                     .handler([](const std::string& x) { return x; });

    auto tool2 = ToolBuilder("tool2", "Second tool")
                     .param<int>("n", "N param")
                     .handler([](int n) { return std::to_string(n * 2); });

    SessionConfig config;
    config.tools = {tool1, tool2};

    EXPECT_EQ(config.tools.size(), 2);
    EXPECT_EQ(config.tools[0].name, "tool1");
    EXPECT_EQ(config.tools[1].name, "tool2");
}

// =============================================================================
// Backward Compatibility Tests
// =============================================================================

TEST(ToolBuilderTest, BackwardCompatibility)
{
    // Old way should still work
    Tool old_tool;
    old_tool.name = "old_tool";
    old_tool.description = "Created the old way";
    old_tool.parameters_schema = json{
        {"type", "object"},
        {"properties", {{"x", {{"type", "string"}}}}},
        {"required", {"x"}}};
    old_tool.handler = [](const ToolInvocation& inv) -> ToolResultObject {
        ToolResultObject r;
        r.text_result_for_llm = "old style";
        r.result_type = ToolResultType::Success;
        return r;
    };

    EXPECT_EQ(old_tool.name, "old_tool");

    ToolInvocation inv;
    inv.arguments = json{{"x", "test"}};
    auto result = old_tool.handler(inv);
    EXPECT_EQ(result.text_result_for_llm, "old style");
}

// =============================================================================
// make_tool Function Tests (Claude SDK compatible API)
// =============================================================================

TEST(MakeToolTest, SingleParam)
{
    auto tool = copilot::make_tool(
        "echo", "Echo message", [](std::string msg) { return msg; }, {"message"});

    EXPECT_EQ(tool.name, "echo");
    EXPECT_EQ(tool.description, "Echo message");

    // Check schema
    EXPECT_EQ(tool.parameters_schema["type"], "object");
    EXPECT_TRUE(tool.parameters_schema["properties"].contains("message"));
    EXPECT_EQ(tool.parameters_schema["properties"]["message"]["type"], "string");

    // Test invocation
    ToolInvocation inv;
    inv.arguments = json{{"message", "Hello"}};
    auto result = tool.handler(inv);
    EXPECT_EQ(result.text_result_for_llm, "Hello");
}

TEST(MakeToolTest, MultipleParams)
{
    auto tool = copilot::make_tool(
        "calc", "Calculator",
        [](double a, double b) { return std::to_string(a + b); },
        {"first", "second"});

    EXPECT_EQ(tool.name, "calc");

    // Check schema
    auto& props = tool.parameters_schema["properties"];
    EXPECT_TRUE(props.contains("first"));
    EXPECT_TRUE(props.contains("second"));
    EXPECT_EQ(props["first"]["type"], "number");
    EXPECT_EQ(props["second"]["type"], "number");

    // Test invocation
    ToolInvocation inv;
    inv.arguments = json{{"first", 10.0}, {"second", 32.0}};
    auto result = tool.handler(inv);
    EXPECT_EQ(result.text_result_for_llm, "42.000000");
}

TEST(MakeToolTest, AutoParamNames)
{
    // Use make_tool without param names - should get arg0, arg1, etc.
    auto tool = copilot::make_tool(
        "greet", "Greet",
        [](std::string name, int count) { return name + ":" + std::to_string(count); });

    auto& props = tool.parameters_schema["properties"];
    EXPECT_TRUE(props.contains("arg0"));
    EXPECT_TRUE(props.contains("arg1"));

    // Test invocation
    ToolInvocation inv;
    inv.arguments = json{{"arg0", "test"}, {"arg1", 5}};
    auto result = tool.handler(inv);
    EXPECT_EQ(result.text_result_for_llm, "test:5");
}

TEST(MakeToolTest, ParamCountMismatch)
{
    EXPECT_THROW(
        copilot::make_tool(
            "bad", "Bad",
            [](std::string a, std::string b) { return a + b; },
            {"only_one"}  // Mismatch: 2 params, 1 name
        ),
        std::invalid_argument);
}

TEST(MakeToolTest, ErrorHandling)
{
    auto tool = copilot::make_tool(
        "fail", "Always fails",
        [](std::string) -> std::string { throw std::runtime_error("boom"); },
        {"input"});

    ToolInvocation inv;
    inv.arguments = json{{"input", "test"}};
    auto result = tool.handler(inv);
    EXPECT_EQ(result.result_type, ToolResultType::Failure);
    EXPECT_TRUE(result.error.has_value());
    EXPECT_EQ(*result.error, "boom");
}

TEST(MakeToolTest, IntAndBoolParams)
{
    auto tool = copilot::make_tool(
        "config", "Config tool",
        [](int port, bool enabled) {
            return "port=" + std::to_string(port) + ",enabled=" + (enabled ? "true" : "false");
        },
        {"port", "enabled"});

    auto& props = tool.parameters_schema["properties"];
    EXPECT_EQ(props["port"]["type"], "integer");
    EXPECT_EQ(props["enabled"]["type"], "boolean");

    ToolInvocation inv;
    inv.arguments = json{{"port", 8080}, {"enabled", true}};
    auto result = tool.handler(inv);
    EXPECT_EQ(result.text_result_for_llm, "port=8080,enabled=true");
}
