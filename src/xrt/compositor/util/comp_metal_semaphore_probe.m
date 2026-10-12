// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS Metal shared-event semaphore provider and diagnostic probe.
 * @ingroup comp_util
 */

#import <Metal/Metal.h>

#include "util/comp_metal_semaphore_probe.h"
#include "util/comp_semaphore.h"
#include "util/u_logging.h"
#include "os/os_time.h"
#include "vk/vk_cmd.h"

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
		U_LOG_W(
		    "Metal timeline shared-event probe unavailable: result=%d; blocking app release handoff remains "
		    "active",
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
			U_LOG_I(
			    "Metal timeline shared-event probe succeeded: event=%p initial_value=%llu; in-process "
			    "client provider registered",
			    (__bridge void *)shared_event, (unsigned long long)shared_event.signaledValue);
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


xrt_result_t
comp_metal_semaphore_import_shared_event_handle(void *mtl_shared_event_handle,
                                                struct xrt_compositor_semaphore **out_xcsem)
{
	if (mtl_shared_event_handle == NULL || out_xcsem == NULL) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	*out_xcsem = NULL;

	pthread_mutex_lock(&g_provider.mutex);
	struct vk_bundle *vk = g_provider.vk;
	if (vk == NULL || vk->vkExportMetalObjectsEXT == NULL) {
		pthread_mutex_unlock(&g_provider.mutex);
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
		return XRT_ERROR_VULKAN;
	}

	MTLSharedEventHandle *handle = (__bridge MTLSharedEventHandle *)mtl_shared_event_handle;
	id<MTLSharedEvent> event = [device newSharedEventWithHandle:handle];
	if (event == nil) {
		pthread_mutex_unlock(&g_provider.mutex);
		return XRT_ERROR_VULKAN;
	}

	uint64_t initial_value = event.signaledValue;
	xrt_result_t xret =
	    comp_semaphore_import_metal_shared_event(vk, (__bridge void *)event, initial_value, out_xcsem);
	[event release];
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

xrt_result_t
comp_metal_semaphore_prime_gpu_wait(struct xrt_compositor_semaphore *xcsem, uint64_t value, uint64_t timeout_ns)
{
	// Only semaphores from comp_metal_semaphore_create_client_pair are expected.
	if (xcsem == NULL) {
		return XRT_ERROR_VULKAN;
	}
	struct comp_semaphore *csem = comp_semaphore(xcsem);
	struct vk_bundle *vk = csem->vk;

	VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence = VK_NULL_HANDLE;
	VkResult ret = vk->vkCreateFence(vk->device, &fence_info, NULL, &fence);
	if (ret != VK_SUCCESS) {
		return XRT_ERROR_VULKAN;
	}

	// A wait-only submission: the GPU's first wait on this event happens now,
	// before the client can signal it, rather than in the first compositor frame.
	const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkTimelineSemaphoreSubmitInfo timeline_info = {
	    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
	    .waitSemaphoreValueCount = 1,
	    .pWaitSemaphoreValues = &value,
	};
	VkSubmitInfo submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .pNext = &timeline_info,
	    .waitSemaphoreCount = 1,
	    .pWaitSemaphores = &csem->semaphore,
	    .pWaitDstStageMask = &stage,
	};

	const int64_t begin_ns = os_monotonic_get_ns();
	ret = vk_cmd_submit_locked(vk, vk->main_queue, 1, &submit, fence);
	if (ret == VK_SUCCESS) {
		ret = vk->vkWaitForFences(vk->device, 1, &fence, VK_TRUE, timeout_ns);
	}
	const int64_t elapsed_ns = os_monotonic_get_ns() - begin_ns;
	if (ret == VK_TIMEOUT) {
		// The submission may still be pending; keep the fence rather than destroy it in use.
		U_LOG_E("Metal producer event GPU prime timed out after %lld ns (value=%llu)", (long long)elapsed_ns,
		        (unsigned long long)value);
		return XRT_TIMEOUT;
	}
	vk->vkDestroyFence(vk->device, fence, NULL);
	if (ret != VK_SUCCESS) {
		U_LOG_E("Metal producer event GPU prime failed: %s", vk_result_string(ret));
		return XRT_ERROR_VULKAN;
	}
	U_LOG_I("Metal producer event GPU prime complete: value=%llu elapsed_ns=%lld", (unsigned long long)value,
	        (long long)elapsed_ns);
	return XRT_SUCCESS;
}
