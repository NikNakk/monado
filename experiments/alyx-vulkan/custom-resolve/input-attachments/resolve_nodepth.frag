#version 450
layout(input_attachment_index=0, set=0, binding=0) uniform subpassInputMS color_in;
layout(location=0) out vec4 color;
void main() {
    vec4 c = (subpassLoad(color_in, 0) + subpassLoad(color_in, 1)) * 0.5;
    color = vec4(c.b, 0.0, c.r, 1.0);
}
