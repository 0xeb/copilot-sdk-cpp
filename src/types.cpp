// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#include <copilot/types.hpp>

#include "win32_strings.hpp"

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

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& value)
{
    if (value.empty())
        return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(), size
    );
    return wide;
}

std::string wide_to_utf8(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr
    );
    std::string narrow(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        narrow.data(),
        size,
        nullptr,
        nullptr
    );
    return narrow;
}
#endif

std::optional<std::string> env_value(const char* name)
{
#ifdef _WIN32
    const std::wstring wide_name = utf8_to_wide(name);
    std::wstring value(256, L'\0');
    for (;;)
    {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableW(
            wide_name.c_str(), value.data(), static_cast<DWORD>(value.size())
        );
        if (length == 0)
        {
            if (GetLastError() == ERROR_ENVVAR_NOT_FOUND)
                return std::nullopt;
            return std::string();
        }
        if (length < value.size())
        {
            value.resize(length);
            return wide_to_utf8(value);
        }
        // Too small: length is the required size including the terminator.
        value.assign(length, L'\0');
    }
#else
    const char* value = std::getenv(name);
    return value ? std::optional<std::string>(value) : std::nullopt;
#endif
}

std::filesystem::path path_from_utf8(const std::string& value)
{
#ifdef _WIN32
    return std::filesystem::path(utf8_to_wide(value));
#else
    return std::filesystem::path(value);
#endif
}

std::string path_to_utf8(const std::filesystem::path& value)
{
#ifdef _WIN32
    return wide_to_utf8(value.native());
#else
    return value.string();
#endif
}

} // namespace copilot::detail
