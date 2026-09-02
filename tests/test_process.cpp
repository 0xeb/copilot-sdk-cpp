// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <chrono>
#include <copilot/process.hpp>
#include <gtest/gtest.h>
#include <thread>

using namespace copilot;

// =============================================================================
// Process Tests
// =============================================================================

TEST(ProcessTest, SpawnAndWait)
{
    Process proc;

#ifdef _WIN32
    // On Windows, use cmd /c echo
    proc.spawn("cmd", {"/c", "echo", "hello"});
#else
    // On POSIX, use echo
    proc.spawn("echo", {"hello"});
#endif

    int exit_code = proc.wait();
    EXPECT_EQ(exit_code, 0);
}

TEST(ProcessTest, ReadStdout)
{
    Process proc;

#ifdef _WIN32
    proc.spawn("cmd", {"/c", "echo", "test output"});
#else
    proc.spawn("echo", {"test output"});
#endif

    std::string output;
    char buffer[256];

    // Read all output
    while (proc.is_running() || proc.stdout_pipe().has_data(100))
    {
        size_t n = proc.stdout_pipe().read(buffer, sizeof(buffer));
        if (n > 0)
            output.append(buffer, n);
        else
            break;
    }

    proc.wait();

    // Should contain "test output" (Windows adds \r\n, POSIX adds \n)
    EXPECT_NE(output.find("test output"), std::string::npos);
}

TEST(ProcessTest, WriteStdin)
{
    Process proc;

#ifdef _WIN32
    // On Windows, use findstr to echo stdin
    proc.spawn("findstr", {".*"});
#else
    // On POSIX, use cat to echo stdin
    proc.spawn("cat", {});
#endif

    // Write to stdin
    proc.stdin_pipe().write("hello world\n");
    proc.stdin_pipe().close(); // Signal EOF

    std::string output;
    char buffer[256];

    // Read all output
    while (true)
    {
        if (!proc.stdout_pipe().has_data(500))
            break;
        size_t n = proc.stdout_pipe().read(buffer, sizeof(buffer));
        if (n == 0)
            break;
        output.append(buffer, n);
    }

    proc.wait();

    EXPECT_NE(output.find("hello world"), std::string::npos);
}

TEST(ProcessTest, ProcessPid)
{
    Process proc;

#ifdef _WIN32
    proc.spawn("cmd", {"/c", "echo", "pid test"});
#else
    proc.spawn("echo", {"pid test"});
#endif

    int pid = proc.pid();
    EXPECT_GT(pid, 0);

    proc.wait();
}

TEST(ProcessTest, IsRunning)
{
    Process proc;

#ifdef _WIN32
    // Use ping to create a longer-running process
    proc.spawn("ping", {"-n", "2", "127.0.0.1"});
#else
    // Use sleep for a longer-running process
    proc.spawn("sleep", {"0.5"});
#endif

    // Should be running initially
    EXPECT_TRUE(proc.is_running());

    // Wait for completion
    proc.wait();

    // Should not be running after wait
    EXPECT_FALSE(proc.is_running());
}

TEST(ProcessTest, TryWait)
{
    Process proc;

#ifdef _WIN32
    proc.spawn("cmd", {"/c", "echo", "done"});
#else
    proc.spawn("echo", {"done"});
#endif

    // Give process time to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // try_wait should return exit code when done
    auto result = proc.try_wait();
    if (!result.has_value())
    {
        // If not done yet, wait for it
        proc.wait();
        result = proc.try_wait();
    }

    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0);
}

TEST(ProcessTest, Terminate)
{
    Process proc;

#ifdef _WIN32
    // Start a long-running process
    proc.spawn("ping", {"-n", "100", "127.0.0.1"});
#else
    proc.spawn("sleep", {"100"});
#endif

    EXPECT_TRUE(proc.is_running());

    // Terminate it
    proc.terminate();
    int exit_code = proc.wait();

    // Process should no longer be running
    EXPECT_FALSE(proc.is_running());

    // Exit code will be non-zero for terminated process
    // (On Windows it's 1 from TerminateProcess, on POSIX it's 128+SIGTERM=143)
#ifndef _WIN32
    EXPECT_NE(exit_code, 0);
#endif
}

TEST(ProcessTest, Kill)
{
    Process proc;

#ifdef _WIN32
    proc.spawn("ping", {"-n", "100", "127.0.0.1"});
#else
    proc.spawn("sleep", {"100"});
#endif

    EXPECT_TRUE(proc.is_running());

    // Force kill
    proc.kill();
    proc.wait();

    EXPECT_FALSE(proc.is_running());
}

TEST(ProcessTest, KillAndClosePipesCompletesWithinBound)
{
    Process proc;
#ifdef _WIN32
    proc.spawn("ping", {"-n", "100", "127.0.0.1"});
#else
    proc.spawn("sleep", {"100"});
#endif

    const auto started = std::chrono::steady_clock::now();
    proc.kill();
    const auto result = proc.wait_for(std::chrono::seconds(5));
    proc.close_pipes();

    EXPECT_TRUE(result.has_value());
    EXPECT_LT(
        std::chrono::steady_clock::now() - started,
        std::chrono::seconds(6));
}

TEST(ProcessTest, Environment)
{
    Process proc;
    ProcessOptions opts;
    opts.environment["TEST_VAR"] = "test_value";
    opts.redirect_stdout = true;

#ifdef _WIN32
    proc.spawn("cmd", {"/c", "echo", "%TEST_VAR%"}, opts);
#else
    // Use env to print the variable
    proc.spawn("sh", {"-c", "echo $TEST_VAR"}, opts);
#endif

    std::string output;
    char buffer[256];

    while (proc.stdout_pipe().has_data(500))
    {
        size_t n = proc.stdout_pipe().read(buffer, sizeof(buffer));
        if (n == 0)
            break;
        output.append(buffer, n);
    }

    proc.wait();

    EXPECT_NE(output.find("test_value"), std::string::npos);
}

TEST(ProcessTest, NonExistentExecutable)
{
    Process proc;

    EXPECT_THROW({ proc.spawn("this_executable_does_not_exist_12345", {}); }, ProcessError);
}

TEST(ProcessTest, ReadLine)
{
    Process proc;

#ifdef _WIN32
    // On Windows, use a single echo command per call - chaining with & is unreliable
    proc.spawn("cmd", {"/c", "echo line1 & echo line2"});
#else
    proc.spawn("sh", {"-c", "echo line1; echo line2"});
#endif

    std::string line1 = proc.stdout_pipe().read_line();
    std::string line2 = proc.stdout_pipe().read_line();

    proc.wait();

    EXPECT_NE(line1.find("line1"), std::string::npos);
    // On Windows, both lines might be on the same line with different formatting
    // Just verify we got some output containing "line"
    EXPECT_TRUE(line1.find("line") != std::string::npos || line2.find("line") != std::string::npos);
}

// =============================================================================
// Utility Function Tests
// =============================================================================

TEST(ProcessUtilTest, FindExecutable)
{
#ifdef _WIN32
    auto cmd = find_executable("cmd");
    EXPECT_TRUE(cmd.has_value());
#else
    auto sh = find_executable("sh");
    EXPECT_TRUE(sh.has_value());
#endif
}

TEST(ProcessUtilTest, FindExecutableNotFound)
{
    auto result = find_executable("this_does_not_exist_xyz123");
    EXPECT_FALSE(result.has_value());
}

TEST(ProcessUtilTest, IsNodeScript)
{
    EXPECT_TRUE(is_node_script("script.js"));
    EXPECT_TRUE(is_node_script("/path/to/script.js"));
    EXPECT_TRUE(is_node_script("script.mjs"));
    EXPECT_FALSE(is_node_script("script.py"));
    EXPECT_FALSE(is_node_script("script"));
    EXPECT_FALSE(is_node_script("js"));
}
