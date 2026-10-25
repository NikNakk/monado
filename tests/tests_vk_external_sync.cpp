// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "vk/vk_helpers.h"
#include "catch_amalgamated.hpp"
#include <cstring>
#include <cstdint>

#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)
namespace {
struct query_state
{
	unsigned fences, semaphores, fence_calls, semaphore_calls;
} state;
VKAPI_ATTR void VKAPI_CALL
properties(VkPhysicalDevice, VkPhysicalDeviceProperties *p)
{
	*p = {};
}
VKAPI_ATTR void VKAPI_CALL
memory(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *p)
{
	*p = {};
}
VKAPI_ATTR void VKAPI_CALL
queues(VkPhysicalDevice, uint32_t *count, VkQueueFamilyProperties *p)
{
	if (p) {
		*p = {};
		p->queueCount = 1;
	}
	*count = 1;
}
VKAPI_ATTR VkResult VKAPI_CALL
images(VkPhysicalDevice, const VkPhysicalDeviceImageFormatInfo2 *, VkImageFormatProperties2 *)
{
	return VK_ERROR_FORMAT_NOT_SUPPORTED;
}
VKAPI_ATTR void VKAPI_CALL
fences(VkPhysicalDevice, const VkPhysicalDeviceExternalFenceInfo *info, VkExternalFenceProperties *p)
{
	state.fence_calls++;
	p->externalFenceFeatures = (state.fences & info->handleType) ? VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT |
	                                                                   VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT
	                                                             : 0;
}
VKAPI_ATTR void VKAPI_CALL
semaphores(VkPhysicalDevice, const VkPhysicalDeviceExternalSemaphoreInfo *info, VkExternalSemaphoreProperties *p)
{
	state.semaphore_calls++;
	p->externalSemaphoreFeatures =
	    (state.semaphores & info->handleType)
	        ? VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT
	        : 0;
}
VKAPI_ATTR void VKAPI_CALL
queue(VkDevice, uint32_t, uint32_t, VkQueue *p)
{
	*p = (VkQueue)(uintptr_t)4;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
device_proc(VkDevice, const char *name)
{
	if (!std::strcmp(name, "vkGetDeviceQueue"))
		return (PFN_vkVoidFunction)queue;
	// Model the crash condition: all FD export entry points remain NULL.
	return nullptr;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
instance_proc(VkInstance, const char *name)
{
#define PROC(n, fn)                                                                                                    \
	if (!std::strcmp(name, n))                                                                                     \
	return (PFN_vkVoidFunction)fn
	PROC("vkGetDeviceProcAddr", device_proc);
	PROC("vkGetPhysicalDeviceProperties", properties);
	PROC("vkGetPhysicalDeviceMemoryProperties", memory);
	PROC("vkGetPhysicalDeviceQueueFamilyProperties", queues);
	PROC("vkGetPhysicalDeviceImageFormatProperties2", images);
	PROC("vkGetPhysicalDeviceImageFormatProperties2KHR", images);
	PROC("vkGetPhysicalDeviceExternalFencePropertiesKHR", fences);
	PROC("vkGetPhysicalDeviceExternalSemaphorePropertiesKHR", semaphores);
#undef PROC
	return nullptr;
}
} // namespace
TEST_CASE("External FD sync requires the application's enabled extensions", "[vulkan][external-sync]")
{
	const unsigned fence_types[] = {VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
	                                VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT};
	const unsigned semaphore_types[] = {VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
	                                    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT};
	for (unsigned enabled = 0; enabled < 4; ++enabled) {
		for (unsigned caps = 0; caps < 16; ++caps) {
			for (unsigned timeline = 0; timeline < 2; ++timeline) {
				CAPTURE(enabled, caps, timeline);
				state = {};
				for (unsigned i = 0; i < 2; ++i) {
					if (caps & (1u << i))
						state.fences |= fence_types[i];
					if (caps & (4u << i))
						state.semaphores |= semaphore_types[i];
				}
				vk_bundle vk{};
				REQUIRE(vk_init_from_given(&vk, instance_proc, (VkInstance)(uintptr_t)1,
				                           (VkPhysicalDevice)(uintptr_t)2, (VkDevice)(uintptr_t)3, 0, 0,
				                           enabled & 1, enabled & 2, timeline, false, false, false,
				                           U_LOGGING_ERROR) == VK_SUCCESS);
				CHECK(vk.external.fence_sync_fd == bool((enabled & 1) && (caps & 1)));
				CHECK(vk.external.fence_opaque_fd == bool((enabled & 1) && (caps & 2)));
				CHECK(vk.external.binary_semaphore_sync_fd == bool((enabled & 2) && (caps & 4)));
				CHECK(vk.external.binary_semaphore_opaque_fd == bool((enabled & 2) && (caps & 8)));
				CHECK(vk.external.timeline_semaphore_sync_fd ==
				      bool((enabled & 2) && timeline && (caps & 4)));
				CHECK(vk.external.timeline_semaphore_opaque_fd ==
				      bool((enabled & 2) && timeline && (caps & 8)));
				CHECK(state.fence_calls == ((enabled & 1) ? 2 : 0));
				CHECK(state.semaphore_calls == ((enabled & 2) ? (timeline ? 4 : 2) : 0));
				CHECK(vk.vkGetFenceFdKHR == nullptr);
				CHECK(vk.vkGetSemaphoreFdKHR == nullptr);
			}
		}
	}
}
#endif
