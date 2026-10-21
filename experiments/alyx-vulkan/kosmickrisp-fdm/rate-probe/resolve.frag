#version 450
layout(input_attachment_index=0, set=0, binding=0) uniform subpassInputMS color_in;
layout(location=0) out vec4 color;
void main() { color = (subpassLoad(color_in, 0) + subpassLoad(color_in, 1)) * 0.5; }
