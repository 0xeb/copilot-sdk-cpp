// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/types.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace copilot::detail
{

std::optional<std::string> env_value(const char* name)
{
#ifdef _WIN32
    std::string value(256, '\0');
    for (;;)
    {
        SetLastError(ERROR_SUCCESS);
        const DWORD length =
            GetEnvironmentVariableA(name, value.data(), static_cast<DWORD>(value.size()));
        if (length == 0)
        {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
                return std::nullopt;
            return std::string();
        }
        if (length < value.size())
        {
            value.resize(length);
            return value;
        }
        // Too small: length is the required size including the terminator.
        value.assign(length, '\0');
    }
#else
    const char* value = std::getenv(name);
    return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

} // namespace copilot::detail
