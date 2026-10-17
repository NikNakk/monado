// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <stdint.h>

// Zero is the Vulkan sentinel for an unspecified/immediate absolute time.
static inline uint64_t
comp_target_timing_apply_offset(uint64_t timestamp_ns, int64_t offset_ns)
{
	return timestamp_ns == 0 ? 0 : (uint64_t)((int64_t)timestamp_ns + offset_ns);
}
