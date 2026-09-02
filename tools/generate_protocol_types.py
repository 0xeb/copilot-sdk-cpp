#!/usr/bin/env python3
"""Generate complete, wire-preserving C++ types from official Copilot schemas."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from collections import Counter
from pathlib import Path
from typing import Any, Iterable


REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASELINE = REPO_ROOT / "schemas" / "official-baseline.json"
DEFAULT_OUTPUT = REPO_ROOT / "include" / "copilot" / "generated"
SCHEMA_GROUPS = ("api", "sessionEvents")


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_json(value: Any) -> str:
    return sha256_bytes(canonical_json(value).encode("utf-8"))


def sha256_names(values: Iterable[str]) -> str:
    return sha256_bytes(("\n".join(sorted(values)) + "\n").encode("utf-8"))


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=False)


def validate_official_sdk(root: Path, baseline: dict[str, Any]) -> None:
    expected = baseline["officialSdk"]
    head = subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if head != expected["commit"]:
        raise ValueError(f"official SDK commit is {head}, expected {expected['commit']}")

    version_file = load_json(root / expected["protocolVersionSource"])
    if version_file["version"] != baseline["protocol"]["version"]:
        raise ValueError(
            f"official protocol version is {version_file['version']}, "
            f"expected {baseline['protocol']['version']}"
        )

    package_lock = load_json(root / "nodejs" / "package-lock.json")
    package = package_lock["packages"]["node_modules/@github/copilot"]
    if package["version"] != expected["schemaPackageVersion"]:
        raise ValueError(
            f"official schema package is {package['version']}, "
            f"expected {expected['schemaPackageVersion']}"
        )


def validate_schema_package(schema_path: Path, expected_version: str) -> None:
    for directory in schema_path.resolve().parents:
        package_json = directory / "package.json"
        if not package_json.exists():
            continue
        package = load_json(package_json)
        name = package.get("name", "")
        if name == "@github/copilot" or name.startswith("@github/copilot-"):
            if package.get("version") != expected_version:
                raise ValueError(
                    f"{schema_path} comes from {name}@{package.get('version')}, "
                    f"expected {expected_version}"
                )
            return
    raise ValueError(f"could not find the @github/copilot package for {schema_path}")


def schema_kind(schema: dict[str, Any]) -> str:
    if schema.get("x-opaque-json"):
        return "opaque"
    if "anyOf" in schema or "oneOf" in schema:
        return "union"
    if "$ref" in schema:
        return "alias"
    kind = schema.get("type")
    if kind == "string" and ("enum" in schema or "const" in schema):
        return "enum"
    if kind == "object":
        return "map" if not schema.get("properties") and schema.get("additionalProperties") else "object"
    if kind in {"array", "string", "integer", "number", "boolean", "null"}:
        return kind
    return "opaque"


def known_string_values(schema: dict[str, Any]) -> list[str]:
    values: list[str] = []
    if isinstance(schema.get("const"), str):
        values.append(schema["const"])
    values.extend(value for value in schema.get("enum", []) if isinstance(value, str))
    for branch in schema.get("anyOf", []) + schema.get("oneOf", []):
        if isinstance(branch, dict):
            values.extend(known_string_values(branch))
    return list(dict.fromkeys(values))


def definition_entry(name: str, schema: dict[str, Any]) -> dict[str, Any]:
    properties = schema.get("properties", {})
    required = set(schema.get("required", []))
    variants = schema.get("anyOf", schema.get("oneOf", []))
    additional = schema.get("additionalProperties")
    return {
        "name": name,
        "kind": schema_kind(schema),
        "schemaSha256": sha256_json(schema),
        "propertyCount": len(properties),
        "requiredPropertyCount": len(required),
        "hasOptionalProperties": bool(set(properties) - required),
        "hasMap": additional not in (None, False),
        "unionVariantCount": len(variants),
        "knownStringValues": known_string_values(schema),
    }


def collect_rpc_methods(document: dict[str, Any]) -> list[str]:
    methods: list[str] = []

    def visit(value: Any) -> None:
        if isinstance(value, dict):
            method = value.get("rpcMethod")
            if isinstance(method, str):
                methods.append(method)
            for child in value.values():
                visit(child)
        elif isinstance(value, list):
            for child in value:
                visit(child)

    for root_name in ("server", "session", "clientSession", "clientGlobal"):
        visit(document[root_name])
    if len(methods) != len(set(methods)):
        raise ValueError("api.schema.json contains duplicate rpcMethod values")
    return methods


def collect_event_variants(document: dict[str, Any]) -> list[dict[str, str]]:
    definitions = document["definitions"]
    union = definitions["SessionEvent"].get("anyOf", [])
    variants: list[dict[str, str]] = []
    for branch in union:
        reference = branch.get("$ref", "")
        name = reference.rsplit("/", 1)[-1]
        event_schema = definitions.get(name)
        if not event_schema:
            raise ValueError(f"SessionEvent references missing definition {name}")
        wire_type = event_schema.get("properties", {}).get("type", {}).get("const")
        if not isinstance(wire_type, str):
            raise ValueError(f"{name} does not have a constant event type discriminator")
        variants.append({"definition": name, "wireType": wire_type})
    wire_types = [variant["wireType"] for variant in variants]
    if len(wire_types) != len(set(wire_types)):
        raise ValueError("session event union contains duplicate wire type values")
    return variants


def build_surface(
    api_path: Path, session_events_path: Path, baseline: dict[str, Any]
) -> dict[str, Any]:
    expected_version = baseline["officialSdk"]["schemaPackageVersion"]
    validate_schema_package(api_path, expected_version)
    validate_schema_package(session_events_path, expected_version)

    api = load_json(api_path)
    events = load_json(session_events_path)
    api_definitions = api["definitions"]
    event_definitions = events["definitions"]

    event_variants = collect_event_variants(events)
    surface = {
        "api": [
            definition_entry(name, api_definitions[name]) for name in sorted(api_definitions)
        ],
        "sessionEvents": [
            definition_entry(name, event_definitions[name])
            for name in sorted(event_definitions)
        ],
        "rpcMethods": collect_rpc_methods(api),
        "sessionEventVariants": event_variants,
    }
    session_event_entry = next(
        entry for entry in surface["sessionEvents"] if entry["name"] == "SessionEvent"
    )
    session_event_entry["knownStringValues"] = [
        variant["wireType"] for variant in event_variants
    ]

    api_names = set(api_definitions)
    event_names = set(event_definitions)
    api_kinds = Counter(entry["kind"] for entry in surface["api"])
    event_kinds = Counter(entry["kind"] for entry in surface["sessionEvents"])
    coverage = {
        "apiDefinitionCount": len(api_names),
        "sessionEventDefinitionCount": len(event_names),
        "sharedDefinitionCount": len(api_names & event_names),
        "uniqueDefinitionCount": len(api_names | event_names),
        "rpcMethodCount": len(surface["rpcMethods"]),
        "sessionEventVariantCount": len(surface["sessionEventVariants"]),
        "apiDefinitionNamesSha256": sha256_names(api_names),
        "sessionEventDefinitionNamesSha256": sha256_names(event_names),
        "rpcMethodNamesSha256": sha256_names(surface["rpcMethods"]),
        "sessionEventWireTypesSha256": sha256_names(
            variant["wireType"] for variant in surface["sessionEventVariants"]
        ),
        "apiKindCounts": dict(sorted(api_kinds.items())),
        "sessionEventKindCounts": dict(sorted(event_kinds.items())),
        "surfaceSha256": sha256_json(surface),
    }
    schema_sources = {
        "apiSha256": sha256_bytes(api_path.read_bytes()),
        "sessionEventsSha256": sha256_bytes(session_events_path.read_bytes()),
    }
    return {"schemaSources": schema_sources, "coverage": coverage, "surface": surface}


def validate_surface(baseline: dict[str, Any]) -> None:
    coverage = baseline["coverage"]
    surface = baseline["surface"]
    api = surface["api"]
    events = surface["sessionEvents"]
    methods = surface["rpcMethods"]
    variants = surface["sessionEventVariants"]

    checks = {
        "apiDefinitionCount": len(api),
        "sessionEventDefinitionCount": len(events),
        "sharedDefinitionCount": len(
            {entry["name"] for entry in api} & {entry["name"] for entry in events}
        ),
        "uniqueDefinitionCount": len(
            {entry["name"] for entry in api} | {entry["name"] for entry in events}
        ),
        "rpcMethodCount": len(methods),
        "sessionEventVariantCount": len(variants),
        "apiDefinitionNamesSha256": sha256_names(entry["name"] for entry in api),
        "sessionEventDefinitionNamesSha256": sha256_names(
            entry["name"] for entry in events
        ),
        "rpcMethodNamesSha256": sha256_names(methods),
        "sessionEventWireTypesSha256": sha256_names(
            variant["wireType"] for variant in variants
        ),
        "apiKindCounts": dict(sorted(Counter(entry["kind"] for entry in api).items())),
        "sessionEventKindCounts": dict(
            sorted(Counter(entry["kind"] for entry in events).items())
        ),
        "surfaceSha256": sha256_json(surface),
    }
    for key, actual in checks.items():
        if coverage.get(key) != actual:
            raise ValueError(
                f"coverage mismatch for {key}: {actual!r}, expected {coverage.get(key)!r}"
            )

    for group in SCHEMA_GROUPS:
        entries = surface[group]
        names = [entry["name"] for entry in entries]
        if names != sorted(names) or len(names) != len(set(names)):
            raise ValueError(f"{group} definitions are not unique and sorted")
        for entry in entries:
            expected_keys = {
                "name",
                "kind",
                "schemaSha256",
                "propertyCount",
                "requiredPropertyCount",
                "hasOptionalProperties",
                "hasMap",
                "unionVariantCount",
                "knownStringValues",
            }
            if set(entry) != expected_keys:
                raise ValueError(f"{group}.{entry.get('name')} has incomplete metadata")


def render_protocol(baseline: dict[str, Any]) -> str:
    official = baseline["officialSdk"]
    protocol = baseline["protocol"]
    compatibility = baseline["cppCompatibility"]
    return f"""// Generated by tools/generate_protocol_types.py. DO NOT EDIT.

#pragma once

#include <string_view>

namespace copilot::generated
{{

inline constexpr int kSdkProtocolVersion = {protocol["version"]};
inline constexpr int kMinProtocolVersion = {compatibility["minimumProtocolVersion"]};
inline constexpr std::string_view kOfficialSdkCommit = "{official["commit"]}";
inline constexpr std::string_view kOfficialSchemaPackageVersion = "{official["schemaPackageVersion"]}";

}} // namespace copilot::generated
"""


def render_schema_value() -> str:
    return """// Generated by tools/generate_protocol_types.py. DO NOT EDIT.

#pragma once

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace copilot::generated
{

enum class SchemaKind
{
    Alias,
    Array,
    Boolean,
    Enum,
    Integer,
    Map,
    Null,
    Number,
    Object,
    Opaque,
    String,
    Union,
};

namespace detail
{

template <typename Tag>
class SchemaValue
{
public:
    SchemaValue()
        : value_(default_value())
    {
    }

    explicit SchemaValue(nlohmann::json value)
        : value_(std::move(value))
    {
    }

    const nlohmann::json& raw() const noexcept
    {
        return value_;
    }

    nlohmann::json& raw() noexcept
    {
        return value_;
    }

    template <typename T>
    T as() const
    {
        return value_.template get<T>();
    }

    bool is_known_string_value() const
    {
        return value_.is_string() && contains_known(value_.template get_ref<const std::string&>());
    }

    std::optional<std::string> discriminator(std::string_view key = "type") const
    {
        if (!value_.is_object())
            return std::nullopt;
        const auto it = value_.find(std::string(key));
        if (it == value_.end() || !it->is_string())
            return std::nullopt;
        return it->template get<std::string>();
    }

    bool has_known_discriminator(std::string_view key = "type") const
    {
        const auto value = discriminator(key);
        return value && contains_known(*value);
    }

    bool operator==(const SchemaValue&) const = default;

private:
    static nlohmann::json default_value()
    {
        if constexpr (Tag::kind == SchemaKind::Object || Tag::kind == SchemaKind::Map)
            return nlohmann::json::object();
        if constexpr (Tag::kind == SchemaKind::Array)
            return nlohmann::json::array();
        return nullptr;
    }

    static bool contains_known(std::string_view value)
    {
        return std::find(Tag::known_string_values.begin(), Tag::known_string_values.end(), value) !=
               Tag::known_string_values.end();
    }

    nlohmann::json value_;
};

template <typename Tag>
inline void to_json(nlohmann::json& j, const SchemaValue<Tag>& value)
{
    j = value.raw();
}

template <typename Tag>
inline void from_json(const nlohmann::json& j, SchemaValue<Tag>& value)
{
    value = SchemaValue<Tag>(j);
}

} // namespace detail
} // namespace copilot::generated
"""


CPP_KIND = {
    "alias": "Alias",
    "array": "Array",
    "boolean": "Boolean",
    "enum": "Enum",
    "integer": "Integer",
    "map": "Map",
    "null": "Null",
    "number": "Number",
    "object": "Object",
    "opaque": "Opaque",
    "string": "String",
    "union": "Union",
}


def render_tag(entry: dict[str, Any]) -> str:
    values = entry["knownStringValues"]
    value_lines = ", ".join(cpp_string(value) for value in values)
    return f"""struct {entry["name"]}SchemaTag
{{
    inline static constexpr std::string_view schema_name = {cpp_string(entry["name"])};
    inline static constexpr SchemaKind kind = SchemaKind::{CPP_KIND[entry["kind"]]};
    inline static constexpr bool has_optional_properties = {str(entry["hasOptionalProperties"]).lower()};
    inline static constexpr bool has_map = {str(entry["hasMap"]).lower()};
    inline static constexpr std::size_t property_count = {entry["propertyCount"]};
    inline static constexpr std::size_t required_property_count = {entry["requiredPropertyCount"]};
    inline static constexpr std::size_t union_variant_count = {entry["unionVariantCount"]};
    inline static constexpr std::array<std::string_view, {len(values)}> known_string_values{{{value_lines}}};
}};
using {entry["name"]} = detail::SchemaValue<{entry["name"]}SchemaTag>;"""


def render_schema_header(
    entries: list[dict[str, Any]],
    namespace_name: str,
    source_name: str,
    extra: str,
) -> str:
    tags = "\n\n".join(render_tag(entry) for entry in entries)
    return f"""// Generated by tools/generate_protocol_types.py from {source_name}. DO NOT EDIT.

#pragma once

#include <copilot/generated/compatibility_types.hpp>
#include <copilot/generated/schema_coverage.hpp>
#include <copilot/generated/schema_value.hpp>

namespace copilot::generated::{namespace_name}
{{

inline constexpr std::size_t kGeneratedDefinitionCount = {len(entries)};
{extra}

{tags}

}} // namespace copilot::generated::{namespace_name}
"""


def render_string_array(name: str, values: list[str]) -> str:
    lines = ",\n".join(f"    {cpp_string(value)}" for value in values)
    return (
        f"inline constexpr std::array<std::string_view, {len(values)}> {name}{{{{\n"
        f"{lines}\n"
        "}};\n"
    )


def render_coverage(baseline: dict[str, Any]) -> str:
    coverage = baseline["coverage"]
    sources = baseline["schemaSources"]
    return f"""// Generated by tools/generate_protocol_types.py. DO NOT EDIT.

#pragma once

#include <cstddef>
#include <string_view>

namespace copilot::generated::coverage
{{

inline constexpr std::size_t kApiDefinitionCount = {coverage["apiDefinitionCount"]};
inline constexpr std::size_t kSessionEventDefinitionCount = {coverage["sessionEventDefinitionCount"]};
inline constexpr std::size_t kSharedDefinitionCount = {coverage["sharedDefinitionCount"]};
inline constexpr std::size_t kUniqueDefinitionCount = {coverage["uniqueDefinitionCount"]};
inline constexpr std::size_t kRpcMethodCount = {coverage["rpcMethodCount"]};
inline constexpr std::size_t kSessionEventVariantCount = {coverage["sessionEventVariantCount"]};
inline constexpr std::string_view kApiSchemaSha256 = "{sources["apiSha256"]}";
inline constexpr std::string_view kSessionEventsSchemaSha256 = "{sources["sessionEventsSha256"]}";
inline constexpr std::string_view kApiDefinitionNamesSha256 = "{coverage["apiDefinitionNamesSha256"]}";
inline constexpr std::string_view kSessionEventDefinitionNamesSha256 = "{coverage["sessionEventDefinitionNamesSha256"]}";
inline constexpr std::string_view kRpcMethodNamesSha256 = "{coverage["rpcMethodNamesSha256"]}";
inline constexpr std::string_view kSessionEventWireTypesSha256 = "{coverage["sessionEventWireTypesSha256"]}";
inline constexpr std::string_view kSurfaceSha256 = "{coverage["surfaceSha256"]}";

}} // namespace copilot::generated::coverage
"""


def render_compatibility_types() -> str:
    return """// Generated compatibility types retained from the first parity tranche. DO NOT EDIT.

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace copilot::generated
{

struct PingRequest
{
    std::optional<std::string> message{};
    bool operator==(const PingRequest&) const = default;
};

inline void to_json(nlohmann::json& j, const PingRequest& value)
{
    j = nlohmann::json::object();
    if (value.message)
        j["message"] = *value.message;
}

inline void from_json(const nlohmann::json& j, PingRequest& value)
{
    if (j.contains("message") && !j.at("message").is_null())
        value.message = j.at("message").get<std::string>();
    else
        value.message.reset();
}

struct PingResult
{
    std::string message{};
    std::string timestamp{};
    std::int64_t protocol_version{};
    bool operator==(const PingResult&) const = default;
};

inline void to_json(nlohmann::json& j, const PingResult& value)
{
    j = nlohmann::json{
        {"message", value.message},
        {"timestamp", value.timestamp},
        {"protocolVersion", value.protocol_version},
    };
}

inline void from_json(const nlohmann::json& j, PingResult& value)
{
    j.at("message").get_to(value.message);
    j.at("timestamp").get_to(value.timestamp);
    j.at("protocolVersion").get_to(value.protocol_version);
}

struct SessionIdleData
{
    std::optional<bool> aborted{};
    bool operator==(const SessionIdleData&) const = default;
};

inline void to_json(nlohmann::json& j, const SessionIdleData& value)
{
    j = nlohmann::json::object();
    if (value.aborted)
        j["aborted"] = *value.aborted;
}

inline void from_json(const nlohmann::json& j, SessionIdleData& value)
{
    if (j.contains("aborted") && !j.at("aborted").is_null())
        value.aborted = j.at("aborted").get<bool>();
    else
        value.aborted.reset();
}

} // namespace copilot::generated
"""


def rendered_files(baseline: dict[str, Any]) -> dict[str, str]:
    validate_surface(baseline)
    surface = baseline["surface"]
    rpc_extra = render_string_array("kRpcMethods", surface["rpcMethods"])
    event_wire_types = [
        variant["wireType"] for variant in surface["sessionEventVariants"]
    ]
    event_extra = render_string_array("kSessionEventWireTypes", event_wire_types)
    return {
        "protocol_version.hpp": render_protocol(baseline),
        "schema_value.hpp": render_schema_value(),
        "schema_coverage.hpp": render_coverage(baseline),
        "api_types.hpp": render_schema_header(
            surface["api"], "api", "api.schema.json", rpc_extra
        ),
        "session_events.hpp": render_schema_header(
            surface["sessionEvents"],
            "events",
            "session-events.schema.json",
            event_extra,
        ),
        "compatibility_types.hpp": render_compatibility_types(),
    }


def write_or_check(files: dict[str, str], output: Path, check: bool) -> list[str]:
    failures: list[str] = []
    for name, content in files.items():
        path = output / name
        if check:
            actual = path.read_text(encoding="utf-8") if path.exists() else None
            if actual != content:
                try:
                    failures.append(str(path.relative_to(REPO_ROOT)))
                except ValueError:
                    failures.append(str(path))
        else:
            output.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8", newline="\n")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=DEFAULT_BASELINE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--refresh", action="store_true")
    parser.add_argument("--official-sdk-root", type=Path)
    parser.add_argument("--api-schema", type=Path)
    parser.add_argument("--session-events-schema", type=Path)
    args = parser.parse_args()

    baseline = load_json(args.baseline)
    if args.official_sdk_root:
        validate_official_sdk(args.official_sdk_root, baseline)

    if args.refresh:
        if args.check:
            parser.error("--refresh and --check cannot be combined")
        if not args.official_sdk_root or not args.api_schema or not args.session_events_schema:
            parser.error(
                "--refresh requires --official-sdk-root, --api-schema, "
                "and --session-events-schema"
            )
        refreshed = build_surface(args.api_schema, args.session_events_schema, baseline)
        baseline.update(refreshed)
        baseline.pop("selectedDefinitions", None)
        baseline["formatVersion"] = 2
        args.baseline.write_text(
            json.dumps(baseline, indent=2, ensure_ascii=False) + "\n",
            encoding="utf-8",
            newline="\n",
        )

    failures = write_or_check(rendered_files(baseline), args.output, args.check)
    if failures:
        print("Generated protocol files are stale:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        print("Run: python tools/generate_protocol_types.py", file=sys.stderr)
        return 1

    action = "checked" if args.check else "generated"
    coverage = baseline["coverage"]
    print(
        f"Protocol files {action}: "
        f"{coverage['apiDefinitionCount']} API definitions, "
        f"{coverage['sessionEventDefinitionCount']} event definitions, "
        f"{coverage['rpcMethodCount']} RPC methods, "
        f"{coverage['sessionEventVariantCount']} event variants"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
