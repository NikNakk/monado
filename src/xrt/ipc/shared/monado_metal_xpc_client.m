// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "shared/monado_metal_xpc_client.h"
#include "shared/ipc_metal_xpc.h"

#include <dispatch/dispatch.h>
#include <stdbool.h>
#include <stdlib.h>

#define MONADO_METAL_XPC_TIMEOUT_NS (5LL * NSEC_PER_SEC)

static bool
standard_token_is_valid(uint64_t token)
{
	return (token & IPC_METAL_XPC_TOKEN_MASK) == IPC_METAL_XPC_TOKEN_MAGIC;
}

static bool
external_token_is_valid(uint64_t token)
{
	return (token & IPC_METAL_XPC_EXTERNAL_TOKEN_MASK) == IPC_METAL_XPC_EXTERNAL_TOKEN_MAGIC;
}

static bool
token_is_valid(uint64_t token)
{
	return standard_token_is_valid(token) || external_token_is_valid(token);
}

static uint64_t
make_standard_token(void)
{
	uint64_t random_bits = 0;
	arc4random_buf(&random_bits, sizeof(random_bits));
	return IPC_METAL_XPC_TOKEN_MAGIC | (random_bits & ~IPC_METAL_XPC_TOKEN_MASK);
}

static uint64_t
make_external_token(void)
{
	uint64_t random_bits = 0;
	arc4random_buf(&random_bits, sizeof(random_bits));
	return IPC_METAL_XPC_EXTERNAL_TOKEN_MAGIC | (random_bits & ~IPC_METAL_XPC_EXTERNAL_TOKEN_MASK);
}

static NSXPCConnection *
create_connection(void)
{
	NSString *service_name = [NSString stringWithUTF8String:IPC_METAL_XPC_SERVICE_NAME];
	NSXPCConnection *connection = [[NSXPCConnection alloc] initWithMachServiceName:service_name options:0];
	if (connection == nil) {
		return nil;
	}
	connection.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(IPCMetalXPCBrokerProtocol)];
	[connection resume];
	return connection;
}

static bool
publish_texture(NSXPCConnection *connection,
                MTLSharedTextureHandle *handle,
                uint64_t token,
                uint32_t index,
                uint32_t image_count)
{
	__block BOOL success = NO;
	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

	id<IPCMetalXPCBrokerProtocol> proxy = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
	  (void)error;
	  dispatch_semaphore_signal(semaphore);
	}];

	[proxy publishTextureHandle:handle
	                      token:token
	                      index:index
	                 imageCount:image_count
	                      reply:^(BOOL remote_success) {
		                success = remote_success;
		                replied = YES;
		                dispatch_semaphore_signal(semaphore);
	                      }];

	long wait_result =
	    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, MONADO_METAL_XPC_TIMEOUT_NS));
	return wait_result == 0 && replied && success;
}

static bool
publish_shared_event(NSXPCConnection *connection, MTLSharedEventHandle *handle, uint64_t token)
{
	__block BOOL success = NO;
	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

	id<IPCMetalXPCBrokerProtocol> proxy = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
	  (void)error;
	  dispatch_semaphore_signal(semaphore);
	}];

	[proxy publishSharedEventHandle:handle
	                         token:token
	                         reply:^(BOOL remote_success) {
		                   success = remote_success;
		                   replied = YES;
		                   dispatch_semaphore_signal(semaphore);
	                         }];

	long wait_result =
	    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, MONADO_METAL_XPC_TIMEOUT_NS));
	return wait_result == 0 && replied && success;
}

static bool
mark_claimable(NSXPCConnection *connection, uint64_t token)
{
	__block BOOL success = NO;
	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

	id<IPCMetalXPCBrokerProtocol> proxy = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
	  (void)error;
	  dispatch_semaphore_signal(semaphore);
	}];

	[proxy markTextureTokenClaimable:token
	                           reply:^(BOOL remote_success) {
		                     success = remote_success;
		                     replied = YES;
		                     dispatch_semaphore_signal(semaphore);
	                           }];

	long wait_result =
	    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, MONADO_METAL_XPC_TIMEOUT_NS));
	return wait_result == 0 && replied && success;
}

static MTLSharedTextureHandle *
take_texture_handle(NSXPCConnection *connection, uint64_t token)
{
	__block MTLSharedTextureHandle *result = nil;
	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

	id<IPCMetalXPCBrokerProtocol> proxy = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
	  (void)error;
	  dispatch_semaphore_signal(semaphore);
	}];

	[proxy takeTextureHandleForToken:token
	                           index:0
	                           reply:^(MTLSharedTextureHandle *handle) {
		                     if (handle != nil) {
			                     result = [handle retain];
		                     }
		                     replied = YES;
		                     dispatch_semaphore_signal(semaphore);
	                           }];

	long wait_result =
	    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, MONADO_METAL_XPC_TIMEOUT_NS));
	if (wait_result != 0 || !replied) {
		[result release];
		return nil;
	}
	return result;
}

static void
discard_token(NSXPCConnection *connection, uint64_t token)
{
	if (connection == nil || !token_is_valid(token)) {
		return;
	}

	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
	id<IPCMetalXPCBrokerProtocol> proxy = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
	  (void)error;
	  dispatch_semaphore_signal(semaphore);
	}];

	[proxy discardToken:token
	              reply:^{
		        replied = YES;
		        dispatch_semaphore_signal(semaphore);
	              }];

	(void)dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, MONADO_METAL_XPC_TIMEOUT_NS));
	(void)replied;
}

static int
publish_textures_common(void *const *metal_textures,
                        uint32_t image_count,
                        bool claimable,
                        uint64_t *out_token)
{
	if (metal_textures == NULL || out_token == NULL || image_count == 0 ||
	    image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return -1;
	}
	*out_token = 0;

	@autoreleasepool {
		NSXPCConnection *connection = create_connection();
		if (connection == nil) {
			return -2;
		}

		uint64_t token = claimable ? make_external_token() : make_standard_token();
		bool published = true;
		for (uint32_t i = 0; i < image_count; i++) {
			id<MTLTexture> texture = (__bridge id<MTLTexture>)metal_textures[i];
			if (texture == nil) {
				published = false;
				break;
			}
			MTLSharedTextureHandle *handle = [texture newSharedTextureHandle];
			if (handle == nil) {
				published = false;
				break;
			}
			bool one_ok = publish_texture(connection, handle, token, i, image_count);
			[handle release];
			if (!one_ok) {
				published = false;
				break;
			}
		}

		bool ready = published && (!claimable || mark_claimable(connection, token));
		if (!ready) {
			discard_token(connection, token);
			[connection invalidate];
			[connection release];
			return -3;
		}

		[connection invalidate];
		[connection release];
		*out_token = token;
		return 0;
	}
}

int
monado_metal_xpc_publish_textures(void *const *metal_textures, uint32_t image_count, uint64_t *out_token)
{
	return publish_textures_common(metal_textures, image_count, false, out_token);
}

int
monado_metal_xpc_publish_claimable_texture(void *metal_texture, uint64_t *out_token)
{
	void *textures[1] = {metal_texture};
	return publish_textures_common(textures, 1, true, out_token);
}

int
monado_metal_xpc_publish_shared_event(void *metal_shared_event, uint64_t *out_token)
{
	if (metal_shared_event == NULL || out_token == NULL) {
		return -1;
	}
	*out_token = 0;

	@autoreleasepool {
		id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)metal_shared_event;
		MTLSharedEventHandle *handle = [event newSharedEventHandle];
		if (handle == nil) {
			return -2;
		}

		NSXPCConnection *connection = create_connection();
		if (connection == nil) {
			[handle release];
			return -3;
		}

		uint64_t token = make_standard_token();
		bool published = publish_shared_event(connection, handle, token);
		[handle release];
		if (!published) {
			discard_token(connection, token);
			[connection invalidate];
			[connection release];
			return -4;
		}

		[connection invalidate];
		[connection release];
		*out_token = token;
		return 0;
	}
}

static int
take_texture_common(uint64_t token, id<MTLDevice> requested_device, void **out_metal_texture)
{
	if (!token_is_valid(token) || out_metal_texture == NULL) {
		return -1;
	}
	*out_metal_texture = NULL;

	@autoreleasepool {
		NSXPCConnection *connection = create_connection();
		if (connection == nil) {
			return -2;
		}

		MTLSharedTextureHandle *handle = take_texture_handle(connection, token);
		if (handle == nil) {
			[connection invalidate];
			[connection release];
			return -3;
		}

		id<MTLDevice> device = requested_device != nil ? requested_device : handle.device;
		id<MTLTexture> texture = device != nil ? [device newSharedTextureWithHandle:handle] : nil;
		[handle release];

		// takeTextureHandleForToken() transfers ownership of a claimable token
		// to this process. Consume it regardless of texture reconstruction
		// success so stale one-shot tokens cannot accumulate.
		discard_token(connection, token);
		[connection invalidate];
		[connection release];

		if (texture == nil) {
			return -4;
		}

		*out_metal_texture = (__bridge void *)texture;
		return 0;
	}
}

int
monado_metal_xpc_take_texture(uint64_t token, void **out_metal_texture)
{
	return take_texture_common(token, nil, out_metal_texture);
}

int
monado_metal_xpc_take_texture_on_device(uint64_t token, void *metal_device, void **out_metal_texture)
{
	if (metal_device == NULL) {
		return -1;
	}
	id<MTLDevice> device = (__bridge id<MTLDevice>)metal_device;
	return take_texture_common(token, device, out_metal_texture);
}

void
monado_metal_xpc_release_texture(void *metal_texture)
{
	id<MTLTexture> texture = (__bridge id<MTLTexture>)metal_texture;
	[texture release];
}
