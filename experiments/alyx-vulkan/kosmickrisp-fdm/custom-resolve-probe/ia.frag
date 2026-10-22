#version 450
layout(input_attachment_index=0, set=0, binding=0) uniform subpassInputMS color_in;
layout(location=0) out vec4 color;
void main() {
    vec4 c = subpassLoad(color_in, 0);
    float l = dot(c.rgb, vec3(0.2125, 0.7154, 0.0721));
    if (l <= 0.0)
        discard;
    color = vec4(1.0);
}
