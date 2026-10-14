// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#import <Metal/Metal.h>
#define VK_USE_PLATFORM_METAL_EXT 1
#include <vulkan/vulkan.h>
#include "client/comp_vk_metal_texture.h"
#import "tests_macos_vk_metal_texture_peer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define CHECK(call)                                                                                                    \
	do {                                                                                                           \
		VkResult result = (call);                                                                              \
		if (result != VK_SUCCESS) {                                                                            \
			fprintf(stderr, "FAIL %s: %d\n", #call, result);                                               \
			exit(1);                                                                                       \
		}                                                                                                      \
	} while (0)

static VkSemaphore
import_event(VkDevice device, id<MTLSharedEvent> event)
{
	VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
	                                  .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	                                  .initialValue = event.signaledValue};
	VkImportMetalSharedEventInfoEXT import = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_METAL_SHARED_EVENT_INFO_EXT, .pNext = &type, .mtlSharedEvent = event};
	VkSemaphoreCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &import};
	VkSemaphore semaphore;
	CHECK(vkCreateSemaphore(device, &info, NULL, &semaphore));
	return semaphore;
}
int
main(int argc, char **argv)
{
	@autoreleasepool {
		NSXPCConnection *connection = nil;
		id<VulkanMetalTexturePeerProtocol> peer = nil;
		if (argc == 2) {
			connection =
			    [[NSXPCConnection alloc] initWithMachServiceName:[NSString stringWithUTF8String:argv[1]]
			                                             options:0];
			connection.remoteObjectInterface =
			    [NSXPCInterface interfaceWithProtocol:@protocol(VulkanMetalTexturePeerProtocol)];
			[connection resume];
			peer = [connection remoteObjectProxyWithErrorHandler:^(NSError *error) {
			  fprintf(stderr, "Peer XPC error: %s\n", error.description.UTF8String);
			  exit(1);
			}];
		}
		VkInstance instance;
		const char *iext[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
		VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		                                 .apiVersion = VK_API_VERSION_1_2};
		VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		                            .pApplicationInfo = &application,
		                            .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
		                            .enabledExtensionCount = 1,
		                            .ppEnabledExtensionNames = iext};
		CHECK(vkCreateInstance(&ici, NULL, &instance));
		uint32_t count = 1;
		VkPhysicalDevice physical;
		CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
		float priority = 1;
		VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		                                      .queueCount = 1,
		                                      .pQueuePriorities = &priority};
		VkPhysicalDeviceTimelineSemaphoreFeatures timeline = {
		    .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
		    .timelineSemaphore = VK_TRUE};
		const char *ext[] = {VK_EXT_METAL_OBJECTS_EXTENSION_NAME, "VK_KHR_portability_subset"};
		VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		                          .pNext = &timeline,
		                          .queueCreateInfoCount = 1,
		                          .pQueueCreateInfos = &queue_info,
		                          .enabledExtensionCount = 2,
		                          .ppEnabledExtensionNames = ext};
		VkDevice device;
		CHECK(vkCreateDevice(physical, &dci, NULL, &device));
		VkQueue queue;
		vkGetDeviceQueue(device, 0, 0, &queue);
		PFN_vkExportMetalObjectsEXT export_objects =
		    (PFN_vkExportMetalObjectsEXT)vkGetDeviceProcAddr(device, "vkExportMetalObjectsEXT");
		struct vk_bundle vk = {.device = device,
		                       .physical_device = physical,
		                       .vkCreateImage = vkCreateImage,
		                       .vkDestroyImage = vkDestroyImage,
		                       .vkExportMetalObjectsEXT = export_objects,
		                       .vkGetPhysicalDeviceFormatProperties = vkGetPhysicalDeviceFormatProperties};
		VkExportMetalDeviceInfoEXT native_device = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT};
		VkExportMetalObjectsInfoEXT objects = {.sType = VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT,
		                                       .pNext = &native_device};
		export_objects(device, &objects);
		id<MTLDevice> metal = native_device.mtlDevice;
		id<MTLCommandQueue> consumer = [metal newCommandQueue];
		VkCommandPool pool;
		VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		                               .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
		CHECK(vkCreateCommandPool(device, &pci, NULL, &pool));
		VkCommandBuffer cmd;
		VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		                                   .commandPool = pool,
		                                   .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		                                   .commandBufferCount = 1};
		CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
		const struct
		{
			VkFormat vk;
			MTLPixelFormat metal;
			unsigned bytes;
		} formats[] = {
		    {VK_FORMAT_R8G8B8A8_UNORM, MTLPixelFormatRGBA8Unorm, 4},
		    {VK_FORMAT_B8G8R8A8_SRGB, MTLPixelFormatBGRA8Unorm_sRGB, 4},
		    {VK_FORMAT_B8G8R8A8_UNORM, MTLPixelFormatBGRA8Unorm, 4},
		    {VK_FORMAT_R8G8B8A8_SRGB, MTLPixelFormatRGBA8Unorm_sRGB, 4},
		    {VK_FORMAT_R16G16B16A16_UNORM, MTLPixelFormatRGBA16Unorm, 8},
		    {VK_FORMAT_R16G16B16A16_SFLOAT, MTLPixelFormatRGBA16Float, 8},
		    {VK_FORMAT_R32_SFLOAT, MTLPixelFormatR32Float, 4},
		    {VK_FORMAT_D16_UNORM, MTLPixelFormatDepth16Unorm, 2},
		    {VK_FORMAT_D32_SFLOAT, MTLPixelFormatDepth32Float, 4},
		    {VK_FORMAT_D32_SFLOAT_S8_UINT, MTLPixelFormatDepth32Float_Stencil8, 4},
		};
		unsigned checked = 0;
		for (unsigned test = 0; test < sizeof(formats) / sizeof(formats[0]); ++test)
			for (unsigned layers = 1; layers <= 2; ++layers) {
				VkFormat format = formats[test].vk;
				bool depth = format >= VK_FORMAT_D16_UNORM;
				bool combined = format == VK_FORMAT_D32_SFLOAT_S8_UINT;
				MTLTextureDescriptor *td =
				    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:formats[test].metal
				                                                       width:64
				                                                      height:64
				                                                   mipmapped:NO];
				td.textureType = layers == 1 ? MTLTextureType2D : MTLTextureType2DArray;
				td.arrayLength = layers;
				td.storageMode = MTLStorageModePrivate;
				td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
				id<MTLTexture> owner = [metal newSharedTextureWithDescriptor:td];
				MTLSharedTextureHandle *handle = [owner newSharedTextureHandle];
				id<MTLTexture> reopened = [metal newSharedTextureWithHandle:handle];
				[handle release];
				if (!owner || !reopened)
					return 1;
				struct xrt_swapchain_create_info info = {.format = formats[test].vk,
				                                         .width = 64,
				                                         .height = 64,
				                                         .array_size = layers,
				                                         .face_count = 1,
				                                         .mip_count = 1,
				                                         .sample_count = 1,
				                                         .bits = XRT_SWAPCHAIN_USAGE_SAMPLED |
				                                                 XRT_SWAPCHAIN_USAGE_TRANSFER_DST |
				                                                 XRT_SWAPCHAIN_USAGE_TRANSFER_SRC};
				info.bits |= depth ? XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL : XRT_SWAPCHAIN_USAGE_COLOR;
				VkImage image;
				CHECK(client_vk_metal_texture_import(&vk, &info, reopened, &image));
				id<MTLSharedEvent> produced = [metal newSharedEvent], gate = [metal newSharedEvent],
				                   done = [metal newSharedEvent];
				VkSemaphore sem = import_event(device, produced), gate_sem = import_event(device, gate),
				            done_sem = import_event(device, done);
				bool remote =
				    peer && !depth && formats[test].bytes == 4 && format != VK_FORMAT_R32_SFLOAT;
				id<MTLSharedEvent> peer_done = remote ? [metal newSharedEvent] : nil;
				VkSemaphore peer_sem = remote ? import_event(device, peer_done) : VK_NULL_HANDLE;
				if (remote) {
					MTLSharedEventHandle *p = [produced newSharedEventHandle],
					                     *d = [peer_done newSharedEventHandle];
					MTLSharedTextureHandle *t = [owner newSharedTextureHandle];
					dispatch_semaphore_t ready = dispatch_semaphore_create(0);
					__block BOOL valid = NO;
					__block int pid = 0;
					[peer configureProducer:p
					                   done:d
					                texture:t
					               registry:metal.registryID
					                  reply:^(BOOL ok, int child) {
						            valid = ok;
						            pid = child;
						            dispatch_semaphore_signal(ready);
					                  }];
					if (dispatch_semaphore_wait(
					        ready, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) ||
					    !valid || pid == getpid())
						return 1;
					printf("Cross-process peer pid=%d producer pid=%d format=%u layers=%u\n", pid,
					       getpid(), format, layers);
					dispatch_release(ready);
					[p release];
					[d release];
					[t release];
				}
				VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
				                                  .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
				VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
				                             .pNext = &type};
				VkSemaphore ack;
				CHECK(vkCreateSemaphore(device, &sci, NULL, &ack));
				unsigned pitch = 64 * formats[test].bytes;
				pitch = pitch < 256 ? 256 : pitch;
				unsigned layer_bytes = pitch * 64;
				id<MTLBuffer> readback = [metal newBufferWithLength:layer_bytes * layers
				                                            options:MTLResourceStorageModeShared];
				id<MTLBuffer> stencil = combined
				                            ? [metal newBufferWithLength:16384 * layers
				                                                 options:MTLResourceStorageModeShared]
				                            : nil;
				for (uint64_t value = 1; value <= 4; ++value) {
					CHECK(vkResetCommandBuffer(cmd, 0));
					VkCommandBufferBeginInfo begin = {
					    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
					CHECK(vkBeginCommandBuffer(cmd, &begin));
					VkImageAspectFlags aspect =
					    depth ? (combined ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
					                      : VK_IMAGE_ASPECT_DEPTH_BIT)
					          : VK_IMAGE_ASPECT_COLOR_BIT;
					VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					                                .newLayout =
					                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					                                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
					                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					                                .image = image,
					                                .subresourceRange = {aspect, 0, 1, 0, layers}};
					vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
					                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
					                     &barrier);
					for (unsigned layer = 0; layer < layers; ++layer) {
						VkImageSubresourceRange range = {aspect, 0, 1, layer, 1};
						if (depth) {
							VkClearDepthStencilValue clear = {
							    .depth = layer ? .75f + (float)value / 32
							                   : .25f + (float)value / 16,
							    .stencil = 17 + layer + (uint32_t)value};
							vkCmdClearDepthStencilImage(
							    cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
							    &range);
						} else {
							float green = (float)(value + (layer ? 3 : 0)) / 8;
							VkClearColorValue clear = {
							    .float32 = {layer ? 0 : 1, green, layer ? 1 : 0, 1}};
							if (format == VK_FORMAT_R32_SFLOAT)
								clear.float32[0] = green;
							vkCmdClearColorImage(cmd, image,
							                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							                     &clear, 1, &range);
						}
					}
					CHECK(vkEndCommandBuffer(cmd));
					VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
					VkTimelineSemaphoreSubmitInfo ts = {
					    .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
					    .waitSemaphoreValueCount = 1,
					    .pWaitSemaphoreValues = &value,
					    .signalSemaphoreValueCount = 1,
					    .pSignalSemaphoreValues = &value};
					VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
					                       .pNext = &ts,
					                       .waitSemaphoreCount = 1,
					                       .pWaitSemaphores = &gate_sem,
					                       .pWaitDstStageMask = &stage,
					                       .commandBufferCount = 1,
					                       .pCommandBuffers = &cmd,
					                       .signalSemaphoreCount = 1,
					                       .pSignalSemaphores = &sem};
					CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
					id<MTLCommandBuffer> cb = [consumer commandBuffer];
					[cb encodeWaitForEvent:produced value:value];
					id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
					for (unsigned layer = 0; layer < layers; ++layer) {
						[blit copyFromTexture:owner
						                 sourceSlice:layer
						                 sourceLevel:0
						                sourceOrigin:MTLOriginMake(0, 0, 0)
						                  sourceSize:MTLSizeMake(64, 64, 1)
						                    toBuffer:readback
						           destinationOffset:layer * layer_bytes
						      destinationBytesPerRow:pitch
						    destinationBytesPerImage:layer_bytes
						                     options:combined
						                                 ? MTLBlitOptionDepthFromDepthStencil
										 : MTLBlitOptionNone];
						if (combined)
							[blit copyFromTexture:owner
							                 sourceSlice:layer
							                 sourceLevel:0
							                sourceOrigin:MTLOriginMake(0, 0, 0)
							                  sourceSize:MTLSizeMake(64, 64, 1)
							                    toBuffer:stencil
							           destinationOffset:layer * 16384
							      destinationBytesPerRow:256
							    destinationBytesPerImage:16384
							                     options:
							                         MTLBlitOptionStencilFromDepthStencil];
					}
					[blit endEncoding];
					[cb encodeSignalEvent:done value:value];
					[cb commit];
					VkSemaphore waits[2] = {done_sem, peer_sem};
					uint64_t wait_values[2] = {value, value};
					VkPipelineStageFlags wait_stages[2] = {stage, stage};
					ts.waitSemaphoreValueCount = remote ? 2 : 1;
					ts.pWaitSemaphoreValues = wait_values;
					VkSubmitInfo acknowledged = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
					                             .pNext = &ts,
					                             .waitSemaphoreCount = remote ? 2 : 1,
					                             .pWaitSemaphores = waits,
					                             .pWaitDstStageMask = wait_stages,
					                             .signalSemaphoreCount = 1,
					                             .pSignalSemaphores = &ack};
					CHECK(vkQueueSubmit(queue, 1, &acknowledged, VK_NULL_HANDLE));
					dispatch_semaphore_t peer_reply = remote ? dispatch_semaphore_create(0) : NULL;
					__block BOOL peer_valid = NO;
					__block uint64_t peer_value = 0;
					if (remote)
						[peer consumeValue:value
						             reply:^(BOOL ok, uint64_t observed) {
							       peer_valid = ok;
							       peer_value = observed;
							       dispatch_semaphore_signal(peer_reply);
						             }];
					if (remote) {
						// Confirm that the peer submitted its GPU wait before opening the gate.
						dispatch_semaphore_t scheduled = dispatch_semaphore_create(0);
						__block uint64_t queued = 0;
						for (unsigned tries = 0; tries < 5000 && queued != value; ++tries) {
							[peer queuedValue:^(uint64_t observed) {
							  queued = observed;
							  dispatch_semaphore_signal(scheduled);
							}];
							if (dispatch_semaphore_wait(
							        scheduled,
							        dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)))
								return 1;
							if (queued != value)
								usleep(1000);
						}
						if (queued != value)
							return 1;
						dispatch_release(scheduled);
					}
					usleep(20000);
					if (produced.signaledValue >= value || done.signaledValue >= value ||
					    (remote && peer_done.signaledValue >= value)) {
						fprintf(stderr, "FAIL consumer ran before producer gate\n");
						gate.signaledValue = value;
						return 1;
					}
					gate.signaledValue = value;
					VkSemaphoreWaitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
					                            .semaphoreCount = 1,
					                            .pSemaphores = &ack,
					                            .pValues = &value};
					CHECK(vkWaitSemaphores(device, &wait, 5000000000ull));
					[cb waitUntilCompleted];
					if (remote) {
						VkSemaphoreWaitInfo remote_wait = {
						    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
						    .semaphoreCount = 1,
						    .pSemaphores = &peer_sem,
						    .pValues = &value};
						CHECK(vkWaitSemaphores(device, &remote_wait, 5000000000ull));
						if (dispatch_semaphore_wait(
						        peer_reply,
						        dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) ||
						    !peer_valid || peer_value != value)
							return 1;
						dispatch_release(peer_reply);
						printf(
						    "PASS cross-process exact pixels format=%u layers=%u value=%llu\n",
						    format, layers, (unsigned long long)value);
					}

					if (cb.status != MTLCommandBufferStatusCompleted ||
					    produced.signaledValue != value || done.signaledValue != value)
						return 1;
					for (unsigned layer = 0; layer < layers; ++layer)
						for (unsigned y = 0; y < 64; ++y)
							for (unsigned x = 0; x < 64; ++x) {
								const uint8_t *pixel =
								    (const uint8_t *)readback.contents +
								    layer * layer_bytes + y * pitch +
								    x * formats[test].bytes;
								if (depth) {
									float expected = layer
									                     ? .75f + (float)value / 32
									                     : .25f + (float)value / 16;
									if (format == VK_FORMAT_D16_UNORM
									        ? abs((int)*(const uint16_t *)pixel -
										      (int)lroundf(expected * 65535)) >
									              1
										: fabsf(*(const float *)pixel -
										        expected) > 1e-6f)
										return 1;
									if (combined &&
									    ((const uint8_t *)stencil
									         .contents)[layer * 16384 + y * 256 +
									                    x] != 17 + layer + value)
										return 1;
								} else {
									float values[4] = {
									    layer ? 0 : 1,
									    (float)(value + (layer ? 3 : 0)) / 8,
									    layer ? 1 : 0, 1};
									if (format == VK_FORMAT_R32_SFLOAT) {
										if (fabsf(*(const float *)pixel -
										          values[1]) > 1e-6f)
											return 1;
									} else
										for (unsigned channel = 0; channel < 4;
										     ++channel) {
											float expected =
											    values[channel];
											unsigned index =
											    (format ==
											         VK_FORMAT_B8G8R8A8_UNORM ||
											     format ==
											         VK_FORMAT_B8G8R8A8_SRGB) &&
											            (channel == 0 ||
												     channel == 2)
											        ? 2 - channel
											        : channel;
											if ((format ==
											         VK_FORMAT_R8G8B8A8_SRGB ||
											     format ==
											         VK_FORMAT_B8G8R8A8_SRGB) &&
											    channel < 3)
												expected =
												    expected <=
												            .0031308f
												        ? 12.92f *
												              expected
												        : 1.055f *
												                  powf(
												                      expected,
												                      1 / 2.4f) -
												              .055f;
											if (formats[test].bytes == 4 &&
											    abs((int)pixel[index] -
											        (int)lroundf(expected *
											                     255)) > 1)
												return 1;
											if (format ==
											        VK_FORMAT_R16G16B16A16_UNORM &&
											    abs((int)((const uint16_t *)
											                  pixel)
											            [channel] -
											        (int)lroundf(expected *
											                     65535)) >
											        1)
												return 1;
											if (format ==
											        VK_FORMAT_R16G16B16A16_SFLOAT &&
											    fabsf(
											        (float)((const __fp16 *)
												            pixel)
											            [channel] -
											        expected) > .001f)
												return 1;
										}
								}
								++checked;
							}
					CHECK(vkQueueWaitIdle(queue));
				}
				printf(
				    "PASS imported texture format=%u layers=%u identity=1 bound_without_memory=1 GPU "
				    "values=1..4\n",
				    formats[test].vk, layers);
				if (remote)
					vkDestroySemaphore(device, peer_sem, NULL);
				[peer_done release];
				vkDestroySemaphore(device, ack, NULL);
				vkDestroySemaphore(device, sem, NULL);
				vkDestroySemaphore(device, gate_sem, NULL);
				vkDestroySemaphore(device, done_sem, NULL);
				[produced release];
				[gate release];
				[done release];
				[readback release];
				[stencil release];
				vkDestroyImage(device, image, NULL);
				[reopened release];
				[owner release];
			}
		printf("PASS %u pixels; GPU producer/consumer ordering; colour, sRGB, float, depth and stencil\n",
		       checked);
		[connection invalidate];
		[connection release];
		[consumer release];
		vkDestroyCommandPool(device, pool, NULL);
		vkDestroyDevice(device, NULL);
		vkDestroyInstance(instance, NULL);
		return 0;
	}
}
