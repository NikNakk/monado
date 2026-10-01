// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#import <AppKit/AppKit.h>
#include "main/comp_macos_remote_layer.h"
#include "catch_amalgamated.hpp"

TEST_CASE("private remote layer interfaces support a complete context and host lifecycle")
{
	@autoreleasepool {
		[NSApplication sharedApplication];
		if (!comp_macos_remote_layer_supported())
			SKIP("Remote layer interfaces are unavailable on this macOS release");
		CALayer *layer = [[CALayer alloc] init];
		CAContext *context = comp_macos_remote_layer_create_context(layer);
		[layer release];
		REQUIRE(context != nil);
		CALayerHost *host = comp_macos_remote_layer_create_host(context.contextId);
		CHECK(host != nil);
		CHECK(host.contextId == context.contextId);
		[host release];
		[context release];
	}
}
