#version 450
layout(input_attachment_index=0, set=0, binding=0) uniform subpassInputMS color_in;
layout(input_attachment_index=1, set=0, binding=1) uniform subpassInputMS depth_in;
layout(location=0) out vec4 color;
void main() {
    vec4 c = (subpassLoad(color_in, 0) + subpassLoad(color_in, 1)) * 0.5;
    float d = subpassLoad(depth_in, 0).r;
    color = vec4(c.b, d, c.r, 1.0);
    gl_FragDepth = d;
}
