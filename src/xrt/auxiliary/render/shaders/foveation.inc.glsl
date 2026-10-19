// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

/*
 * Logical-to-physical reconstruction of a foveated (Metal
 * variable-rasterization-rate) source image for the graphics compositor path.
 * Same transform as distortion.comp and layer.comp, see
 * render_gfx_foveation_ubo_data and doc/macos-openxr-foveation.md.
 *
 * The includer defines FOVEATION_BINDING before including this file.
 */
#ifndef FOVEATION_INC_GLSL
#define FOVEATION_INC_GLSL

const uint FOVEATION_BOUNDARY_COUNT = 129u;
const uint FOVEATION_BOUNDARY_VEC4_COUNT = 33u;

layout(binding = FOVEATION_BINDING, std140) uniform Foveation
{
	// x: enabled, y: boundary count.
	uvec4 info;
	vec4 x[FOVEATION_BOUNDARY_VEC4_COUNT];
	vec4 y[FOVEATION_BOUNDARY_VEC4_COUNT];
} foveation;

bool foveation_enabled()
{
	return foveation.info.x != 0u && foveation.info.y == FOVEATION_BOUNDARY_COUNT;
}

// uv is normalized to the full source texture, after the subimage transform.
vec2 transform_uv_foveation(vec2 uv)
{
	// Preserve out-of-range UVs for the border-black sampler.
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return uv;
	}

	const float intervals = float(FOVEATION_BOUNDARY_COUNT - 1u);
	vec2 scaled = min(uv * intervals, vec2(intervals - 1e-6));
	uvec2 cell = uvec2(floor(scaled));
	vec2 local = fract(scaled);

	uvec2 next = cell + 1u;
	float x0 = foveation.x[cell.x / 4u][cell.x % 4u];
	float x1 = foveation.x[next.x / 4u][next.x % 4u];
	float y0 = foveation.y[cell.y / 4u][cell.y % 4u];
	float y1 = foveation.y[next.y / 4u][next.y % 4u];

	return vec2(mix(x0, x1, local.x), mix(y0, y1, local.y));
}

#endif // FOVEATION_INC_GLSL
