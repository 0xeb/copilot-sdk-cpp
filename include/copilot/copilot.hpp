// Copyright (c) 2025 Elias Bachaalany
// SPDX-License-Identifier: MIT

#pragma once

/// @file copilot.hpp
/// @brief Master include for the Copilot SDK
///
/// This header includes all public API headers for convenience.
/// You can also include individual headers for finer-grained control.

#include <copilot/client.hpp>
#include <copilot/events.hpp>
#include <copilot/jsonrpc.hpp>
#include <copilot/process.hpp>
#include <copilot/rpc_methods.hpp>
#include <copilot/rpc_types.hpp>
#include <copilot/session.hpp>
#include <copilot/tool_builder.hpp>
#include <copilot/transport.hpp>
#include <copilot/transport_stdio.hpp>
#include <copilot/transport_tcp.hpp>
#include <copilot/types.hpp>

namespace copilot
{

/// SDK version string
inline constexpr const char* kSdkVersion = "0.1.0";

} // namespace copilot
