// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#import <Metal/Metal.h>
#include "client/comp_vk_metal.h"
#include "client/comp_metal_client.h"
#include "client/comp_metal_service_semaphore.h"
#include "util/comp_metal_semaphore_provider.h"
#include "util/u_debug.h"
#include "kosmickrisp_metal_interop.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
DEBUG_GET_ONCE_BOOL_OPTION(kosmickrisp, "XRT_MACOS_KOSMICKRISP", false)
struct client_vk_metal_context
{
	void *library;
	const struct kk_metal_interop_api *api;
	VkDevice vk_device;
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
};
bool
client_vk_metal_enabled(void)
{
	return debug_get_bool_option_kosmickrisp();
}
const char *
client_vk_metal_device_extensions(void)
{
	return "VK_KHR_timeline_semaphore";
}
bool
client_vk_metal_matches(PFN_vkGetInstanceProcAddr get_proc, VkInstance instance, VkPhysicalDevice physical)
{
	if (!get_proc || !instance || !physical)
		return false;
	PFN_vkGetPhysicalDeviceProperties2 properties =
	    (PFN_vkGetPhysicalDeviceProperties2)get_proc(instance, "vkGetPhysicalDeviceProperties2");
	if (!properties)
		properties =
		    (PFN_vkGetPhysicalDeviceProperties2)get_proc(instance, "vkGetPhysicalDeviceProperties2KHR");
	if (!properties)
		return false;
	VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
	VkPhysicalDeviceProperties2 info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
	properties(physical, &info);
	return strcmp(driver.driverName, "KosmicKrisp") == 0;
}
int64_t
client_vk_metal_format(int64_t format)
{
	switch (format) {
	case VK_FORMAT_R8G8B8A8_UNORM: return MTLPixelFormatRGBA8Unorm;
	case VK_FORMAT_B8G8R8A8_UNORM: return MTLPixelFormatBGRA8Unorm;
	default: return 0;
	}
}
void
client_vk_metal_destroy(struct client_vk_metal_context *c)
{
	if (!c)
		return;
	[c->queue release];
	[c->device release];
	if (c->library)
		dlclose(c->library);
	free(c);
}
struct client_vk_metal_context *
client_vk_metal_create(struct xrt_compositor_native *xcn,
                       struct vk_bundle *vk,
                       struct xrt_compositor_metal **out_metal,
                       VkSemaphore *out_semaphore,
                       struct xrt_compositor_semaphore **out_native_semaphore)
{
	const char *path = getenv("XRT_MACOS_KOSMICKRISP_LIBRARY");
	if (!path || !vk->features.timeline_semaphore)
		return NULL;
	struct client_vk_metal_context *c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->vk_device = vk->device;
	c->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	PFN_kk_get_metal_interop_api get_api =
	    c->library ? (PFN_kk_get_metal_interop_api)dlsym(c->library, "kk_get_metal_interop_api") : NULL;
	if (!get_api || get_api(KK_METAL_INTEROP_ABI_VERSION, &c->api) != VK_SUCCESS || !c->api ||
	    c->api->struct_size < sizeof(*c->api) || c->api->abi_version != KK_METAL_INTEROP_ABI_VERSION ||
	    !c->api->copy_device || !c->api->device_registry_id || !c->api->import_image || !c->api->import_timeline)
		goto fail;
	Dl_info device_origin = {0}, api_origin = {0};
	if (!dladdr((void *)vk->vkCreateImage, &device_origin) || !dladdr((void *)get_api, &api_origin) ||
	    device_origin.dli_fbase != api_origin.dli_fbase) {
		U_LOG_E("KosmicKrisp ABI library must own this VkDevice dispatch; Vulkan layers are unsupported");
		goto fail;
	}

	void *raw_device = NULL;
	if (c->api->copy_device(vk->device, &raw_device) != VK_SUCCESS)
		goto fail;
	c->device = (id<MTLDevice>)raw_device;
	if (c->device.registryID != c->api->device_registry_id(vk->device))
		goto fail;
	c->queue = [c->device newCommandQueue];
	if (!c->queue)
		goto fail;
	struct xrt_compositor_metal *metal =
	    xcn->is_remote ? client_metal_service_compositor_create(xcn, (void *)c->device, (void *)c->queue)
	                   : client_metal_direct_compositor_create(xcn, (void *)c->device, (void *)c->queue);
	if (!metal)
		goto fail;
	void *event = NULL;
	struct xrt_compositor_semaphore *native = NULL;
	xrt_result_t result =
	    xcn->is_remote
	        ? client_metal_service_semaphore_create_pair(&metal->base, (void *)c->device, &native, &event)
	        : comp_metal_semaphore_create_client_pair(&native, &event);
	VkSemaphore semaphore = VK_NULL_HANDLE;
	if (result != XRT_SUCCESS || !native || !event ||
	    c->api->import_timeline(vk->device, event, [(id<MTLSharedEvent>)event signaledValue], &semaphore) !=
	        VK_SUCCESS) {
		xrt_compositor_semaphore_reference(&native, NULL);
		struct xrt_compositor *base = &metal->base;
		xrt_comp_destroy(&base);
		goto fail;
	}
	*out_metal = metal;
	*out_semaphore = semaphore;
	*out_native_semaphore = native;
	U_LOG_I(
	    "Experimental KosmicKrisp Metal adapter: registry=%llu ABI=%u; native swapchains and readiness semaphore",
	    (unsigned long long)c->device.registryID, c->api->abi_version);
	return c;
fail:
	U_LOG_E("KosmicKrisp Metal interop unavailable: library=%s", path);
	client_vk_metal_destroy(c);
	return NULL;
}
VkResult
client_vk_metal_import_image(struct client_vk_metal_context *c,
                             const struct xrt_swapchain_create_info *info,
                             void *texture,
                             VkImage *out)
{
	if (!c || !client_vk_metal_format(info->format) || info->array_size != 1 || info->face_count != 1 ||
	    info->mip_count != 1 || info->sample_count != 1 || info->create ||
	    (info->bits & (XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL | XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS |
	                   XRT_SWAPCHAIN_USAGE_MUTABLE_FORMAT)))
		return VK_ERROR_FORMAT_NOT_SUPPORTED;
	VkImageCreateInfo im = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	                        .imageType = VK_IMAGE_TYPE_2D,
	                        .format = (VkFormat)info->format,
	                        .extent = {info->width, info->height, 1},
	                        .mipLevels = 1,
	                        .arrayLayers = 1,
	                        .samples = VK_SAMPLE_COUNT_1_BIT,
	                        .tiling = VK_IMAGE_TILING_OPTIMAL,
	                        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	                        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	                        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
	if (info->bits & XRT_SWAPCHAIN_USAGE_TRANSFER_SRC)
		im.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (info->bits & XRT_SWAPCHAIN_USAGE_TRANSFER_DST)
		im.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	return c->api->import_image(c->vk_device, &im, texture, out);
}
