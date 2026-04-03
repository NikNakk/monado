// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Probe a graphics-bound OpenXR Vulkan session on macOS.
 */

#define XR_USE_GRAPHICS_API_VULKAN 1

#include <vulkan/vulkan.h>

#include "openxr/openxr.h"
#include "openxr/openxr_loader_negotiation.h"
#include "openxr/openxr_platform.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
fail_xr(const char *what, XrResult result)
{
	fprintf(stderr, "%s failed: %d\n", what, result);
	return 1;
}

static int
fail_vk(const char *what, VkResult result)
{
	fprintf(stderr, "%s failed: %d\n", what, result);
	return 1;
}

static int
fail_msg(const char *what)
{
	fprintf(stderr, "%s\n", what);
	return 1;
}

static uint32_t
get_frame_count(void)
{
	const char *value = getenv("MACOS_OPENXR_VULKAN_PROBE_FRAMES");
	if (value == NULL || value[0] == '\0') {
		return 1;
	}

	errno = 0;
	char *end = NULL;
	long parsed = strtol(value, &end, 10);
	if (errno != 0 || end == value || *end != '\0' || parsed < 1 || parsed > 3600) {
		fprintf(stderr,
		        "Invalid MACOS_OPENXR_VULKAN_PROBE_FRAMES=%s, expected integer in [1,3600]\n",
		        value);
		return 0;
	}

	return (uint32_t)parsed;
}

static bool
use_per_view_swapchains(void)
{
	const char *value = getenv("MACOS_OPENXR_VULKAN_PROBE_PER_VIEW_SWAPCHAINS");
	if (value == NULL || value[0] == '\0') {
		return false;
	}

	return strcmp(value, "0") != 0;
}

struct probe_swapchain
{
	XrSwapchain handle;
	XrSwapchainCreateInfo create_info;
	XrSwapchainImageVulkanKHR *images;
	VkImageLayout *layouts;
	uint32_t image_count;
};

struct probe_vk_context
{
	VkQueue queue;
	VkCommandPool command_pool;
	VkCommandBuffer command_buffer;
	VkFence fence;
};

static int
get_proc(PFN_xrGetInstanceProcAddr get_instance_proc_addr,
         XrInstance instance,
         const char *name,
         PFN_xrVoidFunction *out_fn)
{
	XrResult result = get_instance_proc_addr(instance, name, out_fn);
	if (result != XR_SUCCESS) {
		return fail_xr(name, result);
	}

	if (*out_fn == NULL) {
		fprintf(stderr, "%s returned NULL function pointer\n", name);
		return 1;
	}

	return 0;
}

static int
wait_for_session_state(PFN_xrPollEvent xrPollEvent, XrInstance instance, XrSessionState target_state)
{
	for (uint32_t i = 0; i < 64; ++i) {
		XrEventDataBuffer event = {
		    .type = XR_TYPE_EVENT_DATA_BUFFER,
		};
		XrResult xr = xrPollEvent(instance, &event);
		if (xr == XR_EVENT_UNAVAILABLE) {
			continue;
		}
		if (xr != XR_SUCCESS) {
			return fail_xr("xrPollEvent", xr);
		}

		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const XrEventDataSessionStateChanged *changed = (const XrEventDataSessionStateChanged *)&event;
			if (changed->state == target_state) {
				return 0;
			}
		}
	}

	return fail_msg("Timed out waiting for OpenXR session state");
}

static bool
have_instance_extension(const char *target)
{
	uint32_t count = 0;
	if (vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS) {
		return false;
	}

	VkExtensionProperties *props = calloc(count, sizeof(*props));
	if (props == NULL) {
		return false;
	}

	bool found = false;
	if (vkEnumerateInstanceExtensionProperties(NULL, &count, props) == VK_SUCCESS) {
		for (uint32_t i = 0; i < count; ++i) {
			if (strcmp(props[i].extensionName, target) == 0) {
				found = true;
				break;
			}
		}
	}

	free(props);
	return found;
}

static bool
have_device_extension(VkPhysicalDevice physical_device, const char *target)
{
	uint32_t count = 0;
	if (vkEnumerateDeviceExtensionProperties(physical_device, NULL, &count, NULL) != VK_SUCCESS) {
		return false;
	}

	VkExtensionProperties *props = calloc(count, sizeof(*props));
	if (props == NULL) {
		return false;
	}

	bool found = false;
	if (vkEnumerateDeviceExtensionProperties(physical_device, NULL, &count, props) == VK_SUCCESS) {
		for (uint32_t i = 0; i < count; ++i) {
			if (strcmp(props[i].extensionName, target) == 0) {
				found = true;
				break;
			}
		}
	}

	free(props);
	return found;
}

static uint32_t
find_graphics_queue_family(VkPhysicalDevice physical_device)
{
	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, NULL);
	VkQueueFamilyProperties *props = calloc(count, sizeof(*props));
	if (props == NULL) {
		return UINT32_MAX;
	}

	vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &count, props);
	uint32_t found = UINT32_MAX;
	for (uint32_t i = 0; i < count; ++i) {
		if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && props[i].queueCount > 0) {
			found = i;
			break;
		}
	}

	free(props);
	return found;
}

static int
init_vk_context(VkDevice device, uint32_t queue_family_index, struct probe_vk_context *ctx)
{
	memset(ctx, 0, sizeof(*ctx));

	vkGetDeviceQueue(device, queue_family_index, 0, &ctx->queue);
	if (ctx->queue == VK_NULL_HANDLE) {
		return fail_msg("vkGetDeviceQueue returned NULL queue");
	}

	VkCommandPoolCreateInfo pool_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	    .queueFamilyIndex = queue_family_index,
	};
	VkResult vk = vkCreateCommandPool(device, &pool_info, NULL, &ctx->command_pool);
	if (vk != VK_SUCCESS) {
		return fail_vk("vkCreateCommandPool", vk);
	}

	VkCommandBufferAllocateInfo cmd_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = ctx->command_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	vk = vkAllocateCommandBuffers(device, &cmd_info, &ctx->command_buffer);
	if (vk != VK_SUCCESS) {
		return fail_vk("vkAllocateCommandBuffers", vk);
	}

	VkFenceCreateInfo fence_info = {
	    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	    .flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};
	vk = vkCreateFence(device, &fence_info, NULL, &ctx->fence);
	if (vk != VK_SUCCESS) {
		return fail_vk("vkCreateFence", vk);
	}

	return 0;
}

static void
destroy_vk_context(VkDevice device, struct probe_vk_context *ctx)
{
	if (ctx->fence != VK_NULL_HANDLE) {
		vkDestroyFence(device, ctx->fence, NULL);
	}
	if (ctx->command_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(device, ctx->command_pool, NULL);
	}
	memset(ctx, 0, sizeof(*ctx));
}

static int
clear_swapchain_image(VkDevice device,
                      struct probe_vk_context *ctx,
                      struct probe_swapchain *swapchain,
                      uint32_t image_index,
                      uint32_t array_layer,
                      const VkClearColorValue *color)
{
	const uint32_t array_size = swapchain->create_info.arraySize;
	VkImageLayout *layout = &swapchain->layouts[image_index * array_size + array_layer];

	VkResult vk = vkWaitForFences(device, 1, &ctx->fence, VK_TRUE, UINT64_MAX);
	if (vk != VK_SUCCESS) {
		return fail_vk("vkWaitForFences", vk);
	}

	vk = vkResetFences(device, 1, &ctx->fence);
	if (vk != VK_SUCCESS) {
		return fail_vk("vkResetFences", vk);
	}

	vk = vkResetCommandPool(device, ctx->command_pool, 0);
	if (vk != VK_SUCCESS) {
		return fail_vk("vkResetCommandPool", vk);
	}

	VkCommandBufferBeginInfo begin_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	vk = vkBeginCommandBuffer(ctx->command_buffer, &begin_info);
	if (vk != VK_SUCCESS) {
		return fail_vk("vkBeginCommandBuffer", vk);
	}

	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = 1,
	    .baseArrayLayer = array_layer,
	    .layerCount = 1,
	};

	VkImageMemoryBarrier to_clear = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .oldLayout = *layout,
	    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = swapchain->images[image_index].image,
	    .subresourceRange = range,
	};
	vkCmdPipelineBarrier(ctx->command_buffer,
	                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     0,
	                     0,
	                     NULL,
	                     0,
	                     NULL,
	                     1,
	                     &to_clear);

	vkCmdClearColorImage(ctx->command_buffer,
	                     swapchain->images[image_index].image,
	                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                     color,
	                     1,
	                     &range);

	VkImageMemoryBarrier to_sample = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = swapchain->images[image_index].image,
	    .subresourceRange = range,
	};
	vkCmdPipelineBarrier(ctx->command_buffer,
	                     VK_PIPELINE_STAGE_TRANSFER_BIT,
	                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
	                     0,
	                     0,
	                     NULL,
	                     0,
	                     NULL,
	                     1,
	                     &to_sample);

	vk = vkEndCommandBuffer(ctx->command_buffer);
	if (vk != VK_SUCCESS) {
		return fail_vk("vkEndCommandBuffer", vk);
	}

	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &ctx->command_buffer,
	};
	vk = vkQueueSubmit(ctx->queue, 1, &submit_info, ctx->fence);
	if (vk != VK_SUCCESS) {
		return fail_vk("vkQueueSubmit", vk);
	}

	vk = vkWaitForFences(device, 1, &ctx->fence, VK_TRUE, UINT64_MAX);
	if (vk != VK_SUCCESS) {
		return fail_vk("vkWaitForFences(submit)", vk);
	}

	*layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	return 0;
}

int
main(void)
{
	int ret = 1;
	void *runtime = NULL;
	XrInstance instance = XR_NULL_HANDLE;
	XrSession session = XR_NULL_HANDLE;
	XrSpace local_space = XR_NULL_HANDLE;
	VkInstance vk_instance = VK_NULL_HANDLE;
	VkDevice vk_device = VK_NULL_HANDLE;
	struct probe_vk_context vk_ctx = {0};
	uint32_t frame_count = get_frame_count();
	bool per_view_swapchains = use_per_view_swapchains();
	struct probe_swapchain *swapchains = NULL;
	uint32_t swapchain_count = 0;

	if (frame_count == 0) {
		return 1;
	}

	const char *runtime_path = getenv("MONADO_OPENXR_RUNTIME_PATH");
	if (runtime_path == NULL || runtime_path[0] == '\0') {
		return fail_msg("Set MONADO_OPENXR_RUNTIME_PATH to libopenxr_monado.dylib");
	}

	runtime = dlopen(runtime_path, RTLD_NOW | RTLD_LOCAL);
	if (runtime == NULL) {
		fprintf(stderr, "dlopen(%s) failed: %s\n", runtime_path, dlerror());
		return 1;
	}

	PFN_xrNegotiateLoaderRuntimeInterface negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(
	    runtime, "xrNegotiateLoaderRuntimeInterface");
	if (negotiate == NULL) {
		fprintf(stderr, "dlsym(xrNegotiateLoaderRuntimeInterface) failed: %s\n", dlerror());
		goto out;
	}

	XrNegotiateLoaderInfo loader_info = {
	    .structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
	    .structVersion = XR_LOADER_INFO_STRUCT_VERSION,
	    .structSize = sizeof(loader_info),
	    .minInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION,
	    .maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION,
	    .minApiVersion = XR_CURRENT_API_VERSION,
	    .maxApiVersion = XR_CURRENT_API_VERSION,
	};
	XrNegotiateRuntimeRequest runtime_request = {
	    .structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
	    .structVersion = XR_RUNTIME_INFO_STRUCT_VERSION,
	    .structSize = sizeof(runtime_request),
	};

	XrResult xr = negotiate(&loader_info, &runtime_request);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrNegotiateLoaderRuntimeInterface", xr);
		goto out;
	}

	PFN_xrGetInstanceProcAddr get_instance_proc_addr = runtime_request.getInstanceProcAddr;
	if (get_instance_proc_addr == NULL) {
		ret = fail_msg("Runtime negotiation returned NULL xrGetInstanceProcAddr");
		goto out;
	}

	PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties = NULL;
	PFN_xrCreateInstance xrCreateInstance = NULL;
	if (get_proc(get_instance_proc_addr, XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
	             (PFN_xrVoidFunction *)&xrEnumerateInstanceExtensionProperties) != 0 ||
	    get_proc(get_instance_proc_addr, XR_NULL_HANDLE, "xrCreateInstance",
	             (PFN_xrVoidFunction *)&xrCreateInstance) != 0) {
		goto out;
	}

	uint32_t xr_ext_count = 0;
	xr = xrEnumerateInstanceExtensionProperties(NULL, 0, &xr_ext_count, NULL);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrEnumerateInstanceExtensionProperties(count)", xr);
		goto out;
	}

	XrExtensionProperties *xr_exts = calloc(xr_ext_count, sizeof(*xr_exts));
	if (xr_exts == NULL) {
		ret = fail_msg("calloc(xr extension properties) failed");
		goto out;
	}
	for (uint32_t i = 0; i < xr_ext_count; ++i) {
		xr_exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	}

	xr = xrEnumerateInstanceExtensionProperties(NULL, xr_ext_count, &xr_ext_count, xr_exts);
	if (xr != XR_SUCCESS) {
		free(xr_exts);
		ret = fail_xr("xrEnumerateInstanceExtensionProperties(list)", xr);
		goto out;
	}

	bool have_vulkan_enable2 = false;
	for (uint32_t i = 0; i < xr_ext_count; ++i) {
		if (strcmp(xr_exts[i].extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0) {
			have_vulkan_enable2 = true;
		}
	}
	free(xr_exts);

	if (!have_vulkan_enable2) {
		ret = fail_msg("Runtime does not advertise XR_KHR_vulkan_enable2");
		goto out;
	}

	const char *enabled_xr_extensions[] = {
	    XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
	};
	XrInstanceCreateInfo instance_info = {
	    .type = XR_TYPE_INSTANCE_CREATE_INFO,
	    .enabledExtensionCount = 1,
	    .enabledExtensionNames = enabled_xr_extensions,
	};
	snprintf(instance_info.applicationInfo.applicationName,
	         sizeof(instance_info.applicationInfo.applicationName), "%s",
	         "tests_macos_openxr_vulkan_probe");
	snprintf(instance_info.applicationInfo.engineName, sizeof(instance_info.applicationInfo.engineName), "%s",
	         "monado");
	instance_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

	xr = xrCreateInstance(&instance_info, &instance);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrCreateInstance", xr);
		goto out;
	}

	PFN_xrDestroyInstance xrDestroyInstance = NULL;
	PFN_xrPollEvent xrPollEvent = NULL;
	PFN_xrGetSystem xrGetSystem = NULL;
	PFN_xrDestroySession xrDestroySession = NULL;
	PFN_xrCreateSession xrCreateSession = NULL;
	PFN_xrBeginSession xrBeginSession = NULL;
	PFN_xrWaitFrame xrWaitFrame = NULL;
	PFN_xrBeginFrame xrBeginFrame = NULL;
	PFN_xrEndFrame xrEndFrame = NULL;
	PFN_xrLocateViews xrLocateViews = NULL;
	PFN_xrCreateReferenceSpace xrCreateReferenceSpace = NULL;
	PFN_xrDestroySpace xrDestroySpace = NULL;
	PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViews = NULL;
	PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormats = NULL;
	PFN_xrCreateSwapchain xrCreateSwapchain = NULL;
	PFN_xrDestroySwapchain xrDestroySwapchain = NULL;
	PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages = NULL;
	PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage = NULL;
	PFN_xrWaitSwapchainImage xrWaitSwapchainImage = NULL;
	PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage = NULL;
	PFN_xrGetVulkanGraphicsRequirements2KHR xrGetVulkanGraphicsRequirements2KHR = NULL;
	PFN_xrCreateVulkanInstanceKHR xrCreateVulkanInstanceKHR = NULL;
	PFN_xrGetVulkanGraphicsDevice2KHR xrGetVulkanGraphicsDevice2KHR = NULL;
	PFN_xrCreateVulkanDeviceKHR xrCreateVulkanDeviceKHR = NULL;

	if (get_proc(get_instance_proc_addr, instance, "xrDestroyInstance", (PFN_xrVoidFunction *)&xrDestroyInstance) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrPollEvent", (PFN_xrVoidFunction *)&xrPollEvent) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrGetSystem", (PFN_xrVoidFunction *)&xrGetSystem) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrCreateSession", (PFN_xrVoidFunction *)&xrCreateSession) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrDestroySession", (PFN_xrVoidFunction *)&xrDestroySession) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrBeginSession", (PFN_xrVoidFunction *)&xrBeginSession) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrWaitFrame", (PFN_xrVoidFunction *)&xrWaitFrame) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrBeginFrame", (PFN_xrVoidFunction *)&xrBeginFrame) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrEndFrame", (PFN_xrVoidFunction *)&xrEndFrame) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrLocateViews", (PFN_xrVoidFunction *)&xrLocateViews) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrCreateReferenceSpace",
	             (PFN_xrVoidFunction *)&xrCreateReferenceSpace) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrDestroySpace", (PFN_xrVoidFunction *)&xrDestroySpace) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrEnumerateViewConfigurationViews",
	             (PFN_xrVoidFunction *)&xrEnumerateViewConfigurationViews) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrEnumerateSwapchainFormats",
	             (PFN_xrVoidFunction *)&xrEnumerateSwapchainFormats) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrCreateSwapchain",
	             (PFN_xrVoidFunction *)&xrCreateSwapchain) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrDestroySwapchain",
	             (PFN_xrVoidFunction *)&xrDestroySwapchain) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrEnumerateSwapchainImages",
	             (PFN_xrVoidFunction *)&xrEnumerateSwapchainImages) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrAcquireSwapchainImage",
	             (PFN_xrVoidFunction *)&xrAcquireSwapchainImage) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrWaitSwapchainImage",
	             (PFN_xrVoidFunction *)&xrWaitSwapchainImage) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrReleaseSwapchainImage",
	             (PFN_xrVoidFunction *)&xrReleaseSwapchainImage) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrGetVulkanGraphicsRequirements2KHR",
	             (PFN_xrVoidFunction *)&xrGetVulkanGraphicsRequirements2KHR) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrCreateVulkanInstanceKHR",
	             (PFN_xrVoidFunction *)&xrCreateVulkanInstanceKHR) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrGetVulkanGraphicsDevice2KHR",
	             (PFN_xrVoidFunction *)&xrGetVulkanGraphicsDevice2KHR) != 0 ||
	    get_proc(get_instance_proc_addr, instance, "xrCreateVulkanDeviceKHR",
	             (PFN_xrVoidFunction *)&xrCreateVulkanDeviceKHR) != 0) {
		goto out;
	}

	XrSystemId system_id = XR_NULL_SYSTEM_ID;
	XrSystemGetInfo system_info = {
	    .type = XR_TYPE_SYSTEM_GET_INFO,
	    .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
	};
	xr = xrGetSystem(instance, &system_info, &system_id);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrGetSystem", xr);
		goto out;
	}

	XrGraphicsRequirementsVulkan2KHR graphics_requirements = {
	    .type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR,
	};
	xr = xrGetVulkanGraphicsRequirements2KHR(instance, system_id, &graphics_requirements);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrGetVulkanGraphicsRequirements2KHR", xr);
		goto out;
	}

	const char *app_instance_extensions[16] = {0};
	uint32_t app_instance_extension_count = 0;
	if (have_instance_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
		app_instance_extensions[app_instance_extension_count++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
	}

	VkApplicationInfo vk_app_info = {
	    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	    .pApplicationName = "tests_macos_openxr_vulkan_probe",
	    .applicationVersion = 1,
	    .pEngineName = "monado",
	    .engineVersion = 1,
	    .apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo vk_instance_info = {
	    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	    .pApplicationInfo = &vk_app_info,
	    .enabledExtensionCount = app_instance_extension_count,
	    .ppEnabledExtensionNames = app_instance_extensions,
	};
	if (have_instance_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
		vk_instance_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}

	XrVulkanInstanceCreateInfoKHR xr_vk_instance_info = {
	    .type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR,
	    .systemId = system_id,
	    .createFlags = 0,
	    .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
	    .vulkanCreateInfo = &vk_instance_info,
	    .vulkanAllocator = NULL,
	};
	VkResult vk = VK_SUCCESS;
	xr = xrCreateVulkanInstanceKHR(instance, &xr_vk_instance_info, &vk_instance, &vk);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrCreateVulkanInstanceKHR", xr);
		goto out;
	}
	if (vk != VK_SUCCESS) {
		ret = fail_vk("xrCreateVulkanInstanceKHR(vulkan)", vk);
		goto out;
	}

	VkPhysicalDevice physical_device = VK_NULL_HANDLE;
	XrVulkanGraphicsDeviceGetInfoKHR get_info = {
	    .type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR,
	    .systemId = system_id,
	    .vulkanInstance = vk_instance,
	};
	xr = xrGetVulkanGraphicsDevice2KHR(instance, &get_info, &physical_device);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrGetVulkanGraphicsDevice2KHR", xr);
		goto out;
	}

	uint32_t queue_family_index = find_graphics_queue_family(physical_device);
	if (queue_family_index == UINT32_MAX) {
		ret = fail_msg("Failed to find graphics-capable Vulkan queue family");
		goto out;
	}

	float priority = 1.0f;
	VkDeviceQueueCreateInfo queue_info = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	    .queueFamilyIndex = queue_family_index,
	    .queueCount = 1,
	    .pQueuePriorities = &priority,
	};

	const char *app_device_extensions[16] = {0};
	uint32_t app_device_extension_count = 0;
	if (have_device_extension(physical_device, "VK_KHR_portability_subset")) {
		app_device_extensions[app_device_extension_count++] = "VK_KHR_portability_subset";
	}

	VkDeviceCreateInfo vk_device_info = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	    .queueCreateInfoCount = 1,
	    .pQueueCreateInfos = &queue_info,
	    .enabledExtensionCount = app_device_extension_count,
	    .ppEnabledExtensionNames = app_device_extensions,
	};

	XrVulkanDeviceCreateInfoKHR xr_vk_device_info = {
	    .type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR,
	    .systemId = system_id,
	    .createFlags = 0,
	    .pfnGetInstanceProcAddr = vkGetInstanceProcAddr,
	    .vulkanPhysicalDevice = physical_device,
	    .vulkanCreateInfo = &vk_device_info,
	    .vulkanAllocator = NULL,
	};
	xr = xrCreateVulkanDeviceKHR(instance, &xr_vk_device_info, &vk_device, &vk);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrCreateVulkanDeviceKHR", xr);
		goto out;
	}
	if (vk != VK_SUCCESS) {
		ret = fail_vk("xrCreateVulkanDeviceKHR(vulkan)", vk);
		goto out;
	}

	XrGraphicsBindingVulkanKHR binding = {
	    .type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR,
	    .instance = vk_instance,
	    .physicalDevice = physical_device,
	    .device = vk_device,
	    .queueFamilyIndex = queue_family_index,
	    .queueIndex = 0,
	};
	XrSessionCreateInfo session_info = {
	    .type = XR_TYPE_SESSION_CREATE_INFO,
	    .next = &binding,
	    .systemId = system_id,
	};
	xr = xrCreateSession(instance, &session_info, &session);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrCreateSession", xr);
		goto out;
	}

	if (init_vk_context(vk_device, queue_family_index, &vk_ctx) != 0) {
		ret = 1;
		goto out;
	}

	if (wait_for_session_state(xrPollEvent, instance, XR_SESSION_STATE_READY) != 0) {
		ret = 1;
		goto out;
	}

	XrSessionBeginInfo begin_info = {
	    .type = XR_TYPE_SESSION_BEGIN_INFO,
	    .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	};
	xr = xrBeginSession(session, &begin_info);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrBeginSession", xr);
		goto out;
	}

	XrReferenceSpaceCreateInfo space_info = {
	    .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
	    .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
	    .poseInReferenceSpace = {
	        .orientation = {.w = 1.0f},
	    },
	};
	xr = xrCreateReferenceSpace(session, &space_info, &local_space);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrCreateReferenceSpace", xr);
		goto out;
	}

	uint32_t view_count = 0;
	xr = xrEnumerateViewConfigurationViews(instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
	                                       &view_count, NULL);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrEnumerateViewConfigurationViews(count)", xr);
		goto out;
	}

	XrViewConfigurationView *views = calloc(view_count, sizeof(*views));
	if (views == NULL) {
		ret = fail_msg("calloc(views) failed");
		goto out;
	}
	for (uint32_t i = 0; i < view_count; ++i) {
		views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	}

	xr = xrEnumerateViewConfigurationViews(instance, system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count,
	                                       &view_count, views);
	if (xr != XR_SUCCESS) {
		free(views);
		ret = fail_xr("xrEnumerateViewConfigurationViews(list)", xr);
		goto out;
	}

	uint32_t format_count = 0;
	xr = xrEnumerateSwapchainFormats(session, 0, &format_count, NULL);
	if (xr != XR_SUCCESS) {
		free(views);
		ret = fail_xr("xrEnumerateSwapchainFormats(count)", xr);
		goto out;
	}

	int64_t *formats = calloc(format_count, sizeof(*formats));
	if (formats == NULL) {
		free(views);
		ret = fail_msg("calloc(formats) failed");
		goto out;
	}

	xr = xrEnumerateSwapchainFormats(session, format_count, &format_count, formats);
	if (xr != XR_SUCCESS) {
		free(formats);
		free(views);
		ret = fail_xr("xrEnumerateSwapchainFormats(list)", xr);
		goto out;
	}

	swapchain_count = per_view_swapchains ? view_count : 1;
	swapchains = calloc(swapchain_count, sizeof(*swapchains));
	if (swapchains == NULL) {
		free(formats);
		free(views);
		ret = fail_msg("calloc(probe swapchains) failed");
		goto out;
	}

	for (uint32_t i = 0; i < swapchain_count; ++i) {
		swapchains[i].handle = XR_NULL_HANDLE;
		swapchains[i].create_info = (XrSwapchainCreateInfo){
		    .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
		    .createFlags = 0,
		    .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
		                  XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
		    .format = formats[0],
		    .sampleCount = views[i].recommendedSwapchainSampleCount,
		    .width = views[i].recommendedImageRectWidth,
		    .height = views[i].recommendedImageRectHeight,
		    .faceCount = 1,
		    .arraySize = per_view_swapchains ? 1 : view_count,
		    .mipCount = 1,
		};
	}
	free(formats);
	free(views);

	for (uint32_t i = 0; i < swapchain_count; ++i) {
		xr = xrCreateSwapchain(session, &swapchains[i].create_info, &swapchains[i].handle);
		if (xr != XR_SUCCESS) {
			ret = fail_xr("xrCreateSwapchain", xr);
			goto out;
		}

		xr = xrEnumerateSwapchainImages(swapchains[i].handle, 0, &swapchains[i].image_count, NULL);
		if (xr != XR_SUCCESS) {
			ret = fail_xr("xrEnumerateSwapchainImages(count)", xr);
			goto out;
		}

		swapchains[i].images = calloc(swapchains[i].image_count, sizeof(*swapchains[i].images));
		if (swapchains[i].images == NULL) {
			ret = fail_msg("calloc(swapchain images) failed");
			goto out;
		}
		swapchains[i].layouts =
		    calloc(swapchains[i].image_count * swapchains[i].create_info.arraySize, sizeof(*swapchains[i].layouts));
		if (swapchains[i].layouts == NULL) {
			ret = fail_msg("calloc(swapchain layouts) failed");
			goto out;
		}
		for (uint32_t j = 0; j < swapchains[i].image_count; ++j) {
			swapchains[i].images[j].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
			for (uint32_t layer = 0; layer < swapchains[i].create_info.arraySize; ++layer) {
				swapchains[i].layouts[j * swapchains[i].create_info.arraySize + layer] = VK_IMAGE_LAYOUT_UNDEFINED;
			}
		}

		xr = xrEnumerateSwapchainImages(swapchains[i].handle, swapchains[i].image_count, &swapchains[i].image_count,
		                                (XrSwapchainImageBaseHeader *)swapchains[i].images);
		if (xr != XR_SUCCESS) {
			ret = fail_xr("xrEnumerateSwapchainImages(list)", xr);
			goto out;
		}
	}

	XrView *located_views = calloc(view_count, sizeof(*located_views));
	if (located_views == NULL) {
		ret = fail_msg("calloc(located views) failed");
		goto out;
	}
	for (uint32_t i = 0; i < view_count; ++i) {
		located_views[i].type = XR_TYPE_VIEW;
	}

	XrCompositionLayerProjectionView *projection_views = calloc(view_count, sizeof(*projection_views));
	if (projection_views == NULL) {
		free(located_views);
		ret = fail_msg("calloc(projection views) failed");
		goto out;
	}

	for (uint32_t frame = 0; frame < frame_count; ++frame) {
		XrFrameWaitInfo frame_wait_info = {
		    .type = XR_TYPE_FRAME_WAIT_INFO,
		};
		XrFrameState frame_state = {
		    .type = XR_TYPE_FRAME_STATE,
		};
		xr = xrWaitFrame(session, &frame_wait_info, &frame_state);
		if (xr != XR_SUCCESS) {
			free(projection_views);
			free(located_views);
			ret = fail_xr("xrWaitFrame", xr);
			goto out;
		}

		XrFrameBeginInfo frame_begin_info = {
		    .type = XR_TYPE_FRAME_BEGIN_INFO,
		};
		xr = xrBeginFrame(session, &frame_begin_info);
		if (xr != XR_SUCCESS) {
			free(projection_views);
			free(located_views);
			ret = fail_xr("xrBeginFrame", xr);
			goto out;
		}

		for (uint32_t i = 0; i < swapchain_count; ++i) {
			uint32_t image_index = 0;
			XrSwapchainImageAcquireInfo acquire_info = {
			    .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
			};
			xr = xrAcquireSwapchainImage(swapchains[i].handle, &acquire_info, &image_index);
			if (xr != XR_SUCCESS) {
				free(projection_views);
				free(located_views);
				ret = fail_xr("xrAcquireSwapchainImage", xr);
				goto out;
			}

			XrSwapchainImageWaitInfo wait_info = {
			    .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
			    .timeout = XR_INFINITE_DURATION,
			};
			xr = xrWaitSwapchainImage(swapchains[i].handle, &wait_info);
			if (xr != XR_SUCCESS) {
				free(projection_views);
				free(located_views);
				ret = fail_xr("xrWaitSwapchainImage", xr);
				goto out;
			}

			VkClearColorValue clear_color = {
			    .float32 =
			        {
			            i == 0 ? 0.85f : 0.10f,
			            i == 0 ? 0.15f : 0.35f,
			            0.20f + 0.10f * (float)(frame % 2),
			            1.0f,
			        },
			};
			uint32_t layer = per_view_swapchains ? 0 : i;
			if (clear_swapchain_image(vk_device, &vk_ctx, &swapchains[i], image_index, layer, &clear_color) != 0) {
				free(projection_views);
				free(located_views);
				ret = 1;
				goto out;
			}
		}

		for (uint32_t i = 0; i < view_count; ++i) {
			located_views[i].type = XR_TYPE_VIEW;
			located_views[i].next = NULL;
		}

		XrViewLocateInfo locate_info = {
		    .type = XR_TYPE_VIEW_LOCATE_INFO,
		    .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
		    .displayTime = frame_state.predictedDisplayTime,
		    .space = local_space,
		};
		XrViewState view_state = {
		    .type = XR_TYPE_VIEW_STATE,
		};
		uint32_t located_view_count = 0;
		xr = xrLocateViews(session, &locate_info, &view_state, view_count, &located_view_count, located_views);
		if (xr != XR_SUCCESS) {
			free(projection_views);
			free(located_views);
			ret = fail_xr("xrLocateViews", xr);
			goto out;
		}

		if (located_view_count != view_count) {
			free(projection_views);
			free(located_views);
			ret = fail_msg("xrLocateViews returned unexpected view count");
			goto out;
		}

		for (uint32_t i = 0; i < swapchain_count; ++i) {
			XrSwapchainImageReleaseInfo release_info = {
			    .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
			};
			xr = xrReleaseSwapchainImage(swapchains[i].handle, &release_info);
			if (xr != XR_SUCCESS) {
				free(projection_views);
				free(located_views);
				ret = fail_xr("xrReleaseSwapchainImage", xr);
				goto out;
			}
		}

		for (uint32_t i = 0; i < view_count; ++i) {
			projection_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projection_views[i].pose = located_views[i].pose;
			projection_views[i].fov = located_views[i].fov;
			projection_views[i].subImage.swapchain =
			    per_view_swapchains ? swapchains[i].handle : swapchains[0].handle;
			projection_views[i].subImage.imageRect.offset.x = 0;
			projection_views[i].subImage.imageRect.offset.y = 0;
			projection_views[i].subImage.imageRect.extent.width =
			    (int32_t)(per_view_swapchains ? swapchains[i].create_info.width : swapchains[0].create_info.width);
			projection_views[i].subImage.imageRect.extent.height =
			    (int32_t)(per_view_swapchains ? swapchains[i].create_info.height : swapchains[0].create_info.height);
			projection_views[i].subImage.imageArrayIndex = per_view_swapchains ? 0 : i;
		}

		XrCompositionLayerProjection projection_layer = {
		    .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION,
		    .space = local_space,
		    .viewCount = view_count,
		    .views = projection_views,
		};
		const XrCompositionLayerBaseHeader *layers[] = {
		    (const XrCompositionLayerBaseHeader *)&projection_layer,
		};
		XrFrameEndInfo frame_end_info = {
		    .type = XR_TYPE_FRAME_END_INFO,
		    .displayTime = frame_state.predictedDisplayTime,
		    .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		    .layerCount = 1,
		    .layers = layers,
		};
		xr = xrEndFrame(session, &frame_end_info);
		if (xr != XR_SUCCESS) {
			free(projection_views);
			free(located_views);
			ret = fail_xr("xrEndFrame", xr);
			goto out;
		}
	}

	fprintf(stdout,
	        "OpenXR Vulkan probe created session, %u swapchain(s), and submitted %u projection frames.\n",
	        swapchain_count, frame_count);
	free(projection_views);
	free(located_views);
	ret = 0;

out:
	if (swapchains != NULL) {
		for (uint32_t i = 0; i < swapchain_count; ++i) {
			free(swapchains[i].layouts);
			free(swapchains[i].images);
			if (swapchains[i].handle != XR_NULL_HANDLE && xrDestroySwapchain != NULL) {
				xrDestroySwapchain(swapchains[i].handle);
			}
		}
		free(swapchains);
	}
	if (local_space != XR_NULL_HANDLE && xrDestroySpace != NULL) {
		xrDestroySpace(local_space);
	}
	if (session != XR_NULL_HANDLE && xrDestroySession != NULL) {
		xrDestroySession(session);
	}
	if (vk_device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(vk_device);
		destroy_vk_context(vk_device, &vk_ctx);
	}
	if (vk_device != VK_NULL_HANDLE) {
		vkDestroyDevice(vk_device, NULL);
	}
	if (vk_instance != VK_NULL_HANDLE) {
		vkDestroyInstance(vk_instance, NULL);
	}
	if (instance != XR_NULL_HANDLE && xrDestroyInstance != NULL) {
		xrDestroyInstance(instance);
	}
	if (runtime != NULL) {
		dlclose(runtime);
	}
	return ret;
}
