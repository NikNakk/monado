// Copyright 2017, James Sarrett.
// Copyright 2017, Bastiaan Olij.
// Copyright 2019, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
// Author: James Sarrett <jsarrett@gmail.com>
// Author: Bastiaan Olij <mux213@gmail.com>
// Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
// Author: Pete Black <pete.black@collabora.com>

#version 450

#extension GL_GOOGLE_include_directive : require

// Has to match mesh.foveation_binding in render_resources.c.
#define FOVEATION_BINDING 2
#include "foveation.inc.glsl"
#include "source_filter.inc.glsl"

// Source filter, SOURCE_FILTER_* in source_filter.inc.glsl.
layout(constant_id = 0) const uint k_source_filter = 0;

// [0]: the source, [1]: the quad-view inset (the source again without one).
layout (binding = 0) uniform sampler2D tex_sampler[2];

layout (binding = 1, std140) uniform Config
{
	vec4 vertex_rot;
	vec4 post_transform;
	vec4 pre_transform;
	mat4 transform_scanout_begin;
	mat4 transform_scanout_end;
	// With has_inset.y (depth) these hold the depth sub image rect and the
	// source UV to tangent rect instead.
	vec4 inset_from_context;
	vec4 inset_post_transform;
	// x: has inset, y: has depth (the second source is the depth image).
	uvec4 has_inset;
	vec4 projection_depth;
	vec4 new_origin;
} ubo;

layout (location = 0)  in vec2 in_ruv;
layout (location = 1)  in vec2 in_guv;
layout (location = 2)  in vec2 in_buv;
layout (location = 3)  in vec2 in_inset_ruv;
layout (location = 4)  in vec2 in_inset_guv;
layout (location = 5)  in vec2 in_inset_buv;
layout (location = 6)  in vec2 in_g_view_uv;
layout (location = 0) out vec4 out_color;

// Matches RENDER_LAYER_DEFAULT_INSET_BLEND_EDGE used by the layer squasher.
const float k_inset_blend_edge = 0.05;

float inset_blend_factor(vec2 uv)
{
	vec2 f = min(smoothstep(vec2(0.0), vec2(k_inset_blend_edge), uv),
	             vec2(1.0) - smoothstep(vec2(1.0 - k_inset_blend_edge), vec2(1.0), uv));
	return min(f.x, f.y);
}


// Depth reprojection, the compute distortion pass's depth_correct_projection_uv:
// one depth sample at the rotationally timewarped source UV, then the ray from
// the new eye origin intersected with that depth.
float projection_depth_to_view_z_distance(float window_depth)
{
	const vec4 d = ubo.projection_depth; // min depth, max depth, near z, far z
	const float range = d.y - d.x;
	if (!(range > 0.0) || !(d.z > 0.0) || !(d.w > 0.0)) {
		return 0.0;
	}
	const float t = clamp((window_depth - d.x) / range, 0.0, 1.0);
	const float inverse_distance = mix(1.0 / d.z, 1.0 / d.w, t);
	return inverse_distance > 1e-8 ? 1.0 / inverse_distance : 0.0;
}

vec2 depth_correct_view_uv(vec2 view_uv)
{
	if (any(lessThan(view_uv, vec2(0.0))) || any(greaterThan(view_uv, vec2(1.0)))) {
		return view_uv;
	}
	const vec4 depth_post_transform = ubo.inset_from_context;
	const vec4 source_uv_to_tanangle = ubo.inset_post_transform;
	vec2 depth_uv = fma(view_uv, depth_post_transform.zw, depth_post_transform.xy);
	float z = projection_depth_to_view_z_distance(textureLod(tex_sampler[1], depth_uv, 0.0).r);
	if (!(z > 0.0)) {
		return view_uv;
	}
	vec3 origin = ubo.new_origin.xyz;
	vec2 extent = source_uv_to_tanangle.zw;
	if (abs(extent.x) < 1e-6 || abs(extent.y) < 1e-6) {
		return view_uv;
	}
	vec2 tan_uv = fma(view_uv, extent, source_uv_to_tanangle.xy);
	float ray_scale = z + origin.z;
	if (!(ray_scale > 0.0)) {
		return view_uv;
	}
	vec2 corrected = vec2((origin.x + ray_scale * tan_uv.x) / z, (-origin.y + ray_scale * tan_uv.y) / z);
	return (corrected - source_uv_to_tanangle.xy) / extent;
}

void main()
{
	vec2 r_uv = in_ruv;
	vec2 g_uv = in_guv;
	vec2 b_uv = in_buv;

	// Depth reprojection: green's correction, applied to all three channels
	// (one depth sample), carried through the affine sub image transform.
	if (ubo.has_inset.y != 0u && !foveation_enabled()) {
		vec2 delta = (depth_correct_view_uv(in_g_view_uv) - in_g_view_uv) * ubo.post_transform.zw;
		r_uv += delta;
		g_uv += delta;
		b_uv += delta;
	}

	// Per fragment: the map is piecewise linear, so it can't be applied to
	// the interpolated mesh vertices.
	if (foveation_enabled()) {
		r_uv = transform_uv_foveation(r_uv);
		g_uv = transform_uv_foveation(g_uv);
		b_uv = transform_uv_foveation(b_uv);
	}

	// Foveated sources are compact physical images: keep the hardware filter.
	uint filter_mode = foveation_enabled() ? SOURCE_FILTER_BILINEAR : k_source_filter;
	vec4 rect = ubo.post_transform;
	vec3 colour = vec3(sample_source(tex_sampler[0], r_uv, rect, filter_mode, 0u).x,
	                   sample_source(tex_sampler[0], g_uv, rect, filter_mode, 1u).y,
	                   sample_source(tex_sampler[0], b_uv, rect, filter_mode, 2u).z);

	// Quad views: blend the inset over its context view, per colour channel.
	if (ubo.has_inset.x != 0u) {
		vec3 f = vec3(inset_blend_factor(in_inset_ruv), inset_blend_factor(in_inset_guv),
		              inset_blend_factor(in_inset_buv));
		if (any(greaterThan(f, vec3(0.0)))) {
			vec4 p = ubo.inset_post_transform;
			uint inset_filter = k_source_filter;
			vec3 inset = vec3(sample_source(tex_sampler[1], fma(in_inset_ruv, p.zw, p.xy), p, inset_filter, 0u).x,
			                  sample_source(tex_sampler[1], fma(in_inset_guv, p.zw, p.xy), p, inset_filter, 1u).y,
			                  sample_source(tex_sampler[1], fma(in_inset_buv, p.zw, p.xy), p, inset_filter, 2u).z);
			colour = mix(colour, inset, f);
		}
	}

	out_color = vec4(colour, 1.0);
}
