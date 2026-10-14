// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Metal shared-event frame handoff for the Vulkan client compositor on macOS.
 * @ingroup comp_client
 */

#import <Metal/Metal.h>

#include "xrt/xrt_config_build.h"
#include "client/comp_vk_client_shared_event.h"
#include "util/comp_metal_semaphore_provider.h"
#ifdef XRT_FEATURE_SERVICE
#include "client/comp_metal_service_semaphore.h"
#endif

#include "util/u_debug.h"
#include "util/u_logging.h"

DEBUG_GET_ONCE_BOOL_OPTION(vk_client_shared_event, "XRT_MACOS_VK_CLIENT_SHARED_EVENT", false)

bool
client_vk_shared_event_enabled(void)
{
	return debug_get_bool_option_vk_client_shared_event();
}

xrt_result_t
client_vk_shared_event_create(struct xrt_compositor_native *xcn,
                              struct vk_bundle *vk,
                              VkSemaphore *out_semaphore,
                              struct xrt_compositor_semaphore **out_xcsem,
                              uint64_t *out_value)
{
	if (!vk->features.timeline_semaphore || !vk->has_EXT_metal_objects || vk->vkExportMetalObjectsEXT == NULL) {
		U_LOG_W("Vulkan client shared-event handoff needs timeline semaphores and VK_EXT_metal_objects");
		return XRT_ERROR_VULKAN;
	}

	// The pair helpers return a borrowed, possibly autoreleased, event.
	@autoreleasepool {
		VkExportMetalDeviceInfoEXT device_info = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
		VkExportMetalObjectsInfoEXT objects_info = {
		    .sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
		    .pNext = &device_info,
		};
		vk->vkExportMetalObjectsEXT(vk->device, &objects_info);
		id<MTLDevice> device = (id<MTLDevice>)device_info.mtlDevice;
		if (device == nil) {
			U_LOG_W("Vulkan client shared-event handoff: VkDevice exported no MTLDevice");
			return XRT_ERROR_VULKAN;
		}

		struct xrt_compositor_semaphore *xcsem = NULL;
		void *raw_event = NULL;
#ifdef XRT_FEATURE_SERVICE
		// Through monado-service, or locally when this client composites in-process.
		xrt_result_t xret =
		    xcn->is_remote
		        ? client_metal_service_semaphore_create_pair(&xcn->base, (void *)device, &xcsem, &raw_event)
		        : comp_metal_semaphore_create_client_pair(&xcsem, &raw_event);
#else
		xrt_result_t xret = comp_metal_semaphore_create_client_pair(&xcsem, &raw_event);
#endif
		id<MTLSharedEvent> event = (id<MTLSharedEvent>)raw_event;
		if (xret != XRT_SUCCESS || xcsem == NULL || event == nil) {
			U_LOG_W("Vulkan client shared-event handoff: no semaphore pair (result=%d)", xret);
			xrt_compositor_semaphore_reference(&xcsem, NULL);
			return xret != XRT_SUCCESS ? xret : XRT_ERROR_VULKAN;
		}
		// Events rebuilt from a handle report no device; the service path built it on ours.
		if (event.device != nil && event.device.registryID != device.registryID) {
			U_LOG_W("Vulkan client shared-event handoff: event is on another GPU (%llu, device %llu)",
			        (unsigned long long)event.device.registryID, (unsigned long long)device.registryID);
			xrt_compositor_semaphore_reference(&xcsem, NULL);
			return XRT_ERROR_VULKAN;
		}

		// Import at the event's current value so the import does not move a live timeline.
		uint64_t value = event.signaledValue;
		VkSemaphoreTypeCreateInfo type_info = {
		    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
		    .initialValue = value,
		};
		VkImportMetalSharedEventInfoEXT import_info = {
		    .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_SHARED_EVENT_INFO_EXT,
		    .pNext = &type_info,
		    .mtlSharedEvent = (MTLSharedEvent_id)event,
		};
		VkSemaphoreCreateInfo create_info = {
		    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		    .pNext = &import_info,
		};
		// The imported semaphore retains the event.
		VkSemaphore semaphore = VK_NULL_HANDLE;
		VkResult ret = vk->vkCreateSemaphore(vk->device, &create_info, NULL, &semaphore);
		if (ret != VK_SUCCESS) {
			VK_ERROR(vk, "vkCreateSemaphore importing MTLSharedEvent: %s", vk_result_string(ret));
			xrt_compositor_semaphore_reference(&xcsem, NULL);
			return XRT_ERROR_VULKAN;
		}
		VK_NAME_SEMAPHORE(vk, semaphore, "client shared-event timeline semaphore");

		*out_semaphore = semaphore;
		*out_xcsem = xcsem;
		*out_value = value;
		U_LOG_I("Vulkan client frame handoff: Metal shared event (%s), initial value %llu; no queue-idle wait",
		        xcn->is_remote ? "service" : "in-process", (unsigned long long)value);
	}
	return XRT_SUCCESS;
}
