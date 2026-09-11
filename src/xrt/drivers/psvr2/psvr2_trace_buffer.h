// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Diagnostic stdio buffering shim for PS VR2 driver timing traces.
 *
 * When PSVR2_TIMING_TRACE_FULLY_BUFFERED=1, the driver's normal 64 KiB
 * fully-buffered CSV streams are enlarged to 16 MiB and explicit fflush()
 * calls on those streams are suppressed during capture. fclose() still
 * performs the final flush at teardown.
 *
 * The stream registry keeps the shim local to files that were opened through
 * the driver's trace-buffer setup: unrelated fflush() calls are passed through.
 * This is intended for short diagnostic captures so stdio I/O cannot create a
 * false periodic relationship with presentation timing.
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSVR2_TRACE_NORMAL_BUFFER_SIZE (64u * 1024u)
#define PSVR2_TRACE_DIAGNOSTIC_BUFFER_SIZE (16u * 1024u * 1024u)
#define PSVR2_TRACE_BUFFERED_STREAM_CAPACITY 8u

static FILE *psvr2_trace_buffered_streams[PSVR2_TRACE_BUFFERED_STREAM_CAPACITY];
static size_t psvr2_trace_buffered_stream_count;

static inline int
psvr2_trace_fully_buffered_enabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		const char *value = getenv("PSVR2_TIMING_TRACE_FULLY_BUFFERED");
		enabled = value != NULL && strcmp(value, "1") == 0;
	}
	return enabled;
}

static inline int
psvr2_trace_buffered_stream_is_tracked(FILE *stream)
{
	for (size_t i = 0; i < psvr2_trace_buffered_stream_count; i++) {
		if (psvr2_trace_buffered_streams[i] == stream) {
			return 1;
		}
	}
	return 0;
}

static inline void
psvr2_trace_buffered_stream_track(FILE *stream)
{
	if (stream == NULL || psvr2_trace_buffered_stream_is_tracked(stream)) {
		return;
	}
	if (psvr2_trace_buffered_stream_count < PSVR2_TRACE_BUFFERED_STREAM_CAPACITY) {
		psvr2_trace_buffered_streams[psvr2_trace_buffered_stream_count++] = stream;
	}
}

static inline int
psvr2_trace_buffered_fflush(FILE *stream)
{
	if (psvr2_trace_fully_buffered_enabled() && stream != NULL &&
	    psvr2_trace_buffered_stream_is_tracked(stream)) {
		return 0;
	}
	return fflush(stream);
}

static inline int
psvr2_trace_buffered_setvbuf(FILE *stream, char *buffer, int mode, size_t size)
{
	if (psvr2_trace_fully_buffered_enabled() && mode == _IOFBF && size == PSVR2_TRACE_NORMAL_BUFFER_SIZE) {
		static int logged = 0;
		size = PSVR2_TRACE_DIAGNOSTIC_BUFFER_SIZE;
		psvr2_trace_buffered_stream_track(stream);
		if (!logged) {
			fprintf(stderr,
			        "PSVR2 driver diagnostic: timing traces fully buffered during capture "
			        "(16 MiB per CSV); stdio flush deferred to teardown\n");
			logged = 1;
		}
	}
	return setvbuf(stream, buffer, mode, size);
}

/* Define these only after the real libc functions above have been referenced. */
#define fflush psvr2_trace_buffered_fflush
#define setvbuf psvr2_trace_buffered_setvbuf
