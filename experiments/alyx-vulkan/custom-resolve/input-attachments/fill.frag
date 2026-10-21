#version 450
#extension GL_EXT_multiview : require
layout(location=0) out vec4 color;
void main() { color = vec4(gl_ViewIndex == 0 ? 0.25 : 0.5, 0.5, 0.75, 1.0); }
