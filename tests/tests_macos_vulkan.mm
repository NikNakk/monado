// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Real GPU regression for in-process Vulkan Metal texture ownership.
 */
#import <Metal/Metal.h>
#include "catch_amalgamated.hpp"
#include "util/comp_vulkan.h"
#include "util/u_extension_list.h"
#include "util/u_handles.h"
#include "vk/vk_image_allocator.h"
#include "vk/vk_cmd_pool.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace {
// Probe capabilities independently of Monado initialization so failures in the
// code under test cannot be mistaken for an unavailable GPU or extension.
static bool
has_metal_objects_device()
{
	id<MTLDevice> metal_device = MTLCreateSystemDefaultDevice();
	if (metal_device == nil) {
		return false;
	}
	[metal_device release];

	auto enumerate_extensions = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
	    vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties"));
	auto create_instance =
	    reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
	REQUIRE(enumerate_extensions != nullptr);
	REQUIRE(create_instance != nullptr);
	uint32_t count = 0;
	REQUIRE(enumerate_extensions(nullptr, &count, nullptr) == VK_SUCCESS);
	std::vector<VkExtensionProperties> extensions(count);
	REQUIRE(enumerate_extensions(nullptr, &count, extensions.data()) == VK_SUCCESS);
	bool portability = std::any_of(extensions.begin(), extensions.end(), [](const auto &ext) {
		return std::strcmp(ext.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0;
	});
	const char *portability_extension = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
	VkApplicationInfo application{};
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo info{};
	info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	info.pApplicationInfo = &application;
	info.flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
	info.enabledExtensionCount = portability ? 1 : 0;
	info.ppEnabledExtensionNames = portability ? &portability_extension : nullptr;
	struct instance_owner
	{
		VkInstance instance = VK_NULL_HANDLE;
		~instance_owner()
		{
			if (instance != VK_NULL_HANDLE) {
				auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(
				    vkGetInstanceProcAddr(instance, "vkDestroyInstance"));
				destroy(instance, nullptr);
			}
		}
	} owner;
	VkResult result = create_instance(&info, nullptr, &owner.instance);
	if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
		return false; // No ICD supporting the required Vulkan version.
	}
	REQUIRE(result == VK_SUCCESS);
	auto enumerate_devices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
	    vkGetInstanceProcAddr(owner.instance, "vkEnumeratePhysicalDevices"));
	auto enumerate_device_extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
	    vkGetInstanceProcAddr(owner.instance, "vkEnumerateDeviceExtensionProperties"));
	REQUIRE(enumerate_devices != nullptr);
	REQUIRE(enumerate_device_extensions != nullptr);
	REQUIRE(enumerate_devices(owner.instance, &count, nullptr) == VK_SUCCESS);
	std::vector<VkPhysicalDevice> devices(count);
	REQUIRE(enumerate_devices(owner.instance, &count, devices.data()) == VK_SUCCESS);
	for (VkPhysicalDevice device : devices) {
		REQUIRE(enumerate_device_extensions(device, nullptr, &count, nullptr) == VK_SUCCESS);
		extensions.resize(count);
		REQUIRE(enumerate_device_extensions(device, nullptr, &count, extensions.data()) == VK_SUCCESS);
		if (std::any_of(extensions.begin(), extensions.end(), [](const auto &ext) {
			    return std::strcmp(ext.extensionName, VK_EXT_METAL_OBJECTS_EXTENSION_NAME) == 0;
		    })) {
			return true;
		}
	}
	return false;
}

struct context
{
	vk_bundle vk{};
	vk_cmd_pool pool{};
	bool pool_initialized = false;

	bool
	init()
	{
		const char *instance_extensions[] = {
		    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
		    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
		    VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
		    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
		};
		const char *device_extensions[] = {VK_EXT_METAL_OBJECTS_EXTENSION_NAME,
		                                   VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
		                                   VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME};
		auto instances =
		    u_extension_list_create_from_array(instance_extensions, ARRAY_SIZE(instance_extensions));
		auto devices = u_extension_list_create_from_array(device_extensions, ARRAY_SIZE(device_extensions));
		auto empty = u_extension_list_create();
		comp_vulkan_arguments args{};
		args.required_instance_version = VK_API_VERSION_1_1;
		args.get_instance_proc_address = vkGetInstanceProcAddr;
		args.required_instance_extensions = instances;
		args.optional_instance_extensions = empty;
		args.required_device_extensions = devices;
		args.optional_device_extensions = empty;
		args.log_level = U_LOGGING_WARN;
		args.selected_gpu_index = -1;
		args.client_gpu_index = -1;
		comp_vulkan_results results{};
		bool ok = comp_vulkan_init_bundle(&vk, &args, &results);
		u_extension_list_destroy(&instances);
		u_extension_list_destroy(&devices);
		u_extension_list_destroy(&empty);
		if (!ok)
			return false;
		pool_initialized = vk_cmd_pool_init(&vk, &pool, 0) == VK_SUCCESS;
		return pool_initialized;
	}
	~context()
	{
		if (vk.device) {
			vk.vkDeviceWaitIdle(vk.device);
			if (pool_initialized)
				vk_cmd_pool_destroy(&vk, &pool);
			vk.vkDestroyDevice(vk.device, nullptr);
		}
		vk_deinit_mutex(&vk);
		if (vk.instance)
			vk.vkDestroyInstance(vk.instance, nullptr);
	}
};
} // namespace

TEST_CASE("Vulkan Metal textures survive export, import and original-image destruction", "[macos][gpu]")
{
	@autoreleasepool {
		if (!has_metal_objects_device()) {
			SKIP("No Vulkan 1.1 device with VK_EXT_metal_objects; select MoltenVK with VK_DRIVER_FILES");
		}
		context producer, consumer;
		REQUIRE(producer.init());
		REQUIRE(consumer.init());
		uint32_t layers = GENERATE(1u, 2u);
		VkFormat format = GENERATE(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_D32_SFLOAT);
		xrt_swapchain_create_info info{};
		info.format = format;
		info.width = 16;
		info.height = 16;
		info.array_size = layers;
		info.face_count = info.mip_count = info.sample_count = 1;
		info.bits =
		    (xrt_swapchain_usage_bits)((format == VK_FORMAT_D32_SFLOAT ? XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL
		                                                               : XRT_SWAPCHAIN_USAGE_COLOR) |
		                               XRT_SWAPCHAIN_USAGE_SAMPLED | XRT_SWAPCHAIN_USAGE_TRANSFER_DST);
		vk_image_collection images{};
		REQUIRE(vk_ic_allocate(&producer.vk, &info, 1, &images) == VK_SUCCESS);
		xrt_graphics_buffer_handle_t handle = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
		REQUIRE(vk_ic_get_handles(&producer.vk, &images, 1, &handle) == VK_SUCCESS);
		REQUIRE(xrt_graphics_buffer_is_valid(handle));
		id<MTLTexture> texture = [(id<MTLTexture>)handle retain];
		xrt_image_native native{};
		native.handle = handle;
		VkImage imported = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		REQUIRE(vk_create_image_from_native(&consumer.vk, &info, &native, &imported, &memory) == VK_SUCCESS);
		CHECK_FALSE(xrt_graphics_buffer_is_valid(native.handle));
		CHECK(memory == VK_NULL_HANDLE);
		// The imported VkImage and the independent Metal reference own their backing.
		vk_ic_destroy(&producer.vk, &images);
		VkCommandBuffer cmd;
		REQUIRE(vk_cmd_pool_create_and_begin_cmd_buffer(&consumer.vk, &consumer.pool, 0, &cmd) == VK_SUCCESS);
		VkImageSubresourceRange range = {format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT
		                                                                : VK_IMAGE_ASPECT_COLOR_BIT,
		                                 0, 1, 0, layers};
		VkImageMemoryBarrier barrier = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .image = imported,
		    .subresourceRange = range,
		};
		consumer.vk.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
		VkClearColorValue color = {.float32 = {1, 0, 0, 1}};
		if (format == VK_FORMAT_D32_SFLOAT) {
			VkClearDepthStencilValue depth = {1.0f, 0};
			auto clear_depth = (PFN_vkCmdClearDepthStencilImage)consumer.vk.vkGetDeviceProcAddr(
			    consumer.vk.device, "vkCmdClearDepthStencilImage");
			REQUIRE(clear_depth != nullptr);
			clear_depth(cmd, imported, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth, 1, &range);
		} else {
			consumer.vk.vkCmdClearColorImage(cmd, imported, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
			                                 &range);
		}
		REQUIRE(vk_cmd_pool_end_submit_wait_and_free_cmd_buffer(&consumer.vk, &consumer.pool, cmd) ==
		        VK_SUCCESS);
		consumer.vk.vkDestroyImage(consumer.vk.device, imported, nullptr);

		id<MTLDevice> device = [texture device];
		id<MTLCommandQueue> queue = [device newCommandQueue];
		REQUIRE(queue != nil);
		// One aligned row per source row, one slice per stereo view.
		const NSUInteger row = 256, slice = row * info.height;
		id<MTLBuffer> buffer = [device newBufferWithLength:slice * layers options:MTLResourceStorageModeShared];
		REQUIRE(buffer != nil);
		id<MTLCommandBuffer> commands = [queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
		for (uint32_t layer = 0; layer < layers; layer++) {
			[blit copyFromTexture:texture
			                 sourceSlice:layer
			                 sourceLevel:0
			                sourceOrigin:MTLOriginMake(0, 0, 0)
			                  sourceSize:MTLSizeMake(info.width, info.height, 1)
			                    toBuffer:buffer
			           destinationOffset:slice * layer
			      destinationBytesPerRow:row
			    destinationBytesPerImage:slice];
		}
		[blit endEncoding];
		[commands commit];
		[commands waitUntilCompleted];
		REQUIRE([commands status] == MTLCommandBufferStatusCompleted);
		auto bytes = (const uint8_t *)[buffer contents];
		for (uint32_t layer = 0; layer < layers; layer++) {
			for (uint32_t y = 0; y < info.height; y++) {
				for (uint32_t x = 0; x < info.width; x++) {
					const uint8_t *pixel = bytes + slice * layer + row * y + 4 * x;
					if (format == VK_FORMAT_D32_SFLOAT) {
						float value = 0;
						memcpy(&value, pixel, sizeof(value));
						REQUIRE(value == 1.0f);
						continue;
					}
					REQUIRE(pixel[0] == (format == VK_FORMAT_R8G8B8A8_UNORM ? 255 : 0));
					REQUIRE(pixel[1] == 0);
					REQUIRE(pixel[2] == (format == VK_FORMAT_R8G8B8A8_UNORM ? 0 : 255));
					REQUIRE(pixel[3] == 255);
				}
			}
		}
		[buffer release];
		[queue release];
		[texture release];
	}
}
