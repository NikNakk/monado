// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: MIT
#version 450
#ifndef SAMPLES
#define SAMPLES 1
#endif
#if SAMPLES == 1
layout(input_attachment_index=0, set=0, binding=0) uniform subpassInput source;
#else
layout(input_attachment_index=0, set=0, binding=0) uniform subpassInputMS source;
#endif
layout(location=0) out vec4 color;
void main() {
#if SAMPLES == 1
    vec4 v = subpassLoad(source);
#else
    vec4 v = vec4(0.0);
    for (int i=0; i<SAMPLES; ++i) v += subpassLoad(source, i) / float(SAMPLES);
#endif
    color = vec4(v.b, v.r, v.g, v.a);
}
