#version 450
#extension GL_EXT_fragment_invocation_density : require
#extension GL_EXT_multiview : require
layout(location=0) out vec4 color;
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float chk = float((p.x + p.y + gl_ViewIndex) & 1);
    color = vec4(chk, float(gl_FragSizeEXT.x) * 0.125, float(gl_FragSizeEXT.y) * 0.125,
                 float(gl_FragInvocationCountEXT) * 0.125);
}
