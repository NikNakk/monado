// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Diagnostic stdio buffering and timed-present shims for the macOS PS VR2 compositor.
 *
 * This header is force-included only for comp_window_macos_latest.m. When
 * PSVR2_TIMING_TRACE_FULLY_BUFFERED=1, explicit fflush() calls from the macOS
 * compositor timing trace are suppressed and fully-buffered streams are enlarged
 * from their normal 64 KiB to 16 MiB. fclose() still performs the final flush at
 * teardown. The enlarged buffers are intended for short (roughly 30-60 second)
 * diagnostic captures so stdio writes do not perturb presentation timing.
 *
 * XRT_MACOS_UNIQUE_PRESENT_SLOTS=1 also enables an experimental final-stage
 * presentation invariant: successive timed Metal presents are never submitted
 * less than one learned physical refresh period apart. This is deliberately
 * applied at presentDrawable:atTime: so all existing compositor prediction,
 * drawable-slot and trace behaviour remains unchanged for the A/B test.
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

static inline int
macos_unique_present_slots_enabled(void)
{
	static int enabled = -1;
	if (enabled < 0) {
		const char *value = getenv("XRT_MACOS_UNIQUE_PRESENT_SLOTS");
		enabled = value != NULL && strcmp(value, "1") == 0;
		if (enabled) {
			fprintf(stderr,
			        "macOS diagnostic: unique timed Metal present-slot reservation enabled; "
			        "successive timed presents will be at least one learned refresh period apart\n");
		}
	}
	return enabled;
}

/*
 * These are process-local because the macOS compositor has one PS VR2 target.
 * Start at the nominal 120 Hz period and continuously learn from distinct timed
 * present requests. Requested times are target_output - prelatch, so their
 * separation is the physical display period even when the absolute phase moves.
 */
static atomic_uint_fast64_t g_macos_present_slot_period_ns = ATOMIC_VAR_INIT(8333333ULL);
static atomic_uint_fast64_t g_macos_present_slot_last_requested_ns = ATOMIC_VAR_INIT(0);
static atomic_uint_fast64_t g_macos_present_slot_last_reserved_ns = ATOMIC_VAR_INIT(0);

static inline uint_fast64_t
macos_update_present_slot_period(uint_fast64_t requested_ns)
{
	uint_fast64_t previous =
	    atomic_exchange_explicit(&g_macos_present_slot_last_requested_ns, requested_ns, memory_order_acq_rel);
	if (previous != 0 && requested_ns > previous) {
		uint_fast64_t delta_ns = requested_ns - previous;
		uint_fast64_t periods = 1;
		/* A missed/unused slot commonly produces an exact two- or three-period jump. */
		if (delta_ns >= 12500000ULL && delta_ns < 20800000ULL) {
			periods = 2;
		} else if (delta_ns >= 20800000ULL && delta_ns < 29200000ULL) {
			periods = 3;
		}
		uint_fast64_t sample_ns = delta_ns / periods;
		if (sample_ns >= 7000000ULL && sample_ns <= 10000000ULL) {
			uint_fast64_t old_ns =
			    atomic_load_explicit(&g_macos_present_slot_period_ns, memory_order_acquire);
			/* Gentle low-pass filtering avoids following individual timestamp wobble. */
			uint_fast64_t filtered_ns = (old_ns * 7ULL + sample_ns) / 8ULL;
			atomic_store_explicit(&g_macos_present_slot_period_ns, filtered_ns, memory_order_release);
		}
	}
	return atomic_load_explicit(&g_macos_present_slot_period_ns, memory_order_acquire);
}

static inline uint_fast64_t
macos_reserve_present_slot_host_ns(uint_fast64_t requested_ns)
{
	uint_fast64_t period_ns = macos_update_present_slot_period(requested_ns);
	uint_fast64_t observed =
	    atomic_load_explicit(&g_macos_present_slot_last_reserved_ns, memory_order_acquire);

	for (;;) {
		uint_fast64_t reserved_ns = requested_ns;
		if (observed != 0) {
			uint_fast64_t next_ns =
			    observed > UINT_FAST64_MAX - period_ns ? UINT_FAST64_MAX : observed + period_ns;
			if (reserved_ns < next_ns) {
				reserved_ns = next_ns;
			}
		}
		if (atomic_compare_exchange_weak_explicit(&g_macos_present_slot_last_reserved_ns, &observed, reserved_ns,
		                                          memory_order_acq_rel, memory_order_acquire)) {
			return reserved_ns;
		}
	}
}

/*
 * The concrete Metal command-buffer class is private, but it inherits NSObject.
 * A category method with a private selector therefore gives this one translation
 * unit a narrow interception point without changing the Metal object or queue.
 * The macro below rewrites only source-level presentDrawable selectors compiled
 * after this header; these implementations call the real selectors before that
 * macro is defined, avoiding recursion.
 */
@interface NSObject (MonadoMacOSPresentSlotReservation)
- (void)monadoPresentDrawable:(id<MTLDrawable>)drawable;
- (void)monadoPresentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)presentationTime;
@end

@implementation NSObject (MonadoMacOSPresentSlotReservation)
- (void)monadoPresentDrawable:(id<MTLDrawable>)drawable
{
	[(id<MTLCommandBuffer>)self presentDrawable:drawable];
}

- (void)monadoPresentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)presentationTime
{
	if (!macos_unique_present_slots_enabled() || !(presentationTime > 0.0)) {
		[(id<MTLCommandBuffer>)self presentDrawable:drawable atTime:presentationTime];
		return;
	}

	double requested_double_ns = presentationTime * 1000000000.0;
	uint_fast64_t requested_ns =
	    requested_double_ns >= (double)UINT_FAST64_MAX ? UINT_FAST64_MAX : (uint_fast64_t)llround(requested_double_ns);
	uint_fast64_t reserved_ns = macos_reserve_present_slot_host_ns(requested_ns);
	CFTimeInterval reserved_time = (CFTimeInterval)((double)reserved_ns / 1000000000.0);
	[(id<MTLCommandBuffer>)self presentDrawable:drawable atTime:reserved_time];
}
@end

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

/* Define these only after the real libc/Metal functions above have been referenced. */
#define fflush macos_trace_buffered_fflush
#define setvbuf macos_trace_buffered_setvbuf
#define presentDrawable monadoPresentDrawable
