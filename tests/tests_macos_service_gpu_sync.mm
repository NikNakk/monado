// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
// Native Metal OpenXR client for isolated service and headset regressions.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#define XR_USE_GRAPHICS_API_METAL
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"
#include "openxr/openxr_loader_negotiation.h"
#include <dlfcn.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unistd.h>

// Suppress only the producer completion signal, never enqueue a future GPU wait.
// All other operations, including real texture writes, use the native queue.
@interface SignalBuffer : NSObject
@property(nonatomic, retain) id<MTLCommandBuffer> buffer;
@property(nonatomic) bool suppress;
@end
@implementation SignalBuffer
- (id)forwardingTargetForSelector:(SEL)selector
{
	return self.buffer;
}
- (void)encodeSignalEvent:(id<MTLEvent>)event value:(uint64_t)value
{
	if (!self.suppress)
		[self.buffer encodeSignalEvent:event value:value];
	else
		fprintf(stdout, "{\"event\":\"suppressed_signal\",\"value\":%llu}\n", (unsigned long long)value);
}
- (void)dealloc
{
	[_buffer release];
	[super dealloc];
}
@end
@interface SignalQueue : NSObject
@property(nonatomic, retain) id<MTLCommandQueue> queue;
@property(nonatomic) bool suppress;
@end
@implementation SignalQueue
- (id)forwardingTargetForSelector:(SEL)selector
{
	return self.queue;
}
- (id<MTLCommandBuffer>)commandBuffer
{
	SignalBuffer *result = [[[SignalBuffer alloc] init] autorelease];
	result.buffer = [self.queue commandBuffer];
	result.suppress = self.suppress;
	return (id<MTLCommandBuffer>)result;
}
- (void)dealloc
{
	[_queue release];
	[super dealloc];
}
@end

static uint64_t
now_ns()
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}
static void
checked(XrResult result, const char *operation)
{
	if (XR_FAILED(result))
		throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
int
main(int argc, char **argv)
{
	@autoreleasepool {
		try {
			const bool budget = argc > 1 && std::string(argv[1]) == "budget";
			const bool exhausted = argc > 1 && std::string(argv[1]) == "exhausted";
			const bool missing = argc > 1 && std::string(argv[1]) == "missing";
			const bool healthy_overlay = argc > 1 && std::string(argv[1]) == "overlay";
			const bool array_three = argc > 1 && std::string(argv[1]) == "array-three";
			const bool array_four = argc > 1 && std::string(argv[1]) == "array-four";
			const bool array_test = array_three || array_four;
			const bool projection_only = argc > 1 && std::string(argv[1]) == "projection-only";
			const bool projection_quads = argc > 1 && std::string(argv[1]) == "projection-quads-four";
			const bool quads_four = projection_quads || (argc > 1 && std::string(argv[1]) == "quads-four");
			const unsigned frames = argc > 2 ? std::stoul(argv[2]) : 120;
			const uint64_t timeout_seconds = argc > 3 ? std::stoull(argv[3]) : 20;
			if (timeout_seconds < 1 || timeout_seconds > 300)
				throw std::runtime_error("Timeout must be between 1 and 300 seconds");
			fprintf(
			    stdout,
			    "{\"event\":\"config\",\"thermal\":%ld,\"low_power\":%s,\"clock\":\"CLOCK_UPTIME_RAW\"}\n",
			    (long)[NSProcessInfo processInfo].thermalState,
			    [NSProcessInfo processInfo].lowPowerModeEnabled ? "true" : "false");
			if (argc > 1 && std::string(argv[1]) == "telemetry")
				return 0;
			const char *path = getenv("MONADO_OPENXR_RUNTIME_PATH");
			if (!path)
				throw std::runtime_error("MONADO_OPENXR_RUNTIME_PATH required");
			void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
			if (!library)
				throw std::runtime_error(dlerror());
			auto negotiate =
			    (PFN_xrNegotiateLoaderRuntimeInterface)dlsym(library, "xrNegotiateLoaderRuntimeInterface");
			if (!negotiate)
				throw std::runtime_error("runtime negotiation unavailable");
			XrNegotiateLoaderInfo loader{};
			loader.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
			loader.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
			loader.structSize = sizeof(loader);
			loader.minInterfaceVersion = loader.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
			loader.minApiVersion = loader.maxApiVersion = XR_CURRENT_API_VERSION;
			XrNegotiateRuntimeRequest request{};
			request.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
			request.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
			request.structSize = sizeof(request);
			checked(negotiate(&loader, &request), "negotiate");
			XrInstance instance = XR_NULL_HANDLE;
#define LOAD(name)                                                                                                     \
	PFN_##name name = nullptr;                                                                                     \
	checked(request.getInstanceProcAddr(instance, #name, (PFN_xrVoidFunction *)&name), #name)
			LOAD(xrCreateInstance);
			const char *extensions[] = {XR_KHR_METAL_ENABLE_EXTENSION_NAME, XR_EXTX_OVERLAY_EXTENSION_NAME};
			XrInstanceCreateInfo info{};
			info.type = XR_TYPE_INSTANCE_CREATE_INFO;
			snprintf(info.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "gpu-sync-%d",
			         getpid());
			info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
			info.enabledExtensionCount = (missing || budget || exhausted) ? 2 : 1;
			info.enabledExtensionNames = extensions;
			checked(xrCreateInstance(&info, &instance), "xrCreateInstance");
			LOAD(xrGetSystem);
			LOAD(xrGetMetalGraphicsRequirementsKHR);
			LOAD(xrCreateSession);
			LOAD(xrPollEvent);
			LOAD(xrBeginSession);
			LOAD(xrEndSession);
			LOAD(xrDestroySession);
			LOAD(xrDestroyInstance);
			LOAD(xrCreateReferenceSpace);
			LOAD(xrDestroySpace);
			LOAD(xrCreateSwapchain);
			LOAD(xrEnumerateSwapchainImages);
			LOAD(xrDestroySwapchain);
			LOAD(xrAcquireSwapchainImage);
			LOAD(xrWaitSwapchainImage);
			LOAD(xrReleaseSwapchainImage);
			LOAD(xrWaitFrame);
			LOAD(xrBeginFrame);
			LOAD(xrEndFrame);
			LOAD(xrLocateViews);
#undef LOAD
			XrSystemGetInfo systemInfo{};
			systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
			systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
			XrSystemId system;
			checked(xrGetSystem(instance, &systemInfo, &system), "xrGetSystem");
			XrGraphicsRequirementsMetalKHR requirements{};
			requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_METAL_KHR;
			checked(xrGetMetalGraphicsRequirementsKHR(instance, system, &requirements),
			        "metal requirements");
			id<MTLDevice> device = (id<MTLDevice>)requirements.metalDevice;
			SignalQueue *queue = [[[SignalQueue alloc] init] autorelease];
			queue.queue = [device newCommandQueue];
			[queue.queue release];
			XrGraphicsBindingMetalKHR binding{};
			binding.type = XR_TYPE_GRAPHICS_BINDING_METAL_KHR;
			binding.commandQueue = (void *)queue;
			XrSessionCreateInfoOverlayEXTX overlay{};
			overlay.type = XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX;
			overlay.sessionLayersPlacement = 1;
			if (missing || budget || exhausted || healthy_overlay)
				binding.next = &overlay;
			XrSessionCreateInfo sessionInfo{};
			sessionInfo.type = XR_TYPE_SESSION_CREATE_INFO;
			sessionInfo.next = &binding;
			sessionInfo.systemId = system;
			XrSession session;
			checked(xrCreateSession(instance, &sessionInfo, &session), "xrCreateSession");
			XrReferenceSpaceCreateInfo spaceInfo{};
			spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
			// A head-locked quad keeps pixel samples valid while a worn or
			// handled headset moves; the default stays world-locked.
			const char *view_locked_env = getenv("GPU_SYNC_VIEW_LOCKED");
			const bool view_locked =
			    quads_four || (view_locked_env != nullptr && strcmp(view_locked_env, "1") == 0);
			spaceInfo.referenceSpaceType =
			    view_locked ? XR_REFERENCE_SPACE_TYPE_VIEW : XR_REFERENCE_SPACE_TYPE_LOCAL;
			spaceInfo.poseInReferenceSpace.orientation.w = 1;
			XrSpace space;
			checked(xrCreateReferenceSpace(session, &spaceInfo, &space), "space");
			XrSwapchainCreateInfo swapInfo{};
			swapInfo.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
			swapInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
			swapInfo.format = MTLPixelFormatBGRA8Unorm;
			swapInfo.sampleCount = 1;
			swapInfo.width = swapInfo.height = 64;
			swapInfo.faceCount = swapInfo.arraySize = swapInfo.mipCount = 1;
			// Arrays force the production shared-Metal path; simple BGRA IOSurfaces
			// already have three images even without compatibility scheduling.
			if (array_test) {
				swapInfo.arraySize = 2;
				swapInfo.width = swapInfo.height = 1024;
			}
			XrSwapchain swapchain = XR_NULL_HANDLE;
			XrResult allocation = xrCreateSwapchain(session, &swapInfo, &swapchain);
			if (exhausted) {
				if (XR_SUCCEEDED(allocation))
					throw std::runtime_error("Exhausted global budget admitted allocation");
				checked(xrDestroySpace(space), "space");
				checked(xrDestroySession(session), "session");
				checked(xrDestroyInstance(instance), "instance");
				fprintf(stdout,
				        "{\"event\":\"result\",\"status\":\"PASS\",\"budget_rejected\":true,\"result\":"
				        "%d}\n",
				        allocation);
				return 0;
			}
			checked(allocation, "swapchain");
			if (budget) {
				XrSwapchain extra = XR_NULL_HANDLE, rejected = XR_NULL_HANDLE;
				checked(xrCreateSwapchain(session, &swapInfo, &extra), "second allocation");
				XrResult result = xrCreateSwapchain(session, &swapInfo, &rejected);
				if (XR_SUCCEEDED(result))
					throw std::runtime_error("Per-client budget admitted a third allocation");
				checked(xrDestroySwapchain(extra), "extra");
				checked(xrDestroySwapchain(swapchain), "swapchain");
				checked(xrDestroySpace(space), "space");
				checked(xrDestroySession(session), "session");
				checked(xrDestroyInstance(instance), "instance");
				fprintf(stdout,
				        "{\"event\":\"result\",\"status\":\"PASS\",\"budget_rejected\":true,\"result\":"
				        "%d}\n",
				        result);
				return 0;
			}
			uint32_t count;
			checked(xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr), "images count");
			if (array_test && count != (array_three ? 3u : 4u))
				throw std::runtime_error("Unexpected production swapchain image count");
			fprintf(stdout, "{\"event\":\"swapchain\",\"images\":%u,\"array_size\":%u,\"width\":%u}\n",
			        count, swapInfo.arraySize, swapInfo.width);
			std::vector<XrSwapchainImageMetalKHR> images(count);
			for (auto &image : images)
				image.type = XR_TYPE_SWAPCHAIN_IMAGE_METAL_KHR;
			checked(xrEnumerateSwapchainImages(swapchain, count, &count,
			                                   (XrSwapchainImageBaseHeader *)images.data()),
			        "images");
			unsigned rendered = 0;
			bool running = false, lost = false;
			uint64_t fault = 0;
			uint64_t deadline = now_ns() + timeout_seconds * 1000000000ULL;
			while (rendered < frames && now_ns() < deadline) {
				XrEventDataBuffer event{};
				event.type = XR_TYPE_EVENT_DATA_BUFFER;
				while (xrPollEvent(instance, &event) == XR_SUCCESS) {
					if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
						auto state = ((XrEventDataSessionStateChanged *)&event)->state;
						if (state == XR_SESSION_STATE_READY) {
							XrSessionBeginInfo begin{};
							begin.type = XR_TYPE_SESSION_BEGIN_INFO;
							begin.primaryViewConfigurationType =
							    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
							checked(xrBeginSession(session, &begin), "begin session");
							running = true;
						}
						if (state == XR_SESSION_STATE_LOSS_PENDING) {
							lost = true;
							break;
						}
					}
					event = {};
					event.type = XR_TYPE_EVENT_DATA_BUFFER;
				}
				if (lost)
					break;
				if (!running) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
				if (fault) {
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
				XrFrameWaitInfo wait{};
				wait.type = XR_TYPE_FRAME_WAIT_INFO;
				XrFrameState state{};
				state.type = XR_TYPE_FRAME_STATE;
				checked(xrWaitFrame(session, &wait, &state), "wait frame");
				XrFrameBeginInfo begin{};
				begin.type = XR_TYPE_FRAME_BEGIN_INFO;
				checked(xrBeginFrame(session, &begin), "begin frame");
				uint32_t image;
				XrSwapchainImageAcquireInfo acquire{};
				acquire.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
				uint64_t acquire_start = now_ns();
				checked(xrAcquireSwapchainImage(swapchain, &acquire, &image), "acquire");
				XrSwapchainImageWaitInfo imageWait{};
				imageWait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
				imageWait.timeout = 1000000000;
				checked(xrWaitSwapchainImage(swapchain, &imageWait), "wait image");
				uint64_t acquire_end = now_ns();
				auto command = [queue.queue commandBuffer];
				MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
				pass.colorAttachments[0].texture = (id<MTLTexture>)images[image].texture;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.colorAttachments[0].clearColor = MTLClearColorMake(.25, .5, .75, 1);
				for (uint32_t slice = 0; slice < swapInfo.arraySize; ++slice) {
					pass.colorAttachments[0].slice = slice;
					[[command renderCommandEncoderWithDescriptor:pass] endEncoding];
				}
				[command commit];
				XrSwapchainImageReleaseInfo release{};
				release.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
				if (missing && rendered == 10) {
					queue.suppress = true;
					fault = now_ns();
					fprintf(stdout, "{\"event\":\"fault\",\"time_ns\":%llu}\n",
					        (unsigned long long)fault);
					fflush(stdout);
				}
				checked(xrReleaseSwapchainImage(swapchain, &release), "release");
				XrCompositionLayerQuad quad{};
				quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				quad.space = space;
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quad.subImage.swapchain = swapchain;
				quad.subImage.imageRect.extent = {(int32_t)swapInfo.width, (int32_t)swapInfo.height};
				quad.subImage.imageArrayIndex = array_test ? rendered % 2 : 0;
				quad.pose.orientation.w = 1;
				quad.pose.position.z = -1.5f;
				// 2 m at 1.5 m spans about ±34°, covering each eye's sample point.
				quad.size = view_locked ? XrExtent2Df{2, 2} : XrExtent2Df{1, 1};
				XrCompositionLayerQuad quads[4] = {quad, quad, quad, quad};
				const XrCompositionLayerBaseHeader *layers[5];
				XrCompositionLayerProjection projection{};
				projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
				XrCompositionLayerProjectionView projection_views[2]{};
				unsigned projection_count = projection_only || projection_quads ? 1 : 0;
				if (projection_count) {
					XrViewLocateInfo locate{};
					locate.type = XR_TYPE_VIEW_LOCATE_INFO;
					locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					locate.displayTime = state.predictedDisplayTime;
					locate.space = space;
					XrViewState view_state{};
					view_state.type = XR_TYPE_VIEW_STATE;
					XrView views[2]{};
					views[0].type = views[1].type = XR_TYPE_VIEW;
					uint32_t view_count = 0;
					checked(xrLocateViews(session, &locate, &view_state, 2, &view_count, views),
					        "locate stereo views");
					if (view_count != 2)
						throw std::runtime_error("Expected stereo projection views");
					for (unsigned eye = 0; eye < 2; ++eye) {
						projection_views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
						projection_views[eye].pose = views[eye].pose;
						projection_views[eye].fov = views[eye].fov;
						projection_views[eye].subImage = quad.subImage;
					}
					projection.space = space;
					projection.viewCount = 2;
					projection.views = projection_views;
					layers[0] = (XrCompositionLayerBaseHeader *)&projection;
				}
				unsigned quad_count = projection_only ? 0 : quads_four ? 4 : 1;
				for (unsigned q = 0; q < quad_count; q++) {
					if (quads_four) {
						quads[q].pose.position.x = (q & 1) ? 0.45f : -0.45f;
						quads[q].pose.position.y = (q & 2) ? 0.45f : -0.45f;
						quads[q].size = {0.65f, 0.65f};
					}
					layers[projection_count + q] = (XrCompositionLayerBaseHeader *)&quads[q];
				}
				XrFrameEndInfo end{};
				end.type = XR_TYPE_FRAME_END_INFO;
				end.displayTime = state.predictedDisplayTime;
				end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
				end.layerCount = projection_count + quad_count;
				end.layers = layers;
				uint64_t before = now_ns();
				checked(xrEndFrame(session, &end), "end frame");
				fprintf(stdout,
				        "{\"event\":\"frame\",\"pid\":%d,\"frame\":%u,\"image\":%u,\"begin_ns\":%llu,"
				        "\"end_ns\":%llu,\"acquire_wait_ns\":%llu}\n",
				        getpid(), ++rendered, image, (unsigned long long)before,
				        (unsigned long long)now_ns(),
				        (unsigned long long)(acquire_end - acquire_start));
				fflush(stdout);
			}
			if (missing ? !lost : rendered != frames)
				throw std::runtime_error("required progress/session loss not observed");
			uint64_t recovery = lost ? now_ns() - fault : 0;
			checked(xrDestroySwapchain(swapchain), "destroy swapchain");
			checked(xrDestroySpace(space), "destroy space");
			checked(xrDestroySession(session), "destroy session");
			checked(xrDestroyInstance(instance), "destroy instance");
			fprintf(stdout,
			        "{\"event\":\"result\",\"status\":\"PASS\",\"frames\":%u,\"lost\":%s,\"recovery_ns\":%"
			        "llu,\"thermal_end\":%ld}\n",
			        rendered, lost ? "true" : "false", (unsigned long long)recovery,
			        (long)[NSProcessInfo processInfo].thermalState);
			// The runtime keeps Objective-C state; leave dlclose to process teardown.
			return 0;
		} catch (const std::exception &error) {
			fprintf(stderr, "FAIL: %s\n", error.what());
			return 1;
		}
	}
}
