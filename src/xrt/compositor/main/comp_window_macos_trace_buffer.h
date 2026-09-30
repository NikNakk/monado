// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Trace flushing and timed-present helpers for the macOS PS VR2 compositor.
 *
 * Used only by comp_window_macos.m, which calls these in place of fflush()
 * and presentDrawable:atTime:. With PSVR2_TIMING_TRACE_FULLY_BUFFERED=1 the
 * timing traces (opened with u_timing_trace_open()) are not flushed until they
 * are closed, so stdio writes do not perturb presentation timing during short
 * captures.
 *
 * XRT_MACOS_PRESENT_MIN_DURATION_US=<us> replaces the absolute timed
 * present with Metal's relative presentDrawable:afterMinimumDuration: primitive.
 * This lets Core Animation preserve a regular minimum visible duration without
 * accumulating absolute presentation-time debt when an earlier slot is missed.
 */

#pragma once

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "util/u_debug.h"
#include "util/u_timing_trace.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int
macos_trace_fully_buffered_enabled(void)
{
	return u_timing_trace_fully_buffered() ? 1 : 0;
}

/* 8000 us is the known-good 120 Hz baseline; 0 gives absolute timed presents. */
DEBUG_GET_ONCE_NUM_OPTION(macos_present_min_duration_us, "XRT_MACOS_PRESENT_MIN_DURATION_US", 8000)

static inline uint64_t
macos_present_min_duration_us(void)
{
	long duration_us = debug_get_num_option_macos_present_min_duration_us();
	return duration_us > 0 ? (uint64_t)duration_us : 0;
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
