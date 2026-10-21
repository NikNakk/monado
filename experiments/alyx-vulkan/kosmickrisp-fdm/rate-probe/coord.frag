#version 450
#extension GL_EXT_multiview : require
layout(std430, set=0, binding=0) buffer Counter { uint invocations; };
layout(location=0) out vec4 color;
void main() {
    atomicAdd(invocations, 1u);
    color = vec4(gl_FragCoord.x / 672.0, gl_FragCoord.y / 768.0, float(gl_ViewIndex), 1.0);
}
