// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "xrt/xrt_compositor.h"

// Diagnostic bit mask: zero means the existing single-projection policy allows
// distortion-only rendering. Keep synchronized with comp_compositor.c's gates.
enum comp_projection_fast_path_rejection
{
	COMP_FAST_PATH_LAYER_COUNT = 1u << 0,
	COMP_FAST_PATH_PASSTHROUGH = 1u << 1,
	COMP_FAST_PATH_PEEK = 1u << 2,
	COMP_FAST_PATH_DEBUG_MIRROR = 1u << 3,
	COMP_FAST_PATH_DEBUG_DISABLED = 1u << 4,
	COMP_FAST_PATH_LAYER_TYPE = 1u << 5,
	COMP_FAST_PATH_COLOR_SCALE_BIAS = 1u << 6,
	COMP_FAST_PATH_CHROMA_KEY = 1u << 7,
};

static inline uint32_t
comp_projection_fast_path_rejections(uint32_t layer_count,
                                     const struct xrt_layer_data *first,
                                     bool passthrough,
                                     bool peek,
                                     bool debug_mirror,
                                     bool debug_disabled)
{
	uint32_t mask = (passthrough ? COMP_FAST_PATH_PASSTHROUGH : 0) | (peek ? COMP_FAST_PATH_PEEK : 0) |
	                (debug_mirror ? COMP_FAST_PATH_DEBUG_MIRROR : 0) |
	                (debug_disabled ? COMP_FAST_PATH_DEBUG_DISABLED : 0);
	if (layer_count != 1 || first == NULL)
		return mask | COMP_FAST_PATH_LAYER_COUNT;
	if (first->flags & XRT_LAYER_COMPOSITION_COLOR_BIAS_SCALE)
		mask |= COMP_FAST_PATH_COLOR_SCALE_BIAS;
	if (first->type == XRT_LAYER_PROJECTION) {
		if (first->proj.chroma_key.curve > 0.0f)
			mask |= COMP_FAST_PATH_CHROMA_KEY;
	} else if (first->type == XRT_LAYER_PROJECTION_DEPTH) {
		if (first->depth.chroma_key.curve > 0.0f)
			mask |= COMP_FAST_PATH_CHROMA_KEY;
	} else {
		mask |= COMP_FAST_PATH_LAYER_TYPE;
	}
	return mask;
}
