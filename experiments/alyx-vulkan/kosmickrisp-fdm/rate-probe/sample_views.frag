#version 450
#extension GL_EXT_multiview : require
layout(set=0, binding=0) uniform sampler2DArray src0;
layout(set=0, binding=1) uniform sampler2DArray src1;
layout(location=0) out vec4 color;
void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(src0, 0).xy);
    color = gl_ViewIndex == 0 ? texture(src0, vec3(uv, 0.0)) : texture(src1, vec3(uv, 0.0));
}
