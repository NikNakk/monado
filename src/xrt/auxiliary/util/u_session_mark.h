// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief One-shot frame capture requests for session marks.
 *
 * The session recorder asks for the next composited frame to be written to a
 * file; the compositor's mirror readback serves the request once.
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Ask for the next composited left-eye frame to be written to @p path (PPM).
void
u_session_mark_request_capture(const char *path);

//! Whether a capture has been requested and not yet taken.
bool
u_session_mark_capture_pending(void);

//! Takes a pending request: copies its path and clears it. Returns false if none.
bool
u_session_mark_take_capture(char *out_path, size_t out_size);

//! Called once per compositor frame; wakes u_session_mark_wait_frame.
void
u_session_mark_frame_tick(void);

//! Waits for the next compositor frame tick or @p timeout_ns. Returns false on timeout.
bool
u_session_mark_wait_frame(int64_t timeout_ns);

#ifdef __cplusplus
}
#endif
