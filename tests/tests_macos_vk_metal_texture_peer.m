// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#import "tests_macos_vk_metal_texture_peer.h"
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
@interface VulkanMetalTexturePeer : NSObject <VulkanMetalTexturePeerProtocol, NSXPCListenerDelegate>
@property(retain) id<MTLDevice> device;
@property(retain) id<MTLCommandQueue> queue;
@property(retain) id<MTLSharedEvent> producer;
@property(retain) id<MTLSharedEvent> done;
@property(retain) id<MTLTexture> texture;
@property(assign) uint64_t queued;
@end
@implementation VulkanMetalTexturePeer
- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
	if (connection.effectiveUserIdentifier != getuid())
		return NO;
	connection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(VulkanMetalTexturePeerProtocol)];
	connection.exportedObject = self;
	[connection resume];
	return YES;
}
- (void)configureProducer:(MTLSharedEventHandle *)producer
                     done:(MTLSharedEventHandle *)done
                  texture:(MTLSharedTextureHandle *)texture
                 registry:(uint64_t)registry
                    reply:(void (^)(BOOL, int))reply
{
	@autoreleasepool {
		NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
		for (id<MTLDevice> device in devices)
			if (device.registryID == registry)
				self.device = device;
		[devices release];
		id<MTLSharedEvent> p = [self.device newSharedEventWithHandle:producer],
		                   d = [self.device newSharedEventWithHandle:done];
		id<MTLTexture> t = [self.device newSharedTextureWithHandle:texture];
		id<MTLCommandQueue> q = [self.device newCommandQueue];
		self.queued = 0;
		self.producer = p;
		self.done = d;
		self.texture = t;
		self.queue = q;
		[p release];
		[d release];
		[t release];
		[q release];
		reply(self.producer && self.done && self.queue && self.texture.width == 64 &&
		          self.texture.height == 64 && self.texture.arrayLength >= 1 && self.texture.arrayLength <= 2,
		      getpid());
	}
}
- (void)queuedValue:(void (^)(uint64_t))reply
{
	reply(self.queued);
}
- (void)consumeValue:(uint64_t)value reply:(void (^)(BOOL, uint64_t))reply
{
	@autoreleasepool {
		id<MTLBuffer> buffer = [self.device newBufferWithLength:16384 * self.texture.arrayLength
		                                                options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = [self.queue commandBuffer];
		// The negative test deliberately removes this wait. It must not pass.
		if (!getenv("VK_METAL_TEST_SKIP_PEER_WAIT"))
			[cb encodeWaitForEvent:self.producer value:value];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		for (unsigned layer = 0; layer < self.texture.arrayLength; ++layer)
			[blit copyFromTexture:self.texture
			                 sourceSlice:layer
			                 sourceLevel:0
			                sourceOrigin:MTLOriginMake(0, 0, 0)
			                  sourceSize:MTLSizeMake(64, 64, 1)
			                    toBuffer:buffer
			           destinationOffset:16384 * layer
			      destinationBytesPerRow:256
			    destinationBytesPerImage:16384];
		[blit endEncoding];
		[cb encodeSignalEvent:self.done value:value];
		[cb addCompletedHandler:^(id<MTLCommandBuffer> completed) {
		  bool valid = completed.status == MTLCommandBufferStatusCompleted;
		  MTLPixelFormat format = self.texture.pixelFormat;
		  bool bgra = format == MTLPixelFormatBGRA8Unorm || format == MTLPixelFormatBGRA8Unorm_sRGB;
		  bool srgb = format == MTLPixelFormatRGBA8Unorm_sRGB || format == MTLPixelFormatBGRA8Unorm_sRGB;
		  const uint8_t *bytes = buffer.contents;
		  for (unsigned layer = 0; layer < self.texture.arrayLength; ++layer)
			  for (unsigned pixel = 0; pixel < 4096; ++pixel) {
				  float values[4] = {layer ? 0 : 1, (float)(value + (layer ? 3 : 0)) / 8, layer ? 1 : 0,
					             1};
				  for (unsigned c = 0; c < 4; ++c) {
					  float expected = values[c];
					  if (srgb && c < 3)
						  expected = expected <= .0031308f
							         ? 12.92f * expected
							         : 1.055f * powf(expected, 1 / 2.4f) - .055f;
					  unsigned channel = bgra && (c == 0 || c == 2) ? 2 - c : c;
					  valid &= abs((int)bytes[(layer * 4096 + pixel) * 4 + channel] -
						       (int)lroundf(expected * 255)) <= 1;
				  }
			  }
		  uint64_t observed = self.done.signaledValue;
		  valid &= self.producer.signaledValue == value && observed == value;
		  reply(valid, observed);
		}];
		[cb commit];
		self.queued = value;
		[buffer release];
	}
}
@end
int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc != 2)
			return 2;
		VulkanMetalTexturePeer *peer = [VulkanMetalTexturePeer new];
		NSXPCListener *listener =
		    [[NSXPCListener alloc] initWithMachServiceName:[NSString stringWithUTF8String:argv[1]]];
		listener.delegate = peer;
		[listener resume];
		[[NSRunLoop currentRunLoop] run];
		[listener release];
		[peer release];
		return 0;
	}
}
