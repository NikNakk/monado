/* Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 */
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>
#include <thread>
#define CHECK(x)                                                                                                       \
	do {                                                                                                           \
		VkResult r = (x);                                                                                      \
		if (r != VK_SUCCESS) {                                                                                 \
			fprintf(stderr, "%s: %d\n", #x, r);                                                            \
			exit(1);                                                                                       \
		}                                                                                                      \
	} while (0)
static bool
has(const std::vector<VkExtensionProperties> &e, const char *name)
{
	for (const auto &p : e)
		if (!strcmp(p.extensionName, name))
			return true;
	return false;
}
int
main(int argc, char **argv)
{
	@autoreleasepool {
		setvbuf(stdout, nullptr, _IOLBF, 0);
		bool negative = argc == 2 && !strcmp(argv[1], "--negative-control");
		VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		app.pApplicationName = "KosmicKrisp isolated interop probe";
		app.apiVersion = VK_API_VERSION_1_3;
		VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ici.pApplicationInfo = &app;
		VkInstance instance;
		CHECK(vkCreateInstance(&ici, nullptr, &instance));
		uint32_t n = 0;
		CHECK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
		std::vector<VkPhysicalDevice> devices(n);
		CHECK(vkEnumeratePhysicalDevices(instance, &n, devices.data()));
		VkPhysicalDevice physical = VK_NULL_HANDLE;
		for (auto d : devices) {
			VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
			VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
			props.pNext = &driver;
			vkGetPhysicalDeviceProperties2(d, &props);
			printf("device=%s driver=%s info=%s api=%u.%u.%u\n", props.properties.deviceName,
			       driver.driverName, driver.driverInfo, VK_API_VERSION_MAJOR(props.properties.apiVersion),
			       VK_API_VERSION_MINOR(props.properties.apiVersion),
			       VK_API_VERSION_PATCH(props.properties.apiVersion));
			if (strstr(driver.driverName, "KosmicKrisp") || strstr(driver.driverName, "kosmickrisp"))
				physical = d;
		}
		if (!physical) {
			fprintf(stderr, "REFUSED: no KosmicKrisp device (set VK_DRIVER_FILES)\n");
			return 2;
		}
		VkPhysicalDeviceFeatures features;
		vkGetPhysicalDeviceFeatures(physical, &features);
		printf(
		    "features geometryShader=%u fillModeNonSolid=%u sparseBinding=%u sparseResidencyImage2D=%u "
		    "shaderInt64=%u pipelineStatisticsQuery=%u\n",
		    features.geometryShader, features.fillModeNonSolid, features.sparseBinding,
		    features.sparseResidencyImage2D, features.shaderInt64, features.pipelineStatisticsQuery);
		CHECK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr));
		std::vector<VkExtensionProperties> extensions(n);
		CHECK(vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, extensions.data()));
		for (auto &e : extensions)
			printf("extension=%s version=%u\n", e.extensionName, e.specVersion);
		for (auto type :
		     {VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLTEXTURE_BIT_EXT,
		      VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLBUFFER_BIT_EXT}) {
			VkPhysicalDeviceExternalImageFormatInfo ext{
			    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
			ext.handleType = type;
			VkPhysicalDeviceImageFormatInfo2 fi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
			fi.pNext = &ext;
			fi.format = VK_FORMAT_R8G8B8A8_UNORM;
			fi.type = VK_IMAGE_TYPE_2D;
			fi.tiling = VK_IMAGE_TILING_OPTIMAL;
			fi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
			VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
			VkImageFormatProperties2 ip{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
			ip.pNext = &ep;
			VkResult r = vkGetPhysicalDeviceImageFormatProperties2(physical, &fi, &ip);
			printf("external_image handle=0x%x result=%d features=0x%x compatible=0x%x\n", type, r,
			       ep.externalMemoryProperties.externalMemoryFeatures,
			       ep.externalMemoryProperties.compatibleHandleTypes);
		}
		for (auto type :
		     {VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT}) {
			for (auto st : {VK_SEMAPHORE_TYPE_BINARY, VK_SEMAPHORE_TYPE_TIMELINE}) {
				VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
				ti.semaphoreType = st;
				VkPhysicalDeviceExternalSemaphoreInfo si{
				    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
				si.pNext = &ti;
				si.handleType = type;
				VkExternalSemaphoreProperties sp{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
				vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &si, &sp);
				printf("external_semaphore handle=0x%x type=%d features=0x%x compatible=0x%x\n", type,
				       st, sp.externalSemaphoreFeatures, sp.compatibleHandleTypes);
			}
		}
		if (!has(extensions, VK_EXT_EXTERNAL_MEMORY_METAL_EXTENSION_NAME))
			return 2;
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
		std::vector<VkQueueFamilyProperties> families(n);
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, families.data());
		uint32_t family = 0;
		while (family < n && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			++family;
		if (family == n)
			return 2;
		float priority = 1;
		VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qci.queueFamilyIndex = family;
		qci.queueCount = 1;
		qci.pQueuePriorities = &priority;
		VkPhysicalDeviceTimelineSemaphoreFeatures tf{
		    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
		tf.timelineSemaphore = VK_TRUE;
		const char *enabled[] = {VK_EXT_EXTERNAL_MEMORY_METAL_EXTENSION_NAME,
		                         VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
		VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dci.pNext = &tf;
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		dci.enabledExtensionCount = 2;
		dci.ppEnabledExtensionNames = enabled;
		VkDevice device;
		CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
		VkQueue queue;
		vkGetDeviceQueue(device, family, 0, &queue);
		VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT;
		VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		im.pNext = &emi;
		im.imageType = VK_IMAGE_TYPE_2D;
		im.format = VK_FORMAT_R8G8B8A8_UNORM;
		im.extent = {64, 64, 1};
		im.mipLevels = 1;
		im.arrayLayers = 1;
		im.samples = VK_SAMPLE_COUNT_1_BIT;
		im.tiling = VK_IMAGE_TILING_OPTIMAL;
		im.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		VkImage image;
		CHECK(vkCreateImage(device, &im, nullptr, &image));
		VkMemoryRequirements mr;
		vkGetImageMemoryRequirements(device, image, &mr);
		VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
		exportInfo.handleTypes = emi.handleTypes;
		VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ma.pNext = &exportInfo;
		ma.allocationSize = mr.size;
		while (!(mr.memoryTypeBits & (1u << ma.memoryTypeIndex)))
			++ma.memoryTypeIndex;
		VkDeviceMemory memory;
		CHECK(vkAllocateMemory(device, &ma, nullptr, &memory));
		CHECK(vkBindImageMemory(device, image, memory, 0));
		auto getHandle =
		    (PFN_vkGetMemoryMetalHandleEXT)vkGetDeviceProcAddr(device, "vkGetMemoryMetalHandleEXT");
		if (!getHandle)
			return 2;
		VkMemoryGetMetalHandleInfoEXT hi{VK_STRUCTURE_TYPE_MEMORY_GET_METAL_HANDLE_INFO_EXT};
		hi.memory = memory;
		hi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT;
		void *handle = nullptr;
		CHECK(getHandle(device, &hi, &handle));
		id<MTLHeap> heap = (id<MTLHeap>)handle;
		MTLTextureDescriptor *td =
		    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		                                                       width:64
		                                                      height:64
		                                                   mipmapped:NO];
		td.resourceOptions = heap.resourceOptions;
		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		td.allowGPUOptimizedContents = YES;
		id<MTLTexture> alias = [heap newTextureWithDescriptor:td offset:0];
		if (!alias || alias.heap != heap || alias.heapOffset != 0) {
			fprintf(stderr, "heap alias failed\n");
			return 1;
		}
		printf("heap_alias same_heap=1 offset=%lu iosurface=%p shared_texture=%d\n",
		       (unsigned long)alias.heapOffset, alias.iosurface, alias.shareable);
		VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = im.format;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView view;
		CHECK(vkCreateImageView(device, &vi, nullptr, &view));
		VkAttachmentDescription ad{};
		ad.format = im.format;
		ad.samples = im.samples;
		ad.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		ad.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		ad.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		ad.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
		VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription sub{};
		sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		sub.colorAttachmentCount = 1;
		sub.pColorAttachments = &ar;
		VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		rpci.attachmentCount = 1;
		rpci.pAttachments = &ad;
		rpci.subpassCount = 1;
		rpci.pSubpasses = &sub;
		VkRenderPass rp;
		CHECK(vkCreateRenderPass(device, &rpci, nullptr, &rp));
		VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fbci.renderPass = rp;
		fbci.attachmentCount = 1;
		fbci.pAttachments = &view;
		fbci.width = 64;
		fbci.height = 64;
		fbci.layers = 1;
		VkFramebuffer fb;
		CHECK(vkCreateFramebuffer(device, &fbci, nullptr, &fb));
		VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.queueFamilyIndex = family;
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VkCommandPool pool;
		CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
		VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ca.commandPool = pool;
		ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ca.commandBufferCount = 1;
		VkCommandBuffer cmd;
		CHECK(vkAllocateCommandBuffers(device, &ca, &cmd));
		VkSemaphoreTypeCreateInfo sti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
		sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		sci.pNext = &sti;
		VkSemaphore sem;
		CHECK(vkCreateSemaphore(device, &sci, nullptr, &sem));
		id<MTLCommandQueue> metalQueue = [heap.device newCommandQueue];
		id<MTLBuffer> readback = [heap.device newBufferWithLength:64 * 64 * 4
		                                                  options:MTLResourceStorageModeShared];
		auto start = std::chrono::steady_clock::now();
		for (uint64_t value = 1; value <= 32; ++value) {
			CHECK(vkResetCommandBuffer(cmd, 0));
			VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			CHECK(vkBeginCommandBuffer(cmd, &bi));
			VkClearValue clear{};
			clear.color.float32[0] = (value & 1) ? 1 : 0;
			clear.color.float32[1] = (value & 1) ? 0 : 1;
			clear.color.float32[3] = 1;
			VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
			rb.renderPass = rp;
			rb.framebuffer = fb;
			rb.renderArea.extent = {64, 64};
			rb.clearValueCount = 1;
			rb.pClearValues = &clear;
			vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdEndRenderPass(cmd);
			CHECK(vkEndCommandBuffer(cmd));
			VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
			ts.signalSemaphoreValueCount = 1;
			ts.pSignalSemaphoreValues = &value;
			VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
			submit.pNext = &ts;
			submit.commandBufferCount = 1;
			submit.pCommandBuffers = &cmd;
			submit.signalSemaphoreCount = 1;
			submit.pSignalSemaphores = &sem;
			CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
			VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
			wi.semaphoreCount = 1;
			wi.pSemaphores = &sem;
			wi.pValues = &value;
			CHECK(vkWaitSemaphores(device, &wi, 5000000000ull));
			uint64_t observed;
			CHECK(vkGetSemaphoreCounterValue(device, sem, &observed));
			if (observed != value)
				return 1;
			id<MTLCommandBuffer> cb = [metalQueue commandBuffer];
			id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
			[blit copyFromTexture:alias
			                 sourceSlice:0
			                 sourceLevel:0
			                sourceOrigin:MTLOriginMake(0, 0, 0)
			                  sourceSize:MTLSizeMake(64, 64, 1)
			                    toBuffer:readback
			           destinationOffset:0
			      destinationBytesPerRow:256
			    destinationBytesPerImage:16384];
			[blit endEncoding];
			[cb commit];
			[cb waitUntilCompleted];
			if (cb.status != MTLCommandBufferStatusCompleted) {
				fprintf(stderr, "Metal readback error: %s\n", cb.error.description.UTF8String);
				return 1;
			}
			auto p = (const unsigned char *)readback.contents;
			bool expectRed = bool(value & 1) != negative;
			for (unsigned i = 0; i < 4096; ++i)
				if (p[4 * i] != (expectRed ? 255 : 0) || p[4 * i + 1] != (expectRed ? 0 : 255) ||
				    p[4 * i + 2] || p[4 * i + 3] != 255) {
					fprintf(stderr, "stale/bad pixel value=%llu pixel=%u rgba=%u,%u,%u,%u\n", value,
					        i, p[4 * i], p[4 * i + 1], p[4 * i + 2], p[4 * i + 3]);
					return 1;
				}
		}
		printf(
		    "PASS same-process heap alias: 32 alternating render clears, 131072 pixels, exact Vulkan timeline "
		    "values; elapsed_ms=%.3f\n",
		    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
		VkExportSemaphoreCreateInfo esi{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
		esi.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		VkSemaphoreCreateInfo bsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		bsci.pNext = &esi;
		VkSemaphore binary;
		CHECK(vkCreateSemaphore(device, &bsci, nullptr, &binary));
		uint64_t gateValue = 33, binaryValue = 0;
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkTimelineSemaphoreSubmitInfo bts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
		bts.waitSemaphoreValueCount = 1;
		bts.pWaitSemaphoreValues = &gateValue;
		bts.signalSemaphoreValueCount = 1;
		bts.pSignalSemaphoreValues = &binaryValue;
		VkSubmitInfo bsub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		bsub.pNext = &bts;
		bsub.waitSemaphoreCount = 1;
		bsub.pWaitSemaphores = &sem;
		bsub.pWaitDstStageMask = &stage;
		bsub.signalSemaphoreCount = 1;
		bsub.pSignalSemaphores = &binary;
		CHECK(vkQueueSubmit(queue, 1, &bsub, VK_NULL_HANDLE));
		std::thread signaler([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
			signal.semaphore = sem;
			signal.value = gateValue;
			CHECK(vkSignalSemaphore(device, &signal));
		});
		VkSemaphoreGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
		fdInfo.semaphore = binary;
		fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		auto getFd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR");
		if (!getFd)
			return 1;
		int fd = -999;
		auto exportStart = std::chrono::steady_clock::now();
		CHECK(getFd(device, &fdInfo, &fd));
		double exportMs =
		    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - exportStart).count();
		signaler.join();
		printf("sync_fd_export fd=%d cpu_call_ms=%.3f gate_delay_ms=50\n", fd, exportMs);
		if (fd != -1)
			return 1;
		CHECK(vkQueueWaitIdle(queue));
		vkDestroySemaphore(device, binary, nullptr);
		printf(
		    "BLOCKED cross-process zero-copy and Metal GPU timeline handoff: metal_objects=%d; only heap "
		    "export tested; CPU waits used\n",
		    has(extensions, "VK_EXT_metal_objects"));
		[readback release];
		[metalQueue release];
		[alias release];
		vkDestroySemaphore(device, sem, nullptr);
		vkDestroyCommandPool(device, pool, nullptr);
		vkDestroyFramebuffer(device, fb, nullptr);
		vkDestroyRenderPass(device, rp, nullptr);
		vkDestroyImageView(device, view, nullptr);
		vkDestroyImage(device, image, nullptr);
		vkFreeMemory(device, memory, nullptr);
		vkDestroyDevice(device, nullptr);
		vkDestroyInstance(instance, nullptr);
		return 0;
	}
}
