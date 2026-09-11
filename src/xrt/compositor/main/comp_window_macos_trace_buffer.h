// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Diagnostic stdio buffering shim for the macOS PS VR2 compositor trace.
 *
 * This header is force-included only for comp_window_macos_latest.m. When
 * PSVR2_TIMING_TRACE_FULLY_BUFFERED=1, explicit fflush() calls from the macOS
 * compositor timing trace are suppressed and fully-buffered streams are enlarged
 * from their normal 64 KiB to 16 MiB. fclose() still performs the final flush at
 * teardown. The enlarged buffers are intended for short (roughly 30-60 second)
 * diagnostic captures so stdio writes do not perturb presentation timing.
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int
macos_trace_fully_buffered_enabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		const char *value = getenv("PSVR2_TIMING_TRACE_FULLY_BUFFERED");
		enabled = value != NULL && strcmp(value, "1") == 0;
	}
	return enabled;
}

static inline int
macos_trace_buffered_fflush(FILE *stream)
{
	if (macos_trace_fully_buffered_enabled()) {
		(void)stream;
		return 0;
	}
	return fflush(stream);
}

static inline int
macos_trace_buffered_setvbuf(FILE *stream, char *buffer, int mode, size_t size)
{
	if (macos_trace_fully_buffered_enabled() && mode == _IOFBF) {
		static int logged = 0;
		const size_t diagnostic_buffer_size = 16u * 1024u * 1024u;
		size = diagnostic_buffer_size;
		if (!logged) {
			fprintf(stderr,
			        "macOS diagnostic: timing traces fully buffered during capture (16 MiB per CSV); "
			        "stdio flush deferred to teardown\n");
			logged = 1;
		}
	}
	return setvbuf(stream, buffer, mode, size);
}

/* Define these only after the real libc functions above have been referenced. */
#define fflush macos_trace_buffered_fflush
#define setvbuf macos_trace_buffered_setvbuf
