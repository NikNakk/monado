// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS PS VR2 display target using IOSurface-backed Vulkan images and Metal presentation.
 * @ingroup comp_main
 */

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <dispatch/dispatch.h>

#include "main/comp_window.h"
#include "util/u_debug.h"
#include "util/u_handles.h"
#include "util/u_misc.h"
#include "util/u_pacing.h"
#include "vk/vk_image_allocator.h"

#include <stdatomic.h>

#define MACOS_TARGET_IMAGE_COUNT 3

DEBUG_GET_ONCE_NUM_OPTION(display_rate_divisor, "XRT_MACOS_DISPLAY_RATE_DIVISOR", 1)
DEBUG_GET_ONCE_BOOL_OPTION(present_timing, "XRT_MACOS_PRESENT_TIMING", false)
DEBUG_GET_ONCE_BOOL_OPTION(present_immediate, "XRT_MACOS_PRESENT_IMMEDIATE", false)
DEBUG_GET_ONCE_BOOL_OPTION(async_present, "XRT_MACOS_ASYNC_PRESENT", false)
DEBUG_GET_ONCE_BOOL_OPTION(present_feedback, "XRT_MACOS_PRESENT_FEEDBACK", false)
DEBUG_GET_ONCE_NUM_OPTION(present_advance_periods, "XRT_MACOS_PRESENT_ADVANCE_PERIODS", 0)

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

struct comp_window_macos
{
	struct comp_target_swapchain base;
	NSScreen *screen;
	NSWindow *window;
	CAMetalLayer *metal_layer;
	id<MTLCommandQueue> present_queue;
	id<MTLTexture> metal_images[MACOS_TARGET_IMAGE_COUNT];
	xrt_graphics_buffer_handle_t io_surfaces[MACOS_TARGET_IMAGE_COUNT];
	struct vk_image_collection vkic;
	CVDisplayLinkRef display_link;
	mach_timebase_info_data_t mach_timebase;
	dispatch_group_t present_completion_group;
	dispatch_queue_t present_work_queue;
	dispatch_group_t present_work_group;
	dispatch_semaphore_t image_available[MACOS_TARGET_IMAGE_COUNT];
	atomic_uint_fast64_t latest_vblank_ns;
	atomic_int_fast64_t measured_present_offset_ns;
	atomic_int_fast64_t applied_present_offset_ns;
	atomic_bool metal_present_failed;
	uint64_t last_vblank_ns;
	bool pacer_vblank_synced;
	int64_t display_period_ns;
	int64_t filtered_present_offset_ns;
	uint32_t pixel_width;
	uint32_t pixel_height;
	uint32_t next_image;

	CFTimeInterval last_presented_time_s;
	CFTimeInterval presented_interval_total_s;
	CFTimeInterval presented_interval_min_s;
	CFTimeInterval presented_interval_max_s;
	CFTimeInterval presented_target_error_total_s;
	CFTimeInterval presented_target_error_min_s;
	CFTimeInterval presented_target_error_max_s;
	CFTimeInterval presented_prediction_error_total_s;
	CFTimeInterval presented_prediction_error_min_s;
	CFTimeInterval presented_prediction_error_max_s;
	uint64_t presented_sample_count;
	uint64_t presented_missed_intervals;

	uint64_t submission_sample_count;
	uint64_t render_wait_total_ns;
	uint64_t render_wait_min_ns;
	uint64_t render_wait_max_ns;
	uint64_t drawable_wait_total_ns;
	uint64_t drawable_wait_min_ns;
	uint64_t drawable_wait_max_ns;
	uint64_t metal_submit_total_ns;
	uint64_t metal_submit_min_ns;
	uint64_t metal_submit_max_ns;
	uint64_t submission_total_ns;
	uint64_t submission_min_ns;
	uint64_t submission_max_ns;
};

static uint64_t
host_time_to_ns(struct comp_window_macos *cwm, uint64_t host_time)
{
	return (uint64_t)(((__uint128_t)host_time * cwm->mach_timebase.numer) / cwm->mach_timebase.denom);
}

static uint64_t
ns_to_host_time(struct comp_window_macos *cwm, uint64_t ns)
{
	return (uint64_t)(((__uint128_t)ns * cwm->mach_timebase.denom) / cwm->mach_timebase.numer);
}

static CFTimeInterval
host_time_to_seconds(struct comp_window_macos *cwm, uint64_t host_time)
{
	return (CFTimeInterval)((double)host_time * (double)cwm->mach_timebase.numer /
	                        (double)cwm->mach_timebase.denom / (double)U_TIME_1S_IN_NS);
}

/*
 * CLOCK_MONOTONIC and mach_absolute_time use different clock domains on
 * macOS. Translate timestamps relative to a pair sampled "now" rather
 * than assuming their epochs are identical.
 */
static int64_t
host_time_to_monotonic_ns(struct comp_window_macos *cwm, uint64_t host_time)
{
	uint64_t host_now = mach_absolute_time();
	int64_t monotonic_now_ns = os_monotonic_get_ns();

	uint64_t host_ns = host_time_to_ns(cwm, host_time);
	uint64_t host_now_ns = host_time_to_ns(cwm, host_now);

	if (host_ns >= host_now_ns) {
		return monotonic_now_ns + (int64_t)(host_ns - host_now_ns);
	}

	return monotonic_now_ns - (int64_t)(host_now_ns - host_ns);
}

static uint64_t
monotonic_ns_to_host_time(struct comp_window_macos *cwm, int64_t monotonic_ns)
{
	uint64_t host_now = mach_absolute_time();
	int64_t monotonic_now_ns = os_monotonic_get_ns();
	int64_t delta_ns = monotonic_ns - monotonic_now_ns;

	if (delta_ns >= 0) {
		return host_now + ns_to_host_time(cwm, (uint64_t)delta_ns);
	}

	uint64_t delta_host_time = ns_to_host_time(cwm, (uint64_t)(-delta_ns));
	return host_now > delta_host_time ? host_now - delta_host_time : 0;
}

static CVReturn
display_link_callback(CVDisplayLinkRef display_link,
                      const CVTimeStamp *in_now,
                      const CVTimeStamp *in_output_time,
                      CVOptionFlags flags_in,
                      CVOptionFlags *flags_out,
                      void *context)
{
	(void)display_link;
	(void)in_now;
	(void)flags_in;
	(void)flags_out;
	struct comp_window_macos *cwm = context;
	if ((in_output_time->flags & kCVTimeStampHostTimeValid) != 0) {
		int64_t output_ns = host_time_to_monotonic_ns(cwm, in_output_time->hostTime);
		if (output_ns > 0) {
			atomic_store_explicit(&cwm->latest_vblank_ns, (uint64_t)output_ns, memory_order_release);
		}
	}
	return kCVReturnSuccess;
}

static inline struct vk_bundle *
get_vk(struct comp_window_macos *cwm)
{
	return &cwm->base.base.c->base.vk;
}

static CGDirectDisplayID
get_display_id(NSScreen *screen)
{
	NSNumber *number = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
	return number != nil ? (CGDirectDisplayID)[number unsignedIntValue] : kCGNullDirectDisplay;
}

static NSScreen *
find_psvr2_screen(struct comp_compositor *c)
{
	NSScreen *width_fallback = nil;
	for (NSScreen *screen in [NSScreen screens]) {
		CGDirectDisplayID display_id = get_display_id(screen);
		size_t width = display_id != kCGNullDirectDisplay ? CGDisplayPixelsWide(display_id) : 0;
		size_t height = display_id != kCGNullDirectDisplay ? CGDisplayPixelsHigh(display_id) : 0;
		NSString *name = [screen localizedName];
		if (c != NULL) {
			COMP_INFO(c, "macOS display: '%s' %zux%zu", [name UTF8String], width, height);
		}
		if ([name caseInsensitiveCompare:@"PS VR2"] == NSOrderedSame) {
			return screen;
		}
		if (width_fallback == nil && width == 4000) {
			width_fallback = screen;
		}
	}
	return width_fallback;
}

static bool
comp_window_macos_init(struct comp_target *ct)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	@autoreleasepool {
		NSScreen *screen = find_psvr2_screen(ct->c);
		if (screen == nil) {
			COMP_ERROR(ct->c, "Could not find a display named 'PS VR2' or a 4000-pixel-wide fallback");
			return false;
		}

		[NSApplication sharedApplication];
		[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
		[NSApp finishLaunching];

		CGDirectDisplayID display_id = get_display_id(screen);
		if (display_id == kCGNullDirectDisplay) {
			COMP_ERROR(ct->c, "Could not get the CoreGraphics display ID for '%s'", [[screen localizedName] UTF8String]);
			return false;
		}
		size_t pixel_width = CGDisplayPixelsWide(display_id);
		size_t pixel_height = CGDisplayPixelsHigh(display_id);
		if (pixel_width == 0 || pixel_height == 0) {
			COMP_ERROR(ct->c, "Selected macOS display has an invalid pixel size");
			return false;
		}

		NSWindow *window = [[NSWindow alloc] initWithContentRect:[screen frame]
		                                                styleMask:NSWindowStyleMaskBorderless
		                                                  backing:NSBackingStoreBuffered
		                                                    defer:NO
		                                                   screen:screen];
		if (window == nil) {
			COMP_ERROR(ct->c, "Failed to create the macOS PS VR2 window");
			return false;
		}

		id<MTLDevice> metal_device = MTLCreateSystemDefaultDevice();
		if (metal_device == nil) {
			[window release];
			COMP_ERROR(ct->c, "Failed to create the default Metal device");
			return false;
		}
		MTKView *metal_view = [[MTKView alloc] initWithFrame:[screen frame] device:metal_device];
		[metal_device release];
		if (metal_view == nil) {
			[window release];
			COMP_ERROR(ct->c, "Failed to create the PS VR2 MTKView");
			return false;
		}
		[metal_view setPaused:YES];
		[metal_view setEnableSetNeedsDisplay:NO];
		[metal_view setColorPixelFormat:MTLPixelFormatBGRA8Unorm];
		[metal_view setFramebufferOnly:NO];
		[window setContentView:metal_view];

		CAMetalLayer *metal_layer = [(CAMetalLayer *)[metal_view layer] retain];
		[metal_layer setContentsScale:[screen backingScaleFactor]];
		[metal_layer setDrawableSize:CGSizeMake(pixel_width, pixel_height)];
		[metal_layer setOpaque:YES];
		[metal_layer setDisplaySyncEnabled:YES];
		id<MTLCommandQueue> present_queue = [[metal_layer device] newCommandQueue];
		[metal_view release];
		if (present_queue == nil) {
			[metal_layer release];
			[window release];
			COMP_ERROR(ct->c, "Failed to create the macOS Metal presentation queue");
			return false;
		}

		[window setBackgroundColor:[NSColor blackColor]];
		[window setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
		                              NSWindowCollectionBehaviorFullScreenAuxiliary |
		                              NSWindowCollectionBehaviorStationary];
		[window setHasShadow:NO];
		[window setHidesOnDeactivate:NO];
		[window setIgnoresMouseEvents:YES];
		[window setLevel:NSMainMenuWindowLevel + 1];
		[window setFrame:[screen frame] display:YES];
		[window orderFrontRegardless];
		[NSApp activateIgnoringOtherApps:YES];
		[CATransaction flush];

		cwm->screen = [screen retain];
		cwm->window = window;
		cwm->metal_layer = metal_layer;
		cwm->present_queue = present_queue;
		cwm->pixel_width = (uint32_t)pixel_width;
		cwm->pixel_height = (uint32_t)pixel_height;
		cwm->present_completion_group = dispatch_group_create();
		if (cwm->present_completion_group == NULL) {
			[present_queue release];
			[metal_layer release];
			[window release];
			COMP_ERROR(ct->c, "Failed to create macOS presentation completion group");
			return false;
		}
		if (debug_get_bool_option_async_present()) {
			cwm->present_work_queue =
			    dispatch_queue_create("org.monado.macos-present", DISPATCH_QUEUE_SERIAL);
			cwm->present_work_group = dispatch_group_create();
			for (uint32_t i = 0; i < MACOS_TARGET_IMAGE_COUNT; i++) {
				cwm->image_available[i] = dispatch_semaphore_create(1);
			}
			if (cwm->present_work_queue == NULL || cwm->present_work_group == NULL) {
				COMP_ERROR(ct->c, "Failed to create asynchronous macOS presentation worker");
				return false;
			}
			for (uint32_t i = 0; i < MACOS_TARGET_IMAGE_COUNT; i++) {
				if (cwm->image_available[i] == NULL) {
					COMP_ERROR(ct->c, "Failed to create macOS compositor image semaphore");
					return false;
				}
			}
			COMP_INFO(ct->c, "Using asynchronous macOS Metal presentation worker");
		}

		mach_timebase_info(&cwm->mach_timebase);
		CVReturn cvret = CVDisplayLinkCreateWithCGDisplay(display_id, &cwm->display_link);
		if (cvret == kCVReturnSuccess) {
			cvret = CVDisplayLinkSetOutputCallback(cwm->display_link, display_link_callback, cwm);
		}
		if (cvret != kCVReturnSuccess) {
			if (cwm->display_link != NULL) {
				CVDisplayLinkRelease(cwm->display_link);
				cwm->display_link = NULL;
			}
			COMP_WARN(ct->c, "Could not create the PS VR2 display link (%d); using estimated pacing", cvret);
		} else {
			CVTime period = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(cwm->display_link);
			if ((period.flags & kCVTimeIsIndefinite) == 0 && period.timeValue > 0 && period.timeScale > 0) {
				cwm->display_period_ns =
				    (int64_t)(((__int128)period.timeValue * U_TIME_1S_IN_NS) / period.timeScale);
				int divisor = debug_get_num_option_display_rate_divisor();
				if (divisor < 1) {
					divisor = 1;
				}
				ct->c->frame_interval_ns = cwm->display_period_ns * divisor;
				COMP_INFO(ct->c, "PS VR2 display period %.3fms; compositor rate divisor %d (%.2f Hz)",
				          (double)cwm->display_period_ns / 1000000.0, divisor,
				          (double)U_TIME_1S_IN_NS / (double)ct->c->frame_interval_ns);
			}
		}

		VkExtent2D extent = {.width = cwm->pixel_width, .height = cwm->pixel_height};
		comp_target_swapchain_override_extents(&cwm->base, extent);
		COMP_INFO(ct->c, "Selected macOS display '%s' at %ux%u", [[screen localizedName] UTF8String], extent.width,
		          extent.height);
	}
	return true;
}

static bool
comp_window_macos_init_vulkan(struct comp_target *ct, uint32_t preferred_width, uint32_t preferred_height)
{
	(void)ct;
	(void)preferred_width;
	(void)preferred_height;
	return true;
}

static bool
comp_window_macos_check_ready(struct comp_target *ct)
{
	(void)ct;
	return true;
}

static void
comp_window_macos_free_images(struct comp_window_macos *cwm)
{
	struct comp_target *ct = &cwm->base.base;
	struct vk_bundle *vk = get_vk(cwm);
	for (uint32_t i = 0; i < MACOS_TARGET_IMAGE_COUNT; i++) {
		[cwm->metal_images[i] release];
		cwm->metal_images[i] = nil;
		u_graphics_buffer_unref(&cwm->io_surfaces[i]);
	}
	if (ct->images != NULL) {
		for (uint32_t i = 0; i < ct->image_count; i++) {
			if (ct->images[i].storage_view != VK_NULL_HANDLE && ct->images[i].storage_view != ct->images[i].view) {
				vk->vkDestroyImageView(vk->device, ct->images[i].storage_view, NULL);
			}
			if (ct->images[i].view != VK_NULL_HANDLE) {
				vk->vkDestroyImageView(vk->device, ct->images[i].view, NULL);
			}
		}
		free(ct->images);
	}
	vk_ic_destroy(vk, &cwm->vkic);
	ct->images = NULL;
	ct->image_count = 0;
	ct->width = 0;
	ct->height = 0;
	ct->format = VK_FORMAT_UNDEFINED;
	ct->final_layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

static bool
comp_window_macos_ensure_render_complete_semaphore(struct comp_window_macos *cwm)
{
	struct comp_target *ct = &cwm->base.base;
	struct vk_bundle *vk = get_vk(cwm);

	if (ct->semaphores.render_complete != VK_NULL_HANDLE) {
		return ct->semaphores.render_complete_is_timeline;
	}

	if (!vk->features.timeline_semaphore) {
		COMP_WARN(ct->c, "Vulkan timeline semaphores unavailable; macOS presentation will use queue-idle fallback");
		return false;
	}

	VkSemaphoreTypeCreateInfo type_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
	    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	    .initialValue = 0,
	};
	VkSemaphoreCreateInfo info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	    .pNext = &type_info,
	};

	VkResult ret = vk->vkCreateSemaphore(vk->device, &info, NULL, &ct->semaphores.render_complete);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "Could not create macOS render-complete timeline semaphore: %s", vk_result_string(ret));
		ct->semaphores.render_complete = VK_NULL_HANDLE;
		return false;
	}

	ct->semaphores.render_complete_is_timeline = true;
	VK_NAME_SEMAPHORE(vk, ct->semaphores.render_complete, "macOS compositor render complete");
	return true;
}

static void
comp_window_macos_create_images(struct comp_target *ct,
                                const struct comp_target_create_images_info *create_info,
                                struct vk_bundle_queue *present_queue)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	struct vk_bundle *vk = get_vk(cwm);
	bool supports_bgra8 = false;
	assert(present_queue != NULL);
	for (uint32_t i = 0; i < create_info->format_count; i++) {
		if (create_info->formats[i] == VK_FORMAT_B8G8R8A8_UNORM) {
			supports_bgra8 = true;
			break;
		}
	}
	if (!supports_bgra8) {
		COMP_ERROR(ct->c, "The macOS Metal target requires VK_FORMAT_B8G8R8A8_UNORM");
		return;
	}

	comp_window_macos_ensure_render_complete_semaphore(cwm);
	comp_window_macos_free_images(cwm);
	struct xrt_swapchain_create_info info = {
	    .create = 0,
	    .bits = XRT_SWAPCHAIN_USAGE_COLOR | XRT_SWAPCHAIN_USAGE_SAMPLED | XRT_SWAPCHAIN_USAGE_TRANSFER_SRC |
	            XRT_SWAPCHAIN_USAGE_TRANSFER_DST | XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .sample_count = 1,
	    .width = cwm->pixel_width,
	    .height = cwm->pixel_height,
	    .face_count = 1,
	    .array_size = 1,
	    .mip_count = 1,
	};
	VkResult ret = vk_ic_allocate(vk, &info, MACOS_TARGET_IMAGE_COUNT, &cwm->vkic);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "Could not allocate macOS compositor images: %s", vk_result_string(ret));
		return;
	}
	ret = vk_ic_get_handles(vk, &cwm->vkic, MACOS_TARGET_IMAGE_COUNT, cwm->io_surfaces);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "Could not export macOS compositor IOSurfaces: %s", vk_result_string(ret));
		comp_window_macos_free_images(cwm);
		return;
	}

	ct->images = U_TYPED_ARRAY_CALLOC(struct comp_target_image, MACOS_TARGET_IMAGE_COUNT);
	if (ct->images == NULL) {
		COMP_ERROR(ct->c, "Could not allocate macOS compositor image metadata");
		comp_window_macos_free_images(cwm);
		return;
	}
	ct->image_count = MACOS_TARGET_IMAGE_COUNT;
	VkImageSubresourceRange range = {
	    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	    .baseMipLevel = 0,
	    .levelCount = 1,
	    .baseArrayLayer = 0,
	    .layerCount = 1,
	};
	MTLTextureDescriptor *descriptor =
	    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                     width:cwm->pixel_width
	                                                    height:cwm->pixel_height
	                                                 mipmapped:NO];
	[descriptor setUsage:MTLTextureUsageShaderRead];
	for (uint32_t i = 0; i < MACOS_TARGET_IMAGE_COUNT; i++) {
		ct->images[i].handle = cwm->vkic.images[i].handle;
		ret = vk_create_view(vk, ct->images[i].handle, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, range,
		                     &ct->images[i].view);
		if (ret != VK_SUCCESS) {
			COMP_ERROR(ct->c, "Could not create macOS compositor image view: %s", vk_result_string(ret));
			comp_window_macos_free_images(cwm);
			return;
		}
		ct->images[i].storage_view = ct->images[i].view;
		cwm->metal_images[i] = [[cwm->metal_layer device] newTextureWithDescriptor:descriptor
		                                                                 iosurface:cwm->io_surfaces[i]
		                                                                     plane:0];
		if (cwm->metal_images[i] == nil) {
			COMP_ERROR(ct->c, "Could not create a Metal texture for macOS compositor image %u", i);
			comp_window_macos_free_images(cwm);
			return;
		}
	}

	ct->width = cwm->pixel_width;
	ct->height = cwm->pixel_height;
	ct->format = VK_FORMAT_B8G8R8A8_UNORM;
	ct->final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	ct->surface_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	cwm->next_image = 0;
	if (cwm->base.upc == NULL) {
		u_pc_fake_create(ct->c->frame_interval_ns, os_monotonic_get_ns(), &cwm->base.upc);
	}
	if (cwm->display_link != NULL && !CVDisplayLinkIsRunning(cwm->display_link)) {
		CVReturn cvret = CVDisplayLinkStart(cwm->display_link);
		if (cvret != kCVReturnSuccess) {
			COMP_WARN(ct->c, "Could not start the PS VR2 display link (%d); using estimated pacing", cvret);
		}
	}
	COMP_INFO(ct->c, "Created %u IOSurface-backed macOS compositor images at %ux%u", ct->image_count, ct->width,
	          ct->height);
}

static bool
comp_window_macos_has_images(struct comp_target *ct)
{
	return ct->images != NULL && ct->image_count != 0;
}

static VkResult
comp_window_macos_acquire(struct comp_target *ct, uint32_t *out_index)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	if (!comp_window_macos_has_images(ct)) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	uint32_t index = cwm->next_image;
	if (debug_get_bool_option_async_present()) {
		dispatch_semaphore_wait(cwm->image_available[index], DISPATCH_TIME_FOREVER);
	}
	*out_index = index;
	cwm->next_image = (cwm->next_image + 1) % ct->image_count;
	return VK_SUCCESS;
}

static VkResult
comp_window_macos_wait_for_render_complete(struct comp_window_macos *cwm,
                                           struct vk_bundle_queue *present_queue,
                                           uint64_t timeline_semaphore_value)
{
	struct comp_target *ct = &cwm->base.base;
	struct vk_bundle *vk = get_vk(cwm);

	if (ct->semaphores.render_complete != VK_NULL_HANDLE && ct->semaphores.render_complete_is_timeline) {
		VkSemaphoreWaitInfo wait_info = {
		    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		    .semaphoreCount = 1,
		    .pSemaphores = &ct->semaphores.render_complete,
		    .pValues = &timeline_semaphore_value,
		};
		VkResult ret = vk->vkWaitSemaphores(vk->device, &wait_info, UINT64_MAX);
		if (ret != VK_SUCCESS) {
			COMP_ERROR(ct->c, "Waiting for macOS render-complete semaphore failed: %s", vk_result_string(ret));
		}
		return ret;
	}

	vk_queue_lock(present_queue);
	VkResult ret = vk->vkQueueWaitIdle(present_queue->queue);
	vk_queue_unlock(present_queue);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "vkQueueWaitIdle before Metal presentation: %s", vk_result_string(ret));
	}
	return ret;
}

static void
comp_window_macos_record_presented_frame(struct comp_window_macos *cwm,
                                         id<MTLDrawable> drawable,
                                         CFTimeInterval desired_present_time_s,
                                         CFTimeInterval predicted_display_time_s)
{
	bool log_timing = debug_get_bool_option_present_timing();
	bool update_feedback = debug_get_bool_option_present_feedback();
	if (!log_timing && !update_feedback) {
		return;
	}

	CFTimeInterval presented_time_s = [drawable presentedTime];
	if (presented_time_s <= 0.0) {
		return;
	}

	struct comp_target *ct = &cwm->base.base;
	@synchronized(cwm->metal_layer) {
		if (cwm->last_presented_time_s > 0.0 && presented_time_s > cwm->last_presented_time_s) {
			CFTimeInterval interval_s = presented_time_s - cwm->last_presented_time_s;
			CFTimeInterval target_error_s = presented_time_s - desired_present_time_s;
			CFTimeInterval prediction_error_s = presented_time_s - predicted_display_time_s;
			if (update_feedback && target_error_s > -0.01 && target_error_s < 0.1) {
				int64_t target_error_ns = (int64_t)(target_error_s * (CFTimeInterval)U_TIME_1S_IN_NS);
				if (target_error_ns < U_TIME_1MS_IN_NS) {
					target_error_ns = U_TIME_1MS_IN_NS;
				}
				atomic_store_explicit(&cwm->measured_present_offset_ns, target_error_ns,
				                      memory_order_release);
			}
			if (!log_timing) {
				cwm->last_presented_time_s = presented_time_s;
				return;
			}

			cwm->presented_interval_total_s += interval_s;
			cwm->presented_target_error_total_s += target_error_s;
			cwm->presented_prediction_error_total_s += prediction_error_s;
			cwm->presented_sample_count++;

			if (cwm->presented_interval_min_s == 0.0 || interval_s < cwm->presented_interval_min_s) {
				cwm->presented_interval_min_s = interval_s;
			}
			if (interval_s > cwm->presented_interval_max_s) {
				cwm->presented_interval_max_s = interval_s;
			}
			if (cwm->presented_sample_count == 1 || target_error_s < cwm->presented_target_error_min_s) {
				cwm->presented_target_error_min_s = target_error_s;
			}
			if (cwm->presented_sample_count == 1 || target_error_s > cwm->presented_target_error_max_s) {
				cwm->presented_target_error_max_s = target_error_s;
			}
			if (cwm->presented_sample_count == 1 || prediction_error_s < cwm->presented_prediction_error_min_s) {
				cwm->presented_prediction_error_min_s = prediction_error_s;
			}
			if (cwm->presented_sample_count == 1 || prediction_error_s > cwm->presented_prediction_error_max_s) {
				cwm->presented_prediction_error_max_s = prediction_error_s;
			}

			if (cwm->display_period_ns > 0) {
				CFTimeInterval period_s = (CFTimeInterval)cwm->display_period_ns / (CFTimeInterval)U_TIME_1S_IN_NS;
				if (interval_s > period_s * 1.5) {
					cwm->presented_missed_intervals++;
				}
			}

			if (cwm->presented_sample_count == 240) {
				CFTimeInterval cadence_avg_ms = cwm->presented_interval_total_s / 240.0 * 1000.0;
				CFTimeInterval target_error_avg_ms = cwm->presented_target_error_total_s / 240.0 * 1000.0;
				CFTimeInterval prediction_error_avg_ms =
				    cwm->presented_prediction_error_total_s / 240.0 * 1000.0;
				if (update_feedback) {
					int64_t applied_offset_ns = atomic_load_explicit(
					    &cwm->applied_present_offset_ns, memory_order_acquire);
					COMP_INFO(ct->c,
					          "macOS drawable presentation: cadence avg %.3fms min %.3fms max %.3fms, missed %llu/240; target error avg %.3fms min %.3fms max %.3fms; feedback %.3fms; pose-time error avg %.3fms min %.3fms max %.3fms",
					          cadence_avg_ms, cwm->presented_interval_min_s * 1000.0,
					          cwm->presented_interval_max_s * 1000.0,
					          (unsigned long long)cwm->presented_missed_intervals,
					          target_error_avg_ms, cwm->presented_target_error_min_s * 1000.0,
					          cwm->presented_target_error_max_s * 1000.0,
					          (double)applied_offset_ns / (double)U_TIME_1MS_IN_NS,
					          prediction_error_avg_ms, cwm->presented_prediction_error_min_s * 1000.0,
					          cwm->presented_prediction_error_max_s * 1000.0);
				} else {
					COMP_INFO(ct->c,
					          "macOS drawable presentation: cadence avg %.3fms min %.3fms max %.3fms, missed %llu/240; target error avg %.3fms min %.3fms max %.3fms",
					          cadence_avg_ms, cwm->presented_interval_min_s * 1000.0,
					          cwm->presented_interval_max_s * 1000.0,
					          (unsigned long long)cwm->presented_missed_intervals,
					          target_error_avg_ms, cwm->presented_target_error_min_s * 1000.0,
					          cwm->presented_target_error_max_s * 1000.0);
				}

				cwm->presented_interval_total_s = 0.0;
				cwm->presented_interval_min_s = 0.0;
				cwm->presented_interval_max_s = 0.0;
				cwm->presented_target_error_total_s = 0.0;
				cwm->presented_target_error_min_s = 0.0;
				cwm->presented_target_error_max_s = 0.0;
				cwm->presented_prediction_error_total_s = 0.0;
				cwm->presented_prediction_error_min_s = 0.0;
				cwm->presented_prediction_error_max_s = 0.0;
				cwm->presented_sample_count = 0;
				cwm->presented_missed_intervals = 0;
			}
		}
		cwm->last_presented_time_s = presented_time_s;
	}
}

static void
comp_window_macos_record_submission(struct comp_window_macos *cwm,
                                    uint64_t render_wait_ns,
                                    uint64_t drawable_wait_ns,
                                    uint64_t metal_submit_ns,
                                    uint64_t total_ns)
{
	if (!debug_get_bool_option_present_timing()) {
		return;
	}

	if (cwm->submission_sample_count == 0) {
		cwm->render_wait_min_ns = render_wait_ns;
		cwm->drawable_wait_min_ns = drawable_wait_ns;
		cwm->metal_submit_min_ns = metal_submit_ns;
		cwm->submission_min_ns = total_ns;
	} else {
		cwm->render_wait_min_ns =
		    render_wait_ns < cwm->render_wait_min_ns ? render_wait_ns : cwm->render_wait_min_ns;
		cwm->drawable_wait_min_ns =
		    drawable_wait_ns < cwm->drawable_wait_min_ns ? drawable_wait_ns : cwm->drawable_wait_min_ns;
		cwm->metal_submit_min_ns =
		    metal_submit_ns < cwm->metal_submit_min_ns ? metal_submit_ns : cwm->metal_submit_min_ns;
		cwm->submission_min_ns = total_ns < cwm->submission_min_ns ? total_ns : cwm->submission_min_ns;
	}
	cwm->render_wait_max_ns =
	    render_wait_ns > cwm->render_wait_max_ns ? render_wait_ns : cwm->render_wait_max_ns;
	cwm->drawable_wait_max_ns =
	    drawable_wait_ns > cwm->drawable_wait_max_ns ? drawable_wait_ns : cwm->drawable_wait_max_ns;
	cwm->metal_submit_max_ns =
	    metal_submit_ns > cwm->metal_submit_max_ns ? metal_submit_ns : cwm->metal_submit_max_ns;
	cwm->submission_max_ns = total_ns > cwm->submission_max_ns ? total_ns : cwm->submission_max_ns;

	cwm->submission_sample_count++;
	cwm->render_wait_total_ns += render_wait_ns;
	cwm->drawable_wait_total_ns += drawable_wait_ns;
	cwm->metal_submit_total_ns += metal_submit_ns;
	cwm->submission_total_ns += total_ns;

	if (cwm->submission_sample_count < 240) {
		return;
	}

	struct comp_target *ct = &cwm->base.base;
	COMP_INFO(ct->c,
	          "macOS present submission: render wait avg %.3fms min %.3fms max %.3fms; drawable wait avg "
	          "%.3fms min %.3fms max %.3fms; Metal CPU avg %.3fms min %.3fms max %.3fms; total avg "
	          "%.3fms min %.3fms max %.3fms",
	          time_ns_to_ms_f(cwm->render_wait_total_ns) / 240.0, time_ns_to_ms_f(cwm->render_wait_min_ns),
	          time_ns_to_ms_f(cwm->render_wait_max_ns), time_ns_to_ms_f(cwm->drawable_wait_total_ns) / 240.0,
	          time_ns_to_ms_f(cwm->drawable_wait_min_ns), time_ns_to_ms_f(cwm->drawable_wait_max_ns),
	          time_ns_to_ms_f(cwm->metal_submit_total_ns) / 240.0, time_ns_to_ms_f(cwm->metal_submit_min_ns),
	          time_ns_to_ms_f(cwm->metal_submit_max_ns), time_ns_to_ms_f(cwm->submission_total_ns) / 240.0,
	          time_ns_to_ms_f(cwm->submission_min_ns), time_ns_to_ms_f(cwm->submission_max_ns));

	cwm->submission_sample_count = 0;
	cwm->render_wait_total_ns = 0;
	cwm->render_wait_min_ns = 0;
	cwm->render_wait_max_ns = 0;
	cwm->drawable_wait_total_ns = 0;
	cwm->drawable_wait_min_ns = 0;
	cwm->drawable_wait_max_ns = 0;
	cwm->metal_submit_total_ns = 0;
	cwm->metal_submit_min_ns = 0;
	cwm->metal_submit_max_ns = 0;
	cwm->submission_total_ns = 0;
	cwm->submission_min_ns = 0;
	cwm->submission_max_ns = 0;
}

static VkResult
comp_window_macos_submit_present(struct comp_window_macos *cwm,
                                 struct vk_bundle_queue *present_queue,
                                 uint32_t index,
                                 uint64_t timeline_semaphore_value,
                                 int64_t desired_present_time_ns,
                                 bool release_image_on_completion)
{
	struct comp_target *ct = &cwm->base.base;

	uint64_t before_submission_ns = os_monotonic_get_ns();
	uint64_t after_drawable_wait_ns = 0;
	uint64_t after_render_wait_ns = 0;
	uint64_t after_metal_submit_ns = 0;
	@autoreleasepool {
		id<CAMetalDrawable> drawable = [cwm->metal_layer nextDrawable];
		after_drawable_wait_ns = os_monotonic_get_ns();
		if (drawable == nil) {
			COMP_ERROR(ct->c, "Could not acquire a CAMetalDrawable");
			if (release_image_on_completion) {
				dispatch_semaphore_signal(cwm->image_available[index]);
			}
			return VK_ERROR_OUT_OF_DATE_KHR;
		}

		/*
		 * Vulkan rendering is already in flight. Acquire the independent Metal
		 * drawable first so its availability wait overlaps GPU execution, then
		 * wait for the IOSurface render to finish before encoding the blit.
		 */
		VkResult ret =
		    comp_window_macos_wait_for_render_complete(cwm, present_queue, timeline_semaphore_value);
		after_render_wait_ns = os_monotonic_get_ns();
		if (ret != VK_SUCCESS) {
			if (release_image_on_completion) {
				dispatch_semaphore_signal(cwm->image_available[index]);
			}
			return ret;
		}

		id<MTLCommandBuffer> command_buffer = [cwm->present_queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
		MTLSize size = MTLSizeMake(ct->width, ct->height, 1);
		[blit copyFromTexture:cwm->metal_images[index]
		             sourceSlice:0
		             sourceLevel:0
		            sourceOrigin:MTLOriginMake(0, 0, 0)
		              sourceSize:size
		               toTexture:[drawable texture]
		        destinationSlice:0
		        destinationLevel:0
		       destinationOrigin:MTLOriginMake(0, 0, 0)];
		[blit endEncoding];

		int64_t scheduled_present_time_ns = desired_present_time_ns;
		int advance_periods = debug_get_num_option_present_advance_periods();
		if (advance_periods < 0) {
			advance_periods = 0;
		} else if (advance_periods > 2) {
			advance_periods = 2;
		}
		if (advance_periods > 0 && cwm->display_period_ns > 0) {
			int64_t advance_ns = (int64_t)advance_periods * cwm->display_period_ns;
			if (scheduled_present_time_ns > advance_ns) {
				scheduled_present_time_ns -= advance_ns;
			}
		}

		uint64_t desired_host_time = monotonic_ns_to_host_time(cwm, desired_present_time_ns);
		CFTimeInterval desired_host_time_seconds = host_time_to_seconds(cwm, desired_host_time);
		uint64_t scheduled_host_time = monotonic_ns_to_host_time(cwm, scheduled_present_time_ns);
		CFTimeInterval scheduled_host_time_seconds = host_time_to_seconds(cwm, scheduled_host_time);
		if (debug_get_bool_option_present_timing() || debug_get_bool_option_present_feedback()) {
			int64_t applied_offset_ns =
			    atomic_load_explicit(&cwm->applied_present_offset_ns, memory_order_acquire);
			CFTimeInterval predicted_display_time_seconds =
			    desired_host_time_seconds + (CFTimeInterval)applied_offset_ns / (CFTimeInterval)U_TIME_1S_IN_NS;
			[drawable addPresentedHandler:^(id<MTLDrawable> presented_drawable) {
				comp_window_macos_record_presented_frame(
				    cwm, presented_drawable, desired_host_time_seconds, predicted_display_time_seconds);
			}];
		}
		if (debug_get_bool_option_present_immediate()) {
			[command_buffer presentDrawable:drawable];
		} else {
			[command_buffer presentDrawable:drawable atTime:scheduled_host_time_seconds];
		}

		dispatch_group_enter(cwm->present_completion_group);
		[command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed_buffer) {
			@autoreleasepool {
				if ([completed_buffer status] == MTLCommandBufferStatusError) {
					COMP_ERROR(ct->c, "Metal presentation failed: %s",
					           [[[completed_buffer error] localizedDescription] UTF8String]);
					atomic_store_explicit(&cwm->metal_present_failed, true, memory_order_release);
				}
			}
			if (release_image_on_completion) {
				dispatch_semaphore_signal(cwm->image_available[index]);
			}
			dispatch_group_leave(cwm->present_completion_group);
		}];
		[command_buffer commit];
		after_metal_submit_ns = os_monotonic_get_ns();
	}

	comp_window_macos_record_submission(cwm, after_render_wait_ns - after_drawable_wait_ns,
	                                      after_drawable_wait_ns - before_submission_ns,
	                                      after_metal_submit_ns - after_render_wait_ns,
	                                      after_metal_submit_ns - before_submission_ns);

	return VK_SUCCESS;
}

static VkResult
comp_window_macos_present(struct comp_target *ct,
                          struct vk_bundle_queue *present_queue,
                          uint32_t index,
                          uint64_t timeline_semaphore_value,
                          int64_t desired_present_time_ns,
                          int64_t present_slop_ns)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	(void)present_slop_ns;
	assert(present_queue != NULL);
	if (index >= ct->image_count || cwm->metal_images[index] == nil) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}
	if (atomic_exchange_explicit(&cwm->metal_present_failed, false, memory_order_acq_rel)) {
		if (debug_get_bool_option_async_present()) {
			dispatch_semaphore_signal(cwm->image_available[index]);
		}
		return VK_ERROR_DEVICE_LOST;
	}

	if (!debug_get_bool_option_async_present()) {
		return comp_window_macos_submit_present(cwm, present_queue, index, timeline_semaphore_value,
		                                        desired_present_time_ns, false);
	}

	dispatch_group_async(cwm->present_work_group, cwm->present_work_queue, ^{
		VkResult ret = comp_window_macos_submit_present(cwm, present_queue, index, timeline_semaphore_value,
		                                                   desired_present_time_ns, true);
		if (ret != VK_SUCCESS) {
			atomic_store_explicit(&cwm->metal_present_failed, true, memory_order_release);
		}
	});

	return VK_SUCCESS;
}

static VkResult
comp_window_macos_wait_for_present(struct comp_target *ct, time_duration_ns timeout_ns)
{
	(void)ct;
	(void)timeout_ns;
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

static VkResult
comp_window_macos_update_timings(struct comp_target *ct)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	if (debug_get_bool_option_present_feedback() && cwm->base.upc != NULL) {
		int64_t measured_offset_ns =
		    atomic_exchange_explicit(&cwm->measured_present_offset_ns, 0, memory_order_acquire);
		if (measured_offset_ns > 0) {
			if (measured_offset_ns < U_TIME_1MS_IN_NS) {
				measured_offset_ns = U_TIME_1MS_IN_NS;
			} else if (measured_offset_ns > 40 * U_TIME_1MS_IN_NS) {
				measured_offset_ns = 40 * U_TIME_1MS_IN_NS;
			}
			if (cwm->filtered_present_offset_ns == 0) {
				cwm->filtered_present_offset_ns = measured_offset_ns;
			} else {
				cwm->filtered_present_offset_ns +=
				    (measured_offset_ns - cwm->filtered_present_offset_ns) / 32;
			}
			u_pc_update_present_offset(cwm->base.upc, 0, cwm->filtered_present_offset_ns);
			atomic_store_explicit(&cwm->applied_present_offset_ns, cwm->filtered_present_offset_ns,
			                      memory_order_release);
		}
	}
	uint64_t vblank_ns = atomic_exchange_explicit(&cwm->latest_vblank_ns, 0, memory_order_acquire);
	if (vblank_ns == 0 || vblank_ns == cwm->last_vblank_ns || cwm->base.upc == NULL) {
		return VK_SUCCESS;
	}

	/*
	 * The fake pacer advances from its last present time by one frame period.
	 * Continuously re-anchoring it to the latest CVDisplayLink sample can lock
	 * it to a lower cadence if the compositor is already missing refreshes, so
	 * only use the first real vblank to establish phase.
	 */
	if (!cwm->pacer_vblank_synced) {
		u_pc_update_vblank_from_display_control(cwm->base.upc, (int64_t)vblank_ns);
		cwm->pacer_vblank_synced = true;
		COMP_DEBUG(ct->c, "macOS fake pacer phase synced to display vblank");
	}

	cwm->last_vblank_ns = vblank_ns;
	return VK_SUCCESS;
}

static xrt_result_t
comp_window_macos_get_refresh_rates(struct comp_target *ct, uint32_t *out_count, float *out_rates)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	int64_t period_ns = cwm->display_period_ns > 0 ? cwm->display_period_ns : ct->c->frame_interval_ns;
	*out_count = 1;
	out_rates[0] = (float)((double)U_TIME_1S_IN_NS / (double)period_ns);
	return XRT_SUCCESS;
}

static xrt_result_t
comp_window_macos_get_current_refresh_rate(struct comp_target *ct, float *out_rate)
{
	uint32_t count = 0;
	return comp_window_macos_get_refresh_rates(ct, &count, out_rate);
}

static VkResult
comp_window_macos_queue_supports_present(struct comp_target *ct,
                                         struct vk_bundle_queue *queue,
                                         VkBool32 *out_supported)
{
	(void)ct;
	(void)queue;
	*out_supported = VK_TRUE;
	return VK_SUCCESS;
}

static void
comp_window_macos_flush(struct comp_target *ct)
{
	(void)ct;
	@autoreleasepool {
		[CATransaction flush];
	}
}

static void
comp_window_macos_set_title(struct comp_target *ct, const char *title)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	@autoreleasepool {
		[cwm->window setTitle:[NSString stringWithUTF8String:title]];
	}
}

static void
comp_window_macos_destroy(struct comp_target *ct)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	struct vk_bundle *vk = get_vk(cwm);
	if (cwm->display_link != NULL) {
		CVDisplayLinkStop(cwm->display_link);
		CVDisplayLinkRelease(cwm->display_link);
		cwm->display_link = NULL;
	}
	if (cwm->present_work_group != NULL) {
		dispatch_group_wait(cwm->present_work_group, DISPATCH_TIME_FOREVER);
	}
	if (cwm->present_completion_group != NULL) {
		dispatch_group_wait(cwm->present_completion_group, DISPATCH_TIME_FOREVER);
	}
	comp_window_macos_free_images(cwm);
	if (ct->semaphores.render_complete != VK_NULL_HANDLE) {
		vk->vkDestroySemaphore(vk->device, ct->semaphores.render_complete, NULL);
		ct->semaphores.render_complete = VK_NULL_HANDLE;
		ct->semaphores.render_complete_is_timeline = false;
	}
	u_pc_destroy(&cwm->base.upc);
	@autoreleasepool {
		[cwm->window orderOut:nil];
		[cwm->window close];
		[cwm->window release];
		[cwm->present_queue release];
		[cwm->metal_layer release];
		[cwm->screen release];
	}
#if !OS_OBJECT_USE_OBJC
	if (cwm->present_completion_group != NULL) {
		dispatch_release(cwm->present_completion_group);
	}
	if (cwm->present_work_group != NULL) {
		dispatch_release(cwm->present_work_group);
	}
	if (cwm->present_work_queue != NULL) {
		dispatch_release(cwm->present_work_queue);
	}
	for (uint32_t i = 0; i < MACOS_TARGET_IMAGE_COUNT; i++) {
		if (cwm->image_available[i] != NULL) {
			dispatch_release(cwm->image_available[i]);
		}
	}
#endif
	free(cwm);
}

struct comp_target *
comp_window_macos_create(struct comp_compositor *c)
{
	struct comp_window_macos *cwm = U_TYPED_CALLOC(struct comp_window_macos);
	comp_target_swapchain_init_and_set_fnptrs(&cwm->base, COMP_TARGET_FORCE_FAKE_DISPLAY_TIMING);
	for (uint32_t i = 0; i < MACOS_TARGET_IMAGE_COUNT; i++) {
		cwm->io_surfaces[i] = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
	}
	cwm->base.base.name = "macOS Metal";
	cwm->base.display = VK_NULL_HANDLE;
	cwm->base.base.destroy = comp_window_macos_destroy;
	cwm->base.base.flush = comp_window_macos_flush;
	cwm->base.base.init_pre_vulkan = comp_window_macos_init;
	cwm->base.base.init_post_vulkan = comp_window_macos_init_vulkan;
	cwm->base.base.check_ready = comp_window_macos_check_ready;
	cwm->base.base.create_images = comp_window_macos_create_images;
	cwm->base.base.has_images = comp_window_macos_has_images;
	cwm->base.base.acquire = comp_window_macos_acquire;
	cwm->base.base.present = comp_window_macos_present;
	cwm->base.base.wait_for_present = comp_window_macos_wait_for_present;
	cwm->base.base.update_timings = comp_window_macos_update_timings;
	cwm->base.base.queue_supports_present = comp_window_macos_queue_supports_present;
	cwm->base.base.set_title = comp_window_macos_set_title;
	cwm->base.base.get_refresh_rates = comp_window_macos_get_refresh_rates;
	cwm->base.base.get_current_refresh_rate = comp_window_macos_get_current_refresh_rate;
	cwm->base.base.wait_for_present_supported = false;
	cwm->base.base.c = c;
	return &cwm->base.base;
}

static bool
detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	(void)ctf;
	(void)c;
	@autoreleasepool {
		return find_psvr2_screen(NULL) != nil;
	}
}

static bool
create_target(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	(void)ctf;
	struct comp_target *ct = comp_window_macos_create(c);
	if (ct == NULL) {
		return false;
	}
	*out_ct = ct;
	return true;
}

const struct comp_target_factory comp_target_factory_macos = {
	.name = "macOS Metal Window",
	.identifier = "macos",
	.requires_vulkan_for_create = false,
	.is_deferred = false,
	.required_instance_version = 0,
	.required_instance_extensions = NULL,
	.required_instance_extension_count = 0,
	.optional_device_extensions = NULL,
	.optional_device_extension_count = 0,
	.detect = detect,
	.create_target = create_target,
};

#pragma clang diagnostic pop
