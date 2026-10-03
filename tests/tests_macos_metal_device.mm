// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#import <Metal/Metal.h>

extern "C" bool
tests_macos_has_metal_device(void)
{
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		const bool available = device != nil;
		[device release];
		return available;
	}
}
