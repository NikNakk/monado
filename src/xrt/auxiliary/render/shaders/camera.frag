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
//! World pose of one eye's view: orientation (x, y, z, w) and position (xyz).
struct View
{
	vec4 orientation;
	vec4 position;
};
// Matches render_camera_mode.
const uint MODE_STATIC = 0u;
const uint MODE_ROTATE = 1u;
const uint MODE_ROOM = 2u;
layout(set = 0, binding = 3, std140) uniform Params
{
	float brightness;
	uint has_app;
	uint map_size;
	uint mode;
	Camera projection[2];
	View views[2];
}
params;

/*
 * Simulated room: a box with a floor at y = 0, so the background has fixed
 * geometry that should stay locked to the application's world.
 */
const vec3 ROOM_MIN = vec3(-2.5, 0.0, -2.5);
const vec3 ROOM_MAX = vec3(2.5, 2.7, 2.5);
const float GRID_M = 0.5;
const float LINE_M = 0.015;

vec3
rotate_vector(vec4 q, vec3 v)
{
	vec3 t = 2.0 * cross(q.xyz, v);
	return v + q.w * t + cross(q.xyz, t);
}

//! Distance to the nearest grid line in the face's two in-plane axes.
float
grid_line(vec2 p)
{
	vec2 d = abs(p / GRID_M - round(p / GRID_M)) * GRID_M;
	return 1.0 - smoothstep(0.0, LINE_M, min(d.x, d.y));
}

//! Optical tangent ray, bilinearly interpolated: 32-bit float textures need not
//! support filtering, and nearest sampling would make the room blocky.
vec2
map_tangent(bool right, vec2 local_uv)
{
	vec2 pos = local_uv * float(params.map_size) - 0.5;
	ivec2 i0 = ivec2(floor(pos));
	vec2 f = pos - vec2(i0);
	ivec2 last = ivec2(params.map_size - 1);
	ivec2 a = clamp(i0, ivec2(0), last);
	ivec2 b = clamp(i0 + 1, ivec2(0), last);
	// Constant descriptor indices avoid requiring dynamic sampled-image indexing.
	vec2 t00 = right ? texelFetch(mappings[1], a, 0).zw : texelFetch(mappings[0], a, 0).zw;
	vec2 t10 = right ? texelFetch(mappings[1], ivec2(b.x, a.y), 0).zw : texelFetch(mappings[0], ivec2(b.x, a.y), 0).zw;
	vec2 t01 = right ? texelFetch(mappings[1], ivec2(a.x, b.y), 0).zw : texelFetch(mappings[0], ivec2(a.x, b.y), 0).zw;
	vec2 t11 = right ? texelFetch(mappings[1], b, 0).zw : texelFetch(mappings[0], b, 0).zw;
	return mix(mix(t00, t10, f.x), mix(t01, t11, f.x), f.y);
}

vec3
room_color(vec2 tangent, View view)
{
	vec3 dir = rotate_vector(view.orientation, normalize(vec3(tangent.x, -tangent.y, -1.0)));
	vec3 origin = view.position.xyz;
	if (any(isnan(dir)) || any(lessThan(origin, ROOM_MIN)) || any(greaterThan(origin, ROOM_MAX)))
		return vec3(0);
	// Inside the box the ray leaves through exactly one face: the nearest exit.
	vec3 bound = mix(ROOM_MIN, ROOM_MAX, greaterThan(dir, vec3(0)));
	vec3 t = vec3(1e30);
	for (int i = 0; i < 3; i++)
		if (abs(dir[i]) > 1e-8)
			t[i] = (bound[i] - origin[i]) / dir[i];
	float hit = min(t.x, min(t.y, t.z));
	vec3 p = origin + dir * hit;
	vec3 base;
	vec2 plane;
	if (hit == t.y) {
		base = dir.y < 0.0 ? vec3(0.35, 0.55, 0.35) : vec3(0.8, 0.8, 0.8); // Floor, ceiling.
		plane = p.xz;
	} else if (hit == t.z) {
		base = dir.z < 0.0 ? vec3(0.35, 0.45, 0.75) : vec3(0.75, 0.65, 0.35); // Front (-Z), back.
		plane = p.xy;
	} else {
		base = dir.x < 0.0 ? vec3(0.75, 0.4, 0.4) : vec3(0.55, 0.4, 0.7); // Left (-X), right.
		plane = p.zy;
	}
	return base * mix(1.0, 0.5, grid_line(plane));
}

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
	if (params.mode == MODE_ROOM) {
		vec2 tangent = map_tangent(right, local_uv);
		vec3 room = right ? room_color(tangent, params.views[1]) : room_color(tangent, params.views[0]);
		vec4 app = params.has_app != 0 ? texelFetch(application, xy, 0) : vec4(0);
		target = vec4(app.rgb + room * (1.0 - app.a), 1);
		return;
	}
	if (params.mode == MODE_ROTATE) {
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
