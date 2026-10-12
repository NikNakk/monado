// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Caller serializes feedback and submission access. Times are monotonic ns. */
struct macos_latency_guard
{
	uint64_t latest_frame;
	uint64_t ignore_through_frame;
	uint64_t latest_present_ns;
	uint64_t deep_since_ns;
	uint64_t last_drain_ns;
	uint64_t suppress_until_ns;
	uint64_t drains;
};

static inline void
macos_latency_guard_observe(struct macos_latency_guard *guard,
                            uint64_t frame,
                            uint64_t presented_ns,
                            int64_t depth,
                            uint64_t period)
{
	if (presented_ns == 0 || depth < 0 || frame <= guard->ignore_through_frame || frame <= guard->latest_frame) {
		return;
	}
	if (period == 0 || (guard->latest_present_ns != 0 &&
	                    (presented_ns < guard->latest_present_ns ||
	                     presented_ns - guard->latest_present_ns > 4 * period))) {
		guard->deep_since_ns = 0;
	}
	guard->latest_frame = frame;
	guard->latest_present_ns = presented_ns;
	if (depth < 2) {
		guard->deep_since_ns = 0;
	} else if (guard->deep_since_ns == 0 && period != 0) {
		guard->deep_since_ns = presented_ns;
	}
}

/* Withhold presentation for a full refresh, rather than immediately catching
 * up with a burst. Require one second of consecutive deep presents and allow
 * at most one drain per ten seconds. Old in-flight feedback cannot retrigger. */
static inline bool
macos_latency_guard_should_skip(struct macos_latency_guard *guard, uint64_t frame, uint64_t now, uint64_t period)
{
	if (period == 0) {
		return false;
	}
	if (now < guard->suppress_until_ns) {
		return true;
	}
	if (guard->deep_since_ns == 0 || now < guard->latest_present_ns ||
	    now - guard->latest_present_ns > 4 * period || now < guard->deep_since_ns ||
	    now - guard->deep_since_ns < 1000000000ULL ||
	    (guard->last_drain_ns != 0 && (now < guard->last_drain_ns || now - guard->last_drain_ns < 10000000000ULL))) {
		return false;
	}
	guard->deep_since_ns = 0;
	guard->ignore_through_frame = frame;
	guard->last_drain_ns = now;
	guard->suppress_until_ns = now + period;
	guard->drains++;
	return true;
}
