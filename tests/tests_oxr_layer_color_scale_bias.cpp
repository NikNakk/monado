// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "catch_amalgamated.hpp"
#include "oxr/oxr_layer_color_scale_bias.h"

#include <cmath>
#include <limits>

static XrCompositionLayerColorScaleBiasKHR
identity()
{
	XrCompositionLayerColorScaleBiasKHR value{};
	value.type = XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR;
	value.colorScale = {1, 1, 1, 1};
	return value;
}

TEST_CASE("Optional OpenXR color transform preserves existing layer policy")
{
	for (auto type : {XRT_LAYER_PROJECTION, XRT_LAYER_PROJECTION_DEPTH}) {
		for (unsigned alpha : {0u, (unsigned)XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
		                       (unsigned)(XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
		                                  XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT)}) {
			for (bool present : {false, true}) {
				xrt_layer_data data{};
				data.type = type;
				data.flags = (xrt_layer_composition_flags)alpha;
				auto value = identity();
				value.colorBias.a = -0.0f; // Signed zero is also an identity.
				oxr_layer_set_color_scale_bias(present ? &value : nullptr, &data);
				CHECK((unsigned)data.flags == alpha);
				CHECK(data.type == type);
			}
		}
	}
}

TEST_CASE("Every non-identity OpenXR color component retains transformation")
{
	for (auto type : {XRT_LAYER_PROJECTION, XRT_LAYER_PROJECTION_DEPTH}) {
		for (bool change_scale : {false, true}) {
			for (unsigned channel = 0; channel < 4; ++channel) {
				for (float changed : {0.25f, -0.5f, std::nextafter(change_scale ? 1.0f : 0.0f, 2.0f),
				                      std::numeric_limits<float>::infinity(),
				                      std::numeric_limits<float>::quiet_NaN()}) {
					auto value = identity();
					XrColor4f &color = change_scale ? value.colorScale : value.colorBias;
					float *components[] = {&color.r, &color.g, &color.b, &color.a};
					*components[channel] = changed;
					xrt_layer_data data{};
					data.type = type;
					data.flags = XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT;
					oxr_layer_set_color_scale_bias(&value, &data);
					CHECK(data.flags == (XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT |
					                     XRT_LAYER_COMPOSITION_COLOR_BIAS_SCALE));
					const auto &result = change_scale ? data.color_scale : data.color_bias;
					float stored[] = {result.r, result.g, result.b, result.a};
					CHECK((std::isnan(changed) ? std::isnan(stored[channel])
					                           : stored[channel] == changed));
				}
			}
		}
	}
}
