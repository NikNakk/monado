// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#version 450

// Exact per-pixel transfer of the private compositor output, alpha included.
layout(set = 0, binding = 0) uniform sampler2D source;
layout(location = 0) out vec4 color;

void
main()
{
	color = texelFetch(source, ivec2(gl_FragCoord.xy), 0);
}
