// SPDX-License-Identifier: BSL-1.0
#version 450
layout(push_constant) uniform Params { uint frame; uint burn; uint width; uint height; } pc;
layout(location = 0) out vec4 color;
void main() {
    // Dependency chain makes burn work observable and prevents dead-code removal.
    float v = gl_FragCoord.x / float(pc.width);
    for (uint i = 0; i < pc.burn; ++i) v = fract(v * 1.000013 + 0.137);
    float c = ((pc.frame & 1u) == 0u) ? 0.1 : 0.8;
    color = vec4(c, c + v * 0.04, c, 1.0);
    // 24-bit frame ID barcode, top strip; a moving marker below it.
    if (gl_FragCoord.y < float(pc.height) * 0.08) {
        uint bit = min(uint(gl_FragCoord.x * 24.0 / float(pc.width)), 23u);
        color = vec4(vec3(float((pc.frame >> bit) & 1u)), 1.0);
    } else if (abs(gl_FragCoord.x - float(pc.frame % pc.width)) < 4.0) {
        color = vec4(1.0, 0.0, 0.0, 1.0);
    }
}
