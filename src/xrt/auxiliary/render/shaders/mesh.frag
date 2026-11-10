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

// [0]: the source, [1]: the quad-view inset (the source again without one).
layout (binding = 0) uniform sampler2D tex_sampler[2];

layout (binding = 1, std140) uniform Config
{
	vec4 vertex_rot;
	vec4 post_transform;
	vec4 pre_transform;
	mat4 transform_scanout_begin;
	mat4 transform_scanout_end;
	vec4 inset_from_context;
	vec4 inset_post_transform;
	uvec4 has_inset;
} ubo;

layout (location = 0)  in vec2 in_ruv;
layout (location = 1)  in vec2 in_guv;
layout (location = 2)  in vec2 in_buv;
layout (location = 3)  in vec2 in_inset_ruv;
layout (location = 4)  in vec2 in_inset_guv;
layout (location = 5)  in vec2 in_inset_buv;
layout (location = 0) out vec4 out_color;

// Matches RENDER_LAYER_DEFAULT_INSET_BLEND_EDGE used by the layer squasher.
const float k_inset_blend_edge = 0.05;

float inset_blend_factor(vec2 uv)
{
	vec2 f = min(smoothstep(vec2(0.0), vec2(k_inset_blend_edge), uv),
	             vec2(1.0) - smoothstep(vec2(1.0 - k_inset_blend_edge), vec2(1.0), uv));
	return min(f.x, f.y);
}


void main()
{
	vec2 r_uv = in_ruv;
	vec2 g_uv = in_guv;
	vec2 b_uv = in_buv;

	// Per fragment: the map is piecewise linear, so it can't be applied to
	// the interpolated mesh vertices.
	if (foveation_enabled()) {
		r_uv = transform_uv_foveation(r_uv);
		g_uv = transform_uv_foveation(g_uv);
		b_uv = transform_uv_foveation(b_uv);
	}

	vec3 colour = vec3(texture(tex_sampler[0], r_uv).x, texture(tex_sampler[0], g_uv).y,
	                   texture(tex_sampler[0], b_uv).z);

	// Quad views: blend the inset over its context view, per colour channel.
	if (ubo.has_inset.x != 0u) {
		vec3 f = vec3(inset_blend_factor(in_inset_ruv), inset_blend_factor(in_inset_guv),
		              inset_blend_factor(in_inset_buv));
		if (any(greaterThan(f, vec3(0.0)))) {
			vec4 p = ubo.inset_post_transform;
			vec3 inset = vec3(texture(tex_sampler[1], fma(in_inset_ruv, p.zw, p.xy)).x,
			                  texture(tex_sampler[1], fma(in_inset_guv, p.zw, p.xy)).y,
			                  texture(tex_sampler[1], fma(in_inset_buv, p.zw, p.xy)).z);
			colour = mix(colour, inset, f);
		}
	}

	out_color = vec4(colour, 1.0);
}
