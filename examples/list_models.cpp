// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

/// @file list_models.cpp
/// @brief Example demonstrating model discovery and vision capabilities
///
/// This example shows how to:
/// 1. Enumerate available models via client.list_models()
/// 2. Inspect model capabilities (vision support, context window)
/// 3. Display ModelVisionLimits (supported media types, image limits)

#include <copilot/copilot.hpp>
#include <iomanip>
#include <iostream>
#include <string>

int main()
{
    try
    {
        copilot::ClientOptions options;
        options.log_level = copilot::LogLevel::Info;

        copilot::Client client(options);

        std::cout << "=== List Models Example ===\n\n";
        std::cout << "Discovering available models and their capabilities...\n\n";

        client.start().get();

        // Fetch the list of available models
        auto models = client.list_models().get();

        std::cout << "Found " << models.size() << " model(s):\n\n";

        for (size_t i = 0; i < models.size(); i++)
        {
            const auto& model = models[i];
            std::cout << "  " << (i + 1) << ". " << model.name
                      << " [" << model.id << "]\n";

            // Context window
            std::cout << "     Context window: "
                      << model.capabilities.limits.max_context_window_tokens
                      << " tokens\n";

            if (model.capabilities.limits.max_prompt_tokens)
            {
                std::cout << "     Max prompt: "
                          << *model.capabilities.limits.max_prompt_tokens
                          << " tokens\n";
            }

            // Vision support
            if (model.capabilities.supports.vision)
            {
                std::cout << "     Vision: SUPPORTED\n";

                if (model.capabilities.limits.vision)
                {
                    const auto& vision = *model.capabilities.limits.vision;

                    if (!vision.supported_media_types.empty())
                    {
                        std::cout << "       Media types: ";
                        for (size_t j = 0; j < vision.supported_media_types.size(); j++)
                        {
                            if (j > 0)
                                std::cout << ", ";
                            std::cout << vision.supported_media_types[j];
                        }
                        std::cout << "\n";
                    }

                    if (vision.max_prompt_images > 0)
                    {
                        std::cout << "       Max images per prompt: "
                                  << vision.max_prompt_images << "\n";
                    }

                    if (vision.max_prompt_image_size > 0)
                    {
                        std::cout << "       Max image size: "
                                  << vision.max_prompt_image_size << " bytes\n";
                    }
                }
            }
            else
            {
                std::cout << "     Vision: not supported\n";
            }

            // Policy info
            if (model.policy)
            {
                std::cout << "     Policy: " << model.policy->state << "\n";
            }

            std::cout << "\n";
        }

        // Summary: count vision-capable models
        int vision_count = 0;
        for (const auto& m : models)
        {
            if (m.capabilities.supports.vision)
                vision_count++;
        }
        std::cout << "Summary: " << vision_count << " of " << models.size()
                  << " model(s) support vision.\n";

        // Cleanup
        client.stop().get();

        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
