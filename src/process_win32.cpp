// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT
//
// Process management code borrowed from claude-agent-sdk-cpp:
// https://github.com/0xeb/claude-agent-sdk-cpp
// See: src/internal/subprocess/process_win32.cpp

#ifdef _WIN32

#include <copilot/process.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <windows.h>

namespace copilot
{

// =============================================================================
// Platform-specific handle structures
// =============================================================================

struct PipeHandle
{
    HANDLE handle = INVALID_HANDLE_VALUE;

    ~PipeHandle()
    {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
};

struct ProcessHandle
{
    HANDLE process_handle = INVALID_HANDLE_VALUE;
    HANDLE thread_handle = INVALID_HANDLE_VALUE;
    DWORD process_id = 0;
    bool running = false;
    int exit_code = -1;

    ~ProcessHandle()
    {
        if (thread_handle != INVALID_HANDLE_VALUE)
            CloseHandle(thread_handle);
        if (process_handle != INVALID_HANDLE_VALUE)
            CloseHandle(process_handle);
    }
};
// =============================================================================
// Job Object for child process cleanup
// =============================================================================

// Singleton job object that kills all child processes when parent exits.
// Using JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE ensures spawned servers die
// when the client process terminates (even abnormally).
static HANDLE get_child_process_job()
{
    static HANDLE job = []() -> HANDLE {
        HANDLE h = CreateJobObjectA(nullptr, nullptr);
        if (h)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(h, JobObjectExtendedLimitInformation, &info, sizeof(info));
        }
        return h;
    }();
    return job;
}

// =============================================================================
// Helper functions
// =============================================================================

static std::string get_last_error_message()
{
    DWORD error = GetLastError();
    if (error == 0)
        return "No error";

    LPSTR buffer = nullptr;
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr
    );

    std::string message(buffer, size);
    LocalFree(buffer);

    // Remove trailing newline
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();

    return message;
}

static std::string quote_argument(const std::string& arg)
{
    // Check if quoting is needed
    bool needs_quotes = arg.empty();
    if (!needs_quotes)
    {
        for (char c : arg)
        {
            if (c == ' ' || c == '\t' || c == '"' || c == '&' || c == '|' || c == '<' || c == '>' ||
                c == '^' || c == '%' || c == '!' || c == '(' || c == ')' || c == '{' || c == '}' ||
                c == '[' || c == ']' || c == ';' || c == ',' || c == '=')
            {
                needs_quotes = true;
                break;
            }
        }
    }

    if (!needs_quotes)
        return arg;

    // Quote and escape backslashes before quotes
    std::string result = "\"";
    for (size_t i = 0; i < arg.size(); ++i)
    {
        if (arg[i] == '"')
        {
            result += "\\\"";
        }
        else if (arg[i] == '\\')
        {
            // Count consecutive backslashes
            size_t num_backslashes = 1;
            while (i + num_backslashes < arg.size() && arg[i + num_backslashes] == '\\')
                ++num_backslashes;
            // Double backslashes if followed by quote or end of string
            if (i + num_backslashes == arg.size() || arg[i + num_backslashes] == '"')
                result.append(num_backslashes * 2, '\\');
            else
                result.append(num_backslashes, '\\');
            i += num_backslashes - 1;
        }
        else
        {
            result += arg[i];
        }
    }
    result += "\"";
    return result;
}

static std::string
build_command_line(const std::string& executable, const std::vector<std::string>& args)
{
    std::string cmdline = quote_argument(executable);
    for (const auto& arg : args)
        cmdline += " " + quote_argument(arg);
    return cmdline;
}

static std::string
resolve_executable_for_spawn(const std::string& executable, const ProcessOptions& options)
{
    std::filesystem::path exe_path(executable);
    if (exe_path.is_absolute())
        return exe_path.string();

    if (exe_path.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::path base_dir = options.working_directory.empty()
            ? std::filesystem::current_path(ec)
            : std::filesystem::path(options.working_directory);
        if (ec)
            return executable;

        std::filesystem::path candidate = (base_dir / exe_path).lexically_normal();
        if (std::filesystem::exists(candidate, ec) && !ec)
            return candidate.string();
        return executable;
    }

    if (auto found = find_executable(executable))
        return *found;

    return executable;
}

// =============================================================================
// ReadPipe implementation
// =============================================================================

ReadPipe::ReadPipe() : handle_(std::make_unique<PipeHandle>()) {}

ReadPipe::~ReadPipe()
{
    close();
}

ReadPipe::ReadPipe(ReadPipe&&) noexcept = default;
ReadPipe& ReadPipe::operator=(ReadPipe&&) noexcept = default;

size_t ReadPipe::read(char* buffer, size_t size)
{
    if (!is_open())
        throw ProcessError("Pipe is not open");

    DWORD bytes_read = 0;
    BOOL success =
        ReadFile(handle_->handle, buffer, static_cast<DWORD>(size), &bytes_read, nullptr);

    if (!success)
    {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA)
            return 0; // EOF
        throw ProcessError("Read failed: " + get_last_error_message());
    }

    return bytes_read;
}

std::string ReadPipe::read_line(size_t max_size)
{
    std::string line;
    line.reserve(256);

    char ch;
    while (line.size() < max_size)
    {
        size_t bytes_read = read(&ch, 1);
        if (bytes_read == 0)
            break; // EOF
        line.push_back(ch);
        if (ch == '\n')
            break;
    }

    return line;
}

bool ReadPipe::has_data(int timeout_ms)
{
    if (!is_open())
        return false;

    DWORD bytes_available = 0;
    if (PeekNamedPipe(handle_->handle, nullptr, 0, nullptr, &bytes_available, nullptr))
    {
        if (bytes_available > 0)
            return true;
    }

    if (timeout_ms > 0)
    {
        // Simple polling implementation for timeout
        int remaining = timeout_ms;
        const int poll_interval = 10;
        while (remaining > 0)
        {
            Sleep(poll_interval);
            remaining -= poll_interval;
            if (PeekNamedPipe(handle_->handle, nullptr, 0, nullptr, &bytes_available, nullptr))
            {
                if (bytes_available > 0)
                    return true;
            }
        }
    }

    return false;
}

void ReadPipe::close()
{
    if (handle_ && handle_->handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle_->handle);
        handle_->handle = INVALID_HANDLE_VALUE;
    }
}

bool ReadPipe::is_open() const
{
    return handle_ && handle_->handle != INVALID_HANDLE_VALUE;
}

// =============================================================================
// WritePipe implementation
// =============================================================================

WritePipe::WritePipe() : handle_(std::make_unique<PipeHandle>()) {}

WritePipe::~WritePipe()
{
    close();
}

WritePipe::WritePipe(WritePipe&&) noexcept = default;
WritePipe& WritePipe::operator=(WritePipe&&) noexcept = default;

size_t WritePipe::write(const char* data, size_t size)
{
    if (!is_open())
        throw ProcessError("Pipe is not open");

    DWORD bytes_written = 0;
    BOOL success =
        WriteFile(handle_->handle, data, static_cast<DWORD>(size), &bytes_written, nullptr);

    if (!success)
    {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_NO_DATA)
            throw ProcessError("Pipe closed by subprocess");
        throw ProcessError("Write failed: " + get_last_error_message());
    }

    return bytes_written;
}

size_t WritePipe::write(const std::string& data)
{
    return write(data.data(), data.size());
}

void WritePipe::flush()
{
    if (is_open())
        FlushFileBuffers(handle_->handle);
}

void WritePipe::close()
{
    if (handle_ && handle_->handle != INVALID_HANDLE_VALUE)
    {
        CloseHandle(handle_->handle);
        handle_->handle = INVALID_HANDLE_VALUE;
    }
}

bool WritePipe::is_open() const
{
    return handle_ && handle_->handle != INVALID_HANDLE_VALUE;
}

// =============================================================================
// Process implementation
// =============================================================================

Process::Process()
    : handle_(std::make_unique<ProcessHandle>()), stdin_(std::make_unique<WritePipe>()),
      stdout_(std::make_unique<ReadPipe>()), stderr_(std::make_unique<ReadPipe>())
{
}

Process::~Process()
{
    // Close pipes first
    if (stdin_)
        stdin_->close();
    if (stdout_)
        stdout_->close();
    if (stderr_)
        stderr_->close();

    // Then terminate if still running
    if (is_running())
    {
        kill();
        wait();
    }
}

Process::Process(Process&&) noexcept = default;
Process& Process::operator=(Process&&) noexcept = default;

void Process::spawn(
    const std::string& executable,
    const std::vector<std::string>& args,
    const ProcessOptions& options
)
{
    // Create pipes
    HANDLE stdin_read = INVALID_HANDLE_VALUE;
    HANDLE stdin_write = INVALID_HANDLE_VALUE;
    HANDLE stdout_read = INVALID_HANDLE_VALUE;
    HANDLE stdout_write = INVALID_HANDLE_VALUE;
    HANDLE stderr_read = INVALID_HANDLE_VALUE;
    HANDLE stderr_write = INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    if (options.redirect_stdin)
    {
        if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0))
            throw ProcessError("Failed to create stdin pipe: " + get_last_error_message());
        // Prevent parent's write end from being inherited
        SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);
    }

    if (options.redirect_stdout)
    {
        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0))
        {
            if (stdin_read != INVALID_HANDLE_VALUE)
                CloseHandle(stdin_read);
            if (stdin_write != INVALID_HANDLE_VALUE)
                CloseHandle(stdin_write);
            throw ProcessError("Failed to create stdout pipe: " + get_last_error_message());
        }
        SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    }

    if (options.redirect_stderr)
    {
        if (!CreatePipe(&stderr_read, &stderr_write, &sa, 0))
        {
            if (stdin_read != INVALID_HANDLE_VALUE)
                CloseHandle(stdin_read);
            if (stdin_write != INVALID_HANDLE_VALUE)
                CloseHandle(stdin_write);
            if (stdout_read != INVALID_HANDLE_VALUE)
                CloseHandle(stdout_read);
            if (stdout_write != INVALID_HANDLE_VALUE)
                CloseHandle(stdout_write);
            throw ProcessError("Failed to create stderr pipe: " + get_last_error_message());
        }
        SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    }

    // Resolve executable for spawning. Passing lpApplicationName avoids Windows searching the
    // current directory when the executable is not fully-qualified.
    std::string resolved_executable = resolve_executable_for_spawn(executable, options);

    // Build command line
    std::string cmdline = build_command_line(resolved_executable, args);

    // Build environment block
    std::string env_block;
    if (!options.environment.empty() || !options.inherit_environment)
    {
        std::map<std::string, std::string> env;

        if (options.inherit_environment)
        {
            // Get current environment
            LPCH env_strings = GetEnvironmentStrings();
            if (env_strings)
            {
                for (LPCH p = env_strings; *p; p += strlen(p) + 1)
                {
                    std::string entry(p);
                    size_t eq = entry.find('=');
                    if (eq != std::string::npos && eq > 0)
                        env[entry.substr(0, eq)] = entry.substr(eq + 1);
                }
                FreeEnvironmentStrings(env_strings);
            }
        }

        // Merge/override with provided environment
        for (const auto& [key, value] : options.environment)
            env[key] = value;

        // Build null-terminated block
        for (const auto& [key, value] : env)
        {
            env_block += key + "=" + value;
            env_block.push_back('\0');
        }
        env_block.push_back('\0');
    }

    // Setup startup info
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read != INVALID_HANDLE_VALUE ? stdin_read : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput =
        stdout_write != INVALID_HANDLE_VALUE ? stdout_write : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError =
        stderr_write != INVALID_HANDLE_VALUE ? stderr_write : GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    DWORD creation_flags = 0;
    if (options.create_no_window)
        creation_flags |= CREATE_NO_WINDOW;

    BOOL success = CreateProcessA(
        resolved_executable.c_str(),
        cmdline.data(),
        nullptr,
        nullptr,
        TRUE, // Inherit handles
        creation_flags,
        env_block.empty() ? nullptr : env_block.data(),
        options.working_directory.empty() ? nullptr : options.working_directory.c_str(),
        &si,
        &pi
    );

    // Close child's ends of pipes
    if (stdin_read != INVALID_HANDLE_VALUE)
        CloseHandle(stdin_read);
    if (stdout_write != INVALID_HANDLE_VALUE)
        CloseHandle(stdout_write);
    if (stderr_write != INVALID_HANDLE_VALUE)
        CloseHandle(stderr_write);

    if (!success)
    {
        if (stdin_write != INVALID_HANDLE_VALUE)
            CloseHandle(stdin_write);
        if (stdout_read != INVALID_HANDLE_VALUE)
            CloseHandle(stdout_read);
        if (stderr_read != INVALID_HANDLE_VALUE)
            CloseHandle(stderr_read);
        throw ProcessError("Failed to create process: " + get_last_error_message());
    }

    // Store handles
    handle_->process_handle = pi.hProcess;
    handle_->thread_handle = pi.hThread;
    handle_->process_id = pi.dwProcessId;
    handle_->running = true;

    // Assign to job object so child dies when parent dies
    HANDLE job = get_child_process_job();
    if (job)
        AssignProcessToJobObject(job, pi.hProcess);

    stdin_->handle_->handle = stdin_write;
    stdout_->handle_->handle = stdout_read;
    stderr_->handle_->handle = stderr_read;
}

WritePipe& Process::stdin_pipe()
{
    if (!stdin_ || !stdin_->is_open())
        throw ProcessError("stdin pipe not available");
    return *stdin_;
}

ReadPipe& Process::stdout_pipe()
{
    if (!stdout_ || !stdout_->is_open())
        throw ProcessError("stdout pipe not available");
    return *stdout_;
}

ReadPipe& Process::stderr_pipe()
{
    if (!stderr_ || !stderr_->is_open())
        throw ProcessError("stderr pipe not available");
    return *stderr_;
}

bool Process::is_running() const
{
    if (!handle_ || handle_->process_handle == INVALID_HANDLE_VALUE)
        return false;

    DWORD exit_code;
    if (GetExitCodeProcess(handle_->process_handle, &exit_code))
        return exit_code == STILL_ACTIVE;
    return false;
}

std::optional<int> Process::try_wait()
{
    if (!handle_ || handle_->process_handle == INVALID_HANDLE_VALUE)
        return std::nullopt;

    DWORD result = WaitForSingleObject(handle_->process_handle, 0);
    if (result == WAIT_OBJECT_0)
    {
        DWORD exit_code;
        GetExitCodeProcess(handle_->process_handle, &exit_code);
        handle_->running = false;
        handle_->exit_code = static_cast<int>(exit_code);
        return handle_->exit_code;
    }

    return std::nullopt;
}

int Process::wait()
{
    if (!handle_ || handle_->process_handle == INVALID_HANDLE_VALUE)
        return handle_ ? handle_->exit_code : -1;

    WaitForSingleObject(handle_->process_handle, INFINITE);

    DWORD exit_code;
    GetExitCodeProcess(handle_->process_handle, &exit_code);
    handle_->running = false;
    handle_->exit_code = static_cast<int>(exit_code);
    return handle_->exit_code;
}

void Process::terminate()
{
    if (handle_ && handle_->process_handle != INVALID_HANDLE_VALUE)
    {
        // On Windows, graceful termination by closing stdin
        stdin_->close();

        // Give process a chance to exit gracefully
        DWORD result = WaitForSingleObject(handle_->process_handle, 1000);
        if (result != WAIT_OBJECT_0)
        {
            // Force kill if it didn't exit
            TerminateProcess(handle_->process_handle, 1);
        }
    }
}

void Process::kill()
{
    if (handle_ && handle_->process_handle != INVALID_HANDLE_VALUE)
        TerminateProcess(handle_->process_handle, 1);
}

int Process::pid() const
{
    return handle_ ? static_cast<int>(handle_->process_id) : 0;
}

// =============================================================================
// Utility functions
// =============================================================================

std::optional<std::string> find_executable(const std::string& name)
{
    // Check if it's an absolute path
    if (std::filesystem::path(name).is_absolute())
    {
        if (std::filesystem::exists(name))
            return name;
        return std::nullopt;
    }

    // Get PATH
    const char* path_env = std::getenv("PATH");
    if (!path_env)
        return std::nullopt;

    // Get PATHEXT for Windows executable extensions
    const char* pathext_env = std::getenv("PATHEXT");
    std::vector<std::string> extensions;
    if (pathext_env)
    {
        std::string pathext(pathext_env);
        size_t start = 0;
        size_t end;
        while ((end = pathext.find(';', start)) != std::string::npos)
        {
            extensions.push_back(pathext.substr(start, end - start));
            start = end + 1;
        }
        extensions.push_back(pathext.substr(start));
    }
    else
    {
        extensions = {".COM", ".EXE", ".BAT", ".CMD"};
    }

    // Search PATH
    std::string path(path_env);
    size_t start = 0;
    size_t end;
    while ((end = path.find(';', start)) != std::string::npos)
    {
        std::string dir = path.substr(start, end - start);
        start = end + 1;

        // Try with extensions
        for (const auto& ext : extensions)
        {
            std::filesystem::path candidate = std::filesystem::path(dir) / (name + ext);
            if (std::filesystem::exists(candidate))
                return candidate.string();
        }

        // Try without extension (in case name already has one)
        std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (std::filesystem::exists(candidate))
            return candidate.string();
    }

    // Check last directory in PATH
    std::string dir = path.substr(start);
    for (const auto& ext : extensions)
    {
        std::filesystem::path candidate = std::filesystem::path(dir) / (name + ext);
        if (std::filesystem::exists(candidate))
            return candidate.string();
    }

    std::filesystem::path candidate = std::filesystem::path(dir) / name;
    if (std::filesystem::exists(candidate))
        return candidate.string();

    return std::nullopt;
}

bool is_node_script(const std::string& path)
{
    std::string lower_path = path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
    return lower_path.size() >= 3 &&
           (lower_path.substr(lower_path.size() - 3) == ".js" ||
            (lower_path.size() >= 4 && lower_path.substr(lower_path.size() - 4) == ".mjs"));
}

std::optional<std::string> find_node()
{
    return find_executable("node");
}

} // namespace copilot

#endif // _WIN32
