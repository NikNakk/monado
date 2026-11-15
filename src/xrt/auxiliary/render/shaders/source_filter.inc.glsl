// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
//
// Source filters for the distortion passes. Bilinear is one hardware sample;
// Catmull-Rom is a bicubic filter, sharper when the compositor magnifies the
// source (lower render scales), built from five bilinear samples.

#ifndef SOURCE_FILTER_INC_GLSL
#define SOURCE_FILTER_INC_GLSL

// Matches enum render_source_filter.
#define SOURCE_FILTER_BILINEAR 0u
#define SOURCE_FILTER_CATMULL_ROM 1u
#define SOURCE_FILTER_CATMULL_ROM_GREEN 2u

// Catmull-Rom sample of tex at uv. Sample positions are clamped to the
// sub image rect (xy origin, zw size, either may be negative for a flip), so
// a view sharing its swapchain image with another never reads its neighbour.
vec4
sample_catmull_rom(sampler2D tex, vec2 uv, vec4 rect)
{
	vec2 size = vec2(textureSize(tex, 0));
	vec2 half_texel = 0.5 / size;
	vec2 lo = min(rect.xy, rect.xy + rect.zw) + half_texel;
	vec2 hi = max(lo, max(rect.xy, rect.xy + rect.zw) - half_texel);

	vec2 pos = uv * size;
	vec2 p1 = floor(pos - 0.5) + 0.5;
	vec2 f = pos - p1;

	vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
	vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
	vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
	vec2 w3 = f * f * (-0.5 + 0.5 * f);

	// The middle two taps share one bilinear sample per axis.
	vec2 w12 = w1 + w2;
	vec2 t0 = clamp((p1 - 1.0) / size, lo, hi);
	vec2 t12 = clamp((p1 + w2 / w12) / size, lo, hi);
	vec2 t3 = clamp((p1 + 2.0) / size, lo, hi);

	// The four corner taps carry under 2% of the weight between them: drop
	// them and renormalise, five bilinear samples instead of nine.
	float wc = w12.x * w0.y;
	float wl = w0.x * w12.y;
	float wm = w12.x * w12.y;
	float wr = w3.x * w12.y;
	float wb = w12.x * w3.y;
	vec4 c = textureLod(tex, vec2(t12.x, t0.y), 0.0) * wc;
	c += textureLod(tex, vec2(t0.x, t12.y), 0.0) * wl;
	c += textureLod(tex, vec2(t12.x, t12.y), 0.0) * wm;
	c += textureLod(tex, vec2(t3.x, t12.y), 0.0) * wr;
	c += textureLod(tex, vec2(t12.x, t3.y), 0.0) * wb;
	c /= wc + wl + wm + wr + wb;

	// The negative lobes can overshoot below zero next to hard edges.
	return max(c, vec4(0.0));
}

// channel: 0 red, 1 green, 2 blue. SOURCE_FILTER_CATMULL_ROM_GREEN filters
// only green (most of the perceived sharpness) and keeps red and blue bilinear.
vec4
sample_source(sampler2D tex, vec2 uv, vec4 rect, uint filter_mode, uint channel)
{
	if (filter_mode == SOURCE_FILTER_CATMULL_ROM ||
	    (filter_mode == SOURCE_FILTER_CATMULL_ROM_GREEN && channel == 1u)) {
		return sample_catmull_rom(tex, uv, rect);
	}
	return texture(tex, uv);
}

#endif
