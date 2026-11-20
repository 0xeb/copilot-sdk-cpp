// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

/// @file session_fs.hpp
/// @brief Client-provided session filesystem (SessionFs) provider API.
///
/// A provider backs one session's files and, optionally, its SQLite database.
/// Providers throw on failure and return values directly; the adapter built by
/// create_session_fs_adapter() converts them into the `sessionFs.*` wire results.
// NODEJS REFERENCE: nodejs/src/sessionFsProvider.ts
// C# REFERENCE: dotnet/src/SessionFsProvider.cs

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace copilot
{

using json = nlohmann::json;

/// File metadata returned by SessionFsProvider::stat. Timestamps are ISO 8601.
struct SessionFsFileInfo
{
    bool is_file = false;
    bool is_directory = false;
    std::int64_t size = 0;
    std::string mtime;
    std::string birthtime;
};

enum class SessionFsEntryType
{
    File,
    Directory,
};

/// One entry returned by SessionFsProvider::readdir_with_types.
struct SessionFsDirEntry
{
    std::string name;
    SessionFsEntryType type = SessionFsEntryType::File;
};

/// Raw bytes and the authoritative total file byte length captured by one read.
struct SessionFsByteRange
{
    /// Requested bytes; boundaries may split UTF-8 characters.
    std::vector<std::uint8_t> content;
    /// Total file byte length, independent of the requested range.
    std::uint64_t byte_length = 0;
};

/// How a SQLite statement executes: `Exec` for DDL/multi-statement, `Query` for
/// SELECT, `Run` for INSERT/UPDATE/DELETE.
enum class SessionFsSqliteQueryType
{
    Exec,
    Query,
    Run,
};

/// Failure classification reported to the runtime for a SQLite transaction.
/// `BusyOrLocked` guarantees rollback; `PostCommitAmbiguous` must never be retried.
enum class SessionFsSqliteTransactionErrorClass
{
    BusyOrLocked,
    Fatal,
    PostCommitAmbiguous,
};

/// Named bind parameters. Values must be strings, numbers, or null.
using SessionFsSqliteParams = std::map<std::string, json>;

/// Result of one SQLite statement.
struct SessionFsSqliteQueryResult
{
    /// For SELECT: one JSON object per row. Empty otherwise.
    std::vector<json> rows;
    std::vector<std::string> columns;
    std::int64_t rows_affected = 0;
    /// SQLite last_insert_rowid() for INSERT.
    std::optional<std::int64_t> last_insert_rowid;
};

/// One statement in an atomic transaction passed to SessionFsSqliteProvider::transaction.
struct SessionFsSqliteStatement
{
    SessionFsSqliteQueryType query_type = SessionFsSqliteQueryType::Exec;
    std::string query;
    std::optional<SessionFsSqliteParams> params;
};

/// Thrown by SessionFsSqliteProvider::transaction to classify a failure. Any other
/// exception is reported as `Fatal`. Throw with `BusyOrLocked` when SQLite reported
/// BUSY/LOCKED before commit and the batch was rolled back, so the call is retryable.
class SessionFsSqliteTransactionFailure : public std::runtime_error
{
  public:
    explicit SessionFsSqliteTransactionFailure(
        const std::string& message,
        SessionFsSqliteTransactionErrorClass error_class = SessionFsSqliteTransactionErrorClass::Fatal
    )
        : std::runtime_error(message), error_class_(error_class)
    {
    }

    SessionFsSqliteTransactionErrorClass error_class() const noexcept
    {
        return error_class_;
    }

  private:
    SessionFsSqliteTransactionErrorClass error_class_;
};

/// Throw from SessionFsProvider::write_file only when a failed write changed the target.
class SessionFsWriteFailure : public std::runtime_error
{
  public:
    using std::runtime_error::runtime_error;
};

/// Per-session SQLite database operations.
class SessionFsSqliteProvider
{
  public:
    virtual ~SessionFsSqliteProvider() = default;

    /// Execute a query against the per-session database. Returning nullopt reports an
    /// empty result. Errors propagate to the runtime as JSON-RPC errors.
    virtual std::optional<SessionFsSqliteQueryResult> query(
        SessionFsSqliteQueryType query_type,
        const std::string& query,
        const std::optional<SessionFsSqliteParams>& params
    ) = 0;

    /// Whether the per-session database already exists, without creating it.
    virtual bool exists() = 0;

    /// Override together with transaction() to support atomic batches.
    virtual bool supports_transactions() const
    {
        return false;
    }

    /// Execute `statements` atomically: apply busy handling to every statement and roll
    /// back the whole batch if any fails. Returns one result per statement, in order.
    virtual std::vector<SessionFsSqliteQueryResult>
    transaction(const std::vector<SessionFsSqliteStatement>& statements)
    {
        (void)statements;
        throw std::logic_error("SQLite transactions are not supported by this provider");
    }
};

/// Session filesystem provider. Throw on error; a std::system_error (including
/// std::filesystem::filesystem_error) whose code is `no_such_file_or_directory`
/// maps to ENOENT, everything else to UNKNOWN.
///
/// Optional features are advertised by overriding the matching `supports_*`
/// predicate (or sqlite()) alongside the operation; SessionFsConfig::capabilities
/// may only declare features the provider implements.
class SessionFsProvider
{
  public:
    virtual ~SessionFsProvider() = default;

    /// Full content of a file. Throw if it does not exist.
    virtual std::string read_file(const std::string& path) = 0;
    /// Write a file, creating parent directories if needed. Throw SessionFsWriteFailure
    /// if a failed write changed the target.
    virtual void
    write_file(const std::string& path, const std::string& content, std::optional<int> mode) = 0;
    /// Append to a file, creating parent directories if needed.
    virtual void
    append_file(const std::string& path, const std::string& content, std::optional<int> mode) = 0;
    virtual bool exists(const std::string& path) = 0;
    /// Metadata about a file or directory. Throw if it does not exist.
    virtual SessionFsFileInfo stat(const std::string& path) = 0;
    /// Create a directory; when `recursive`, create parents as needed.
    virtual void mkdir(const std::string& path, bool recursive, std::optional<int> mode) = 0;
    /// Entry names in a directory. Throw if it does not exist.
    virtual std::vector<std::string> readdir(const std::string& path) = 0;
    /// Entries with type info. Throw if the directory does not exist.
    virtual std::vector<SessionFsDirEntry> readdir_with_types(const std::string& path) = 0;
    /// Remove a file or directory. When `force`, do not throw on ENOENT.
    virtual void rm(const std::string& path, bool recursive, bool force) = 0;
    virtual void rename(const std::string& src, const std::string& dest) = 0;

    /// Override together with read_file_bytes() and write_file_bytes() to support
    /// exact binary reads and writes (`capabilities.binary`).
    virtual bool supports_binary() const
    {
        return false;
    }
    virtual std::vector<std::uint8_t> read_file_bytes(const std::string& path)
    {
        (void)path;
        throw std::logic_error("Binary reads are not supported");
    }
    virtual void write_file_bytes(
        const std::string& path, const std::vector<std::uint8_t>& content, std::optional<int> mode
    )
    {
        (void)path;
        (void)content;
        (void)mode;
        throw std::logic_error("Binary writes are not supported");
    }

    /// Override together with read_file_range() to support bounded byte-range reads
    /// (`capabilities.readFileRange`), independently of binary support.
    virtual bool supports_read_file_range() const
    {
        return false;
    }
    /// Read raw bytes starting at `offset`. A missing `length` reads through EOF; zero
    /// returns only the byte length. At or beyond EOF, content is empty.
    virtual SessionFsByteRange read_file_range(
        const std::string& path, std::uint64_t offset, std::optional<std::uint64_t> length
    )
    {
        (void)path;
        (void)offset;
        (void)length;
        throw std::logic_error("Byte-range reads are not supported by this SessionFs provider");
    }

    /// Per-session SQLite database, or nullptr when SQLite is not supported.
    virtual std::shared_ptr<SessionFsSqliteProvider> sqlite()
    {
        return nullptr;
    }
};

/// Dispatches one `sessionFs.*` request (method name + params) to a provider and
/// returns the wire result.
using SessionFsHandler = std::function<json(const std::string& method, const json& params)>;

/// Wrap a provider into the handler the SDK routes `sessionFs.*` requests to,
/// converting thrown filesystem errors into SessionFsError results. Unknown methods
/// raise a JSON-RPC MethodNotFound error.
// NODEJS REFERENCE: nodejs/src/sessionFsProvider.ts createSessionFsAdapter
SessionFsHandler create_session_fs_adapter(std::shared_ptr<SessionFsProvider> provider);

namespace detail
{
/// Whether `method` is one of the per-session `sessionFs.*` requests the runtime sends.
bool is_session_fs_request_method(const std::string& method);
} // namespace detail

} // namespace copilot
