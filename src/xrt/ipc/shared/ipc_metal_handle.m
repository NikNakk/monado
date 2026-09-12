// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS Metal shared-handle serialization helpers for Monado IPC.
 * @ingroup ipc_shared
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#include "shared/ipc_metal_handle.h"
#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>

static xrt_result_t
archive_secure_coding_object(id object, uint8_t **out_bytes, uint32_t *out_size)
{
	if (object == nil || out_bytes == NULL || out_size == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	*out_bytes = NULL;
	*out_size = 0;

	@autoreleasepool {
		if (![object conformsToProtocol:@protocol(NSSecureCoding)]) {
			U_LOG_E("Metal IPC object does not conform to NSSecureCoding: %s", object_getClassName(object));
			return XRT_ERROR_IPC_FAILURE;
		}

		NSError *error = nil;
		NSData *data = [NSKeyedArchiver archivedDataWithRootObject:object requiringSecureCoding:YES error:&error];
		if (data == nil) {
			const char *message = error != nil ? error.localizedDescription.UTF8String : "unknown error";
			U_LOG_E("Could not archive Metal shared handle: %s", message != NULL ? message : "unknown error");
			return XRT_ERROR_IPC_FAILURE;
		}

		NSUInteger length = data.length;
		if (length == 0 || length > UINT32_MAX) {
			U_LOG_E("Archived Metal shared handle has invalid size: %llu", (unsigned long long)length);
			return XRT_ERROR_IPC_FAILURE;
		}

		uint8_t *bytes = malloc(length);
		if (bytes == NULL) {
			return XRT_ERROR_ALLOCATION;
		}
		memcpy(bytes, data.bytes, length);

		*out_bytes = bytes;
		*out_size = (uint32_t)length;
		return XRT_SUCCESS;
	}
}

static xrt_result_t
unarchive_secure_coding_object(Class expected_class,
                               const uint8_t *bytes,
                               uint32_t size,
                               void **out_shared_handle)
{
	if (expected_class == Nil || bytes == NULL || size == 0 || out_shared_handle == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}

	*out_shared_handle = NULL;

	@autoreleasepool {
		NSData *data = [NSData dataWithBytes:bytes length:size];
		if (data == nil) {
			return XRT_ERROR_ALLOCATION;
		}

		NSError *error = nil;
		id object = [NSKeyedUnarchiver unarchivedObjectOfClass:expected_class fromData:data error:&error];
		if (object == nil) {
			const char *message = error != nil ? error.localizedDescription.UTF8String : "unknown error";
			U_LOG_E("Could not unarchive Metal shared handle: %s", message != NULL ? message : "unknown error");
			return XRT_ERROR_IPC_FAILURE;
		}

		/* Return an explicit +1 reference across the C API boundary. */
		[object retain];
		*out_shared_handle = (__bridge void *)object;
		return XRT_SUCCESS;
	}
}

xrt_result_t
ipc_metal_archive_shared_texture_handle(void *shared_handle, uint8_t **out_bytes, uint32_t *out_size)
{
	id object = (__bridge id)shared_handle;
	if (object != nil && ![object isKindOfClass:[MTLSharedTextureHandle class]]) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	return archive_secure_coding_object(object, out_bytes, out_size);
}

xrt_result_t
ipc_metal_unarchive_shared_texture_handle(const uint8_t *bytes, uint32_t size, void **out_shared_handle)
{
	return unarchive_secure_coding_object([MTLSharedTextureHandle class], bytes, size, out_shared_handle);
}

xrt_result_t
ipc_metal_archive_shared_event_handle(void *shared_handle, uint8_t **out_bytes, uint32_t *out_size)
{
	id object = (__bridge id)shared_handle;
	if (object != nil && ![object isKindOfClass:[MTLSharedEventHandle class]]) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	return archive_secure_coding_object(object, out_bytes, out_size);
}

xrt_result_t
ipc_metal_unarchive_shared_event_handle(const uint8_t *bytes, uint32_t size, void **out_shared_handle)
{
	return unarchive_secure_coding_object([MTLSharedEventHandle class], bytes, size, out_shared_handle);
}

void
ipc_metal_shared_handle_release(void *shared_handle)
{
	if (shared_handle == NULL) {
		return;
	}

	id object = (__bridge id)shared_handle;
	[object release];
}
