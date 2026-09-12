// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Source-local redirect for macOS Metal-token swapchain imports.
 * @ingroup ipc_client
 */

#pragma once

/*
 * Include the generated declarations before defining the redirect macro so
 * their prototypes retain the real function names.
 */
#include "ipc_client_generated.h"
#include "client/ipc_metal_swapchain_import.h"

#define ipc_call_swapchain_import ipc_metal_call_swapchain_import_or_default
