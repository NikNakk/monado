// Copyright 2019-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Independent semaphore implementation.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_util
 */

#include "util/u_handles.h"

#include "util/comp_semaphore.h"


/*
 *
 * Member functions.
 *
 */

#ifdef VK_KHR_timeline_semaphore
static xrt_result_t
comp_semaphore_wait(struct xrt_compositor_semaphore *xcsem, uint64_t value, uint64_t timeout_ns)
{
	struct comp_semaphore *csem = comp_semaphore(xcsem);
	struct vk_bundle *vk = csem->vk;
	VkResult ret;

	VkSemaphoreWaitInfo wait_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
	    .flags = 0,
	    .semaphoreCount = 1,
	    .pSemaphores = &csem->semaphore,
	    .pValues = &value,
	};

	ret = vk->vkWaitSemaphores( //
	    vk->device,             // device
	    &wait_info,             // pWaitInfo
	    timeout_ns);            // timeout
	if (ret == VK_TIMEOUT) {
		return XRT_TIMEOUT;
	}
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkWaitSemaphores: %s", vk_result_string(ret));
		return XRT_ERROR_VULKAN;
	}

	return XRT_SUCCESS;
}

static void
comp_semaphore_destroy(struct xrt_compositor_semaphore *xcsem)
{
	struct comp_semaphore *csem = comp_semaphore(xcsem);
	struct vk_bundle *vk = csem->vk;

	if (csem->semaphore != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore( //
		    vk->device,         // device
		    csem->semaphore,    // semaphore
		    NULL);              // pAllocator
		csem->semaphore = VK_NULL_HANDLE;
	}

	// Does invalid checking and sets to invalid.
	u_graphics_sync_unref(&csem->handle);

	// Do the final freeing.
	free(csem);
}
#endif


/*
 *
 * 'Exported' functions.
 *
 */

xrt_result_t
comp_semaphore_create(struct vk_bundle *vk,
                      xrt_graphics_sync_handle_t *out_handle,
                      struct xrt_compositor_semaphore **out_xcsem)
{
#ifdef VK_KHR_timeline_semaphore
	VkResult ret;

	if (!vk->features.timeline_semaphore) {
		return XRT_ERROR_VULKAN;
	}

	VkSemaphore semaphore;
	xrt_graphics_sync_handle_t handle;
	ret = vk_create_timeline_semaphore_and_native(vk, &semaphore, &handle);
	if (ret != VK_SUCCESS) {
		return XRT_ERROR_VULKAN;
	}

	VK_NAME_SEMAPHORE(vk, semaphore, "comp_semaphore timeline");

	struct comp_semaphore *csem = U_TYPED_CALLOC(struct comp_semaphore);

	csem->base.reference.count = 1;
	csem->base.destroy = comp_semaphore_destroy;
	csem->base.wait = comp_semaphore_wait;
	csem->semaphore = semaphore;
	csem->handle = handle;
	csem->vk = vk;

	*out_xcsem = &csem->base;
	*out_handle = handle;

	return XRT_SUCCESS;
#else
	// How did you even get here?
	VK_ERROR(vk, "No compile time support for VK_KHR_timeline_semaphore!");
	return XRT_ERROR_VULKAN;
#endif
}

#ifdef XRT_OS_OSX
xrt_result_t
comp_semaphore_create_metal_shared_event(struct vk_bundle *vk,
                                         struct xrt_compositor_semaphore **out_xcsem,
                                         void **out_mtl_shared_event)
{
#ifdef VK_KHR_timeline_semaphore
	if (!vk->features.timeline_semaphore || !vk->has_EXT_metal_objects || vk->vkExportMetalObjectsEXT == NULL) {
		return XRT_ERROR_VULKAN;
	}

	VkSemaphoreTypeCreateInfo type_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
	    .pNext = NULL,
	    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	    .initialValue = 0,
	};

	VkExportMetalObjectCreateInfoEXT metal_export_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT,
	    .pNext = &type_info,
	    .exportObjectType = VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT,
	};

	VkSemaphoreCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	    .pNext = &metal_export_info,
	    .flags = 0,
	};

	VkSemaphore semaphore = VK_NULL_HANDLE;
	VkResult ret = vk->vkCreateSemaphore(vk->device, &create_info, NULL, &semaphore);
	if (ret != VK_SUCCESS) {
		VK_ERROR(vk, "vkCreateSemaphore for Metal shared event: %s", vk_result_string(ret));
		return XRT_ERROR_VULKAN;
	}

	VK_NAME_SEMAPHORE(vk, semaphore, "comp_semaphore Metal shared-event timeline");

	VkExportMetalSharedEventInfoEXT shared_event_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT,
	    .pNext = NULL,
	    .semaphore = semaphore,
	    .event = VK_NULL_HANDLE,
	    .mtlSharedEvent = NULL,
	};
	VkExportMetalObjectsInfoEXT export_info = {
	    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
	    .pNext = &shared_event_info,
	};

	vk->vkExportMetalObjectsEXT(vk->device, &export_info);
	if (shared_event_info.mtlSharedEvent == NULL) {
		VK_ERROR(vk, "vkExportMetalObjectsEXT returned no MTLSharedEvent for timeline semaphore");
		vk->vkDestroySemaphore(vk->device, semaphore, NULL);
		return XRT_ERROR_VULKAN;
	}

	struct comp_semaphore *csem = U_TYPED_CALLOC(struct comp_semaphore);
	if (csem == NULL) {
		vk->vkDestroySemaphore(vk->device, semaphore, NULL);
		return XRT_ERROR_ALLOCATION;
	}

	csem->base.reference.count = 1;
	csem->base.destroy = comp_semaphore_destroy;
	csem->base.wait = comp_semaphore_wait;
	csem->semaphore = semaphore;
	csem->handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	csem->vk = vk;

	*out_xcsem = &csem->base;
	*out_mtl_shared_event = (void *)shared_event_info.mtlSharedEvent;

	return XRT_SUCCESS;
#else
	VK_ERROR(vk, "No compile time support for VK_KHR_timeline_semaphore!");
	return XRT_ERROR_VULKAN;
#endif
}
#endif
