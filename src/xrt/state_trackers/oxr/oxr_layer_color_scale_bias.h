// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "xrt/xrt_openxr_includes.h"
#include "xrt/xrt_compositor.h"

/*!
 * Convert an optional color transform into native layer data. An exact identity
 * needs no transform flag, allowing the compositor's existing fast paths.
 * Do not use a tolerance: even small non-identity transforms must be applied.
 * The caller initializes layer flags before calling this once per submission.
 */
static inline void
oxr_layer_set_color_scale_bias(const XrCompositionLayerColorScaleBiasKHR *transform, struct xrt_layer_data *data)
{
	if (transform == NULL) {
		return;
	}

	const XrColor4f *scale = &transform->colorScale;
	const XrColor4f *bias = &transform->colorBias;
	data->color_scale.r = scale->r;
	data->color_scale.g = scale->g;
	data->color_scale.b = scale->b;
	data->color_scale.a = scale->a;
	data->color_bias.r = bias->r;
	data->color_bias.g = bias->g;
	data->color_bias.b = bias->b;
	data->color_bias.a = bias->a;

	if (scale->r == 1.0f && scale->g == 1.0f && scale->b == 1.0f && scale->a == 1.0f && bias->r == 0.0f &&
	    bias->g == 0.0f && bias->b == 0.0f && bias->a == 0.0f) {
		return;
	}

	data->flags = (enum xrt_layer_composition_flags)(data->flags | XRT_LAYER_COMPOSITION_COLOR_BIAS_SCALE);
}
