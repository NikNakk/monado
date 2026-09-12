// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Source-local redirects for macOS Metal IPC calls.
 * @ingroup ipc_client
 */

#pragma once

/*
 * Include the generated declarations before defining redirect macros so their
 * prototypes retain the real generated function names.
 */
#include "ipc_client_generated.h"
#include "client/ipc_metal_swapchain_import.h"

#define ipc_call_swapchain_import ipc_metal_call_swapchain_import_or_default
#define ipc_call_compositor_semaphore_create ipc_metal_call_compositor_semaphore_create_or_default
