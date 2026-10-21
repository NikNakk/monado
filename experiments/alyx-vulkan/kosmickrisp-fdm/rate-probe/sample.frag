#version 450
#extension GL_EXT_multiview : require
layout(set=0, binding=0) uniform sampler2DArray src;
layout(location=0) out vec4 color;
void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(src, 0).xy);
    color = texture(src, vec3(uv, float(gl_ViewIndex)));
}
