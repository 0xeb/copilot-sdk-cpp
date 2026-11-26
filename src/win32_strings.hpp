// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

#ifdef _WIN32

#include <string>

namespace copilot::detail
{
/// UTF-8 <-> UTF-16 for the Windows wide APIs.
std::wstring utf8_to_wide(const std::string& value);
std::string wide_to_utf8(const std::wstring& value);
} // namespace copilot::detail

#endif
