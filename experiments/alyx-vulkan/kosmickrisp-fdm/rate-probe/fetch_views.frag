#version 450
#extension GL_EXT_multiview : require
layout(set=0, binding=0) uniform sampler2DArray src0;
layout(set=0, binding=1) uniform sampler2DArray src1;
layout(location=0) out vec4 color;
void main() {
    ivec3 p = ivec3(gl_FragCoord.xy, 0);
    color = gl_ViewIndex == 0 ? texelFetch(src0, p, 0) : texelFetch(src1, p, 0);
}
