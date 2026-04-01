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

int
main(void)
{
	int ret = 1;
	void *runtime = NULL;
	XrInstance instance = XR_NULL_HANDLE;
	XrSession session = XR_NULL_HANDLE;
	XrSpace local_space = XR_NULL_HANDLE;
	XrSwapchain swapchain = XR_NULL_HANDLE;
	VkInstance vk_instance = VK_NULL_HANDLE;
	VkDevice vk_device = VK_NULL_HANDLE;

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

	XrSwapchainCreateInfo swapchain_info = {
	    .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
	    .createFlags = 0,
	    .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
	    .format = formats[0],
	    .sampleCount = views[0].recommendedSwapchainSampleCount,
	    .width = views[0].recommendedImageRectWidth,
	    .height = views[0].recommendedImageRectHeight,
	    .faceCount = 1,
	    .arraySize = view_count,
	    .mipCount = 1,
	};
	free(formats);
	free(views);

	xr = xrCreateSwapchain(session, &swapchain_info, &swapchain);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrCreateSwapchain", xr);
		goto out;
	}

	uint32_t image_count = 0;
	xr = xrEnumerateSwapchainImages(swapchain, 0, &image_count, NULL);
	if (xr != XR_SUCCESS) {
		ret = fail_xr("xrEnumerateSwapchainImages(count)", xr);
		goto out;
	}

	XrSwapchainImageVulkanKHR *images = calloc(image_count, sizeof(*images));
	if (images == NULL) {
		ret = fail_msg("calloc(swapchain images) failed");
		goto out;
	}
	for (uint32_t i = 0; i < image_count; ++i) {
		images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	}

	xr = xrEnumerateSwapchainImages(swapchain, image_count, &image_count, (XrSwapchainImageBaseHeader *)images);
	if (xr != XR_SUCCESS) {
		free(images);
		ret = fail_xr("xrEnumerateSwapchainImages(list)", xr);
		goto out;
	}

	XrFrameWaitInfo frame_wait_info = {
	    .type = XR_TYPE_FRAME_WAIT_INFO,
	};
	XrFrameState frame_state = {
	    .type = XR_TYPE_FRAME_STATE,
	};
	xr = xrWaitFrame(session, &frame_wait_info, &frame_state);
	if (xr != XR_SUCCESS) {
		free(images);
		ret = fail_xr("xrWaitFrame", xr);
		goto out;
	}

	XrFrameBeginInfo frame_begin_info = {
	    .type = XR_TYPE_FRAME_BEGIN_INFO,
	};
	xr = xrBeginFrame(session, &frame_begin_info);
	if (xr != XR_SUCCESS) {
		free(images);
		ret = fail_xr("xrBeginFrame", xr);
		goto out;
	}

	uint32_t image_index = 0;
	XrSwapchainImageAcquireInfo acquire_info = {
	    .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO,
	};
	xr = xrAcquireSwapchainImage(swapchain, &acquire_info, &image_index);
	if (xr != XR_SUCCESS) {
		free(images);
		ret = fail_xr("xrAcquireSwapchainImage", xr);
		goto out;
	}

	XrSwapchainImageWaitInfo wait_info = {
	    .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
	    .timeout = XR_INFINITE_DURATION,
	};
	xr = xrWaitSwapchainImage(swapchain, &wait_info);
	if (xr != XR_SUCCESS) {
		free(images);
		ret = fail_xr("xrWaitSwapchainImage", xr);
		goto out;
	}

	XrView *located_views = calloc(view_count, sizeof(*located_views));
	if (located_views == NULL) {
		free(images);
		ret = fail_msg("calloc(located views) failed");
		goto out;
	}
	for (uint32_t i = 0; i < view_count; ++i) {
		located_views[i].type = XR_TYPE_VIEW;
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
		free(located_views);
		free(images);
		ret = fail_xr("xrLocateViews", xr);
		goto out;
	}

	if (located_view_count != view_count) {
		free(located_views);
		free(images);
		ret = fail_msg("xrLocateViews returned unexpected view count");
		goto out;
	}

	XrSwapchainImageReleaseInfo release_info = {
	    .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO,
	};
	xr = xrReleaseSwapchainImage(swapchain, &release_info);
	if (xr != XR_SUCCESS) {
		free(located_views);
		free(images);
		ret = fail_xr("xrReleaseSwapchainImage", xr);
		goto out;
	}

	XrCompositionLayerProjectionView *projection_views = calloc(view_count, sizeof(*projection_views));
	if (projection_views == NULL) {
		free(located_views);
		free(images);
		ret = fail_msg("calloc(projection views) failed");
		goto out;
	}

	for (uint32_t i = 0; i < view_count; ++i) {
		projection_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		projection_views[i].pose = located_views[i].pose;
		projection_views[i].fov = located_views[i].fov;
		projection_views[i].subImage.swapchain = swapchain;
		projection_views[i].subImage.imageRect.offset.x = 0;
		projection_views[i].subImage.imageRect.offset.y = 0;
		projection_views[i].subImage.imageRect.extent.width = (int32_t)swapchain_info.width;
		projection_views[i].subImage.imageRect.extent.height = (int32_t)swapchain_info.height;
		projection_views[i].subImage.imageArrayIndex = i;
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
		free(images);
		ret = fail_xr("xrEndFrame", xr);
		goto out;
	}

	fprintf(stdout,
	        "OpenXR Vulkan probe created session, swapchain, and submitted one projection frame with %u images.\n",
	        image_count);
	free(projection_views);
	free(located_views);
	free(images);
	ret = 0;

out:
	if (swapchain != XR_NULL_HANDLE && xrDestroySwapchain != NULL) {
		xrDestroySwapchain(swapchain);
	}
	if (local_space != XR_NULL_HANDLE && xrDestroySpace != NULL) {
		xrDestroySpace(local_space);
	}
	if (session != XR_NULL_HANDLE && xrDestroySession != NULL) {
		xrDestroySession(session);
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
