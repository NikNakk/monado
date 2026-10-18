// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#version 450

layout(set = 0, binding = 0) uniform sampler2D application;
layout(set = 0, binding = 1) uniform sampler2D cameras[2];
layout(set = 0, binding = 2) uniform sampler2D mappings[2];
layout(location = 0) out vec4 target;
struct Camera
{
	vec4 intrinsics;
	vec4 k;
	vec4 rows[3];
};
layout(set = 0, binding = 3, std140) uniform Params
{
	float brightness;
	uint has_app;
	uint map_size;
	uint rotate;
	Camera projection[2];
}
params;

vec2
camera_uv(vec2 tangent, Camera c, ivec2 extent)
{
	vec3 head = vec3(tangent.x, -tangent.y, -1.0);
	vec3 ray = vec3(dot(c.rows[0].xyz, head), -dot(c.rows[1].xyz, head), -dot(c.rows[2].xyz, head));
	if (any(isnan(ray)) || any(isinf(ray)) || ray.z <= 0.0)
		return vec2(-1.0);
	float r = length(ray.xy);
	float theta = atan(r, ray.z);
	float t2 = theta * theta;
	float d = theta * (1.0 + t2 * (c.k.x + t2 * (c.k.y + t2 * (c.k.z + t2 * c.k.w))));
	vec2 pixel = c.intrinsics.xy * ray.xy * (r > 1e-7 ? d / r : 0.0) + c.intrinsics.zw;
	vec2 uv = (pixel + 0.5) / vec2(extent);
	return !any(isnan(uv)) && !any(isinf(uv)) && d >= 0.0 && all(greaterThanEqual(uv, vec2(0))) &&
	               all(lessThanEqual(uv, vec2(1)))
	           ? uv
		   : vec2(-1.0);
}

void
main()
{
	ivec2 xy = ivec2(gl_FragCoord.xy);
	ivec2 extent = textureSize(application, 0);
	vec2 uv = (vec2(xy) + 0.5) / vec2(extent);
	bool right = uv.x >= 0.5;
	vec2 local_uv = vec2(right ? (uv.x - 0.5) * 2.0 : uv.x * 2.0, uv.y);
	ivec2 mi = min(ivec2(local_uv * float(params.map_size)), ivec2(params.map_size - 1));
	// Constant descriptor indices avoid requiring dynamic sampled-image indexing.
	vec4 mapping = right ? texelFetch(mappings[1], mi, 0) : texelFetch(mappings[0], mi, 0);
	vec2 cuv = mapping.xy;
	if (params.rotate != 0) {
		cuv = right ? camera_uv(mapping.zw, params.projection[1], textureSize(cameras[1], 0))
		            : camera_uv(mapping.zw, params.projection[0], textureSize(cameras[0], 0));
	}
	vec3 camera = vec3(0);
	if (cuv.x >= 0.0 && cuv.y >= 0.0) {
		// Explicit LOD: implicit derivatives are undefined in this non-uniform branch.
		float gray = right ? textureLod(cameras[1], cuv, 0.0).r : textureLod(cameras[0], cuv, 0.0).r;
		camera = vec3(clamp(gray * params.brightness, 0.0, 1.0));
	}
	vec3 color = camera;
	if (params.has_app != 0) {
		vec4 app = texelFetch(application, xy, 0);
		color = app.rgb + camera * (1.0 - app.a);
	}
	target = vec4(color, 1);
}
