// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT
//
// Process management code borrowed from claude-agent-sdk-cpp:
// https://github.com/anthropics/claude-code-sdk-cpp
// See: src/internal/subprocess/process.hpp

#pragma once

/// @file process.hpp
/// @brief Cross-platform process management for Copilot CLI

#include <map>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace copilot
{

// Forward declarations for platform-specific types
struct ProcessHandle;
struct PipeHandle;

// =============================================================================
// Error Types
// =============================================================================

/// Exception thrown when process operations fail
class ProcessError : public std::runtime_error
{
  public:
    explicit ProcessError(const std::string& message) : std::runtime_error(message) {}
};

// =============================================================================
// ReadPipe - Read from subprocess stdout/stderr
// =============================================================================

/// Pipe for reading output from a subprocess
class ReadPipe
{
  public:
    ReadPipe();
    ~ReadPipe();

    // Move-only
    ReadPipe(const ReadPipe&) = delete;
    ReadPipe& operator=(const ReadPipe&) = delete;
    ReadPipe(ReadPipe&&) noexcept;
    ReadPipe& operator=(ReadPipe&&) noexcept;

    /// Read up to size bytes into buffer
    /// @return Number of bytes read, 0 on EOF
    /// @throws ProcessError on read failure
    size_t read(char* buffer, size_t size);

    /// Read a line (up to newline or max_size)
    /// @return Line including newline, or partial line on EOF
    std::string read_line(size_t max_size = 4096);

    /// Check if data is available without blocking
    /// @param timeout_ms Timeout in milliseconds (0 = non-blocking check)
    /// @return true if data is available
    bool has_data(int timeout_ms = 0);

    /// Close the pipe
    void close();

    /// Check if pipe is open
    bool is_open() const;

  private:
    friend class Process;
    std::unique_ptr<PipeHandle> handle_;
};

// =============================================================================
// WritePipe - Write to subprocess stdin
// =============================================================================

/// Pipe for writing input to a subprocess
class WritePipe
{
  public:
    WritePipe();
    ~WritePipe();

    // Move-only
    WritePipe(const WritePipe&) = delete;
    WritePipe& operator=(const WritePipe&) = delete;
    WritePipe(WritePipe&&) noexcept;
    WritePipe& operator=(WritePipe&&) noexcept;

    /// Write data to the pipe
    /// @return Number of bytes written
    /// @throws ProcessError on write failure
    size_t write(const char* data, size_t size);

    /// Write string to the pipe
    size_t write(const std::string& data);

    /// Flush write buffer (no-op on most platforms but included for consistency)
    void flush();

    /// Close the pipe
    void close();

    /// Check if pipe is open
    bool is_open() const;

  private:
    friend class Process;
    std::unique_ptr<PipeHandle> handle_;
};

// =============================================================================
// ProcessOptions - Configuration for process spawning
// =============================================================================

/// Options for spawning a subprocess
struct ProcessOptions
{
    /// Working directory for the subprocess (empty = inherit from parent)
    std::string working_directory;

    /// Environment variables to set
    std::map<std::string, std::string> environment;

    /// Whether to inherit the parent's environment variables
    bool inherit_environment = true;

    /// Whether to redirect stdin (pipe to subprocess)
    bool redirect_stdin = true;

    /// Whether to redirect stdout (pipe from subprocess)
    bool redirect_stdout = true;

    /// Whether to redirect stderr (pipe from subprocess)
    bool redirect_stderr = false;

    /// On Windows: whether to create the process in a new console window
    bool create_no_window = true;
};

// =============================================================================
// Process - Cross-platform subprocess management
// =============================================================================

/// Cross-platform subprocess management
///
/// Example usage:
/// @code
/// Process proc;
/// proc.spawn("copilot", {"--server", "--stdio"});
///
/// proc.stdin_pipe().write("Hello\n");
/// std::string line = proc.stdout_pipe().read_line();
///
/// proc.terminate();
/// int exit_code = proc.wait();
/// @endcode
class Process
{
  public:
    Process();
    ~Process();

    // Move-only
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&) noexcept;
    Process& operator=(Process&&) noexcept;

    /// Spawn a new process
    /// @param executable Path to executable (can be relative or absolute)
    /// @param args Command line arguments (not including executable)
    /// @param options Process configuration
    /// @throws ProcessError if spawn fails
    void spawn(
        const std::string& executable,
        const std::vector<std::string>& args,
        const ProcessOptions& options = {}
    );

    /// Get stdin pipe (only valid if redirect_stdin was true)
    /// @throws ProcessError if stdin was not redirected
    WritePipe& stdin_pipe();

    /// Get stdout pipe (only valid if redirect_stdout was true)
    /// @throws ProcessError if stdout was not redirected
    ReadPipe& stdout_pipe();

    /// Get stderr pipe (only valid if redirect_stderr was true)
    /// @throws ProcessError if stderr was not redirected
    ReadPipe& stderr_pipe();

    /// Check if process is still running
    bool is_running() const;

    /// Non-blocking wait for process termination
    /// @return Exit code if process has terminated, std::nullopt if still running
    std::optional<int> try_wait();

    /// Blocking wait for process termination
    /// @return Exit code
    int wait();

    /// Wait up to a bounded duration for process termination.
    /// @return Exit code, or nullopt when the timeout expires.
    std::optional<int> wait_for(std::chrono::milliseconds timeout);

    /// Request graceful termination (SIGTERM on POSIX, close handles on Windows)
    void terminate();

    /// Forcefully kill the process (SIGKILL on POSIX, TerminateProcess on Windows)
    void kill();

    /// Close redirected pipe handles to unblock transport readers.
    void close_pipes();

    /// Get process ID
    /// @return Process ID, or 0 if not spawned
    int pid() const;

  private:
    std::unique_ptr<ProcessHandle> handle_;
    std::unique_ptr<WritePipe> stdin_;
    std::unique_ptr<ReadPipe> stdout_;
    std::unique_ptr<ReadPipe> stderr_;
};

// =============================================================================
// Utility Functions
// =============================================================================

/// Find an executable in the system PATH
/// @param name Executable name (without extension on POSIX, with or without on Windows)
/// @return Full path to executable, or std::nullopt if not found
std::optional<std::string> find_executable(const std::string& name);

/// Check if a path looks like a Node.js script
/// @param path Path to check
/// @return true if path ends with .js or .mjs
bool is_node_script(const std::string& path);

/// Get the system's Node.js executable path
/// @return Path to node executable, or std::nullopt if not found
std::optional<std::string> find_node();

} // namespace copilot
