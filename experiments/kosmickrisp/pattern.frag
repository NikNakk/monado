// Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0
#version 450
layout(push_constant) uniform Frame { uint value; } frame;
layout(location=0) out vec4 color;
void main()
{
    uvec2 p = uvec2(gl_FragCoord.xy);
    color = vec4(float(frame.value%251),float((p.x/8+frame.value*3)%251),float((p.y/8+frame.value*5)%251),255)/255;
}
