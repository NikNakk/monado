// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <stdint.h>

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
