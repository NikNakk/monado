// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS XPC transport for Metal shared handles.
 * @ingroup ipc_shared
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "shared/ipc_metal_xpc.h"
#include "util/u_logging.h"

#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>

#define IPC_METAL_XPC_TIMEOUT_NS (5LL * NSEC_PER_SEC)

NSXPCInterface *
ipc_metal_xpc_create_interface(void)
{
	return [NSXPCInterface interfaceWithProtocol:@protocol(IPCMetalXPCBrokerProtocol)];
}

static NSXPCConnection *
create_connection(void)
{
	NSString *service_name = [NSString stringWithUTF8String:IPC_METAL_XPC_SERVICE_NAME];
	NSXPCConnection *connection = [[NSXPCConnection alloc] initWithMachServiceName:service_name options:0];
	connection.remoteObjectInterface = ipc_metal_xpc_create_interface();
	[connection resume];
	return connection;
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

static bool
publish_one(NSXPCConnection *connection,
            MTLSharedTextureHandle *handle,
            uint64_t token,
            uint32_t index,
            uint32_t image_count)
{
	__block BOOL success = NO;
	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

	id<IPCMetalXPCBrokerProtocol> proxy =
	    [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
		    const char *message = error.localizedDescription.UTF8String;
		    U_LOG_E("Metal XPC publish failed: %s", message != NULL ? message : "unknown error");
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
	    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, IPC_METAL_XPC_TIMEOUT_NS));
	return wait_result == 0 && replied && success;
}

static MTLSharedTextureHandle *
take_one(NSXPCConnection *connection, uint64_t token, uint32_t index)
{
	__block MTLSharedTextureHandle *result = nil;
	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

	id<IPCMetalXPCBrokerProtocol> proxy =
	    [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
		    const char *message = error.localizedDescription.UTF8String;
		    U_LOG_E("Metal XPC take failed: %s", message != NULL ? message : "unknown error");
		    dispatch_semaphore_signal(semaphore);
	    }];

	[proxy takeTextureHandleForToken:token
	                          index:index
	                          reply:^(MTLSharedTextureHandle *handle) {
		                          if (handle != nil) {
			                          result = [handle retain];
		                          }
		                          replied = YES;
		                          dispatch_semaphore_signal(semaphore);
	                          }];

	long wait_result =
	    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, IPC_METAL_XPC_TIMEOUT_NS));
	if (wait_result != 0 || !replied) {
		[result release];
		return nil;
	}
	return result;
}

static bool
discard_sync(NSXPCConnection *connection, uint64_t token)
{
	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

	id<IPCMetalXPCBrokerProtocol> proxy =
	    [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
		    const char *message = error.localizedDescription.UTF8String;
		    U_LOG_E("Metal XPC discard failed: %s", message != NULL ? message : "unknown error");
		    dispatch_semaphore_signal(semaphore);
	    }];

	[proxy discardToken:token
	              reply:^{
		              replied = YES;
		              dispatch_semaphore_signal(semaphore);
	              }];

	long wait_result =
	    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, IPC_METAL_XPC_TIMEOUT_NS));
	return wait_result == 0 && replied;
}

xrt_result_t
ipc_metal_xpc_publish_textures(void *const *metal_textures, uint32_t image_count, uint64_t *out_token)
{
	if (metal_textures == NULL || out_token == NULL || image_count == 0 ||
	    image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	*out_token = 0;

	@autoreleasepool {
		NSXPCConnection *connection = create_connection();
		if (connection == nil) {
			return XRT_ERROR_IPC_FAILURE;
		}

		uint64_t token = make_token();
		bool ok = true;

		for (uint32_t i = 0; i < image_count; i++) {
			id<MTLTexture> texture = (__bridge id<MTLTexture>)metal_textures[i];
			if (texture == nil) {
				ok = false;
				break;
			}

			MTLSharedTextureHandle *handle = [texture newSharedTextureHandle];
			if (handle == nil) {
				U_LOG_E("Metal XPC could not create shared texture handle for image %u", i);
				ok = false;
				break;
			}

			ok = publish_one(connection, handle, token, i, image_count);
			[handle release];
			if (!ok) {
				break;
			}
		}

		if (!ok) {
			(void)discard_sync(connection, token);
			[connection invalidate];
			[connection release];
			return XRT_ERROR_IPC_FAILURE;
		}

		[connection invalidate];
		[connection release];

		*out_token = token;
		U_LOG_I("Metal XPC published %u shared texture handle(s) token=0x%016llx",
		        image_count,
		        (unsigned long long)token);
		return XRT_SUCCESS;
	}
}

xrt_result_t
ipc_metal_xpc_take_textures(uint64_t token, uint32_t expected_count, void **out_metal_textures)
{
	if (!token_is_valid(token) || out_metal_textures == NULL || expected_count == 0 ||
	    expected_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	memset(out_metal_textures, 0, expected_count * sizeof(*out_metal_textures));

	@autoreleasepool {
		NSXPCConnection *connection = create_connection();
		if (connection == nil) {
			return XRT_ERROR_IPC_FAILURE;
		}

		xrt_result_t xret = XRT_SUCCESS;
		for (uint32_t i = 0; i < expected_count; i++) {
			MTLSharedTextureHandle *handle = take_one(connection, token, i);
			if (handle == nil) {
				U_LOG_E("Metal XPC broker had no texture handle for token=0x%016llx image=%u",
				        (unsigned long long)token,
				        i);
				xret = XRT_ERROR_IPC_FAILURE;
				break;
			}

			id<MTLDevice> device = handle.device;
			id<MTLTexture> texture = device != nil ? [device newSharedTextureWithHandle:handle] : nil;
			[handle release];
			if (texture == nil) {
				U_LOG_E("Metal XPC could not recreate shared texture token=0x%016llx image=%u",
				        (unsigned long long)token,
				        i);
				xret = XRT_ERROR_ALLOCATION;
				break;
			}

			out_metal_textures[i] = (__bridge void *)texture;
		}

		(void)discard_sync(connection, token);
		[connection invalidate];
		[connection release];

		if (xret != XRT_SUCCESS) {
			ipc_metal_xpc_release_textures(out_metal_textures, expected_count);
			return xret;
		}

		U_LOG_I("Metal XPC took and recreated %u shared texture(s) token=0x%016llx",
		        expected_count,
		        (unsigned long long)token);
		return XRT_SUCCESS;
	}
}

void
ipc_metal_xpc_release_textures(void **metal_textures, uint32_t image_count)
{
	if (metal_textures == NULL) {
		return;
	}

	for (uint32_t i = 0; i < image_count; i++) {
		id<MTLTexture> texture = (__bridge id<MTLTexture>)metal_textures[i];
		if (texture != nil) {
			[texture release];
			metal_textures[i] = NULL;
		}
	}
}

void
ipc_metal_xpc_discard_token(uint64_t token)
{
	if (!token_is_valid(token)) {
		return;
	}

	@autoreleasepool {
		NSXPCConnection *connection = create_connection();
		if (connection == nil) {
			return;
		}
		(void)discard_sync(connection, token);
		[connection invalidate];
		[connection release];
	}
}

void
ipc_metal_xpc_make_token_images(uint64_t token, uint32_t image_count, struct xrt_image_native *out_images)
{
	if (!token_is_valid(token) || out_images == NULL) {
		return;
	}

	for (uint32_t i = 0; i < image_count; i++) {
		out_images[i].handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		out_images[i].size = token;
		out_images[i].use_dedicated_allocation = false;
		out_images[i].is_dxgi_handle = false;
	}
}

bool
ipc_metal_xpc_get_token_from_images(const struct xrt_image_native *images,
                                    uint32_t image_count,
                                    uint64_t *out_token)
{
	if (images == NULL || out_token == NULL || image_count == 0 || image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		return false;
	}

	uint64_t token = images[0].size;
	if (!token_is_valid(token)) {
		return false;
	}

	for (uint32_t i = 0; i < image_count; i++) {
		if (xrt_graphics_buffer_is_valid(images[i].handle) || images[i].size != token) {
			return false;
		}
	}

	*out_token = token;
	return true;
}
