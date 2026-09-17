// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Diagnostic macOS process activity assertion for active XR sessions.
 * @ingroup ipc_server
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Update the process activity assertion to match aggregate XR session state.
 *
 * This diagnostic is disabled unless XRT_MACOS_PROCESS_ACTIVITY is set to a
 * supported value. Callers serialize updates with the IPC server global-state
 * lock so begin/end transitions cannot be reordered across client threads.
 */
void
ipc_server_macos_process_activity_update(bool any_session_active);

/*! End any still-held diagnostic process activity assertion during shutdown. */
void
ipc_server_macos_process_activity_shutdown(void);

#ifdef __cplusplus
}
#endif
