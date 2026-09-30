// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS service cleanup hook for per-client IPC teardown.
 * @ingroup ipc_server
 */

#pragma once

#include "server/ipc_server.h"
#include "shared/ipc_metal_xpc_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Release a disconnecting client's unconsumed Metal XPC registry entries,
 * unless another connection from the same process is still open. Called from
 * common_shutdown() with the global state lock held, before the client state
 * (and so its PID) is cleared.
 */
void
ipc_metal_client_cleanup(volatile struct ipc_client_state *ics);

#ifdef __cplusplus
}
#endif
