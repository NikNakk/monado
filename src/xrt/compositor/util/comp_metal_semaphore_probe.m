// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS Metal shared-event semaphore provider and diagnostic probe.
 * @ingroup comp_util
 */

#import <Metal/Metal.h>
#import <mach/mach.h>
#import <servers/bootstrap.h>

#include "util/comp_metal_semaphore_probe.h"
#include "util/comp_semaphore.h"
#include "util/u_logging.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

struct comp_metal_semaphore_provider
{
	pthread_mutex_t mutex;
	bool attempted;
	struct vk_bundle *vk;
};

static struct comp_metal_semaphore_provider g_provider = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};

void
comp_metal_semaphore_probe(struct vk_bundle *vk)
{
	pthread_mutex_lock(&g_provider.mutex);
	if (g_provider.attempted) {
		pthread_mutex_unlock(&g_provider.mutex);
		return;
	}
	g_provider.attempted = true;
	pthread_mutex_unlock(&g_provider.mutex);

	struct xrt_compositor_semaphore *xcsem = NULL;
	void *raw_shared_event = NULL;
	xrt_result_t xret = comp_semaphore_create_metal_shared_event(vk, &xcsem, &raw_shared_event);
	if (xret != XRT_SUCCESS) {
		U_LOG_W("Metal timeline shared-event probe unavailable: result=%d; blocking app release handoff remains active",
		        xret);
		return;
	}

	bool usable = false;
	@autoreleasepool {
		// vkExportMetalObjectsEXT does not transfer Objective-C ownership.
		// Retain the export while inspecting it, then release it before the
		// backing Vulkan semaphore is destroyed below.
		id<MTLSharedEvent> shared_event = [(__bridge id<MTLSharedEvent>)raw_shared_event retain];
		if (shared_event == nil) {
			U_LOG_W("Metal timeline shared-event probe returned a nil MTLSharedEvent after export");
		} else {
			usable = true;
			U_LOG_I("Metal timeline shared-event probe succeeded: event=%p initial_value=%llu; in-process client provider registered",
			        (__bridge void *)shared_event,
			        (unsigned long long)shared_event.signaledValue);
			[shared_event release];
		}
	}

	xrt_compositor_semaphore_reference(&xcsem, NULL);

	if (usable) {
		pthread_mutex_lock(&g_provider.mutex);
		g_provider.vk = vk;
		pthread_mutex_unlock(&g_provider.mutex);
	}
}

xrt_result_t
comp_metal_semaphore_create_client_pair(struct xrt_compositor_semaphore **out_xcsem, void **out_mtl_shared_event)
{
	if (out_xcsem == NULL || out_mtl_shared_event == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

	*out_xcsem = NULL;
	*out_mtl_shared_event = NULL;

	pthread_mutex_lock(&g_provider.mutex);
	struct vk_bundle *vk = g_provider.vk;
	if (vk == NULL) {
		pthread_mutex_unlock(&g_provider.mutex);
		return XRT_ERROR_VULKAN;
	}

	// Keep the provider registered and its Vulkan bundle alive while creating
	// this client-owned pair. comp_base_fini clears the provider only after all
	// in-process clients have been torn down.
	xrt_result_t xret = comp_semaphore_create_metal_shared_event(vk, out_xcsem, out_mtl_shared_event);
	pthread_mutex_unlock(&g_provider.mutex);
	return xret;
}


@protocol MonadoMTLDeviceSharedEventSPI <MTLDevice>
- (id<MTLSharedEvent>)newSharedEventWithMachPort:(mach_port_t)machPort;
@end

xrt_result_t
comp_metal_semaphore_import_bootstrap_event(const char *bootstrap_name,
                                            struct xrt_compositor_semaphore **out_xcsem)
{
	if (bootstrap_name == NULL || bootstrap_name[0] == '\0' || out_xcsem == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	*out_xcsem = NULL;

	pthread_mutex_lock(&g_provider.mutex);
	struct vk_bundle *vk = g_provider.vk;
	if (vk == NULL) {
		pthread_mutex_unlock(&g_provider.mutex);
		U_LOG_E("DXMT shared-event import unavailable: Metal semaphore provider is not registered");
		return XRT_ERROR_VULKAN;
	}
	if (vk->vkExportMetalObjectsEXT == NULL) {
		pthread_mutex_unlock(&g_provider.mutex);
		U_LOG_E("DXMT shared-event import unavailable: vkExportMetalObjectsEXT is not available");
		return XRT_ERROR_VULKAN;
	}

	VkExportMetalDeviceInfoEXT device_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT,
	};
	VkExportMetalObjectsInfoEXT objects_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	    .pNext = &device_info,
	};
	vk->vkExportMetalObjectsEXT(vk->device, &objects_info);
	id<MTLDevice> device = (__bridge id<MTLDevice>)device_info.mtlDevice;
	if (device == nil) {
		pthread_mutex_unlock(&g_provider.mutex);
		U_LOG_E("DXMT shared-event import unavailable: Vulkan Metal device export returned nil");
		return XRT_ERROR_VULKAN;
	}

	mach_port_t bootstrap_port = MACH_PORT_NULL;
	kern_return_t kr = task_get_bootstrap_port(mach_task_self(), &bootstrap_port);
	if (kr != KERN_SUCCESS || bootstrap_port == MACH_PORT_NULL) {
		pthread_mutex_unlock(&g_provider.mutex);
		U_LOG_E("DXMT shared-event import could not get bootstrap port: %d", kr);
		return XRT_ERROR_IPC_FAILURE;
	}

	mach_port_t event_port = MACH_PORT_NULL;
	kr = bootstrap_look_up(bootstrap_port, (char *)bootstrap_name, &event_port);
	mach_port_deallocate(mach_task_self(), bootstrap_port);
	if (kr != KERN_SUCCESS || event_port == MACH_PORT_NULL) {
		pthread_mutex_unlock(&g_provider.mutex);
		U_LOG_E("DXMT shared-event bootstrap lookup failed for '%s': %d", bootstrap_name, kr);
		return XRT_ERROR_IPC_FAILURE;
	}

	xrt_result_t xret = XRT_ERROR_VULKAN;
	@autoreleasepool {
		id<MonadoMTLDeviceSharedEventSPI> spi_device = (id<MonadoMTLDeviceSharedEventSPI>)device;
		id<MTLSharedEvent> event = [spi_device newSharedEventWithMachPort:event_port];
		if (event == nil) {
			U_LOG_E("DXMT shared-event reconstruction failed for '%s'", bootstrap_name);
		} else {
			uint64_t initial_value = event.signaledValue;
			xret = comp_semaphore_import_metal_shared_event(
			    vk, (__bridge void *)event, initial_value, out_xcsem);
			if (xret == XRT_SUCCESS) {
				U_LOG_I("DXMT shared-event imported: name='%s' initial_value=%llu event=%p",
				        bootstrap_name,
				        (unsigned long long)initial_value,
				        (__bridge void *)event);
			}
			[event release];
		}
	}

	mach_port_deallocate(mach_task_self(), event_port);
	pthread_mutex_unlock(&g_provider.mutex);
	return xret;
}

void
comp_metal_semaphore_provider_clear(struct vk_bundle *vk)
{
	pthread_mutex_lock(&g_provider.mutex);
	if (g_provider.vk == vk) {
		g_provider.vk = NULL;
		g_provider.attempted = false;
	}
	pthread_mutex_unlock(&g_provider.mutex);
}
