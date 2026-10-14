/* Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 */
#import "array-texture-peer.h"
#include <unistd.h>
@interface KKArrayPeer : NSObject <KKArrayPeerProtocol, NSXPCListenerDelegate>
@property(retain) id<MTLDevice> device;
@property(retain) id<MTLCommandQueue> queue;
@property(retain) id<MTLSharedEvent> producer;
@property(retain) id<MTLSharedEvent> done;
@property(retain) id<MTLTexture> texture;
@end
@implementation KKArrayPeer
- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
	(void)listener;
	if (connection.effectiveUserIdentifier != getuid())
		return NO;
	connection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(KKArrayPeerProtocol)];
	connection.exportedObject = self;
	[connection resume];
	return YES;
}
- (void)configureProducer:(MTLSharedEventHandle *)producer
                     done:(MTLSharedEventHandle *)done
                  texture:(MTLSharedTextureHandle *)texture
                   format:(uint64_t)format
                 registry:(uint64_t)registry
                    reply:(void (^)(BOOL, int, uint32_t))reply
{
	@autoreleasepool {
		if (self.queue || !texture) {
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
		id<MTLTexture> t = [self.device newSharedTextureWithHandle:texture];
		self.texture = t;
		[t release];
		if (!self.texture || self.texture.width != 64 || self.texture.height != 64 ||
		    self.texture.arrayLength < 1 || self.texture.arrayLength > 2 ||
		    self.texture.pixelFormat != (MTLPixelFormat)format) {
			reply(NO, getpid(), 0);
			return;
		}
		id<MTLCommandQueue> q = [self.device newCommandQueue];
		self.queue = q;
		[q release];
		reply(self.producer && self.done && self.texture && self.queue, getpid(),
		      (uint32_t)self.texture.arrayLength);
	}
}
- (void)consumeValue:(uint64_t)value reply:(void (^)(BOOL, uint64_t, uint32_t))reply
{
	@autoreleasepool {
		if (!self.queue || !value) {
			reply(NO, 0, 0);
			return;
		}
		id<MTLBuffer> readback = [self.device newBufferWithLength:16384 * self.texture.arrayLength
		                                                  options:MTLResourceStorageModeShared];
		id<MTLCommandBuffer> cb = [self.queue commandBuffer];
		if (!getenv("KK_TEST_SKIP_WAIT"))
			[cb encodeWaitForEvent:self.producer value:value];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		for (unsigned layer = 0; layer < self.texture.arrayLength; ++layer)
			[blit copyFromTexture:self.texture
			                 sourceSlice:layer
			                 sourceLevel:0
			                sourceOrigin:MTLOriginMake(0, 0, 0)
			                  sourceSize:MTLSizeMake(64, 64, 1)
			                    toBuffer:readback
			           destinationOffset:16384 * layer
			      destinationBytesPerRow:256
			    destinationBytesPerImage:16384];
		id<MTLBuffer> marker = [self.device newBufferWithLength:256 * self.texture.arrayLength
		                                                options:MTLResourceStorageModeShared];
		for (unsigned layer = 0; layer < self.texture.arrayLength; ++layer) {
			uint8_t *bytes = (uint8_t *)marker.contents + 256 * layer;
			bytes[0] = 17 + layer;
			bytes[1] = 33 + layer;
			bytes[2] = 49 + layer;
			bytes[3] = 255;
			[blit copyFromBuffer:marker
			           sourceOffset:256 * layer
			      sourceBytesPerRow:256
			    sourceBytesPerImage:256
			             sourceSize:MTLSizeMake(1, 1, 1)
			              toTexture:self.texture
			       destinationSlice:layer
			       destinationLevel:0
			      destinationOrigin:MTLOriginMake(63, 63, 0)];
		}
		[blit endEncoding];
		[cb encodeSignalEvent:self.done value:value];
		[cb addCompletedHandler:^(id<MTLCommandBuffer> completed) {
		  BOOL valid = completed.status == MTLCommandBufferStatusCompleted;
		  bool bgra = self.texture.pixelFormat == MTLPixelFormatBGRA8Unorm ||
			      self.texture.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
		  bool srgb = self.texture.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB ||
			      self.texture.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
		  valid = valid && checkArrayPixels((const uint8_t *)readback.contents, value,
			                            (unsigned)self.texture.arrayLength, bgra, srgb);
		  uint64_t observed = self.done.signaledValue;
		  valid = valid && self.producer.signaledValue >= value && observed == value;
		  reply(valid, observed, (uint32_t)self.texture.arrayLength);
		}];
		[cb commit];
		[readback release];
		[marker release];
	}
}
@end
int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc != 2)
			return 2;
		KKArrayPeer *peer = [KKArrayPeer new];
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
