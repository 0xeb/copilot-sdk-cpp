// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file attachments.cpp
/// @brief Example demonstrating file and directory attachments
///
/// This example shows how to:
/// 1. Attach files to messages for analysis
/// 2. Attach directories for broader context
/// 3. Use attachments for code review workflows

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <copilot/copilot.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace fs = std::filesystem;

/// Create a sample file for demonstration
void create_sample_file(const std::string& path, const std::string& content)
{
    std::ofstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Failed to create sample file: " + path);
    file << content;
    if (!file)
        throw std::runtime_error("Failed to write sample file: " + path);
    std::cout << "Created sample file: " << path << "\n";
}

int main()
{
    try
    {
        copilot::ClientOptions options;
        options.log_level = copilot::LogLevel::Info;

        copilot::Client client(options);

        std::cout << "=== File Attachments Example ===\n\n";
        std::cout << "This example demonstrates attaching files and directories to messages.\n";
        std::cout << "Attachments provide context to Copilot for code review, analysis, etc.\n\n";

        // Create sample files for demonstration
        fs::path temp_dir = fs::temp_directory_path() / "copilot-sdk-cpp-attachments-example";
        fs::create_directories(temp_dir);
        fs::path sample_cpp = temp_dir / "sample_code.cpp";
        fs::path sample_header = temp_dir / "sample_code.h";

        create_sample_file(sample_cpp.string(), R"(
#include "sample_code.h"
#include <iostream>
#include <vector>

// TODO: Add error handling
void processData(std::vector<int>& data) {
    for (int i = 0; i <= data.size(); i++) {  // Bug: off-by-one error
        data[i] = data[i] * 2;
    }
}

int divide(int a, int b) {
    return a / b;  // Bug: no check for division by zero
}

char* allocateBuffer(size_t size) {
    char* buf = new char[size];
    // Bug: potential memory leak - no delete
    return buf;
}

int main() {
    std::vector<int> nums = {1, 2, 3, 4, 5};
    processData(nums);

    int result = divide(10, 0);  // Will crash
    std::cout << "Result: " << result << std::endl;

    return 0;
}
)");

        create_sample_file(sample_header.string(), R"(
#ifndef SAMPLE_CODE_H
#define SAMPLE_CODE_H

#include <vector>

void processData(std::vector<int>& data);
int divide(int a, int b);
char* allocateBuffer(size_t size);

#endif // SAMPLE_CODE_H
)");

        client.start().get();

        copilot::SessionConfig config;
        auto session = client.create_session(config).get();
        std::cout << "\nSession created: " << session->session_id() << "\n\n";

        // Synchronization
        std::mutex mtx;
        std::condition_variable cv;
        std::atomic<bool> idle{false};

        auto subscription = session->on(
            [&](const copilot::SessionEvent& event)
            {
                if (auto* msg = event.try_as<copilot::AssistantMessageData>())
                {
                    std::cout << "\nAssistant: " << msg->content << "\n";
                }
                else if (auto* delta = event.try_as<copilot::AssistantMessageDeltaData>())
                {
                    std::cout << delta->delta_content << std::flush;
                }
                else if (auto* tool_start = event.try_as<copilot::ToolExecutionStartData>())
                {
                    std::cout << "\n[Tool: " << tool_start->tool_name << "]\n";
                }
                else if (auto* error = event.try_as<copilot::SessionErrorData>())
                {
                    std::cerr << "Error: " << error->message << "\n";
                }
                else if (event.type == copilot::SessionEventType::SessionIdle)
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    idle = true;
                    cv.notify_one();
                }
            }
        );

        // ---------------------------------------------------------------------
        // Demo 1: Single file attachment
        // ---------------------------------------------------------------------
        std::cout << "=== Demo 1: Single File Attachment ===\n";
        {
            copilot::MessageOptions opts;
            opts.prompt = "Review this C++ code and identify all bugs and issues.";
            opts.attachments = std::vector<copilot::UserMessageAttachment>{
                {copilot::AttachmentType::File, sample_cpp.string(), "sample_code.cpp"}
            };

            std::cout << "Sending message with attachment: " << sample_cpp.string() << "\n";
            idle = false;
            session->send(opts).get();

            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&idle]() { return idle.load(); });
        }

        // ---------------------------------------------------------------------
        // Demo 2: Multiple file attachments
        // ---------------------------------------------------------------------
        std::cout << "\n=== Demo 2: Multiple File Attachments ===\n";
        {
            copilot::MessageOptions opts;
            opts.prompt = "Review both files. Does the header match the implementation?";
            opts.attachments = std::vector<copilot::UserMessageAttachment>{
                {copilot::AttachmentType::File, sample_cpp.string(), "sample_code.cpp"},
                {copilot::AttachmentType::File, sample_header.string(), "sample_code.h"}
            };

            std::cout << "Sending message with 2 attachments\n";
            idle = false;
            session->send(opts).get();

            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&idle]() { return idle.load(); });
        }

        // ---------------------------------------------------------------------
        // Demo 3: Directory attachment
        // ---------------------------------------------------------------------
        std::cout << "\n=== Demo 3: Directory Attachment ===\n";
        {
            copilot::MessageOptions opts;
            opts.prompt = "What files are in this directory and what do they contain?";
            opts.attachments = std::vector<copilot::UserMessageAttachment>{
                {copilot::AttachmentType::Directory, temp_dir.string(), "temp_directory"}
            };

            std::cout << "Sending message with directory attachment: " << temp_dir.string() << "\n";
            idle = false;
            session->send(opts).get();

            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [&idle]() { return idle.load(); });
        }

        // ---------------------------------------------------------------------
        // Interactive mode
        // ---------------------------------------------------------------------
        std::cout << "\n=== Interactive Mode ===\n";
        std::cout << "Commands:\n";
        std::cout << "  attach <path> - Attach a file to your next message\n";
        std::cout << "  attachdir <path> - Attach a directory to your next message\n";
        std::cout << "  clear - Clear attachments\n";
        std::cout << "  quit - Exit\n";
        std::cout << "\nOr just type a message (with any pending attachments).\n\n";

        std::vector<copilot::UserMessageAttachment> pending_attachments;

        std::cout << "> ";
        std::string line;
        while (std::getline(std::cin, line))
        {
            if (line == "quit" || line == "exit")
                break;

            if (line.empty())
            {
                std::cout << "> ";
                continue;
            }

            // Handle attach command
            if (line.substr(0, 7) == "attach ")
            {
                std::string path = line.substr(7);
                if (fs::exists(path))
                {
                    std::string name = fs::path(path).filename().string();
                    pending_attachments.push_back(
                        {copilot::AttachmentType::File, path, name}
                    );
                    std::cout << "Added attachment: " << path << " (as '" << name << "')\n";
                    std::cout << "Pending attachments: " << pending_attachments.size() << "\n";
                }
                else
                {
                    std::cout << "File not found: " << path << "\n";
                }
                std::cout << "> ";
                continue;
            }

            // Handle attachdir command
            if (line.substr(0, 10) == "attachdir ")
            {
                std::string path = line.substr(10);
                if (fs::exists(path) && fs::is_directory(path))
                {
                    std::string name = fs::path(path).filename().string();
                    pending_attachments.push_back(
                        {copilot::AttachmentType::Directory, path, name}
                    );
                    std::cout << "Added directory attachment: " << path << "\n";
                    std::cout << "Pending attachments: " << pending_attachments.size() << "\n";
                }
                else
                {
                    std::cout << "Directory not found: " << path << "\n";
                }
                std::cout << "> ";
                continue;
            }

            // Handle clear command
            if (line == "clear")
            {
                pending_attachments.clear();
                std::cout << "Attachments cleared.\n> ";
                continue;
            }

            // Send message with any pending attachments
            idle = false;

            copilot::MessageOptions opts;
            opts.prompt = line;
            if (!pending_attachments.empty())
            {
                opts.attachments = pending_attachments;
                std::cout << "Sending with " << pending_attachments.size() << " attachment(s)...\n";
                pending_attachments.clear();
            }

            session->send(opts).get();

            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [&idle]() { return idle.load(); });
            }

            std::cout << "\n> ";
        }

        // Cleanup sample files
        std::error_code ec;
        fs::remove(sample_cpp, ec);
        fs::remove(sample_header, ec);
        fs::remove(temp_dir, ec);

        // Cleanup session
        std::cout << "\nCleaning up...\n";
        session->destroy().get();
        client.stop().get();

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
