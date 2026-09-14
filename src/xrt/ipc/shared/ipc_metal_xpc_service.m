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
#include <sys/types.h>

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
	NSMutableDictionary *_ownersByToken;
}

- (BOOL)storeTextureHandle:(MTLSharedTextureHandle *)handle
                     token:(uint64_t)token
                     index:(uint32_t)index
                imageCount:(uint32_t)imageCount
                  ownerPID:(pid_t)ownerPID;
- (MTLSharedTextureHandle *)copyTextureHandleForToken:(uint64_t)token index:(uint32_t)index ownerPID:(pid_t)ownerPID;
- (BOOL)storeSharedEventHandle:(MTLSharedEventHandle *)handle token:(uint64_t)token ownerPID:(pid_t)ownerPID;
- (MTLSharedEventHandle *)copySharedEventHandleForToken:(uint64_t)token ownerPID:(pid_t)ownerPID;
- (void)discardToken:(uint64_t)token ownerPID:(pid_t)ownerPID;
- (NSUInteger)discardAllForPID:(pid_t)ownerPID;
@end

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

static pid_t
current_xpc_pid(void)
{
	NSXPCConnection *connection = [NSXPCConnection currentConnection];
	return connection != nil ? connection.processIdentifier : (pid_t)0;
}

@implementation IPCMetalXPCServiceObject

- (instancetype)init
{
	self = [super init];
	if (self != nil) {
		_lock = [[NSLock alloc] init];
		_handlesByToken = [[NSMutableDictionary alloc] init];
		_countsByToken = [[NSMutableDictionary alloc] init];
		_eventsByToken = [[NSMutableDictionary alloc] init];
		_ownersByToken = [[NSMutableDictionary alloc] init];
	}
	return self;
}

- (void)dealloc
{
	[_ownersByToken release];
	[_eventsByToken release];
	[_countsByToken release];
	[_handlesByToken release];
	[_lock release];
	[super dealloc];
}

- (BOOL)token:(NSNumber *)key belongsToPIDLocked:(pid_t)ownerPID allowClaim:(BOOL)allowClaim
{
	if (ownerPID <= 0) {
		return NO;
	}

	NSNumber *known_owner = [_ownersByToken objectForKey:key];
	if (known_owner == nil) {
		if (!allowClaim) {
			return NO;
		}
		[_ownersByToken setObject:[NSNumber numberWithInt:ownerPID] forKey:key];
		return YES;
	}

	return known_owner.intValue == ownerPID;
}

- (BOOL)storeTextureHandle:(MTLSharedTextureHandle *)handle
                     token:(uint64_t)token
                     index:(uint32_t)index
                imageCount:(uint32_t)imageCount
                  ownerPID:(pid_t)ownerPID
{
	if (handle == nil || !token_is_valid(token) || imageCount == 0 || imageCount > XRT_MAX_SWAPCHAIN_IMAGES ||
	    index >= imageCount || ownerPID <= 0) {
		return NO;
	}

	NSNumber *key = [NSNumber numberWithUnsignedLongLong:token];
	BOOL success = NO;
	[_lock lock];

	if ([self token:key belongsToPIDLocked:ownerPID allowClaim:YES]) {
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
	}

	[_lock unlock];
	return success;
}

- (MTLSharedTextureHandle *)copyTextureHandleForToken:(uint64_t)token index:(uint32_t)index ownerPID:(pid_t)ownerPID
{
	if (!token_is_valid(token) || ownerPID <= 0) {
		return nil;
	}

	NSNumber *key = [NSNumber numberWithUnsignedLongLong:token];
	MTLSharedTextureHandle *handle = nil;
	[_lock lock];
	if ([self token:key belongsToPIDLocked:ownerPID allowClaim:NO]) {
		NSNumber *count = [_countsByToken objectForKey:key];
		NSMutableDictionary *images = [_handlesByToken objectForKey:key];
		if (count != nil && index < count.unsignedIntValue) {
			handle = [[images objectForKey:[NSNumber numberWithUnsignedInt:index]] retain];
		}
	}
	[_lock unlock];
	return handle;
}

- (BOOL)storeSharedEventHandle:(MTLSharedEventHandle *)handle token:(uint64_t)token ownerPID:(pid_t)ownerPID
{
	if (handle == nil || !token_is_valid(token) || ownerPID <= 0) {
		return NO;
	}

	NSNumber *key = [NSNumber numberWithUnsignedLongLong:token];
	BOOL success = NO;
	[_lock lock];
	if ([self token:key belongsToPIDLocked:ownerPID allowClaim:YES] && [_eventsByToken objectForKey:key] == nil) {
		[_eventsByToken setObject:handle forKey:key];
		success = YES;
	}
	[_lock unlock];
	return success;
}

- (MTLSharedEventHandle *)copySharedEventHandleForToken:(uint64_t)token ownerPID:(pid_t)ownerPID
{
	if (!token_is_valid(token) || ownerPID <= 0) {
		return nil;
	}

	NSNumber *key = [NSNumber numberWithUnsignedLongLong:token];
	MTLSharedEventHandle *handle = nil;
	[_lock lock];
	if ([self token:key belongsToPIDLocked:ownerPID allowClaim:NO]) {
		handle = [[_eventsByToken objectForKey:key] retain];
	}
	[_lock unlock];
	return handle;
}

- (void)discardToken:(uint64_t)token ownerPID:(pid_t)ownerPID
{
	if (!token_is_valid(token) || ownerPID <= 0) {
		return;
	}

	NSNumber *key = [NSNumber numberWithUnsignedLongLong:token];
	[_lock lock];
	if ([self token:key belongsToPIDLocked:ownerPID allowClaim:NO]) {
		[_handlesByToken removeObjectForKey:key];
		[_countsByToken removeObjectForKey:key];
		[_eventsByToken removeObjectForKey:key];
		[_ownersByToken removeObjectForKey:key];
	}
	[_lock unlock];
}

- (NSUInteger)discardAllForPID:(pid_t)ownerPID
{
	if (ownerPID <= 0) {
		return 0;
	}

	NSNumber *owner = [NSNumber numberWithInt:ownerPID];
	NSMutableArray *keys = [NSMutableArray array];

	[_lock lock];
	for (NSNumber *key in _ownersByToken) {
		NSNumber *known_owner = [_ownersByToken objectForKey:key];
		if ([known_owner isEqualToNumber:owner]) {
			[keys addObject:key];
		}
	}

	for (NSNumber *key in keys) {
		[_handlesByToken removeObjectForKey:key];
		[_countsByToken removeObjectForKey:key];
		[_eventsByToken removeObjectForKey:key];
		[_ownersByToken removeObjectForKey:key];
	}
	NSUInteger count = keys.count;
	[_lock unlock];

	return count;
}

- (void)activateWithReply:(void (^)(BOOL ready))reply
{
	reply(YES);
}

- (void)publishTextureHandle:(MTLSharedTextureHandle *)handle
                       token:(uint64_t)token
                       index:(uint32_t)index
                  imageCount:(uint32_t)imageCount
                       reply:(void (^)(BOOL success))reply
{
	pid_t pid = current_xpc_pid();
	BOOL success = [self storeTextureHandle:handle token:token index:index imageCount:imageCount ownerPID:pid];
	if (!success) {
		U_LOG_W("Rejected Metal texture token=0x%016llx from XPC pid=%d", (unsigned long long)token, (int)pid);
	}
	reply(success);
}

- (void)takeTextureHandleForToken:(uint64_t)token
                            index:(uint32_t)index
                            reply:(void (^)(MTLSharedTextureHandle *handle))reply
{
	pid_t pid = current_xpc_pid();
	MTLSharedTextureHandle *handle = [self copyTextureHandleForToken:token index:index ownerPID:pid];
	if (handle == nil) {
		U_LOG_W("Rejected/missing Metal texture token=0x%016llx image=%u for XPC pid=%d",
		        (unsigned long long)token,
		        index,
		        (int)pid);
	}
	reply(handle);
	[handle release];
}

- (void)publishSharedEventHandle:(MTLSharedEventHandle *)handle
                           token:(uint64_t)token
                           reply:(void (^)(BOOL success))reply
{
	pid_t pid = current_xpc_pid();
	BOOL success = [self storeSharedEventHandle:handle token:token ownerPID:pid];
	if (!success) {
		U_LOG_W("Rejected Metal shared-event token=0x%016llx from XPC pid=%d",
		        (unsigned long long)token,
		        (int)pid);
	}
	reply(success);
}

- (void)takeSharedEventHandleForToken:(uint64_t)token
                                reply:(void (^)(MTLSharedEventHandle *handle))reply
{
	pid_t pid = current_xpc_pid();
	MTLSharedEventHandle *handle = [self copySharedEventHandleForToken:token ownerPID:pid];
	if (handle == nil) {
		U_LOG_W("Rejected/missing Metal shared-event token=0x%016llx for XPC pid=%d",
		        (unsigned long long)token,
		        (int)pid);
	}
	reply(handle);
	[handle release];
}

- (void)discardToken:(uint64_t)token reply:(void (^)(void))reply
{
	[self discardToken:token ownerPID:current_xpc_pid()];
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

static IPCMetalXPCServiceObject *
copy_service_object(void)
{
	NSLock *lock = get_service_lock();
	[lock lock];
	IPCMetalXPCServiceObject *service = [g_service_object retain];
	[lock unlock];
	return service;
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

			U_LOG_I("Monado service is hosting PID-scoped Metal XPC endpoint '%s' directly",
			        IPC_METAL_XPC_SERVICE_NAME);
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
ipc_metal_xpc_service_take_textures_for_pid(uint64_t token,
                                             uint32_t expected_count,
                                             void **out_metal_textures,
                                             pid_t owner_pid)
{
	if (!token_is_valid(token) || out_metal_textures == NULL || expected_count == 0 ||
	    expected_count > XRT_MAX_SWAPCHAIN_IMAGES || owner_pid <= 0) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	for (uint32_t i = 0; i < expected_count; i++) {
		out_metal_textures[i] = NULL;
	}

	@autoreleasepool {
		IPCMetalXPCServiceObject *service = copy_service_object();
		if (service == nil) {
			return XRT_ERROR_IPC_FAILURE;
		}

		xrt_result_t xret = XRT_SUCCESS;
		for (uint32_t i = 0; i < expected_count; i++) {
			MTLSharedTextureHandle *handle =
			    [service copyTextureHandleForToken:token index:i ownerPID:owner_pid];
			if (handle == nil) {
				U_LOG_E("Metal token ownership mismatch/missing texture token=0x%016llx image=%u pid=%d",
				        (unsigned long long)token,
				        i,
				        (int)owner_pid);
				xret = XRT_ERROR_IPC_FAILURE;
				break;
			}

			id<MTLDevice> device = handle.device;
			id<MTLTexture> texture = device != nil ? [device newSharedTextureWithHandle:handle] : nil;
			[handle release];
			if (texture == nil) {
				xret = XRT_ERROR_ALLOCATION;
				break;
			}
			out_metal_textures[i] = (__bridge void *)texture;
		}

		[service discardToken:token ownerPID:owner_pid];
		[service release];

		if (xret != XRT_SUCCESS) {
			ipc_metal_xpc_release_textures(out_metal_textures, expected_count);
			return xret;
		}

		U_LOG_I("Metal XPC consumed %u in-process texture handle(s) token=0x%016llx pid=%d",
		        expected_count,
		        (unsigned long long)token,
		        (int)owner_pid);
		return XRT_SUCCESS;
	}
}

xrt_result_t
ipc_metal_xpc_service_publish_shared_event_for_pid(void *metal_shared_event,
                                                    uint64_t *out_token,
                                                    pid_t owner_pid)
{
	if (metal_shared_event == NULL || out_token == NULL || owner_pid <= 0) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	*out_token = 0;

	@autoreleasepool {
		IPCMetalXPCServiceObject *service = copy_service_object();
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
		BOOL success = [service storeSharedEventHandle:handle token:token ownerPID:owner_pid];
		[handle release];
		[service release];
		if (!success) {
			return XRT_ERROR_IPC_FAILURE;
		}

		*out_token = token;
		U_LOG_I("Metal XPC published in-process shared event token=0x%016llx pid=%d",
		        (unsigned long long)token,
		        (int)owner_pid);
		return XRT_SUCCESS;
	}
}

void
ipc_metal_xpc_service_discard_token_for_pid(uint64_t token, pid_t owner_pid)
{
	if (!token_is_valid(token) || owner_pid <= 0) {
		return;
	}

	@autoreleasepool {
		IPCMetalXPCServiceObject *service = copy_service_object();
		if (service != nil) {
			[service discardToken:token ownerPID:owner_pid];
			[service release];
		}
	}
}

void
ipc_metal_xpc_service_discard_all_for_pid(pid_t owner_pid)
{
	if (owner_pid <= 0) {
		return;
	}

	@autoreleasepool {
		IPCMetalXPCServiceObject *service = copy_service_object();
		if (service == nil) {
			return;
		}

		NSUInteger count = [service discardAllForPID:owner_pid];
		[service release];
		if (count > 0) {
			U_LOG_I("Metal XPC discarded %lu pending resource token(s) for disconnected pid=%d",
			        (unsigned long)count,
			        (int)owner_pid);
		}
	}
}
