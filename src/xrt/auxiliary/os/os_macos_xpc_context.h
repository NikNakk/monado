// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Propagate foreground XPC request context onto custom macOS worker threads.
 * @ingroup aux_os
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*os_macos_xpc_context_func_t)(void *ptr);

/*!
 * Capture the current execution context, including any active XPC request
 * properties, and retain it as the foreground XR context.
 *
 * Calls are reference-counted and must be balanced with
 * os_macos_xpc_context_release().
 */
void
os_macos_xpc_context_acquire_current(void);

/*! Release one active foreground-XR context reference. */
void
os_macos_xpc_context_release(void);

/*!
 * Run @p func synchronously on the calling thread while applying the most
 * recently captured foreground-XR XPC context. If no context is active, calls
 * @p func directly.
 */
void
os_macos_xpc_context_run(os_macos_xpc_context_func_t func, void *ptr);

#ifdef __cplusplus
}
#endif
