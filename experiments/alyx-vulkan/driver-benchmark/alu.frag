#version 450
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
layout(push_constant) uniform PC { uint iters; uint frame; } pc;
layout(location = 0) out vec4 o;
void main() {
    vec2 uv = gl_FragCoord.xy * 0.001 + float(pc.frame) * 0.01;
    vec3 c = vec3(uv, 0.5);
    for (uint i = 0u; i < pc.iters; i++) {
        c = fract(sin(c.yzx * 12.9898 + c * 78.233) * 43758.5453);
    }
    o = vec4(c, 1.0);
}
