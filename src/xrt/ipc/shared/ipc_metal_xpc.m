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
#include <unistd.h>

#define IPC_METAL_XPC_TIMEOUT_NS (5LL * NSEC_PER_SEC)
#define IPC_XPC_IMPORTANCE_TIMEOUT_NS (5LL * NSEC_PER_SEC)

struct ipc_metal_xpc_importance_lease
{
	NSXPCConnection *connection;
	uint64_t session_id;
};

static __thread bool g_shared_event_request_active = false;
static __thread void *g_shared_event_request_event = NULL;
static __thread void *g_shared_event_request_device = NULL;

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

static NSXPCConnection *
create_service_connection(void)
{
	NSString *service_name = [NSString stringWithUTF8String:IPC_METAL_XPC_SERVICE_NAME];
	NSXPCConnection *connection = [[NSXPCConnection alloc] initWithMachServiceName:service_name options:0];
	connection.remoteObjectInterface = [NSXPCInterface interfaceWithProtocol:@protocol(IPCMetalXPCServiceProtocol)];
	[connection resume];
	return connection;
}

static uint64_t
make_importance_session_id(void)
{
	uint64_t session_id = 0;
	while (session_id == 0) {
		arc4random_buf(&session_id, sizeof(session_id));
	}
	return session_id;
}

static bool
standard_token_is_valid(uint64_t token)
{
	return (token & IPC_METAL_XPC_TOKEN_MASK) == IPC_METAL_XPC_TOKEN_MAGIC;
}

static bool
external_texture_token_is_valid(uint64_t token)
{
	return (token & IPC_METAL_XPC_EXTERNAL_TOKEN_MASK) == IPC_METAL_XPC_EXTERNAL_TOKEN_MAGIC;
}

static bool
texture_token_is_valid(uint64_t token)
{
	return standard_token_is_valid(token) || external_texture_token_is_valid(token);
}

static uint64_t
make_token(void)
{
	uint64_t random_bits = 0;
	arc4random_buf(&random_bits, sizeof(random_bits));
	return IPC_METAL_XPC_TOKEN_MAGIC | (random_bits & ~IPC_METAL_XPC_TOKEN_MASK);
}

static uint64_t
make_external_texture_token(void)
{
	uint64_t random_bits = 0;
	arc4random_buf(&random_bits, sizeof(random_bits));
	return IPC_METAL_XPC_EXTERNAL_TOKEN_MAGIC |
	       (random_bits & ~IPC_METAL_XPC_EXTERNAL_TOKEN_MASK);
}

static bool
publish_texture_one(NSXPCConnection *connection,
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
		    U_LOG_E("Metal XPC texture publish failed: %s", message != NULL ? message : "unknown error");
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
take_texture_one(NSXPCConnection *connection, uint64_t token, uint32_t index)
{
	__block MTLSharedTextureHandle *result = nil;
	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

	id<IPCMetalXPCBrokerProtocol> proxy =
	    [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
		    const char *message = error.localizedDescription.UTF8String;
		    U_LOG_E("Metal XPC texture take failed: %s", message != NULL ? message : "unknown error");
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
publish_event_one(NSXPCConnection *connection, MTLSharedEventHandle *handle, uint64_t token)
{
	__block BOOL success = NO;
	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

	id<IPCMetalXPCBrokerProtocol> proxy =
	    [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
		    const char *message = error.localizedDescription.UTF8String;
		    U_LOG_E("Metal XPC shared-event publish failed: %s", message != NULL ? message : "unknown error");
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
	    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, IPC_METAL_XPC_TIMEOUT_NS));
	return wait_result == 0 && replied && success;
}

static MTLSharedEventHandle *
take_event_one(NSXPCConnection *connection, uint64_t token)
{
	__block MTLSharedEventHandle *result = nil;
	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

	id<IPCMetalXPCBrokerProtocol> proxy =
	    [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
		    const char *message = error.localizedDescription.UTF8String;
		    U_LOG_E("Metal XPC shared-event take failed: %s", message != NULL ? message : "unknown error");
		    dispatch_semaphore_signal(semaphore);
	    }];

	[proxy takeSharedEventHandleForToken:token
	                              reply:^(MTLSharedEventHandle *handle) {
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
mark_texture_token_claimable_sync(NSXPCConnection *connection, uint64_t token)
{
	__block BOOL success = NO;
	__block BOOL replied = NO;
	dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

	id<IPCMetalXPCBrokerProtocol> proxy =
	    [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
		    const char *message = error.localizedDescription.UTF8String;
		    U_LOG_E("Metal XPC mark-claimable failed: %s", message != NULL ? message : "unknown error");
		    dispatch_semaphore_signal(semaphore);
	    }];

	[proxy markTextureTokenClaimable:token
	                          reply:^(BOOL remote_success) {
		                          success = remote_success;
		                          replied = YES;
		                          dispatch_semaphore_signal(semaphore);
	                          }];

	long wait_result =
	    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, IPC_METAL_XPC_TIMEOUT_NS));
	return wait_result == 0 && replied && success;
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


bool
ipc_metal_xpc_importance_enabled(void)
{
	const char *value = getenv("XRT_MACOS_XPC_IMPORTANCE");
	if (value == NULL || value[0] == '\0' || strcmp(value, "0") == 0 || strcmp(value, "off") == 0 ||
	    strcmp(value, "false") == 0) {
		return false;
	}
	return true;
}

xrt_result_t
ipc_metal_xpc_importance_acquire(struct ipc_metal_xpc_importance_lease **out_lease)
{
	if (out_lease == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	*out_lease = NULL;

	if (!ipc_metal_xpc_importance_enabled()) {
		return XRT_SUCCESS;
	}

	@autoreleasepool {
		NSXPCConnection *connection = create_service_connection();
		if (connection == nil) {
			U_LOG_E("XR XPC importance could not create service connection");
			return XRT_ERROR_IPC_FAILURE;
		}

		uint64_t session_id = make_importance_session_id();
		__block BOOL barrier_replied = NO;
		__block BOOL barrier_active = NO;
		dispatch_semaphore_t barrier = dispatch_semaphore_create(0);

		connection.interruptionHandler = ^{
			U_LOG_W("XR XPC importance connection interrupted session=0x%016llx",
			        (unsigned long long)session_id);
		};
		connection.invalidationHandler = ^{
			U_LOG_D("XR XPC importance connection invalidated session=0x%016llx",
			        (unsigned long long)session_id);
		};

		id<IPCMetalXPCServiceProtocol> proxy =
		    [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
			    const char *message = error.localizedDescription.UTF8String;
			    U_LOG_E("XR XPC importance request failed session=0x%016llx: %s",
			            (unsigned long long)session_id,
			            message != NULL ? message : "unknown error");
			    dispatch_semaphore_signal(barrier);
		    }];

		/*
		 * The service deliberately keeps this reply block outstanding until
		 * release. That long-lived request is the foreground-client provenance
		 * signal this diagnostic is testing.
		 */
		[proxy acquireXRSessionImportance:session_id
		                           reply:^{
			                           U_LOG_D("XR XPC importance acquire reply completed session=0x%016llx",
			                                   (unsigned long long)session_id);
		                           }];

		/*
		 * A second message on the same connection is a readiness barrier. When
		 * it replies, the acquire message has already run and the server has
		 * retained its outstanding reply.
		 */
		[proxy importanceLeaseBarrier:session_id
		                       reply:^(BOOL active) {
			                       barrier_active = active;
			                       barrier_replied = YES;
			                       dispatch_semaphore_signal(barrier);
		                       }];

		long wait_result =
		    dispatch_semaphore_wait(barrier, dispatch_time(DISPATCH_TIME_NOW, IPC_XPC_IMPORTANCE_TIMEOUT_NS));
		if (wait_result != 0 || !barrier_replied || !barrier_active) {
			U_LOG_E("XR XPC importance barrier failed session=0x%016llx replied=%s active=%s",
			        (unsigned long long)session_id,
			        barrier_replied ? "true" : "false",
			        barrier_active ? "true" : "false");
			[connection invalidate];
			[connection release];
			return XRT_ERROR_IPC_FAILURE;
		}

		struct ipc_metal_xpc_importance_lease *lease =
		    calloc(1, sizeof(struct ipc_metal_xpc_importance_lease));
		if (lease == NULL) {
			[connection invalidate];
			[connection release];
			return XRT_ERROR_ALLOCATION;
		}

		lease->connection = connection;
		lease->session_id = session_id;
		*out_lease = lease;

		U_LOG_I("XR XPC importance lease established pid=%d session=0x%016llx",
		        (int)getpid(),
		        (unsigned long long)session_id);
		return XRT_SUCCESS;
	}
}

void
ipc_metal_xpc_importance_release(struct ipc_metal_xpc_importance_lease **lease_ptr)
{
	if (lease_ptr == NULL || *lease_ptr == NULL) {
		return;
	}

	struct ipc_metal_xpc_importance_lease *lease = *lease_ptr;
	*lease_ptr = NULL;

	@autoreleasepool {
		NSXPCConnection *connection = lease->connection;
		uint64_t session_id = lease->session_id;

		__block BOOL release_replied = NO;
		__block BOOL released = NO;
		dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
		id<IPCMetalXPCServiceProtocol> proxy =
		    [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
			    const char *message = error.localizedDescription.UTF8String;
			    U_LOG_W("XR XPC importance release failed session=0x%016llx: %s",
			            (unsigned long long)session_id,
			            message != NULL ? message : "unknown error");
			    dispatch_semaphore_signal(semaphore);
		    }];

		[proxy releaseXRSessionImportance:session_id
		                           reply:^(BOOL remote_released) {
			                           released = remote_released;
			                           release_replied = YES;
			                           dispatch_semaphore_signal(semaphore);
		                           }];

		long wait_result =
		    dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, IPC_XPC_IMPORTANCE_TIMEOUT_NS));
		if (wait_result != 0 || !release_replied || !released) {
			U_LOG_W("XR XPC importance release not acknowledged session=0x%016llx replied=%s released=%s",
			        (unsigned long long)session_id,
			        release_replied ? "true" : "false",
			        released ? "true" : "false");
		} else {
			U_LOG_I("XR XPC importance lease released pid=%d session=0x%016llx",
			        (int)getpid(),
			        (unsigned long long)session_id);
		}

		[connection invalidate];
		[connection release];
		free(lease);
	}
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

			ok = publish_texture_one(connection, handle, token, i, image_count);
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
ipc_metal_xpc_publish_claimable_textures(void *const *metal_textures,
                                         uint32_t image_count,
                                         uint64_t *out_token)
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

		uint64_t token = make_external_texture_token();
		bool ok = true;
		for (uint32_t i = 0; i < image_count; i++) {
			id<MTLTexture> texture = (__bridge id<MTLTexture>)metal_textures[i];
			if (texture == nil) {
				ok = false;
				break;
			}

			MTLSharedTextureHandle *handle = [texture newSharedTextureHandle];
			if (handle == nil) {
				ok = false;
				break;
			}
			ok = publish_texture_one(connection, handle, token, i, image_count);
			[handle release];
			if (!ok) {
				break;
			}
		}

		ok = ok && mark_texture_token_claimable_sync(connection, token);
		if (!ok) {
			(void)discard_sync(connection, token);
			[connection invalidate];
			[connection release];
			return XRT_ERROR_IPC_FAILURE;
		}

		[connection invalidate];
		[connection release];
		*out_token = token;
		U_LOG_I("Metal XPC published %u claimable texture handle(s) token=0x%016llx",
		        image_count,
		        (unsigned long long)token);
		return XRT_SUCCESS;
	}
}

xrt_result_t
ipc_metal_xpc_take_textures_on_device(uint64_t token,
                                      uint32_t expected_count,
                                      void *metal_device,
                                      void **out_metal_textures)
{
	if (!texture_token_is_valid(token) || out_metal_textures == NULL || expected_count == 0 ||
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
			MTLSharedTextureHandle *handle = take_texture_one(connection, token, i);
			if (handle == nil) {
				U_LOG_E("Metal XPC broker had no texture handle for token=0x%016llx image=%u",
				        (unsigned long long)token,
				        i);
				xret = XRT_ERROR_IPC_FAILURE;
				break;
			}

			id<MTLDevice> device = metal_device != NULL
			                           ? (__bridge id<MTLDevice>)metal_device
			                           : handle.device;
			id<MTLTexture> texture =
			    device != nil ? [device newSharedTextureWithHandle:handle] : nil;
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

xrt_result_t
ipc_metal_xpc_take_textures(uint64_t token, uint32_t expected_count, void **out_metal_textures)
{
	return ipc_metal_xpc_take_textures_on_device(token, expected_count, NULL, out_metal_textures);
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

xrt_result_t
ipc_metal_xpc_publish_shared_event(void *metal_shared_event, uint64_t *out_token)
{
	if (metal_shared_event == NULL || out_token == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	*out_token = 0;

	@autoreleasepool {
		id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)metal_shared_event;
		MTLSharedEventHandle *handle = [event newSharedEventHandle];
		if (handle == nil) {
			U_LOG_E("Metal XPC could not create MTLSharedEventHandle");
			return XRT_ERROR_ALLOCATION;
		}

		NSXPCConnection *connection = create_connection();
		if (connection == nil) {
			[handle release];
			return XRT_ERROR_IPC_FAILURE;
		}

		uint64_t token = make_token();
		bool ok = publish_event_one(connection, handle, token);
		[handle release];
		if (!ok) {
			(void)discard_sync(connection, token);
			[connection invalidate];
			[connection release];
			return XRT_ERROR_IPC_FAILURE;
		}

		[connection invalidate];
		[connection release];
		*out_token = token;
		U_LOG_I("Metal XPC published shared event token=0x%016llx value=%llu",
		        (unsigned long long)token,
		        (unsigned long long)event.signaledValue);
		return XRT_SUCCESS;
	}
}

xrt_result_t
ipc_metal_xpc_take_shared_event(uint64_t token, void *metal_device, void **out_metal_shared_event)
{
	if (!standard_token_is_valid(token) || metal_device == NULL || out_metal_shared_event == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	*out_metal_shared_event = NULL;

	@autoreleasepool {
		NSXPCConnection *connection = create_connection();
		if (connection == nil) {
			return XRT_ERROR_IPC_FAILURE;
		}

		MTLSharedEventHandle *handle = take_event_one(connection, token);
		if (handle == nil) {
			(void)discard_sync(connection, token);
			[connection invalidate];
			[connection release];
			U_LOG_E("Metal XPC broker had no shared-event handle for token=0x%016llx",
			        (unsigned long long)token);
			return XRT_ERROR_IPC_FAILURE;
		}

		id<MTLDevice> device = (__bridge id<MTLDevice>)metal_device;
		id<MTLSharedEvent> event = [device newSharedEventWithHandle:handle];
		[handle release];
		(void)discard_sync(connection, token);
		[connection invalidate];
		[connection release];

		if (event == nil) {
			U_LOG_E("Metal XPC could not recreate shared event token=0x%016llx on device=%p",
			        (unsigned long long)token,
			        metal_device);
			return XRT_ERROR_ALLOCATION;
		}

		*out_metal_shared_event = (__bridge void *)event;
		U_LOG_I("Metal XPC took and recreated shared event token=0x%016llx device=%p value=%llu",
		        (unsigned long long)token,
		        metal_device,
		        (unsigned long long)event.signaledValue);
		return XRT_SUCCESS;
	}
}

void
ipc_metal_xpc_release_shared_event(void *metal_shared_event)
{
	id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)metal_shared_event;
	[event release];
}

void
ipc_metal_xpc_begin_shared_event_request(void *metal_device)
{
	if (g_shared_event_request_event != NULL) {
		ipc_metal_xpc_release_shared_event(g_shared_event_request_event);
		g_shared_event_request_event = NULL;
	}
	g_shared_event_request_device = metal_device;
	g_shared_event_request_active = true;
}

bool
ipc_metal_xpc_shared_event_request_active(void)
{
	return g_shared_event_request_active;
}

xrt_result_t
ipc_metal_xpc_resolve_shared_event_request(uint64_t token)
{
	if (!g_shared_event_request_active || g_shared_event_request_device == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	void *raw_event = NULL;
	xrt_result_t xret =
	    ipc_metal_xpc_take_shared_event(token, g_shared_event_request_device, &raw_event);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	if (g_shared_event_request_event != NULL) {
		ipc_metal_xpc_release_shared_event(g_shared_event_request_event);
	}
	g_shared_event_request_event = raw_event;
	return XRT_SUCCESS;
}

bool
ipc_metal_xpc_end_shared_event_request(void **out_metal_shared_event)
{
	if (out_metal_shared_event == NULL) {
		return false;
	}
	*out_metal_shared_event = NULL;

	void *raw_event = g_shared_event_request_event;
	g_shared_event_request_event = NULL;
	bool had_request = g_shared_event_request_active;
	g_shared_event_request_active = false;
	g_shared_event_request_device = NULL;

	if (!had_request || raw_event == NULL) {
		if (raw_event != NULL) {
			ipc_metal_xpc_release_shared_event(raw_event);
		}
		return false;
	}

	/* Match the borrowed-object contract of the in-process provider. */
	id<MTLSharedEvent> event = (__bridge id<MTLSharedEvent>)raw_event;
	*out_metal_shared_event = (__bridge void *)[event autorelease];
	return true;
}

void
ipc_metal_xpc_discard_token(uint64_t token)
{
	if (!texture_token_is_valid(token)) {
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
	if (!standard_token_is_valid(token) || out_images == NULL) {
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
	if (!standard_token_is_valid(token)) {
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
