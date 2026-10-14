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
#include <dlfcn.h>
#include "kosmickrisp_metal_interop.h"
#include <fstream>
#import "array-texture-peer.h"
#include <unistd.h>
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
		if (argc != 7) {
			fprintf(stderr,
			        "usage: shared-texture-probe patched-ICD.dylib peer-service rgba|bgra vertex.spv "
			        "fragment.spv layers\n");
			return 2;
		}
		void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
		if (!library) {
			fprintf(stderr, "%s\n", dlerror());
			return 2;
		}
		auto getApi = (PFN_kk_get_metal_interop_api)dlsym(library, "kk_get_metal_interop_api");
		const kk_metal_interop_api *api = nullptr;
		if (!getApi)
			return 2;
		CHECK(getApi(KK_METAL_INTEROP_ABI_VERSION, &api));
		const kk_metal_interop_api *old = nullptr;
		if (getApi(1, &old) != VK_ERROR_FEATURE_NOT_PRESENT || old)
			return 1;
		bool bgra = !strncmp(argv[3], "bgra", 4);
		bool srgb = strstr(argv[3], "srgb");
		unsigned layers = (unsigned)atoi(argv[6]);
		if (layers < 1 || layers > 2)
			return 2;
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
		VkImageCreateInfo im{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		im.pNext = nullptr;
		im.imageType = VK_IMAGE_TYPE_2D;
		im.format = bgra ? (srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM)
		                 : (srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
		im.extent = {64, 64, 1};
		im.mipLevels = 1;
		im.arrayLayers = layers;
		im.samples = VK_SAMPLE_COUNT_1_BIT;
		im.tiling = VK_IMAGE_TILING_OPTIMAL;
		im.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		void *rawDevice = nullptr;
		CHECK(api->copy_device(device, &rawDevice));
		id<MTLDevice> metalDevice = (id<MTLDevice>)rawDevice;
		MTLTextureDescriptor *td = [MTLTextureDescriptor
		    texture2DDescriptorWithPixelFormat:bgra ? (srgb ? MTLPixelFormatBGRA8Unorm_sRGB
		                                                    : MTLPixelFormatBGRA8Unorm)
		                                            : (srgb ? MTLPixelFormatRGBA8Unorm_sRGB
		                                                    : MTLPixelFormatRGBA8Unorm)
		                                 width:64
		                                height:64
		                             mipmapped:NO];
		td.textureType = layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
		td.arrayLength = layers;
		td.storageMode = MTLStorageModePrivate;
		td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
		id<MTLTexture> alias = [metalDevice newSharedTextureWithDescriptor:td];
		if (!alias)
			return 1;
		VkImage image;
		CHECK(api->import_image(device, &im, (void *)alias, &image));
		printf(
		    "storage format=%s layers=%u carrier=MTLSharedTextureHandle native_resource=%llu "
		    "no_image_memory_allocation=1\n",
		    argv[3], layers, (unsigned long long)alias.gpuResourceID._impl);
		VkImage invalidImage = VK_NULL_HANDLE;
		for (unsigned test = 0; test < 5; ++test) {
			VkImageCreateInfo invalid = im;
			if (test == 0)
				invalid.extent.width = 65;
			if (test == 1)
				invalid.format = bgra ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
			if (test == 2)
				invalid.arrayLayers = layers == 1 ? 2 : 1;
			if (test == 3)
				invalid.mipLevels = 2;
			if (test == 4)
				invalid.arrayLayers = 3;
			if (api->import_image(device, &invalid, (void *)alias, &invalidImage) !=
			        VK_ERROR_FORMAT_NOT_SUPPORTED ||
			    invalidImage)
				return 1;
		}
		printf("descriptor_rejections width=1 format=1 layers=1 mips=1 unsupported_array=1\n");
		VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = im.format;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView views[2]{};
		for (unsigned layer = 0; layer < layers; ++layer) {
			vi.subresourceRange.baseArrayLayer = layer;
			CHECK(vkCreateImageView(device, &vi, nullptr, &views[layer]));
		}
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
		auto shaderModule = [&](const char *path) {
			std::ifstream input(path, std::ios::binary | std::ios::ate);
			auto size = input.tellg();
			if (size <= 0 || size % 4) {
				fprintf(stderr, "bad SPIR-V file\n");
				exit(1);
			}
			std::vector<uint32_t> words((size_t)size / 4);
			input.seekg(0);
			input.read((char *)words.data(), size);
			if (!input)
				exit(1);
			VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
			ci.codeSize = (size_t)size;
			ci.pCode = words.data();
			VkShaderModule module;
			CHECK(vkCreateShaderModule(device, &ci, nullptr, &module));
			return module;
		};
		VkShaderModule vertex = shaderModule(argv[4]), fragment = shaderModule(argv[5]);
		VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
		VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
		lci.pushConstantRangeCount = 1;
		lci.pPushConstantRanges = &push;
		VkPipelineLayout pipelineLayout;
		CHECK(vkCreatePipelineLayout(device, &lci, nullptr, &pipelineLayout));
		VkPipelineShaderStageCreateInfo stages[2]{};
		for (unsigned i = 0; i < 2; ++i) {
			stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
			stages[i].module = i ? fragment : vertex;
			stages[i].pName = "main";
		}
		VkPipelineVertexInputStateCreateInfo vertexInput{
		    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
		VkPipelineInputAssemblyStateCreateInfo assembly{
		    VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
		assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		VkViewport viewport{0, 0, 64, 64, 0, 1};
		VkRect2D scissor{{0, 0}, {64, 64}};
		VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
		vp.viewportCount = 1;
		vp.pViewports = &viewport;
		vp.scissorCount = 1;
		vp.pScissors = &scissor;
		VkPipelineRasterizationStateCreateInfo raster{
		    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE;
		raster.lineWidth = 1;
		VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		VkPipelineColorBlendAttachmentState blend{};
		blend.colorWriteMask = 0xf;
		VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
		bs.attachmentCount = 1;
		bs.pAttachments = &blend;
		VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
		gp.stageCount = 2;
		gp.pStages = stages;
		gp.pVertexInputState = &vertexInput;
		gp.pInputAssemblyState = &assembly;
		gp.pViewportState = &vp;
		gp.pRasterizationState = &raster;
		gp.pMultisampleState = &ms;
		gp.pColorBlendState = &bs;
		gp.layout = pipelineLayout;
		gp.renderPass = rp;
		VkPipeline pipeline;
		CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline));
		VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fbci.renderPass = rp;
		fbci.attachmentCount = 1;
		fbci.pAttachments = &views[0];
		fbci.width = 64;
		fbci.height = 64;
		fbci.layers = 1;
		VkFramebuffer fbs[2]{};
		for (unsigned layer = 0; layer < layers; ++layer) {
			fbci.pAttachments = &views[layer];
			CHECK(vkCreateFramebuffer(device, &fbci, nullptr, &fbs[layer]));
		}
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
		id<MTLSharedEvent> produced = [metalDevice newSharedEvent];
		id<MTLSharedEvent> done = [metalDevice newSharedEvent];
		id<MTLSharedEvent> gate = [metalDevice newSharedEvent];
		VkSemaphore sem, doneSem, gateSem, ackSem;
		CHECK(api->import_timeline(device, (void *)produced, 0, &sem));
		CHECK(api->import_timeline(device, (void *)done, 0, &doneSem));
		CHECK(api->import_timeline(device, (void *)gate, 0, &gateSem));
		VkSemaphoreTypeCreateInfo sti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
		sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		sci.pNext = &sti;
		CHECK(vkCreateSemaphore(device, &sci, nullptr, &ackSem));
		NSXPCConnection *peerConnection = nil;
		id<KKArrayPeerProtocol> peer = nil;
		id<MTLSharedEvent> peerDone = nil;
		VkSemaphore peerSem = VK_NULL_HANDLE;
		if (argc == 7) {
			peerDone = [metalDevice newSharedEvent];
			CHECK(api->import_timeline(device, (void *)peerDone, 0, &peerSem));
			peerConnection =
			    [[NSXPCConnection alloc] initWithMachServiceName:[NSString stringWithUTF8String:argv[2]]
			                                             options:0];
			peerConnection.remoteObjectInterface =
			    [NSXPCInterface interfaceWithProtocol:@protocol(KKArrayPeerProtocol)];
			[peerConnection resume];
			peer = [peerConnection remoteObjectProxyWithErrorHandler:^(NSError *error) {
			  fprintf(stderr, "peer XPC error: %s\n", error.description.UTF8String);
			  exit(1);
			}];
			MTLSharedEventHandle *pHandle = [produced newSharedEventHandle];
			MTLSharedEventHandle *dHandle = [peerDone newSharedEventHandle];
			dispatch_semaphore_t configured = dispatch_semaphore_create(0);
			__block BOOL configuredOK = NO;
			__block int peerPID = 0;
			__block uint32_t peerSurfaceID = 0;
			MTLSharedTextureHandle *textureHandle = [alias newSharedTextureHandle];
			[peer configureProducer:pHandle
			                   done:dHandle
			                texture:textureHandle
			                 format:alias.pixelFormat
			               registry:metalDevice.registryID
			                  reply:^(BOOL ok, int pid, uint32_t sid) {
				            configuredOK = ok;
				            peerPID = pid;
				            peerSurfaceID = sid;
				            dispatch_semaphore_signal(configured);
			                  }];
			if (dispatch_semaphore_wait(configured, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) ||
			    !configuredOK || peerPID == getpid() || peerSurfaceID != layers)
				return 1;
			printf(
			    "cross_process local_pid=%d peer_pid=%d same_registry=1 peer_layers=%u "
			    "shared_handle_import=1\n",
			    getpid(), peerPID, peerSurfaceID);
			dispatch_release(configured);
			[textureHandle release];
			[pHandle release];
			[dHandle release];
		}
		id<MTLCommandQueue> metalQueue = [metalDevice newCommandQueue];
		id<MTLBuffer> readback = [metalDevice newBufferWithLength:64 * 64 * 4 * layers
		                                                  options:MTLResourceStorageModeShared];
		auto start = std::chrono::steady_clock::now();
		for (uint64_t value = 1; value <= 32; ++value) {
			CHECK(vkResetCommandBuffer(cmd, 0));
			VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
			CHECK(vkBeginCommandBuffer(cmd, &bi));
			for (unsigned layer = 0; layer < layers; ++layer) {
				VkClearValue clear{};
				clear.color.float32[0] = (value & 1) ? 1 : 0;
				clear.color.float32[1] = (value & 1) ? 0 : 1;
				clear.color.float32[3] = 1;
				VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
				rb.renderPass = rp;
				rb.framebuffer = fbs[layer];
				rb.renderArea.extent = {64, 64};
				rb.clearValueCount = 1;
				rb.pClearValues = &clear;
				vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
				uint32_t frame = (uint32_t)value + layer * 73;
				vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &frame);
				vkCmdDraw(cmd, 3, 1, 0, 0);
				vkCmdEndRenderPass(cmd);
			}
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
			VkPipelineStageFlags all = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
			ts.waitSemaphoreValueCount = 1;
			ts.pWaitSemaphoreValues = &value;
			submit.waitSemaphoreCount = 1;
			submit.pWaitSemaphores = &gateSem;
			submit.pWaitDstStageMask = &all;
			auto submittedAt = std::chrono::steady_clock::now();
			CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
			double submitUs =
			    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - submittedAt)
			        .count();
			dispatch_semaphore_t peerReply = dispatch_semaphore_create(0);
			__block BOOL peerOK = NO;
			__block uint64_t peerObserved = 0;
			__block uint32_t consumedSurfaceID = 0;
			if (peer)
				[peer consumeValue:value
				             reply:^(BOOL ok, uint64_t seen, uint32_t sid) {
					       peerOK = ok;
					       peerObserved = seen;
					       consumedSurfaceID = sid;
					       dispatch_semaphore_signal(peerReply);
				             }];
			id<MTLCommandBuffer> cb = [metalQueue commandBuffer];
			[cb encodeWaitForEvent:produced value:value];
			if (peer)
				[cb encodeWaitForEvent:peerDone value:value];
			id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
			for (unsigned layer = 0; layer < layers; ++layer)
				[blit copyFromTexture:alias
				                 sourceSlice:layer
				                 sourceLevel:0
				                sourceOrigin:MTLOriginMake(0, 0, 0)
				                  sourceSize:MTLSizeMake(64, 64, 1)
				                    toBuffer:readback
				           destinationOffset:16384 * layer
				      destinationBytesPerRow:256
				    destinationBytesPerImage:16384];
			[blit endEncoding];
			[cb encodeSignalEvent:done value:value];
			[cb commit];
			VkTimelineSemaphoreSubmitInfo ats{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
			uint64_t waitValues[2] = {value, value};
			VkSemaphore waitSemaphores[2] = {doneSem, peerSem};
			VkPipelineStageFlags waitStages[2] = {all, all};
			ats.waitSemaphoreValueCount = peer ? 2 : 1;
			ats.pWaitSemaphoreValues = waitValues;
			ats.signalSemaphoreValueCount = 1;
			ats.pSignalSemaphoreValues = &value;
			VkSubmitInfo ackSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
			ackSubmit.pNext = &ats;
			ackSubmit.waitSemaphoreCount = peer ? 2 : 1;
			ackSubmit.pWaitSemaphores = waitSemaphores;
			ackSubmit.pWaitDstStageMask = waitStages;
			ackSubmit.signalSemaphoreCount = 1;
			ackSubmit.pSignalSemaphores = &ackSem;
			CHECK(vkQueueSubmit(queue, 1, &ackSubmit, VK_NULL_HANDLE));
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			uint64_t before = 999;
			CHECK(vkGetSemaphoreCounterValue(device, ackSem, &before));
			if (produced.signaledValue >= value || done.signaledValue >= value || before >= value ||
			    (peer && peerDone.signaledValue >= value) || cb.status == MTLCommandBufferStatusCompleted) {
				fprintf(stderr, "premature completion value=%llu\n", value);
				gate.signaledValue = value;
				CHECK(vkQueueWaitIdle(queue));
				return 1;
			}
			gate.signaledValue = value;
			VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
			wi.semaphoreCount = 1;
			wi.pSemaphores = &ackSem;
			wi.pValues = &value;
			CHECK(vkWaitSemaphores(device, &wi, 5000000000ull));
			uint64_t observed;
			CHECK(vkGetSemaphoreCounterValue(device, ackSem, &observed));
			if (observed != value || produced.signaledValue != value || done.signaledValue != value)
				return 1;
			if (peer &&
			    (dispatch_semaphore_wait(peerReply, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) ||
			     !peerOK || peerObserved != value || peerDone.signaledValue != value ||
			     consumedSurfaceID != layers))
				return 1;
			dispatch_release(peerReply);
			[cb waitUntilCompleted];
			printf(
			    "gpu_chain value=%llu producer=%llu done=%llu ack=%llu submit_us=%.3f "
			    "pending_before_gate=1\n",
			    value, produced.signaledValue, done.signaledValue, observed, submitUs);
			if (cb.status != MTLCommandBufferStatusCompleted) {
				fprintf(stderr, "Metal readback error: %s\n", cb.error.description.UTF8String);
				return 1;
			}
			if (!checkArrayPixels((const uint8_t *)readback.contents, value, layers, bgra, srgb,
			                      peer != nil)) {
				fprintf(stderr, "bad local pixels value=%llu\n", value);
				return 1;
			}
		}
		printf(
		    "PASS Vulkan imported shared texture: 32 delayed shader-rendered frames, %u local pixels, "
		    "exact "
		    "producer/done/ack values; elapsed_ms=%.3f\n",
		    131072u * layers,
		    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
		CHECK(vkQueueWaitIdle(queue));
		if (peer) {
			printf(
			    "PASS cross-process texture zero-copy: 32 values; peer verified 131072 pixels per layer on "
			    "shared "
			    "texture; "
			    "GPU producer and consumer ordering passed\n");
			[peerConnection invalidate];
			[peerConnection release];
			vkDestroySemaphore(device, peerSem, nullptr);
			[peerDone release];
		}
		vkDestroySemaphore(device, doneSem, nullptr);
		vkDestroySemaphore(device, gateSem, nullptr);
		vkDestroySemaphore(device, ackSem, nullptr);
		[produced release];
		[done release];
		[gate release];
		[readback release];
		[metalQueue release];
		[alias release];
		vkDestroySemaphore(device, sem, nullptr);
		vkDestroyCommandPool(device, pool, nullptr);
		for (unsigned layer = 0; layer < layers; ++layer)
			vkDestroyFramebuffer(device, fbs[layer], nullptr);
		vkDestroyPipeline(device, pipeline, nullptr);
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		vkDestroyShaderModule(device, vertex, nullptr);
		vkDestroyShaderModule(device, fragment, nullptr);
		vkDestroyRenderPass(device, rp, nullptr);
		for (unsigned layer = 0; layer < layers; ++layer)
			vkDestroyImageView(device, views[layer], nullptr);
		vkDestroyImage(device, image, nullptr);

		[metalDevice release];
		vkDestroyDevice(device, nullptr);
		vkDestroyInstance(instance, nullptr);
		dlclose(library);
		return 0;
	}
}
