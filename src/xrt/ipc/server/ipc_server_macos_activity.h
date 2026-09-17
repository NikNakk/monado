// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Diagnostic macOS process activity assertion.
 * @ingroup ipc_server
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Read XRT_MACOS_PROCESS_ACTIVITY and, when configured, acquire a supported
 * NSProcessInfo activity assertion for the lifetime of monado-service.
 *
 * This is intentionally process-lifetime for the current RunningBoard
 * diagnostic so the assertion is active before compositor creation and cannot
 * race a later XR session transition.
 */
void
ipc_server_macos_process_activity_startup(void);

/*! End and release any diagnostic process activity assertion. */
void
ipc_server_macos_process_activity_shutdown(void);

#ifdef __cplusplus
}
#endif
