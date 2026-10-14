/* Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 */
#import "shared-event-peer.h"
#include <unistd.h>
@interface KKEventPeer : NSObject <KKEventPeerProtocol, NSXPCListenerDelegate>
@property(retain) id<MTLDevice> device;
@property(retain) id<MTLCommandQueue> queue;
@property(retain) id<MTLSharedEvent> producer;
@property(retain) id<MTLSharedEvent> done;
@end
@implementation KKEventPeer
- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)connection
{
	(void)listener;
	if (connection.effectiveUserIdentifier != getuid())
		return NO;
	connection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(KKEventPeerProtocol)];
	connection.exportedObject = self;
	[connection resume];
	return YES;
}
- (void)configureProducer:(MTLSharedEventHandle *)producer
                     done:(MTLSharedEventHandle *)done
                 registry:(uint64_t)registry
                    reply:(void (^)(BOOL, int))reply
{
	@autoreleasepool {
		if (self.queue) {
			reply(NO, getpid());
			return;
		}
		NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
		for (id<MTLDevice> device in devices)
			if (device.registryID == registry)
				self.device = device;
		[devices release];
		if (!self.device) {
			reply(NO, getpid());
			return;
		}
		id<MTLSharedEvent> p = [self.device newSharedEventWithHandle:producer];
		id<MTLSharedEvent> d = [self.device newSharedEventWithHandle:done];
		self.producer = p;
		self.done = d;
		[p release];
		[d release];
		id<MTLCommandQueue> q = [self.device newCommandQueue];
		self.queue = q;
		[q release];
		reply(self.producer && self.done && self.queue, getpid());
	}
}
- (void)consumeValue:(uint64_t)value reply:(void (^)(BOOL, uint64_t))reply
{
	@autoreleasepool {
		if (!self.queue || !value) {
			reply(NO, 0);
			return;
		}
		id<MTLBuffer> buffer = [self.device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
		if (!buffer) {
			reply(NO, 0);
			return;
		}
		id<MTLCommandBuffer> cb = [self.queue commandBuffer];
		if (!getenv("KK_TEST_SKIP_WAIT"))
			[cb encodeWaitForEvent:self.producer value:value];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit fillBuffer:buffer range:NSMakeRange(0, 4096) value:(uint8_t)value];
		[blit endEncoding];
		[cb encodeSignalEvent:self.done value:value];
		[cb addCompletedHandler:^(id<MTLCommandBuffer> completed) {
		  BOOL valid = completed.status == MTLCommandBufferStatusCompleted;
		  uint8_t *bytes = (uint8_t *)buffer.contents;
		  for (size_t i = 0; i < 4096; ++i)
			  if (bytes[i] != (uint8_t)value)
				  valid = NO;
		  uint64_t observed = self.done.signaledValue;
		  valid = valid && self.producer.signaledValue >= value && observed == value;
		  reply(valid, observed);
		}];
		[cb commit];
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
		KKEventPeer *peer = [KKEventPeer new];
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
