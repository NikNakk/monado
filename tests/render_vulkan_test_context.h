// Copyright 2026, NVIDIA CORPORATION.
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "util/comp_vulkan.h"
#include "util/u_extension_list.h"
#include "vk/vk_helpers.h"

#ifdef XRT_OS_OSX
extern "C" bool
tests_macos_has_metal_device(void);
#endif

struct MinimalVulkanContext
{
	vk_bundle vk{};

	~MinimalVulkanContext()
	{
		shutdown();
	}

	bool
	init()
	{
#ifdef XRT_OS_OSX
		// Hosted macOS VMs may not expose a Metal GPU. Avoid entering the ICD
		// in that case; the fixture reports the unavailable device as a skip.
		if (!tests_macos_has_metal_device()) {
			return false;
		}
#endif
		static const char *required_instance_extensions[] = {nullptr};
		static const char *optional_instance_extensions[] = {
		    VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
		    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
		    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
		    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
#ifdef XRT_OS_OSX
		    VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
#endif
		};
		static const char *required_device_extensions[] = {nullptr};
		static const char *optional_device_extensions[] = {
#ifdef XRT_OS_OSX
		    "VK_KHR_portability_subset", // May be absent from non-beta Vulkan headers.
#endif
		    nullptr,
		};

		auto required_instance_ext_list = u_extension_list_create_from_array(
		    required_instance_extensions, ARRAY_SIZE(required_instance_extensions));
		auto optional_instance_ext_list = u_extension_list_create_from_array(
		    optional_instance_extensions, ARRAY_SIZE(optional_instance_extensions));
		auto required_device_ext_list = u_extension_list_create_from_array(
		    required_device_extensions, ARRAY_SIZE(required_device_extensions));
		auto optional_device_ext_list = u_extension_list_create_from_array(
		    optional_device_extensions, ARRAY_SIZE(optional_device_extensions));

		struct comp_vulkan_arguments vk_args = {
		    .required_instance_version = VK_MAKE_VERSION(1, 0, 0),
		    .get_instance_proc_address = vkGetInstanceProcAddr,
		    .required_instance_extensions = required_instance_ext_list,
		    .optional_instance_extensions = optional_instance_ext_list,
		    .required_device_extensions = required_device_ext_list,
		    .optional_device_extensions = optional_device_ext_list,
		    .log_level = U_LOGGING_WARN,
		    .only_compute_queue = false,
		    .timeline_semaphore = false,
		    .selected_gpu_index = -1,
		    .lookup_with_uuid = false,
		    .selected_gpu_uuid = {},
		    .client_gpu_index = -1,
		};

		struct comp_vulkan_results vk_res = {};
		const bool ok = comp_vulkan_init_bundle(&vk, &vk_args, &vk_res);

		u_extension_list_destroy(&required_instance_ext_list);
		u_extension_list_destroy(&optional_instance_ext_list);
		u_extension_list_destroy(&required_device_ext_list);
		u_extension_list_destroy(&optional_device_ext_list);

		return ok;
	}

	void
	shutdown()
	{
		if (vk.device != VK_NULL_HANDLE) {
			vk.vkDeviceWaitIdle(vk.device);
			vk_deinit_mutex(&vk);
			vk.vkDestroyDevice(vk.device, nullptr);
		}
		if (vk.instance != VK_NULL_HANDLE) {
			vk.vkDestroyInstance(vk.instance, nullptr);
		}
		vk = {};
	}
};
