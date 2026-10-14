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
#import "shared-event-peer.h"
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
		if (argc != 2 && argc != 3) {
			fprintf(stderr, "usage: shared-event-probe patched-ICD.dylib [peer-service]\n");
			return 2;
		}
		void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
		if (!library) {
			fprintf(stderr, "%s\n", dlerror());
			return 2;
		}
		using CopyEvent = VkResult (*)(VkSemaphore, void **);
		using SetEvent = VkResult (*)(VkSemaphore, void *);
		auto copyEvent = (CopyEvent)dlsym(library, "kk_test_copy_shared_event");
		auto setEvent = (SetEvent)dlsym(library, "kk_test_set_shared_event");
		if (!copyEvent || !setEvent) {
			fprintf(stderr, "diagnostic hooks missing\n");
			return 2;
		}
		bool negative = false;
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
		void *exported = nullptr;
		CHECK(copyEvent(sem, &exported));
		id<MTLSharedEvent> produced = (id<MTLSharedEvent>)exported;
		id<MTLSharedEvent> done = [heap.device newSharedEvent];
		id<MTLSharedEvent> gate = [heap.device newSharedEvent];
		VkSemaphore doneSem, gateSem, ackSem;
		CHECK(vkCreateSemaphore(device, &sci, nullptr, &doneSem));
		CHECK(vkCreateSemaphore(device, &sci, nullptr, &gateSem));
		CHECK(vkCreateSemaphore(device, &sci, nullptr, &ackSem));
		CHECK(setEvent(doneSem, (void *)done));
		CHECK(setEvent(gateSem, (void *)gate));
		void *roundtrip = nullptr;
		CHECK(copyEvent(doneSem, &roundtrip));
		if ((id<MTLSharedEvent>)roundtrip != done)
			return 1;
		[(id)roundtrip release];
		VkSemaphore binaryCheck;
		VkSemaphoreCreateInfo binaryCI{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		CHECK(vkCreateSemaphore(device, &binaryCI, nullptr, &binaryCheck));
		void *rejected = nullptr;
		if (copyEvent(binaryCheck, &rejected) != VK_ERROR_FEATURE_NOT_PRESENT || rejected)
			return 1;
		vkDestroySemaphore(device, binaryCheck, nullptr);
		NSXPCConnection *peerConnection = nil;
		id<KKEventPeerProtocol> peer = nil;
		id<MTLSharedEvent> peerDone = nil;
		VkSemaphore peerSem = VK_NULL_HANDLE;
		if (argc == 3) {
			peerDone = [heap.device newSharedEvent];
			CHECK(vkCreateSemaphore(device, &sci, nullptr, &peerSem));
			CHECK(setEvent(peerSem, (void *)peerDone));
			peerConnection =
			    [[NSXPCConnection alloc] initWithMachServiceName:[NSString stringWithUTF8String:argv[2]]
			                                             options:0];
			peerConnection.remoteObjectInterface =
			    [NSXPCInterface interfaceWithProtocol:@protocol(KKEventPeerProtocol)];
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
			[peer configureProducer:pHandle
			                   done:dHandle
			               registry:heap.device.registryID
			                  reply:^(BOOL ok, int pid) {
				            configuredOK = ok;
				            peerPID = pid;
				            dispatch_semaphore_signal(configured);
			                  }];
			if (dispatch_semaphore_wait(configured, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) ||
			    !configuredOK || peerPID == getpid())
				return 1;
			printf("cross_process local_pid=%d peer_pid=%d same_registry=1\n", getpid(), peerPID);
			dispatch_release(configured);
			[pHandle release];
			[dHandle release];
		}
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
			if (peer)
				[peer consumeValue:value
				             reply:^(BOOL ok, uint64_t seen) {
					       peerOK = ok;
					       peerObserved = seen;
					       dispatch_semaphore_signal(peerReply);
				             }];
			id<MTLCommandBuffer> cb = [metalQueue commandBuffer];
			[cb encodeWaitForEvent:produced value:value];
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
			     !peerOK || peerObserved != value || peerDone.signaledValue != value))
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
		    "PASS Vulkan -> Metal -> Vulkan GPU event chain: 32 delayed frames, 131072 pixels, exact "
		    "producer/done/ack values; elapsed_ms=%.3f\n",
		    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
		CHECK(vkQueueWaitIdle(queue));
		if (peer) {
			printf(
			    "PASS cross-process GPU event handle chain: 32 values; peer GPU filled and verified 131072 "
			    "bytes; texture storage not transferred\n");
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
		vkDestroyFramebuffer(device, fb, nullptr);
		vkDestroyRenderPass(device, rp, nullptr);
		vkDestroyImageView(device, view, nullptr);
		vkDestroyImage(device, image, nullptr);
		vkFreeMemory(device, memory, nullptr);
		vkDestroyDevice(device, nullptr);
		vkDestroyInstance(instance, nullptr);
		dlclose(library);
		return 0;
	}
}
