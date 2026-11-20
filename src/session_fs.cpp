// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/jsonrpc.hpp>
#include <copilot/session_fs.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <system_error>

namespace copilot
{

namespace
{

// NODEJS REFERENCE: nodejs/src/sessionFsProvider.ts MAX_BINARY_BYTES / MAX_BINARY_CONTENT_LENGTH
constexpr std::uint64_t kMaxBinaryBytes = ((64ULL * 1024 * 1024 - 1024) / 4) * 3;
constexpr std::uint64_t kMaxBinaryContentLength = ((kMaxBinaryBytes + 2) / 3) * 4;
constexpr std::uint64_t kMaxSafeInteger = 9007199254740991ULL;

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::vector<std::uint8_t>& bytes)
{
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3)
    {
        const std::uint32_t n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
        out.push_back(kBase64Alphabet[(n >> 18) & 63]);
        out.push_back(kBase64Alphabet[(n >> 12) & 63]);
        out.push_back(kBase64Alphabet[(n >> 6) & 63]);
        out.push_back(kBase64Alphabet[n & 63]);
    }
    if (i + 1 == bytes.size())
    {
        const std::uint32_t n = bytes[i] << 16;
        out.push_back(kBase64Alphabet[(n >> 18) & 63]);
        out.push_back(kBase64Alphabet[(n >> 12) & 63]);
        out += "==";
    }
    else if (i + 2 == bytes.size())
    {
        const std::uint32_t n = (bytes[i] << 16) | (bytes[i + 1] << 8);
        out.push_back(kBase64Alphabet[(n >> 18) & 63]);
        out.push_back(kBase64Alphabet[(n >> 12) & 63]);
        out.push_back(kBase64Alphabet[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

// Lenient decode (like Node's Buffer.from(s, "base64")); callers re-encode and
// compare to reject anything that is not canonical standard base64.
std::vector<std::uint8_t> base64_decode_lenient(const std::string& text)
{
    std::array<int, 256> table{};
    table.fill(-1);
    for (int i = 0; i < 64; ++i)
        table[static_cast<unsigned char>(kBase64Alphabet[i])] = i;
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text)
    {
        if (c == '=')
            break;
        const int value = table[static_cast<unsigned char>(c)];
        if (value < 0)
            continue;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::optional<std::uint64_t> safe_non_negative_integer(const json& value)
{
    if (value.is_number_unsigned())
    {
        const auto v = value.get<std::uint64_t>();
        if (v <= kMaxSafeInteger)
            return v;
    }
    else if (value.is_number_integer())
    {
        const auto v = value.get<std::int64_t>();
        if (v >= 0 && static_cast<std::uint64_t>(v) <= kMaxSafeInteger)
            return static_cast<std::uint64_t>(v);
    }
    return std::nullopt;
}

std::string now_iso8601()
{
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char buffer[32];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
        utc.tm_year + 1900,
        utc.tm_mon + 1,
        utc.tm_mday,
        utc.tm_hour,
        utc.tm_min,
        utc.tm_sec,
        static_cast<int>(millis)
    );
    return buffer;
}

// NODEJS REFERENCE: nodejs/src/sessionFsProvider.ts toSessionFsError
json to_session_fs_error(const std::exception& error)
{
    bool enoent = false;
    if (const auto* system = dynamic_cast<const std::system_error*>(&error))
        enoent = system->code() == std::errc::no_such_file_or_directory;
    return json{{"code", enoent ? "ENOENT" : "UNKNOWN"}, {"message", error.what()}};
}

json unknown_error(const std::string& message)
{
    return json{{"code", "UNKNOWN"}, {"message", message}};
}

std::optional<int> optional_mode(const json& params)
{
    const auto it = params.find("mode");
    if (it == params.end() || it->is_null())
        return std::nullopt;
    return it->get<int>();
}

bool optional_flag(const json& params, const char* key)
{
    const auto it = params.find(key);
    return it != params.end() && it->is_boolean() && it->get<bool>();
}

SessionFsSqliteQueryType parse_query_type(const json& value)
{
    const auto text = value.get<std::string>();
    if (text == "exec")
        return SessionFsSqliteQueryType::Exec;
    if (text == "query")
        return SessionFsSqliteQueryType::Query;
    if (text == "run")
        return SessionFsSqliteQueryType::Run;
    throw std::invalid_argument("Unknown SQLite queryType: " + text);
}

// NODEJS REFERENCE: nodejs/src/sessionFsProvider.ts normalizeSqliteParams
std::optional<SessionFsSqliteParams> normalize_sqlite_params(const json& params, const char* key)
{
    const auto it = params.find(key);
    if (it == params.end() || it->is_null())
        return std::nullopt;
    SessionFsSqliteParams normalized;
    for (const auto& [name, value] : it->items())
        normalized[name] = value;
    return normalized;
}

json to_wire(const SessionFsSqliteQueryResult& result)
{
    json wire{
        {"rows", result.rows},
        {"columns", result.columns},
        {"rowsAffected", result.rows_affected},
    };
    if (result.last_insert_rowid)
        wire["lastInsertRowid"] = *result.last_insert_rowid;
    return wire;
}

const char* to_wire(SessionFsSqliteTransactionErrorClass error_class)
{
    switch (error_class)
    {
    case SessionFsSqliteTransactionErrorClass::BusyOrLocked:
        return "busyOrLocked";
    case SessionFsSqliteTransactionErrorClass::PostCommitAmbiguous:
        return "postCommitAmbiguous";
    case SessionFsSqliteTransactionErrorClass::Fatal:
    default:
        return "fatal";
    }
}

json read_file_range(SessionFsProvider& provider, const json& params)
{
    try
    {
        const auto path = params.at("path").get<std::string>();
        const auto offset = safe_non_negative_integer(params.value("offset", json()));
        std::optional<std::uint64_t> length;
        bool length_ok = true;
        if (const auto it = params.find("length"); it != params.end() && !it->is_null())
        {
            length = safe_non_negative_integer(*it);
            length_ok = length.has_value();
        }
        if (!offset || !length_ok)
            throw std::out_of_range(
                "SessionFs byte offset and length must be non-negative safe integers"
            );
        if (!provider.supports_read_file_range())
            throw std::runtime_error("Byte-range reads are not supported by this SessionFs provider");
        const auto result = provider.read_file_range(path, *offset, length);
        if (result.byte_length > kMaxSafeInteger)
            throw std::runtime_error("SessionFs byte length must be a non-negative safe integer");
        const std::uint64_t available =
            result.byte_length > *offset ? result.byte_length - *offset : 0;
        const std::uint64_t expected = length ? std::min(*length, available) : available;
        if (result.content.size() != expected)
            throw std::runtime_error("SessionFs returned an incorrect byte range length");
        return json{{"content", base64_encode(result.content)}, {"byteLength", result.byte_length}};
    }
    catch (const std::exception& e)
    {
        return json{{"content", ""}, {"byteLength", 0}, {"error", to_session_fs_error(e)}};
    }
}

json read_file_bytes(SessionFsProvider& provider, const json& params)
{
    if (!provider.supports_binary())
        return json{{"content", ""}, {"error", unknown_error("Binary reads are not supported")}};
    try
    {
        const auto bytes = provider.read_file_bytes(params.at("path").get<std::string>());
        if (bytes.size() > kMaxBinaryBytes)
            return json{
                {"content", ""},
                {"error",
                 unknown_error("sessionFs.readFileBytes content exceeds the binary read limit")},
            };
        return json{{"content", base64_encode(bytes)}};
    }
    catch (const std::exception& e)
    {
        return json{{"content", ""}, {"error", to_session_fs_error(e)}};
    }
}

json write_file_bytes(SessionFsProvider& provider, const json& params)
{
    if (!provider.supports_binary())
        return unknown_error("Binary writes are not supported");
    const auto content = params.at("content").get<std::string>();
    if (content.size() > kMaxBinaryContentLength)
        return unknown_error("sessionFs.writeFileBytes content exceeds the binary write limit");
    const auto bytes = base64_decode_lenient(content);
    if (base64_encode(bytes) != content || bytes.size() > kMaxBinaryBytes)
        return unknown_error("invalid sessionFs.writeFileBytes base64 content");
    try
    {
        provider.write_file_bytes(params.at("path").get<std::string>(), bytes, optional_mode(params));
        return nullptr;
    }
    catch (const std::exception& e)
    {
        return to_session_fs_error(e);
    }
}

json sqlite_transaction(SessionFsProvider& provider, const json& params)
{
    const auto sqlite = provider.sqlite();
    if (!sqlite || !sqlite->supports_transactions())
        return json{
            {"results", json::array()},
            {"error",
             {{"errorClass", "fatal"},
              {"message", "SQLite transactions are not supported by this provider"}}},
        };
    try
    {
        std::vector<SessionFsSqliteStatement> statements;
        for (const auto& statement : params.at("statements"))
            statements.push_back(SessionFsSqliteStatement{
                .query_type = parse_query_type(statement.at("queryType")),
                .query = statement.at("query").get<std::string>(),
                .params = normalize_sqlite_params(statement, "params"),
            });
        json results = json::array();
        for (const auto& result : sqlite->transaction(statements))
            results.push_back(to_wire(result));
        return json{{"results", std::move(results)}};
    }
    catch (const SessionFsSqliteTransactionFailure& e)
    {
        // Transaction failures carry a retry classification, so they are reported as a
        // result-level error instead of a JSON-RPC error.
        return json{
            {"results", json::array()},
            {"error", {{"errorClass", to_wire(e.error_class())}, {"message", e.what()}}},
        };
    }
    catch (const std::exception& e)
    {
        return json{
            {"results", json::array()},
            {"error", {{"errorClass", "fatal"}, {"message", e.what()}}},
        };
    }
}

std::shared_ptr<SessionFsSqliteProvider> require_sqlite(SessionFsProvider& provider)
{
    auto sqlite = provider.sqlite();
    if (!sqlite)
        throw std::runtime_error("SQLite is not supported by this provider");
    return sqlite;
}

json dispatch(SessionFsProvider& provider, const std::string& method, const json& params)
{
    // Methods returning `undefined` upstream reply with a JSON null result.
    const auto void_call = [](auto&& fn) -> json
    {
        try
        {
            fn();
            return nullptr;
        }
        catch (const std::exception& e)
        {
            return to_session_fs_error(e);
        }
    };
    const auto path = [&params] { return params.at("path").get<std::string>(); };

    if (method == "sessionFs.readFileRange")
        return read_file_range(provider, params);
    if (method == "sessionFs.readFile")
    {
        try
        {
            return json{{"content", provider.read_file(path())}};
        }
        catch (const std::exception& e)
        {
            return json{{"content", ""}, {"error", to_session_fs_error(e)}};
        }
    }
    if (method == "sessionFs.readFileBytes")
        return read_file_bytes(provider, params);
    if (method == "sessionFs.writeFile")
    {
        try
        {
            provider.write_file(path(), params.at("content").get<std::string>(), optional_mode(params));
            return nullptr;
        }
        catch (const SessionFsWriteFailure& e)
        {
            auto error = to_session_fs_error(e);
            error["writeChanged"] = true;
            return error;
        }
        catch (const std::exception& e)
        {
            return to_session_fs_error(e);
        }
    }
    if (method == "sessionFs.writeFileBytes")
        return write_file_bytes(provider, params);
    if (method == "sessionFs.appendFile")
        return void_call(
            [&]
            {
                provider.append_file(
                    path(), params.at("content").get<std::string>(), optional_mode(params)
                );
            }
        );
    if (method == "sessionFs.exists")
    {
        try
        {
            return json{{"exists", provider.exists(path())}};
        }
        catch (...)
        {
            return json{{"exists", false}};
        }
    }
    if (method == "sessionFs.stat")
    {
        try
        {
            const auto info = provider.stat(path());
            return json{
                {"isFile", info.is_file},
                {"isDirectory", info.is_directory},
                {"size", info.size},
                {"mtime", info.mtime},
                {"birthtime", info.birthtime},
            };
        }
        catch (const std::exception& e)
        {
            const auto now = now_iso8601();
            return json{
                {"isFile", false},
                {"isDirectory", false},
                {"size", 0},
                {"mtime", now},
                {"birthtime", now},
                {"error", to_session_fs_error(e)},
            };
        }
    }
    if (method == "sessionFs.mkdir")
        return void_call(
            [&] { provider.mkdir(path(), optional_flag(params, "recursive"), optional_mode(params)); }
        );
    if (method == "sessionFs.readdir")
    {
        try
        {
            return json{{"entries", provider.readdir(path())}};
        }
        catch (const std::exception& e)
        {
            return json{{"entries", json::array()}, {"error", to_session_fs_error(e)}};
        }
    }
    if (method == "sessionFs.readdirWithTypes")
    {
        try
        {
            json entries = json::array();
            for (const auto& entry : provider.readdir_with_types(path()))
                entries.push_back(
                    {{"name", entry.name},
                     {"type", entry.type == SessionFsEntryType::Directory ? "directory" : "file"}}
                );
            return json{{"entries", std::move(entries)}};
        }
        catch (const std::exception& e)
        {
            return json{{"entries", json::array()}, {"error", to_session_fs_error(e)}};
        }
    }
    if (method == "sessionFs.rm")
        return void_call(
            [&]
            {
                provider.rm(
                    path(), optional_flag(params, "recursive"), optional_flag(params, "force")
                );
            }
        );
    if (method == "sessionFs.rename")
        return void_call(
            [&]
            {
                provider.rename(
                    params.at("src").get<std::string>(), params.at("dest").get<std::string>()
                );
            }
        );
    // SQLite query/exists errors propagate as JSON-RPC errors so the original message
    // reaches the runtime; errno mapping is meaningless for SQL failures.
    if (method == "sessionFs.sqliteQuery")
    {
        const auto result = require_sqlite(provider)->query(
            parse_query_type(params.at("queryType")),
            params.at("query").get<std::string>(),
            normalize_sqlite_params(params, "params")
        );
        return to_wire(result.value_or(SessionFsSqliteQueryResult{}));
    }
    if (method == "sessionFs.sqliteTransaction")
        return sqlite_transaction(provider, params);
    if (method == "sessionFs.sqliteExists")
        return json{{"exists", require_sqlite(provider)->exists()}};
    throw JsonRpcError(JsonRpcErrorCode::MethodNotFound, "Unknown method: " + method);
}

} // namespace

SessionFsHandler create_session_fs_adapter(std::shared_ptr<SessionFsProvider> provider)
{
    if (!provider)
        throw std::invalid_argument("SessionFs provider must not be null");
    return [provider = std::move(provider)](const std::string& method, const json& params)
    { return dispatch(*provider, method, params); };
}

namespace detail
{
bool is_session_fs_request_method(const std::string& method)
{
    static constexpr std::array<const char*, 16> kMethods{
        "sessionFs.readFile",
        "sessionFs.readFileBytes",
        "sessionFs.readFileRange",
        "sessionFs.writeFile",
        "sessionFs.writeFileBytes",
        "sessionFs.appendFile",
        "sessionFs.exists",
        "sessionFs.stat",
        "sessionFs.mkdir",
        "sessionFs.readdir",
        "sessionFs.readdirWithTypes",
        "sessionFs.rm",
        "sessionFs.rename",
        "sessionFs.sqliteQuery",
        "sessionFs.sqliteTransaction",
        "sessionFs.sqliteExists",
    };
    return std::find(kMethods.begin(), kMethods.end(), method) != kMethods.end();
}
} // namespace detail

} // namespace copilot
