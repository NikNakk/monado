// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <stdint.h>
#include <stdbool.h>

//! Project the next output timestamp to the most recent boundary, without overflow.
static inline uint64_t
comp_macos_last_vblank_ns(uint64_t output_ns, uint64_t now_ns, uint64_t period_ns)
{
	if (output_ns == 0 || now_ns == 0 || period_ns == 0) {
		return now_ns;
	}
	if (output_ns > now_ns) {
		uint64_t remainder = (output_ns - now_ns) % period_ns;
		uint64_t back = remainder == 0 ? 0 : period_ns - remainder;
		return now_ns >= back ? now_ns - back : 0;
	}
	return now_ns - (now_ns - output_ns) % period_ns;
}

//! Ignore imprecise clock samples: CPU preemption is not a clock-domain jump.
static inline bool
comp_macos_clock_bridge_changed(int64_t before_ns, int64_t host_ns, int64_t after_ns, int64_t fixed_offset_ns)
{
	if (after_ns < before_ns || after_ns - before_ns > 1000000) {
		return false;
	}
	int64_t offset = before_ns + (after_ns - before_ns) / 2 - host_ns;
	return offset > fixed_offset_ns ? offset - fixed_offset_ns > 5000000 : fixed_offset_ns - offset > 5000000;
}
