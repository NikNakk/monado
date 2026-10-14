/* Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 */
#import "shared-texture-peer.h"
#include <unistd.h>
@interface KKTexturePeer : NSObject <KKTexturePeerProtocol, NSXPCListenerDelegate>
@property(retain) id<MTLDevice> device;
@property(retain) id<MTLCommandQueue> queue;
@property(retain) id<MTLSharedEvent> producer;
@property(retain) id<MTLSharedEvent> done;
@property(retain) id<MTLTexture> texture;
@end
@implementation KKTexturePeer
- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
	(void)listener;
	if (connection.effectiveUserIdentifier != getuid())
		return NO;
	connection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(KKTexturePeerProtocol)];
	connection.exportedObject = self;
	[connection resume];
	return YES;
}
- (void)configureProducer:(MTLSharedEventHandle *)producer
                     done:(MTLSharedEventHandle *)done
                  surface:(IOSurface *)surface
                   format:(uint64_t)format
                 registry:(uint64_t)registry
                    reply:(void (^)(BOOL, int, uint32_t))reply
{
	@autoreleasepool {
		if (self.queue || surface.width != 64 || surface.height != 64 ||
		    (format != MTLPixelFormatRGBA8Unorm && format != MTLPixelFormatBGRA8Unorm)) {
			reply(NO, getpid(), 0);
			return;
		}
		NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
		for (id<MTLDevice> d in devices)
			if (d.registryID == registry)
				self.device = d;
		[devices release];
		if (!self.device) {
			reply(NO, getpid(), 0);
			return;
		}
		id<MTLSharedEvent> p = [self.device newSharedEventWithHandle:producer];
		id<MTLSharedEvent> d = [self.device newSharedEventWithHandle:done];
		self.producer = p;
		self.done = d;
		[p release];
		[d release];
		MTLTextureDescriptor *td =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)format
		                                                       width:64
		                                                      height:64
		                                                   mipmapped:NO];
		td.storageMode = MTLStorageModeShared;
		td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
		id<MTLTexture> t = [self.device newTextureWithDescriptor:td iosurface:(IOSurfaceRef)surface plane:0];
		self.texture = t;
		[t release];
		id<MTLCommandQueue> q = [self.device newCommandQueue];
		self.queue = q;
		[q release];
		uint32_t sid = self.texture ? IOSurfaceGetID(self.texture.iosurface) : 0;
		reply(self.producer && self.done && self.texture && self.queue && sid == surface.surfaceID, getpid(),
		      sid);
	}
}
- (void)consumeValue:(uint64_t)value reply:(void (^)(BOOL, uint64_t, uint32_t))reply
{
	@autoreleasepool {
		if (!self.queue || !value) {
			reply(NO, 0, 0);
			return;
		}
		id<MTLBuffer> readback = [self.device newBufferWithLength:16384 options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = [self.queue commandBuffer];
		if (!getenv("KK_TEST_SKIP_WAIT"))
			[cb encodeWaitForEvent:self.producer value:value];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit copyFromTexture:self.texture
		                 sourceSlice:0
		                 sourceLevel:0
		                sourceOrigin:MTLOriginMake(0, 0, 0)
		                  sourceSize:MTLSizeMake(64, 64, 1)
		                    toBuffer:readback
		           destinationOffset:0
		      destinationBytesPerRow:256
		    destinationBytesPerImage:16384];
		[blit endEncoding];
		[cb encodeSignalEvent:self.done value:value];
		[cb addCompletedHandler:^(id<MTLCommandBuffer> completed) {
		  BOOL valid = completed.status == MTLCommandBufferStatusCompleted;
		  uint8_t *pixels = (uint8_t *)readback.contents;
		  bool bgra = self.texture.pixelFormat == MTLPixelFormatBGRA8Unorm;
		  for (unsigned y = 0; y < 64; ++y)
			  for (unsigned x = 0; x < 64; ++x) {
				  uint8_t expected[4] = {(uint8_t)(value % 251), (uint8_t)((x / 8 + value * 3) % 251),
					                 (uint8_t)((y / 8 + value * 5) % 251), 255};
				  if (bgra) {
					  uint8_t tmp = expected[0];
					  expected[0] = expected[2];
					  expected[2] = tmp;
				  }
				  for (unsigned c = 0; c < 4; ++c)
					  if (pixels[(y * 64 + x) * 4 + c] != expected[c])
						  valid = NO;
			  }
		  uint64_t observed = self.done.signaledValue;
		  valid = valid && self.producer.signaledValue >= value && observed == value;
		  reply(valid, observed, IOSurfaceGetID(self.texture.iosurface));
		}];
		[cb commit];
		[readback release];
	}
}
@end
int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc != 2)
			return 2;
		KKTexturePeer *peer = [KKTexturePeer new];
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
