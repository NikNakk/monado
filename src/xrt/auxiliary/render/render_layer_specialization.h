// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "render_interface.h"
#include "shaders/layer_defines.inc.glsl"

// Check the final per-view shader inputs after visibility filtering and limits.
// A common pipeline may only omit features whose absence is established here.
static inline bool
render_layer_common_eligible(const struct render_compute_layer_ubo_data *data,
                             const struct render_compute_layer_foveation_ubo_data *foveation)
{
	uint32_t count = data->layer_count.value;
	if (count < 2 || count > RENDER_MAX_LAYERS ||
	    data->layers[0].layer_data.layer_type != LAYER_COMP_TYPE_PROJECTION) {
		return false;
	}
	for (uint32_t i = 0; i < count; ++i) {
		if (i > 0 && data->layers[i].layer_data.layer_type != LAYER_COMP_TYPE_QUAD) return false;
		if (data->layers[i].image_info.has_depth != 0 || foveation->layer_slot[i] != 0 ||
		    !(data->layers[i].chroma_key.curve <= 0.0f)) return false;
	}
	return true;
}
