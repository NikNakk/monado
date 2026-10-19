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

layout (binding = 0) uniform sampler2D tex_sampler;

layout (location = 0)  in vec2 in_ruv;
layout (location = 1)  in vec2 in_guv;
layout (location = 2)  in vec2 in_buv;
layout (location = 0) out vec4 out_color;


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

	float r = texture(tex_sampler, r_uv).x;
	float g = texture(tex_sampler, g_uv).y;
	float b = texture(tex_sampler, b_uv).z;

	out_color = vec4(r, g, b, 1.0);
}
