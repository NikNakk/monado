// Copyright 2019-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
// Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
// Author: Pete Black <pete.black@collabora.com>
// Author: Jakob Bornecrantz <jakob@collabora.com>

#version 450

// Should we do timewarp.
layout(constant_id = 0) const bool do_timewarp = false;

layout (binding = 1, std140) uniform Config
{
	vec4 vertex_rot;
	vec4 post_transform;

	// Only used for timewarp.
	vec4 pre_transform;
	mat4 transform_scanout_begin;
	mat4 transform_scanout_end;

	// Quad-view inset: UV = context UV (before post_transform) * zw + xy.
	vec4 inset_from_context;
	vec4 inset_post_transform;
	uvec4 has_inset;
} ubo;

layout (location = 0)  in vec4 in_pos_ruv;
layout (location = 1)  in vec4 in_guv_buv;
layout (location = 0) out vec2 out_r_uv;
layout (location = 1) out vec2 out_g_uv;
layout (location = 2) out vec2 out_b_uv;
// Inset UVs before its post transform; only used with has_inset.
layout (location = 3) out vec2 out_inset_r_uv;
layout (location = 4) out vec2 out_inset_g_uv;
layout (location = 5) out vec2 out_inset_b_uv;

out gl_PerVertex
{
	vec4 gl_Position;
};


vec2 transform_uv_subimage(vec2 uv)
{
	vec2 values = uv;

	// To deal with OpenGL flip and sub image view.
	values.xy = fma(values.xy, ubo.post_transform.zw, ubo.post_transform.xy);

	// Ready to be used.
	return values.xy;
}

vec2 transform_uv_timewarp(vec2 uv, float scanout_fraction)
{
	vec4 values = vec4(uv, -1, 1);

	// From uv to tan angle (tangent space).
	values.xy = fma(values.xy, ubo.pre_transform.zw, ubo.pre_transform.xy);
	values.y = -values.y; // Flip to OpenXR coordinate system.

	// Timewarp + scanout compensation.
	values = ubo.transform_scanout_begin * values * (1.0 - scanout_fraction) +
	         ubo.transform_scanout_end * values * scanout_fraction;
	values.xy = values.xy * (1.0 / max(values.w, 0.00001));

	// From [-1, 1] to [0, 1]
	values.xy = values.xy * 0.5 + 0.5;

	// Done; the sub image transform is applied separately.
	return values.xy;
}

// Source view UV, before the sub image transform.
vec2 transform_uv_view(vec2 uv, float scanout_fraction)
{
	return do_timewarp ? transform_uv_timewarp(uv, scanout_fraction) : uv;
}

void main()
{
	mat2x2 rot = {
		ubo.vertex_rot.xy,
		ubo.vertex_rot.zw,
	};

	vec2 pos = rot * in_pos_ruv.xy;
	gl_Position = vec4(pos, 0.0f, 1.0f);

	float scanout_fraction = pos.y * 0.5 + 0.5;

	vec2 r_uv = in_pos_ruv.zw;
	vec2 g_uv = in_guv_buv.xy;
	vec2 b_uv = in_guv_buv.zw;

	r_uv = transform_uv_view(r_uv, scanout_fraction);
	g_uv = transform_uv_view(g_uv, scanout_fraction);
	b_uv = transform_uv_view(b_uv, scanout_fraction);

	// The inset UV is affine in the context view UV, so it interpolates like it.
	vec4 a = ubo.inset_from_context;
	out_inset_r_uv = fma(r_uv, a.zw, a.xy);
	out_inset_g_uv = fma(g_uv, a.zw, a.xy);
	out_inset_b_uv = fma(b_uv, a.zw, a.xy);

	out_r_uv = transform_uv_subimage(r_uv);
	out_g_uv = transform_uv_subimage(g_uv);
	out_b_uv = transform_uv_subimage(b_uv);
}
