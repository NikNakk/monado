// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Direct monado-service XPC endpoint for macOS Metal transport.
 * @ingroup ipc_shared
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "shared/ipc_metal_xpc.h"
#include "shared/ipc_metal_xpc_service.h"
#include "util/u_logging.h"

#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define IPC_METAL_XPC_ACTIVATION_TIMEOUT_NS (15LL * NSEC_PER_SEC)

@protocol IPCMetalXPCServiceProtocol <IPCMetalXPCBrokerProtocol>
- (void)activateWithReply:(void (^)(BOOL ready))reply;
@end

@interface IPCMetalXPCServiceObject : NSObject <IPCMetalXPCServiceProtocol>
{
	NSLock *_lock;
	NSMutableDictionary *_handlesByToken;
	NSMutableDictionary *_countsByToken;
	NSMutableDictionary *_eventsByToken;
}
@end

@implementation IPCMetalXPCServiceObject

- (instancetype)init
{
	self = [super init];
	if (self != nil) {
		_lock = [[NSLock alloc] init];
		_handlesByToken = [[NSMutableDictionary alloc] init];
		_countsByToken = [[NSMutableDictionary alloc] init];
		_eventsByToken = [[NSMutableDictionary alloc] init];
	}
	return self;
}

- (void)dealloc
{
	[_eventsByToken release];
	[_countsByToken release];
	[_handlesByToken release];
	[_lock release];
	[super dealloc];
}

- (void)activateWithReply:(void (^)(BOOL ready))reply
{
	/*
	 * ipc_metal_xpc_service_start() is called only after the ordinary Unix
	 * listening socket has been created. Reaching this method therefore means
	 * the normal Monado IPC transport is ready for the activating client.
	 */
	reply(YES);
}

- (void)publishTextureHandle:(MTLSharedTextureHandle *)handle
                       token:(uint64_t)token
                       index:(uint32_t)index
                  imageCount:(uint32_t)imageCount
                       reply:(void (^)(BOOL success))reply
{
	BOOL success = NO;

	if (handle != nil && imageCount > 0 && imageCount <= XRT_MAX_SWAPCHAIN_IMAGES && index < imageCount &&
	    (token & IPC_METAL_XPC_TOKEN_MASK) == IPC_METAL_XPC_TOKEN_MAGIC) {
		NSNumber *key = [NSNumber numberWithUnsignedLongLong:token];

		[_lock lock];

		NSNumber *known_count = [_countsByToken objectForKey:key];
		if (known_count == nil || known_count.unsignedIntValue == imageCount) {
			NSMutableDictionary *images = [_handlesByToken objectForKey:key];
			if (images == nil) {
				images = [NSMutableDictionary dictionaryWithCapacity:imageCount];
				[_handlesByToken setObject:images forKey:key];
				[_countsByToken setObject:[NSNumber numberWithUnsignedInt:imageCount] forKey:key];
			}

			[images setObject:handle forKey:[NSNumber numberWithUnsignedInt:index]];
			success = YES;
		}

		[_lock unlock];
	}

	reply(success);
}

- (void)takeTextureHandleForToken:(uint64_t)token
                            index:(uint32_t)index
                            reply:(void (^)(MTLSharedTextureHandle *handle))reply
{
	MTLSharedTextureHandle *handle = nil;
	NSNumber *key = [NSNumber numberWithUnsignedLongLong:token];

	[_lock lock];
	NSNumber *count = [_countsByToken objectForKey:key];
	NSMutableDictionary *images = [_handlesByToken objectForKey:key];
	if (count != nil && index < count.unsignedIntValue) {
		handle = [[images objectForKey:[NSNumber numberWithUnsignedInt:index]] retain];
	}
	[_lock unlock];

	reply(handle);
	[handle release];
}

- (void)publishSharedEventHandle:(MTLSharedEventHandle *)handle
                           token:(uint64_t)token
                           reply:(void (^)(BOOL success))reply
{
	BOOL success = NO;
	if (handle != nil && (token & IPC_METAL_XPC_TOKEN_MASK) == IPC_METAL_XPC_TOKEN_MAGIC) {
		NSNumber *key = [NSNumber numberWithUnsignedLongLong:token];
		[_lock lock];
		if ([_eventsByToken objectForKey:key] == nil) {
			[_eventsByToken setObject:handle forKey:key];
			success = YES;
		}
		[_lock unlock];
	}
	reply(success);
}

- (void)takeSharedEventHandleForToken:(uint64_t)token
                                reply:(void (^)(MTLSharedEventHandle *handle))reply
{
	NSNumber *key = [NSNumber numberWithUnsignedLongLong:token];
	MTLSharedEventHandle *handle = nil;
	[_lock lock];
	handle = [[_eventsByToken objectForKey:key] retain];
	[_lock unlock];

	reply(handle);
	[handle release];
}

- (void)discardToken:(uint64_t)token reply:(void (^)(void))reply
{
	NSNumber *key = [NSNumber numberWithUnsignedLongLong:token];
	[_lock lock];
	[_handlesByToken removeObjectForKey:key];
	[_countsByToken removeObjectForKey:key];
	[_eventsByToken removeObjectForKey:key];
	[_lock unlock];
	reply();
}

@end

@interface IPCMetalXPCServiceListenerDelegate : NSObject <NSXPCListenerDelegate>
{
	IPCMetalXPCServiceObject *_service;
}
- (instancetype)initWithService:(IPCMetalXPCServiceObject *)service;
@end

@implementation IPCMetalXPCServiceListenerDelegate

- (instancetype)initWithService:(IPCMetalXPCServiceObject *)service
{
	self = [super init];
	if (self != nil) {
		_service = [service retain];
	}
	return self;
}

- (void)dealloc
{
	[_service release];
	[super dealloc];
}

- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)newConnection
{
	(void)listener;
	newConnection.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(IPCMetalXPCServiceProtocol)];
	newConnection.exportedObject = _service;
	[newConnection resume];
	return YES;
}

@end

static NSLock *g_service_lock = nil;
static IPCMetalXPCServiceObject *g_service_object = nil;
static IPCMetalXPCServiceListenerDelegate *g_service_delegate = nil;
static NSXPCListener *g_service_listener = nil;

static NSLock *
get_service_lock(void)
{
	@synchronized([IPCMetalXPCServiceObject class]) {
		if (g_service_lock == nil) {
			g_service_lock = [[NSLock alloc] init];
		}
	}
	return g_service_lock;
}

static bool
token_is_valid(uint64_t token)
{
	return (token & IPC_METAL_XPC_TOKEN_MASK) == IPC_METAL_XPC_TOKEN_MAGIC;
}

static uint64_t
make_token(void)
{
	uint64_t random_bits = 0;
	arc4random_buf(&random_bits, sizeof(random_bits));
	return IPC_METAL_XPC_TOKEN_MAGIC | (random_bits & ~IPC_METAL_XPC_TOKEN_MASK);
}

xrt_result_t
ipc_metal_xpc_service_start(void)
{
	@autoreleasepool {
		NSLock *lock = get_service_lock();
		[lock lock];
		if (g_service_listener != nil) {
			[lock unlock];
			return XRT_SUCCESS;
		}

		@try {
			NSString *service_name = [NSString stringWithUTF8String:IPC_METAL_XPC_SERVICE_NAME];
			IPCMetalXPCServiceObject *service = [[IPCMetalXPCServiceObject alloc] init];
			IPCMetalXPCServiceListenerDelegate *delegate =
			    [[IPCMetalXPCServiceListenerDelegate alloc] initWithService:service];
			NSXPCListener *listener = [[NSXPCListener alloc] initWithMachServiceName:service_name];
			if (service == nil || delegate == nil || listener == nil) {
				[listener release];
				[delegate release];
				[service release];
				[lock unlock];
				return XRT_ERROR_ALLOCATION;
			}

			listener.delegate = delegate;
			[listener resume];

			g_service_object = service;
			g_service_delegate = delegate;
			g_service_listener = listener;
			[lock unlock];

			U_LOG_I("Monado service is hosting Metal XPC endpoint '%s' directly", IPC_METAL_XPC_SERVICE_NAME);
			return XRT_SUCCESS;
		} @catch (NSException *exception) {
			const char *reason = exception.reason.UTF8String;
			[lock unlock];
			U_LOG_W("Could not start direct Metal XPC endpoint '%s': %s",
			        IPC_METAL_XPC_SERVICE_NAME,
			        reason != NULL ? reason : "unknown exception");
			return XRT_ERROR_IPC_FAILURE;
		}
	}
}

void
ipc_metal_xpc_service_stop(void)
{
	@autoreleasepool {
		NSLock *lock = get_service_lock();
		[lock lock];

		if (g_service_listener != nil) {
			[g_service_listener invalidate];
			[g_service_listener release];
			g_service_listener = nil;
		}
		[g_service_delegate release];
		g_service_delegate = nil;
		[g_service_object release];
		g_service_object = nil;

		[lock unlock];
	}
}

xrt_result_t
ipc_metal_xpc_activate_service(void)
{
	@autoreleasepool {
		NSString *service_name = [NSString stringWithUTF8String:IPC_METAL_XPC_SERVICE_NAME];
		NSXPCConnection *connection = [[NSXPCConnection alloc] initWithMachServiceName:service_name options:0];
		if (connection == nil) {
			return XRT_ERROR_IPC_FAILURE;
		}

		connection.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(IPCMetalXPCServiceProtocol)];
		[connection resume];

		__block BOOL replied = NO;
		__block BOOL ready = NO;
		dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

		id<IPCMetalXPCServiceProtocol> proxy =
		    [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
			    const char *message = error.localizedDescription.UTF8String;
			    U_LOG_D("Monado launchd XPC activation unavailable: %s",
			            message != NULL ? message : "unknown error");
			    dispatch_semaphore_signal(semaphore);
		    }];

		[proxy activateWithReply:^(BOOL remote_ready) {
			replied = YES;
			ready = remote_ready;
			dispatch_semaphore_signal(semaphore);
		}];

		long wait_result = dispatch_semaphore_wait(
		    semaphore, dispatch_time(DISPATCH_TIME_NOW, IPC_METAL_XPC_ACTIVATION_TIMEOUT_NS));

		[connection invalidate];
		[connection release];

		if (wait_result != 0 || !replied || !ready) {
			return XRT_ERROR_IPC_FAILURE;
		}

		U_LOG_I("Monado launchd XPC activation ready");
		return XRT_SUCCESS;
	}
}

xrt_result_t
ipc_metal_xpc_service_take_textures(uint64_t token, uint32_t expected_count, void **out_metal_textures)
{
	if (!token_is_valid(token) || out_metal_textures == NULL || expected_count == 0 ||
	    expected_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	for (uint32_t i = 0; i < expected_count; i++) {
		out_metal_textures[i] = NULL;
	}

	@autoreleasepool {
		NSLock *service_lock = get_service_lock();
		[service_lock lock];
		IPCMetalXPCServiceObject *service = [g_service_object retain];
		[service_lock unlock];
		if (service == nil) {
			return XRT_ERROR_IPC_FAILURE;
		}

		xrt_result_t xret = XRT_SUCCESS;
		for (uint32_t i = 0; i < expected_count; i++) {
			__block MTLSharedTextureHandle *handle = nil;
			[service takeTextureHandleForToken:token
			                             index:i
			                             reply:^(MTLSharedTextureHandle *remote_handle) {
				                             if (remote_handle != nil) {
					                             handle = [remote_handle retain];
				                             }
			                             }];

			if (handle == nil) {
				U_LOG_E("Metal XPC local registry had no texture handle for token=0x%016llx image=%u",
				        (unsigned long long)token,
				        i);
				xret = XRT_ERROR_IPC_FAILURE;
				break;
			}

			id<MTLDevice> device = handle.device;
			id<MTLTexture> texture = device != nil ? [device newSharedTextureWithHandle:handle] : nil;
			[handle release];
			if (texture == nil) {
				U_LOG_E("Metal XPC local registry could not recreate texture token=0x%016llx image=%u",
				        (unsigned long long)token,
				        i);
				xret = XRT_ERROR_ALLOCATION;
				break;
			}

			out_metal_textures[i] = (__bridge void *)texture;
		}

		[service discardToken:token reply:^{}];
		[service release];

		if (xret != XRT_SUCCESS) {
			ipc_metal_xpc_release_textures(out_metal_textures, expected_count);
			return xret;
		}

		U_LOG_I("Metal XPC consumed %u texture handle(s) from in-process registry token=0x%016llx",
		        expected_count,
		        (unsigned long long)token);
		return XRT_SUCCESS;
	}
}

xrt_result_t
ipc_metal_xpc_service_publish_shared_event(void *metal_shared_event, uint64_t *out_token)
{
	if (metal_shared_event == NULL || out_token == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	*out_token = 0;

	@autoreleasepool {
		NSLock *service_lock = get_service_lock();
		[service_lock lock];
		IPCMetalXPCServiceObject *service = [g_service_object retain];
		[service_lock unlock];
		if (service == nil) {
			return XRT_ERROR_IPC_FAILURE;
		}

		id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)metal_shared_event;
		MTLSharedEventHandle *handle = [event newSharedEventHandle];
		if (handle == nil) {
			[service release];
			return XRT_ERROR_ALLOCATION;
		}

		uint64_t token = make_token();
		__block BOOL success = NO;
		[service publishSharedEventHandle:handle
		                            token:token
		                            reply:^(BOOL remote_success) { success = remote_success; }];
		[handle release];
		[service release];

		if (!success) {
			return XRT_ERROR_IPC_FAILURE;
		}

		*out_token = token;
		U_LOG_I("Metal XPC published shared event directly into in-process registry token=0x%016llx",
		        (unsigned long long)token);
		return XRT_SUCCESS;
	}
}

void
ipc_metal_xpc_service_discard_token(uint64_t token)
{
	if (!token_is_valid(token)) {
		return;
	}

	@autoreleasepool {
		NSLock *service_lock = get_service_lock();
		[service_lock lock];
		IPCMetalXPCServiceObject *service = [g_service_object retain];
		[service_lock unlock];
		if (service != nil) {
			[service discardToken:token reply:^{}];
			[service release];
		}
	}
}
