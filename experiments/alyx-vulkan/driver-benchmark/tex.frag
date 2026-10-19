#version 450
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
layout(push_constant) uniform PC { uint iters; uint frame; } pc;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 0) out vec4 o;
void main() {
    vec2 sz = vec2(textureSize(src, 0));
    vec2 uv = gl_FragCoord.xy / sz;
    // Distortion-like warp with several taps, like a compositor's
    // chromatic-aberration-corrected lens pass.
    vec2 d = uv - 0.5;
    float r2 = dot(d, d);
    vec4 acc = vec4(0.0);
    for (uint i = 0u; i < pc.iters; i++) {
        float k = 1.0 + (0.22 + 0.01 * float(i)) * r2;
        acc += texture(src, 0.5 + d * k + float(pc.frame & 1u) * 1e-4);
    }
    o = acc / float(max(pc.iters, 1u));
}
