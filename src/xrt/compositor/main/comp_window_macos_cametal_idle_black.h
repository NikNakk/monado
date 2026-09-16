// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Deterministic idle presentation for CAMetalDisplayLink drive mode.
 *
 * CAMetalDisplayLink hands the driver a drawable even when there is no active
 * Monado client. Presenting that untouched drawable can cycle stale contents
 * left in the layer's drawable pool after a client exits, producing a rapid
 * flicker between recent frames. This wrapper makes the direct drawable
 * `present` calls in comp_window_macos_cametal_drive.h render opaque black first.
 * Normal compositor frames use MTLCommandBuffer presentDrawable: and are not
 * affected.
 */
#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <dispatch/dispatch.h>

@interface NSObject (MonadoCAMetalDisplayLinkIdleBlack)
- (void)monadoCAMetalIdlePresent;
@end

@implementation NSObject (MonadoCAMetalDisplayLinkIdleBlack)
- (void)monadoCAMetalIdlePresent
{
	id<CAMetalDrawable> drawable = (id<CAMetalDrawable>)self;
	id<MTLTexture> texture = [drawable texture];
	id<MTLDevice> device = [texture device];

	static dispatch_once_t once_token;
	static id<MTLCommandQueue> idle_queue = nil;
	dispatch_once(&once_token, ^{
		if (device != nil) {
			idle_queue = [device newCommandQueue];
		}
	});

	if (idle_queue == nil || texture == nil) {
		[drawable present];
		return;
	}

	id<MTLCommandBuffer> command_buffer = [idle_queue commandBuffer];
	if (command_buffer == nil) {
		[drawable present];
		return;
	}

	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

	id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
	if (encoder == nil) {
		[drawable present];
		return;
	}

	[encoder endEncoding];
	/* presentDrawable is intentionally the plain-present wrapper from the
	 * preceding trace-buffer include; CAMetalDisplayLink owns the timing. */
	[command_buffer presentDrawable:drawable];
	[command_buffer commit];
}
@end

/* Rewrite only the direct [drawable present] calls while parsing the driven
 * compositor header, then immediately restore the selector token. */
#define present monadoCAMetalIdlePresent
#include "comp_window_macos_cametal_drive.h"
#undef present
