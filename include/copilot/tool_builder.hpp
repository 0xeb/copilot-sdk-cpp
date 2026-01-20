// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

/// @file tool_builder.hpp
/// @brief Fluent builder API for creating tools with automatic schema generation
///
/// This provides a modern C++ alternative to manually constructing Tool objects.
/// The existing Tool struct and manual construction remain fully supported.
///
/// Example (Option A - Fluent Builder):
/// @code
/// auto calc = copilot::ToolBuilder("calculate", "Perform arithmetic")
///     .param<double>("a", "First operand")
///     .param<double>("b", "Second operand")
///     .param<std::string>("op", "Operation").one_of("add", "subtract", "multiply")
///     .handler([](double a, double b, const std::string& op) {
///         if (op == "add") return std::to_string(a + b);
///         return std::string("unknown");
///     });
/// @endcode
///
/// Example (Option B - Struct-based):
/// @code
/// struct SearchArgs {
///     std::string query;
///     int limit = 10;
///     NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(SearchArgs, query, limit)
/// };
///
/// auto search = copilot::ToolBuilder::create<SearchArgs>("search", "Search docs")
///     .describe(&SearchArgs::query, "Search query")
///     .describe(&SearchArgs::limit, "Max results")
///     .handler([](const SearchArgs& args) {
///         return "Searching: " + args.query;
///     });
/// @endcode

#include <copilot/types.hpp>

#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace copilot
{

// =============================================================================
// Type Traits for JSON Schema Generation
// =============================================================================

namespace detail
{

/// Primary template for JSON schema type names
template<typename T, typename Enable = void>
struct schema_type
{
    static constexpr const char* type_name = "object";
    static json schema() { return {{"type", type_name}}; }
};

template<>
struct schema_type<bool>
{
    static constexpr const char* type_name = "boolean";
    static json schema() { return {{"type", type_name}}; }
};

template<>
struct schema_type<int>
{
    static constexpr const char* type_name = "integer";
    static json schema() { return {{"type", type_name}}; }
};

template<>
struct schema_type<long>
{
    static constexpr const char* type_name = "integer";
    static json schema() { return {{"type", type_name}}; }
};

template<>
struct schema_type<long long>
{
    static constexpr const char* type_name = "integer";
    static json schema() { return {{"type", type_name}}; }
};

template<>
struct schema_type<float>
{
    static constexpr const char* type_name = "number";
    static json schema() { return {{"type", type_name}}; }
};

template<>
struct schema_type<double>
{
    static constexpr const char* type_name = "number";
    static json schema() { return {{"type", type_name}}; }
};

template<>
struct schema_type<std::string>
{
    static constexpr const char* type_name = "string";
    static json schema() { return {{"type", type_name}}; }
};

template<>
struct schema_type<const char*>
{
    static constexpr const char* type_name = "string";
    static json schema() { return {{"type", type_name}}; }
};

/// Vector/array specialization
template<typename T>
struct schema_type<std::vector<T>>
{
    static constexpr const char* type_name = "array";
    static json schema()
    {
        return {{"type", "array"}, {"items", schema_type<T>::schema()}};
    }
};

/// Optional specialization - uses underlying type
template<typename T>
struct schema_type<std::optional<T>>
{
    static constexpr const char* type_name = schema_type<T>::type_name;
    static json schema() { return schema_type<T>::schema(); }
};

// Type trait to detect std::optional
template<typename T>
struct is_optional : std::false_type
{
};

template<typename T>
struct is_optional<std::optional<T>> : std::true_type
{
};

template<typename T>
inline constexpr bool is_optional_v = is_optional<T>::value;

/// Convert value to string for tool result
template<typename T>
std::string to_result_string(const T& value)
{
    if constexpr (std::is_same_v<T, std::string>)
    {
        return value;
    }
    else if constexpr (std::is_same_v<T, bool>)
    {
        return value ? "true" : "false";
    }
    else if constexpr (std::is_arithmetic_v<T>)
    {
        std::ostringstream oss;
        oss << value;
        return oss.str();
    }
    else
    {
        // For complex types, serialize to JSON
        return json(value).dump();
    }
}

/// Extract argument from JSON by name
template<typename T>
T extract_arg(const json& args, const std::string& name)
{
    if constexpr (is_optional_v<std::decay_t<T>>)
    {
        using value_type = typename std::decay_t<T>::value_type;
        if (!args.contains(name) || args.at(name).is_null())
        {
            return std::nullopt;
        }
        return extract_arg<value_type>(args, name);
    }
    else if constexpr (std::is_same_v<std::decay_t<T>, std::string>)
    {
        return args.at(name).get<std::string>();
    }
    else
    {
        return args.at(name).get<std::decay_t<T>>();
    }
}

/// Extract argument with default value
template<typename T>
T extract_arg_or(const json& args, const std::string& name, const T& default_val)
{
    if (args.contains(name) && !args.at(name).is_null())
    {
        return extract_arg<T>(args, name);
    }
    return default_val;
}

} // namespace detail

// =============================================================================
// Parameter Descriptor
// =============================================================================

/// Describes a single tool parameter
struct ParamDescriptor
{
    std::string name;
    std::string description;
    json type_schema;
    bool required = true;
    std::optional<json> enum_values;
    std::optional<json> default_value;
};

// =============================================================================
// ToolBuilder - Fluent Builder (Option A)
// =============================================================================

// Forward declarations
template<typename... Args>
class ToolBuilderWithParams;

template<typename... Args>
class ToolBuilderFinal;

/// Entry point for fluent tool building
class ToolBuilder
{
  public:
    /// Start building a tool with name and description
    ToolBuilder(std::string name, std::string description)
        : name_(std::move(name)), description_(std::move(description))
    {
    }

    /// Add a required parameter
    template<typename T>
    ToolBuilderWithParams<T> param(std::string name, std::string description)
    {
        ParamDescriptor p;
        p.name = std::move(name);
        p.description = std::move(description);
        p.type_schema = detail::schema_type<T>::schema();
        p.required = true;
        return ToolBuilderWithParams<T>(name_, description_, {p});
    }

    /// Create a struct-based tool builder (Option B)
    template<typename ArgsStruct>
    class StructBuilder;

    template<typename ArgsStruct>
    static StructBuilder<ArgsStruct> create(std::string name, std::string description)
    {
        return StructBuilder<ArgsStruct>(std::move(name), std::move(description));
    }

  private:
    std::string name_;
    std::string description_;
};

/// Builder with accumulated parameters
template<typename... Args>
class ToolBuilderWithParams
{
  public:
    ToolBuilderWithParams(std::string name, std::string desc, std::vector<ParamDescriptor> params)
        : name_(std::move(name)), description_(std::move(desc)), params_(std::move(params))
    {
    }

    /// Add enum constraint to last parameter
    template<typename... Vals>
    ToolBuilderWithParams& one_of(Vals&&... vals)
    {
        params_.back().enum_values = json::array({std::forward<Vals>(vals)...});
        return *this;
    }

    /// Mark last parameter as optional with default value
    template<typename T>
    ToolBuilderWithParams& default_value(T val)
    {
        params_.back().required = false;
        params_.back().default_value = json(val);
        return *this;
    }

    /// Add another required parameter
    template<typename T>
    ToolBuilderWithParams<Args..., T> param(std::string name, std::string description)
    {
        ParamDescriptor p;
        p.name = std::move(name);
        p.description = std::move(description);
        p.type_schema = detail::schema_type<T>::schema();
        p.required = true;

        std::vector<ParamDescriptor> new_params = params_;
        new_params.push_back(std::move(p));
        return ToolBuilderWithParams<Args..., T>(name_, description_, std::move(new_params));
    }

    /// Set handler and finalize (returns Tool)
    template<typename Func>
    Tool handler(Func&& fn)
    {
        return build_tool(std::forward<Func>(fn), std::index_sequence_for<Args...>{});
    }

  private:
    template<typename Func, std::size_t... Is>
    Tool build_tool(Func&& fn, std::index_sequence<Is...>)
    {
        Tool tool;
        tool.name = name_;
        tool.description = description_;
        tool.parameters_schema = generate_schema();

        // Capture params by value for the lambda
        auto params = params_;
        tool.handler = [fn = std::forward<Func>(fn), params](const ToolInvocation& inv) -> ToolResultObject
        {
            ToolResultObject result;
            try
            {
                const json& args = inv.arguments.value_or(json::object());

                // Extract each argument by name in order
                auto extracted = std::make_tuple(
                    extract_param<Args>(args, params[Is])...);

                // Call the handler with extracted arguments
                auto ret = std::apply(fn, extracted);
                result.text_result_for_llm = detail::to_result_string(ret);
                result.result_type = "success";
            }
            catch (const std::exception& e)
            {
                result.result_type = "error";
                result.error = e.what();
                result.text_result_for_llm = std::string("Error: ") + e.what();
            }
            return result;
        };

        return tool;
    }

    template<typename T>
    static T extract_param(const json& args, const ParamDescriptor& param)
    {
        if (param.default_value.has_value())
        {
            return detail::extract_arg_or<T>(args, param.name, param.default_value->get<T>());
        }
        return detail::extract_arg<T>(args, param.name);
    }

    json generate_schema() const
    {
        json props = json::object();
        json required = json::array();

        for (const auto& p : params_)
        {
            json prop = p.type_schema;
            prop["description"] = p.description;
            if (p.enum_values)
            {
                prop["enum"] = *p.enum_values;
            }
            props[p.name] = prop;

            if (p.required)
            {
                required.push_back(p.name);
            }
        }

        return {{"type", "object"}, {"properties", props}, {"required", required}};
    }

    std::string name_;
    std::string description_;
    std::vector<ParamDescriptor> params_;
};

// =============================================================================
// StructToolBuilder - Struct-based Builder (Option B)
// =============================================================================

/// Builder for struct-based tool arguments
template<typename ArgsStruct>
class ToolBuilder::StructBuilder
{
  public:
    StructBuilder(std::string name, std::string description)
        : name_(std::move(name)), description_(std::move(description))
    {
    }

    /// Describe a field (uses field name as JSON key)
    template<typename FieldType>
    StructBuilder& describe(FieldType ArgsStruct::*, std::string field_name, std::string description)
    {
        ParamDescriptor p;
        p.name = std::move(field_name);
        p.description = std::move(description);
        p.type_schema = detail::schema_type<FieldType>::schema();

        // Check if optional type
        p.required = !detail::is_optional_v<FieldType>;

        params_.push_back(std::move(p));
        return *this;
    }

    /// Convenience: describe with just description (field name inferred - requires macro)
    /// For now, users must provide field name explicitly

    /// Add enum constraint to last parameter
    template<typename... Vals>
    StructBuilder& one_of(Vals&&... vals)
    {
        if (!params_.empty())
        {
            params_.back().enum_values = json::array({std::forward<Vals>(vals)...});
        }
        return *this;
    }

    /// Set default value for last parameter
    template<typename T>
    StructBuilder& default_value(T val)
    {
        if (!params_.empty())
        {
            params_.back().required = false;
            params_.back().default_value = json(val);
        }
        return *this;
    }

    /// Set handler and finalize (returns Tool)
    template<typename Func>
    Tool handler(Func&& fn)
    {
        Tool tool;
        tool.name = name_;
        tool.description = description_;
        tool.parameters_schema = generate_schema();

        tool.handler = [fn = std::forward<Func>(fn)](const ToolInvocation& inv) -> ToolResultObject
        {
            ToolResultObject result;
            try
            {
                const json& args = inv.arguments.value_or(json::object());

                // Deserialize directly to struct (requires NLOHMANN_DEFINE_TYPE_INTRUSIVE)
                ArgsStruct parsed = args.get<ArgsStruct>();

                auto ret = fn(parsed);
                result.text_result_for_llm = detail::to_result_string(ret);
                result.result_type = "success";
            }
            catch (const std::exception& e)
            {
                result.result_type = "error";
                result.error = e.what();
                result.text_result_for_llm = std::string("Error: ") + e.what();
            }
            return result;
        };

        return tool;
    }

  private:
    json generate_schema() const
    {
        json props = json::object();
        json required = json::array();

        for (const auto& p : params_)
        {
            json prop = p.type_schema;
            prop["description"] = p.description;
            if (p.enum_values)
            {
                prop["enum"] = *p.enum_values;
            }
            props[p.name] = prop;

            if (p.required)
            {
                required.push_back(p.name);
            }
        }

        return {{"type", "object"}, {"properties", props}, {"required", required}};
    }

    std::string name_;
    std::string description_;
    std::vector<ParamDescriptor> params_;
};

// =============================================================================
// Convenience Aliases
// =============================================================================

/// Start building a tool (shorthand)
inline ToolBuilder tool(std::string name, std::string description)
{
    return ToolBuilder(std::move(name), std::move(description));
}

// =============================================================================
// Function Traits for make_tool
// =============================================================================

namespace detail
{

/// Remove cv-ref qualifiers
template <typename T>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

/// Function traits primary template
template <typename T>
struct function_traits : function_traits<decltype(&T::operator())>
{
};

/// Specialization for function pointers
template <typename R, typename... Args>
struct function_traits<R (*)(Args...)>
{
    using return_type = R;
    static constexpr size_t arity = sizeof...(Args);

    template <size_t N>
    using arg_type = std::tuple_element_t<N, std::tuple<Args...>>;
};

/// Specialization for member function pointers (const)
template <typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...) const>
{
    using return_type = R;
    static constexpr size_t arity = sizeof...(Args);

    template <size_t N>
    using arg_type = std::tuple_element_t<N, std::tuple<Args...>>;
};

/// Specialization for member function pointers (non-const)
template <typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...)>
{
    using return_type = R;
    static constexpr size_t arity = sizeof...(Args);

    template <size_t N>
    using arg_type = std::tuple_element_t<N, std::tuple<Args...>>;
};

/// Helper to invoke function with JSON args
template <typename Func, size_t... Is>
auto invoke_with_json_impl(Func&& func, const json& args,
                           const std::vector<std::string>& names, std::index_sequence<Is...>)
{
    using traits = function_traits<remove_cvref_t<Func>>;
    return func(
        extract_arg<remove_cvref_t<typename traits::template arg_type<Is>>>(args, names[Is])...);
}

template <typename Func>
auto invoke_with_json(Func&& func, const json& args, const std::vector<std::string>& names)
{
    using traits = function_traits<remove_cvref_t<Func>>;
    return invoke_with_json_impl<Func>(std::forward<Func>(func), args, names,
                                       std::make_index_sequence<traits::arity>{});
}

template<typename T>
void add_required_if(json& required, const std::string& name)
{
    if constexpr (!is_optional_v<T>)
    {
        required.push_back(name);
    }
}

/// Generate schema from function signature
template <typename Func, size_t... Is>
json generate_schema_impl(const std::vector<std::string>& names, std::index_sequence<Is...>)
{
    using traits = function_traits<remove_cvref_t<Func>>;
    json schema = {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}};

    ((schema["properties"][names[Is]] =
          schema_type<remove_cvref_t<typename traits::template arg_type<Is>>>::schema(),
      add_required_if<remove_cvref_t<typename traits::template arg_type<Is>>>(
          schema["required"], names[Is])),
     ...);

    return schema;
}

template <typename Func>
json generate_schema(const std::vector<std::string>& names)
{
    using traits = function_traits<remove_cvref_t<Func>>;
    return generate_schema_impl<Func>(names, std::make_index_sequence<traits::arity>{});
}

} // namespace detail

// =============================================================================
// make_tool - Claude SDK compatible API
// =============================================================================

/// Create a tool from a function with custom parameter names
/// Similar to claude::mcp::make_tool for API consistency
///
/// Example:
/// @code
/// auto tool = copilot::make_tool("dbg_exec", "Execute debugger command",
///     [](std::string command) { return execute(command); },
///     {"command"});
/// @endcode
template <typename Func>
Tool make_tool(std::string name, std::string description, Func&& func,
               std::vector<std::string> param_names)
{
    using traits = detail::function_traits<detail::remove_cvref_t<Func>>;

    if (param_names.size() != traits::arity)
    {
        throw std::invalid_argument("Parameter name count mismatch for tool '" + name +
                                    "': expected " + std::to_string(traits::arity) + ", got " +
                                    std::to_string(param_names.size()));
    }

    Tool tool;
    tool.name = std::move(name);
    tool.description = std::move(description);
    tool.parameters_schema = detail::generate_schema<Func>(param_names);

    // Create handler that extracts args and invokes function
    tool.handler = [f = std::forward<Func>(func),
                    names = std::move(param_names)](const ToolInvocation& inv) -> ToolResultObject
    {
        ToolResultObject result;
        try
        {
            json args = inv.arguments.value_or(json::object());
            auto output = detail::invoke_with_json(f, args, names);
            result.text_result_for_llm = detail::to_result_string(output);
        }
        catch (const std::exception& e)
        {
            result.result_type = "error";
            result.error = e.what();
        }
        return result;
    };

    return tool;
}

/// Create a tool with a single string parameter (common case)
/// @code
/// auto tool = copilot::make_tool("echo", "Echo message",
///     [](std::string msg) { return msg; });  // Auto-names param "arg0"
/// @endcode
template <typename Func>
Tool make_tool(std::string name, std::string description, Func&& func)
{
    using traits = detail::function_traits<detail::remove_cvref_t<Func>>;
    std::vector<std::string> names;
    for (size_t i = 0; i < traits::arity; ++i)
        names.push_back("arg" + std::to_string(i));
    return make_tool(std::move(name), std::move(description), std::forward<Func>(func),
                     std::move(names));
}

} // namespace copilot
