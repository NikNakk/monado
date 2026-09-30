// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Diagnostic stdio buffering and timed-present shims for the macOS PS VR2 compositor.
 *
 * Used only by comp_window_macos.m, which calls these in place of fflush(),
 * setvbuf() and presentDrawable:atTime:. When
 * PSVR2_TIMING_TRACE_FULLY_BUFFERED=1, explicit flushes of the macOS
 * compositor timing trace are suppressed and fully-buffered streams are enlarged
 * from their normal 64 KiB to 16 MiB. fclose() still performs the final flush at
 * teardown. The enlarged buffers are intended for short (roughly 30-60 second)
 * diagnostic captures so stdio writes do not perturb presentation timing.
 *
 * XRT_MACOS_PRESENT_MIN_DURATION_US=<us> replaces the absolute timed
 * present with Metal's relative presentDrawable:afterMinimumDuration: primitive.
 * This lets Core Animation preserve a regular minimum visible duration without
 * accumulating absolute presentation-time debt when an earlier slot is missed.
 */

#pragma once

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
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

static inline uint64_t
macos_present_min_duration_us(void)
{
	static int initialized = 0;
	/* 8000 us is the known-good 120 Hz baseline; set 0 for absolute timed presents. */
	static uint64_t duration_us = 8000;
	if (!initialized) {
		const char *value = getenv("XRT_MACOS_PRESENT_MIN_DURATION_US");
		if (value != NULL && value[0] != '\0') {
			char *end = NULL;
			unsigned long long parsed = strtoull(value, &end, 10);
			if (end != value && *end == '\0') {
				duration_us = (uint64_t)parsed;
			}
		}
		if (duration_us != 0) {
			fprintf(stderr,
			        "macOS diagnostic: Metal afterMinimumDuration presentation enabled at %llu us; "
			        "absolute present-at-time scheduling is bypassed\n",
			        (unsigned long long)duration_us);
		}
		initialized = 1;
	}
	return duration_us;
}

/*!
 * Present @p drawable at @p present_host_s, or after the minimum duration when
 * XRT_MACOS_PRESENT_MIN_DURATION_US is non-zero (the default).
 */
static inline void
macos_present_drawable_at_time(id<MTLCommandBuffer> command_buffer,
                               id<MTLDrawable> drawable,
                               CFTimeInterval present_host_s)
{
	uint64_t minimum_duration_us = macos_present_min_duration_us();
	if (minimum_duration_us != 0) {
		CFTimeInterval minimum_duration_s = (CFTimeInterval)((double)minimum_duration_us / 1000000.0);
		[command_buffer presentDrawable:drawable afterMinimumDuration:minimum_duration_s];
		return;
	}

	[command_buffer presentDrawable:drawable atTime:present_host_s];
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
