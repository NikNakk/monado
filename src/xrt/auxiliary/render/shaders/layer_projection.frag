// Copyright 2020-2023, Collabora Ltd.
//
// Author: Jakob Bornecrantz <jakob@collabora.com>
// Author: Lubosz Sarnecki <lubosz.sarnecki@collabora.com>
//
// SPDX-License-Identifier: BSL-1.0

#version 460

#extension GL_GOOGLE_include_directive : require

// Has to match RENDER_BINDING_LAYER_PROJECTION_FOVEATION in render_interface.h.
#define FOVEATION_BINDING 2
#include "foveation.inc.glsl"

layout (binding = 0, std140) uniform Config
{
	vec4 post_transform;
	vec4 to_tangent;
	mat4 mvp;
	vec4 color_scale;
	vec4 color_bias;
} ubo;

layout (binding = 1) uniform sampler2D image;

layout (location = 0)  in vec2 uv;
layout (location = 0) out vec4 out_color;


void main ()
{
	vec2 image_uv = uv;
	if (foveation_enabled()) {
		image_uv = transform_uv_foveation(image_uv);
	}

	out_color = texture(image, image_uv);
	out_color = clamp(out_color * ubo.color_scale + ubo.color_bias, 0.0, 1.0);
}
