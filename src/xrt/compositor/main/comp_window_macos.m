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

#include "main/comp_window.h"
#include "util/u_debug.h"
#include "util/u_handles.h"
#include "util/u_misc.h"
#include "util/u_pacing.h"
#include "vk/vk_image_allocator.h"

#include <dispatch/dispatch.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#define MACOS_TARGET_IMAGE_COUNT 3

DEBUG_GET_ONCE_NUM_OPTION(display_rate_divisor, "XRT_MACOS_DISPLAY_RATE_DIVISOR", 1)
DEBUG_GET_ONCE_BOOL_OPTION(macos_psvr2_timing_trace, "PSVR2_TIMING_TRACE", false)
DEBUG_GET_ONCE_BOOL_OPTION(macos_cvdisplaylink_pacing, "XRT_MACOS_CVDISPLAYLINK_PACING", true)
DEBUG_GET_ONCE_NUM_OPTION(macos_present_min_lead_us, "XRT_MACOS_PRESENT_MIN_LEAD_US", 2000)
DEBUG_GET_ONCE_NUM_OPTION(macos_present_prelatch_us, "XRT_MACOS_PRESENT_PRELATCH_US", 2000)
DEBUG_GET_ONCE_NUM_OPTION(macos_max_drawables, "XRT_MACOS_MAX_DRAWABLES", 3)

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
	atomic_uint_fast64_t latest_vblank_ns;
	atomic_uint_fast64_t latest_displaylink_now_host_ns;
	atomic_uint_fast64_t latest_displaylink_output_host_ns;
	atomic_uint_fast64_t latest_displaylink_now_ns;
	atomic_uint_fast64_t latest_displaylink_output_ns;
	atomic_uint_fast64_t latest_displaylink_callback_ns;
	atomic_int_fast64_t host_to_monotonic_offset_ns;
	atomic_int_fast64_t latest_observed_present_offset_ns;
	atomic_uint_fast64_t present_offset_sample_serial;
	uint64_t consumed_present_offset_sample_serial;
	int64_t calibrated_present_offset_ns;
	uint32_t present_offset_sample_count;
	uint64_t last_vblank_ns;
	uint64_t trace_frame_id;
	uint64_t cadence_sample_count;
	uint64_t cadence_total_ns;
	uint64_t cadence_min_ns;
	uint64_t cadence_max_ns;
	uint64_t last_present_ns;
	uint64_t present_sample_count;
	uint64_t present_total_ns;
	uint64_t present_min_ns;
	uint64_t present_max_ns;
	uint64_t present_missed_intervals;
	uint64_t present_vk_wait_total_ns;
	uint64_t present_drawable_wait_total_ns;
	uint64_t present_metal_wait_total_ns;
	int64_t display_period_ns;
	uint32_t pixel_width;
	uint32_t pixel_height;
	uint32_t next_image;
	bool logged_layer_state;
	FILE *trace_present;
	FILE *trace_presented;
	FILE *trace_vblank;
	dispatch_group_t trace_present_group;
	uint64_t trace_present_rows;
	uint64_t trace_vblank_rows;
};

static FILE *
macos_timing_trace_open_file(const char *suffix, const char *header)
{
	const char *dir = getenv("PSVR2_TIMING_TRACE_DIR");
	if (dir == NULL || dir[0] == '\0') {
		dir = "/tmp";
	}
	char path[1024];
	size_t dir_len = strlen(dir);
	const char *separator = dir_len > 0 && dir[dir_len - 1] == '/' ? "" : "/";
	snprintf(path, sizeof(path), "%s%smonado_psvr2_%d_%s.csv", dir, separator, (int)getpid(), suffix);
	FILE *file = fopen(path, "w");
	if (file == NULL) {
		return NULL;
	}
	setvbuf(file, NULL, _IOFBF, 64 * 1024);
	fputs(header, file);
	fputc('\n', file);
	fflush(file);
	return file;
}

static void
macos_timing_trace_open(struct comp_window_macos *cwm)
{
	if (!debug_get_bool_option_macos_psvr2_timing_trace()) {
		return;
	}
	cwm->trace_present = macos_timing_trace_open_file(
	    "present",
	    "frame_id,host_call_ns,desired_present_ns,desired_minus_call_ns,target_output_ns,target_minus_desired_ns,"
	    "metal_request_ns,metal_request_minus_target_ns,metal_request_minus_call_ns,scheduled_present_host_s,"
	    "present_slop_ns,image_index,"
	    "timeline_value,wait_mode,after_vk_wait_ns,"
	    "after_drawable_ns,before_present_call_ns,after_present_call_ns,after_commit_ns,after_metal_wait_ns,"
	    "latest_displaylink_output_ns,gpu_start_time_s,gpu_end_time_s");
	cwm->trace_presented = macos_timing_trace_open_file(
	    "presented",
	    "frame_id,presented_handler_ns,desired_present_ns,target_output_ns,presented_time_host_s,"
	    "presented_monotonic_ns,presented_minus_desired_ns,presented_minus_target_ns,observed_present_offset_ns");
	cwm->trace_vblank = macos_timing_trace_open_file(
	    "vblank",
	    "host_consumed_ns,displaylink_callback_ns,displaylink_now_host_ns,displaylink_output_host_ns,"
	    "host_to_monotonic_offset_ns,displaylink_now_ns,displaylink_output_ns,derived_last_vblank_ns,"
	    "output_minus_now_ns,callback_minus_output_ns,callback_minus_vblank_ns,interval_from_previous_vblank_ns,"
	    "display_period_ns");
	cwm->trace_present_group = dispatch_group_create();
}

static void
macos_timing_trace_close(struct comp_window_macos *cwm)
{
	if (cwm->trace_present_group != NULL) {
		long wait_result =
		    dispatch_group_wait(cwm->trace_present_group, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC));
		if (wait_result != 0 && cwm->trace_presented != NULL) {
			COMP_WARN(cwm->base.base.c,
			          "Timed out waiting for Metal presented handlers; leaving presented trace open until process exit");
			fflush(cwm->trace_presented);
			cwm->trace_presented = NULL;
		}
		cwm->trace_present_group = NULL;
	}
	if (cwm->trace_present != NULL) {
		fflush(cwm->trace_present);
		fclose(cwm->trace_present);
		cwm->trace_present = NULL;
	}
	if (cwm->trace_presented != NULL) {
		fflush(cwm->trace_presented);
		fclose(cwm->trace_presented);
		cwm->trace_presented = NULL;
	}
	if (cwm->trace_vblank != NULL) {
		fflush(cwm->trace_vblank);
		fclose(cwm->trace_vblank);
		cwm->trace_vblank = NULL;
	}
}

static uint64_t
host_time_to_ns(struct comp_window_macos *cwm, uint64_t host_time)
{
	return (uint64_t)(((__uint128_t)host_time * cwm->mach_timebase.numer) / cwm->mach_timebase.denom);
}

static int64_t
refresh_host_to_monotonic_offset_ns(struct comp_window_macos *cwm)
{
	uint64_t host_ns = host_time_to_ns(cwm, CVGetCurrentHostTime());
	int64_t monotonic_ns = os_monotonic_get_ns();
	int64_t offset_ns = monotonic_ns - (int64_t)host_ns;
	atomic_store_explicit(&cwm->host_to_monotonic_offset_ns, offset_ns, memory_order_release);
	return offset_ns;
}

static uint64_t
host_ns_to_monotonic_ns(uint64_t host_ns, int64_t offset_ns)
{
	int64_t monotonic_ns = (int64_t)host_ns + offset_ns;
	return monotonic_ns > 0 ? (uint64_t)monotonic_ns : 0;
}

static double
monotonic_ns_to_host_seconds(struct comp_window_macos *cwm, int64_t monotonic_ns)
{
	int64_t offset_ns = atomic_load_explicit(&cwm->host_to_monotonic_offset_ns, memory_order_acquire);
	int64_t host_ns = monotonic_ns - offset_ns;
	return host_ns > 0 ? (double)host_ns / (double)U_TIME_1S_IN_NS : 0.0;
}

static uint64_t
derive_last_vblank_ns(struct comp_window_macos *cwm, uint64_t output_ns, uint64_t now_ns)
{
	if (output_ns == 0 || now_ns == 0 || cwm->display_period_ns <= 0) {
		return now_ns;
	}

	uint64_t period_ns = (uint64_t)cwm->display_period_ns;
	if (output_ns > now_ns) {
		uint64_t delta_ns = output_ns - now_ns;
		uint64_t periods = (delta_ns + period_ns - 1) / period_ns;
		uint64_t adjustment = periods * period_ns;
		return output_ns > adjustment ? output_ns - adjustment : 0;
	}

	uint64_t periods = (now_ns - output_ns) / period_ns;
	return output_ns + periods * period_ns;
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
	(void)flags_in;
	(void)flags_out;
	struct comp_window_macos *cwm = context;

	int64_t offset_ns = refresh_host_to_monotonic_offset_ns(cwm);
	uint64_t callback_ns = (uint64_t)os_monotonic_get_ns();
	uint64_t now_host_ns = 0;
	uint64_t output_host_ns = 0;
	uint64_t now_ns = callback_ns;
	uint64_t output_ns = 0;

	if ((in_now->flags & kCVTimeStampHostTimeValid) != 0) {
		now_host_ns = host_time_to_ns(cwm, in_now->hostTime);
		now_ns = host_ns_to_monotonic_ns(now_host_ns, offset_ns);
		atomic_store_explicit(&cwm->latest_displaylink_now_host_ns, now_host_ns, memory_order_release);
		atomic_store_explicit(&cwm->latest_displaylink_now_ns, now_ns, memory_order_release);
	}
	if ((in_output_time->flags & kCVTimeStampHostTimeValid) != 0) {
		output_host_ns = host_time_to_ns(cwm, in_output_time->hostTime);
		output_ns = host_ns_to_monotonic_ns(output_host_ns, offset_ns);
		atomic_store_explicit(&cwm->latest_displaylink_output_host_ns, output_host_ns, memory_order_release);
		atomic_store_explicit(&cwm->latest_displaylink_output_ns, output_ns, memory_order_release);
	}

	if (output_ns != 0) {
		uint64_t last_vblank_ns = derive_last_vblank_ns(cwm, output_ns, now_ns);
		atomic_store_explicit(&cwm->latest_vblank_ns, last_vblank_ns, memory_order_release);
	}
	atomic_store_explicit(&cwm->latest_displaylink_callback_ns, callback_ns, memory_order_release);
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
		int max_drawables = debug_get_num_option_macos_max_drawables();
		if (max_drawables != 2 && max_drawables != 3) {
			COMP_WARN(ct->c, "XRT_MACOS_MAX_DRAWABLES must be 2 or 3; using default 3 instead of %d", max_drawables);
			max_drawables = 3;
		}
		[metal_layer setMaximumDrawableCount:(NSUInteger)max_drawables];
		COMP_INFO(ct->c, "macOS CAMetalLayer maximumDrawableCount=%lu",
		          (unsigned long)[metal_layer maximumDrawableCount]);
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

		mach_timebase_info(&cwm->mach_timebase);
		refresh_host_to_monotonic_offset_ns(cwm);
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
	(void)preferred_width;
	(void)preferred_height;
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	struct vk_bundle *vk = get_vk(cwm);

	if (!vk->features.timeline_semaphore || vk->vkWaitSemaphores == NULL) {
		COMP_WARN(ct->c, "Timeline semaphores unavailable; macOS presentation will fall back to queue-idle waits");
		return true;
	}

	VkSemaphoreTypeCreateInfo type_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
	    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	    .initialValue = 0,
	};
	VkSemaphoreCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	    .pNext = &type_info,
	};
	VkResult ret = vk->vkCreateSemaphore(vk->device, &create_info, NULL, &ct->semaphores.render_complete);
	if (ret != VK_SUCCESS) {
		COMP_WARN(ct->c, "Could not create macOS render-complete timeline semaphore: %s; using queue-idle fallback",
		          vk_result_string(ret));
		ct->semaphores.render_complete = VK_NULL_HANDLE;
		ct->semaphores.render_complete_is_timeline = false;
		return true;
	}
	ct->semaphores.render_complete_is_timeline = true;
	COMP_INFO(ct->c, "macOS target using render-complete timeline semaphore");
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
	*out_index = cwm->next_image;
	cwm->next_image = (cwm->next_image + 1) % ct->image_count;
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
	struct vk_bundle *vk = get_vk(cwm);
	uint64_t frame_id = ++cwm->trace_frame_id;
	uint64_t after_drawable_ns = 0;
	uint64_t before_present_call_ns = 0;
	uint64_t after_present_call_ns = 0;
	uint64_t after_commit_ns = 0;
	uint64_t after_metal_wait_ns = 0;
	uint64_t target_output_ns = 0;
	uint64_t metal_request_ns = 0;
	const char *wait_mode = "queue_idle";
	double scheduled_present_host_s = 0.0;
	double gpu_start_time_s = 0.0;
	double gpu_end_time_s = 0.0;
	assert(present_queue != NULL);
	if (index >= ct->image_count || cwm->metal_images[index] == nil) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	uint64_t before_vk_wait_ns = os_monotonic_get_ns();
	VkResult ret = VK_SUCCESS;
	if (ct->semaphores.render_complete != VK_NULL_HANDLE && ct->semaphores.render_complete_is_timeline &&
	    vk->vkWaitSemaphores != NULL) {
		VkSemaphoreWaitInfo wait_info = {
		    .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		    .semaphoreCount = 1,
		    .pSemaphores = &ct->semaphores.render_complete,
		    .pValues = &timeline_semaphore_value,
		};
		wait_mode = "timeline";
		ret = vk->vkWaitSemaphores(vk->device, &wait_info, UINT64_MAX);
	} else {
		vk_queue_lock(present_queue);
		ret = vk->vkQueueWaitIdle(present_queue->queue);
		vk_queue_unlock(present_queue);
	}
	uint64_t after_vk_wait_ns = os_monotonic_get_ns();
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "Vulkan render-complete wait before Metal presentation: %s", vk_result_string(ret));
		return ret;
	}

	@autoreleasepool {
		id<CAMetalDrawable> drawable = [cwm->metal_layer nextDrawable];
		after_drawable_ns = os_monotonic_get_ns();
		if (drawable == nil) {
			COMP_ERROR(ct->c, "Could not acquire a CAMetalDrawable");
			return VK_ERROR_OUT_OF_DATE_KHR;
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

		before_present_call_ns = os_monotonic_get_ns();
		uint64_t latest_output_ns =
		    atomic_load_explicit(&cwm->latest_displaylink_output_ns, memory_order_acquire);
		int64_t min_lead_us = debug_get_num_option_macos_present_min_lead_us();
		if (min_lead_us < 0) {
			min_lead_us = 0;
		}
		uint64_t earliest_output_ns = before_present_call_ns + (uint64_t)min_lead_us * 1000ULL;
		if (desired_present_time_ns > 0 && (uint64_t)desired_present_time_ns > earliest_output_ns) {
			earliest_output_ns = (uint64_t)desired_present_time_ns;
		}
		target_output_ns = latest_output_ns;
		if (target_output_ns == 0) {
			target_output_ns = earliest_output_ns;
		} else if (cwm->display_period_ns > 0) {
			uint64_t period_ns = (uint64_t)cwm->display_period_ns;
			while (target_output_ns < earliest_output_ns) {
				target_output_ns += period_ns;
			}
		}

		if (cwm->trace_presented != NULL && cwm->trace_present_group != NULL) {
			FILE *trace_file = cwm->trace_presented;
			dispatch_group_t trace_group = cwm->trace_present_group;
			uint64_t traced_frame_id = frame_id;
			int64_t traced_desired_present_ns = desired_present_time_ns;
			uint64_t traced_target_output_ns = target_output_ns;
			dispatch_group_enter(trace_group);
			[drawable addPresentedHandler:^(id<MTLDrawable> presented_drawable) {
				double presented_time_s = [presented_drawable presentedTime];
				int64_t handler_ns = os_monotonic_get_ns();
				double host_frequency = CVGetHostClockFrequency();
				uint64_t current_host_ticks = CVGetCurrentHostTime();
				int64_t current_host_ns =
				    host_frequency > 0.0 ? (int64_t)llround((double)current_host_ticks * 1e9 / host_frequency) : 0;
				int64_t handler_offset_ns = handler_ns - current_host_ns;
				int64_t presented_host_ns =
				    presented_time_s > 0.0 ? (int64_t)llround(presented_time_s * (double)U_TIME_1S_IN_NS) : 0;
				int64_t presented_monotonic_ns =
				    presented_host_ns != 0 ? presented_host_ns + handler_offset_ns : 0;
				int64_t presented_minus_desired_ns =
				    presented_monotonic_ns != 0 ? presented_monotonic_ns - traced_desired_present_ns : 0;
				int64_t presented_minus_target_ns =
				    presented_monotonic_ns != 0 ? presented_monotonic_ns - (int64_t)traced_target_output_ns : 0;
				int64_t observed_present_offset_ns =
				    presented_monotonic_ns != 0 ? presented_monotonic_ns - traced_desired_present_ns : 0;
				if (observed_present_offset_ns > 0) {
					atomic_store_explicit(&cwm->latest_observed_present_offset_ns, observed_present_offset_ns,
					                      memory_order_release);
					atomic_fetch_add_explicit(&cwm->present_offset_sample_serial, 1, memory_order_release);
				}
				flockfile(trace_file);
				fprintf(trace_file,
				        "%llu,%" PRIi64 ",%" PRIi64 ",%llu,%.17g,%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%" PRIi64 "\n",
				        (unsigned long long)traced_frame_id, handler_ns, traced_desired_present_ns,
				        (unsigned long long)traced_target_output_ns, presented_time_s, presented_monotonic_ns,
				        presented_minus_desired_ns, presented_minus_target_ns, observed_present_offset_ns);
				fflush(trace_file);
				funlockfile(trace_file);
				dispatch_group_leave(trace_group);
			}];
		}

		int64_t prelatch_us = debug_get_num_option_macos_present_prelatch_us();
		if (prelatch_us < 0) {
			prelatch_us = 0;
		}
		uint64_t prelatch_ns = (uint64_t)prelatch_us * 1000ULL;
		metal_request_ns = target_output_ns > prelatch_ns ? target_output_ns - prelatch_ns : target_output_ns;
		scheduled_present_host_s = monotonic_ns_to_host_seconds(cwm, (int64_t)metal_request_ns);
		if (scheduled_present_host_s > 0.0) {
			[command_buffer presentDrawable:drawable atTime:scheduled_present_host_s];
		} else {
			[command_buffer presentDrawable:drawable];
		}
		after_present_call_ns = os_monotonic_get_ns();
		[command_buffer commit];
		after_commit_ns = os_monotonic_get_ns();
		[command_buffer waitUntilCompleted];
		after_metal_wait_ns = os_monotonic_get_ns();
		gpu_start_time_s = [command_buffer GPUStartTime];
		gpu_end_time_s = [command_buffer GPUEndTime];
		if ([command_buffer status] == MTLCommandBufferStatusError) {
			COMP_ERROR(ct->c, "Metal presentation failed: %s",
			           [[[command_buffer error] localizedDescription] UTF8String]);
			return VK_ERROR_DEVICE_LOST;
		}
		cwm->present_vk_wait_total_ns += after_vk_wait_ns - before_vk_wait_ns;
		cwm->present_drawable_wait_total_ns += after_drawable_ns - after_vk_wait_ns;
		cwm->present_metal_wait_total_ns += after_metal_wait_ns - after_drawable_ns;
	}

	if (cwm->trace_present != NULL) {
		uint64_t latest_output_ns = atomic_load_explicit(&cwm->latest_displaylink_output_ns, memory_order_acquire);
		fprintf(cwm->trace_present,
		        "%llu,%llu,%" PRIi64 ",%" PRIi64 ",%llu,%" PRIi64 ",%llu,%" PRIi64 ",%" PRIi64 ",%.17g,%" PRIi64 ",%u,%llu,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.17g,%.17g\n",
		        (unsigned long long)frame_id, (unsigned long long)before_vk_wait_ns, desired_present_time_ns,
		        desired_present_time_ns - (int64_t)before_vk_wait_ns, (unsigned long long)target_output_ns,
		        (int64_t)target_output_ns - desired_present_time_ns, (unsigned long long)metal_request_ns,
		        (int64_t)metal_request_ns - (int64_t)target_output_ns,
		        (int64_t)metal_request_ns - (int64_t)before_present_call_ns, scheduled_present_host_s, present_slop_ns, index,
		        (unsigned long long)timeline_semaphore_value, wait_mode, (unsigned long long)after_vk_wait_ns,
		        (unsigned long long)after_drawable_ns, (unsigned long long)before_present_call_ns,
		        (unsigned long long)after_present_call_ns, (unsigned long long)after_commit_ns,
		        (unsigned long long)after_metal_wait_ns, (unsigned long long)latest_output_ns, gpu_start_time_s, gpu_end_time_s);
		cwm->trace_present_rows++;
		if (cwm->trace_present_rows % 256 == 0) {
			fflush(cwm->trace_present);
		}
	}

	uint64_t now_ns = os_monotonic_get_ns();
	if (cwm->last_present_ns != 0 && now_ns > cwm->last_present_ns) {
		uint64_t interval_ns = now_ns - cwm->last_present_ns;
		if (cwm->display_period_ns <= 0 || interval_ns <= (uint64_t)cwm->display_period_ns * 4) {
			cwm->present_total_ns += interval_ns;
			cwm->present_sample_count++;
			if (cwm->present_min_ns == 0 || interval_ns < cwm->present_min_ns) {
				cwm->present_min_ns = interval_ns;
			}
			if (interval_ns > cwm->present_max_ns) {
				cwm->present_max_ns = interval_ns;
			}
			if (cwm->display_period_ns > 0 && interval_ns > (uint64_t)cwm->display_period_ns * 3 / 2) {
				cwm->present_missed_intervals++;
			}
		}
	}
	cwm->last_present_ns = now_ns;
	if (cwm->present_sample_count == 240) {
		double average_ms = (double)cwm->present_total_ns / (double)cwm->present_sample_count / 1000000.0;
		COMP_INFO(ct->c, "macOS completed-frame cadence: average %.3fms, min %.3fms, max %.3fms, late %llu/240",
		          average_ms, (double)cwm->present_min_ns / 1000000.0, (double)cwm->present_max_ns / 1000000.0,
		          (unsigned long long)cwm->present_missed_intervals);
		COMP_INFO(ct->c, "macOS presentation waits: Vulkan %.3fms, drawable %.3fms, Metal %.3fms",
		          (double)cwm->present_vk_wait_total_ns / 240.0 / 1000000.0,
		          (double)cwm->present_drawable_wait_total_ns / 240.0 / 1000000.0,
		          (double)cwm->present_metal_wait_total_ns / 240.0 / 1000000.0);
		cwm->present_sample_count = 0;
		cwm->present_total_ns = 0;
		cwm->present_min_ns = 0;
		cwm->present_max_ns = 0;
		cwm->present_missed_intervals = 0;
		cwm->present_vk_wait_total_ns = 0;
		cwm->present_drawable_wait_total_ns = 0;
		cwm->present_metal_wait_total_ns = 0;
	}
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
	uint64_t vblank_ns = atomic_exchange_explicit(&cwm->latest_vblank_ns, 0, memory_order_acquire);
	uint64_t displaylink_now_host_ns =
	    atomic_load_explicit(&cwm->latest_displaylink_now_host_ns, memory_order_acquire);
	uint64_t displaylink_output_host_ns =
	    atomic_load_explicit(&cwm->latest_displaylink_output_host_ns, memory_order_acquire);
	uint64_t displaylink_now_ns = atomic_load_explicit(&cwm->latest_displaylink_now_ns, memory_order_acquire);
	uint64_t displaylink_output_ns = atomic_load_explicit(&cwm->latest_displaylink_output_ns, memory_order_acquire);
	uint64_t displaylink_callback_ns = atomic_load_explicit(&cwm->latest_displaylink_callback_ns, memory_order_acquire);
	int64_t host_to_monotonic_offset_ns =
	    atomic_load_explicit(&cwm->host_to_monotonic_offset_ns, memory_order_acquire);

	uint64_t present_offset_serial =
	    atomic_load_explicit(&cwm->present_offset_sample_serial, memory_order_acquire);
	if (present_offset_serial != cwm->consumed_present_offset_sample_serial && cwm->display_period_ns > 0) {
		int64_t sample_ns =
		    atomic_load_explicit(&cwm->latest_observed_present_offset_ns, memory_order_acquire);
		cwm->consumed_present_offset_sample_serial = present_offset_serial;
		int64_t max_reasonable_ns = cwm->display_period_ns * 4;
		if (sample_ns > 0 && sample_ns <= max_reasonable_ns) {
			if (cwm->present_offset_sample_count == 0) {
				cwm->calibrated_present_offset_ns = sample_ns;
			} else {
				// 1/8 EMA: stable enough to reject callback/clock-conversion noise while adapting quickly.
				cwm->calibrated_present_offset_ns =
				    (cwm->calibrated_present_offset_ns * 7 + sample_ns) / 8;
			}
			if (cwm->present_offset_sample_count < UINT32_MAX) {
				cwm->present_offset_sample_count++;
			}
			if (cwm->present_offset_sample_count >= 8 && cwm->base.upc != NULL) {
				u_pc_update_present_offset(cwm->base.upc, 0, cwm->calibrated_present_offset_ns);
			}
		}
	}
	if (vblank_ns == 0 || vblank_ns == cwm->last_vblank_ns || cwm->base.upc == NULL) {
		return VK_SUCCESS;
	}

	uint64_t consumed_ns = os_monotonic_get_ns();
	uint64_t previous_vblank_ns = cwm->last_vblank_ns;
	if (cwm->trace_vblank != NULL) {
		fprintf(cwm->trace_vblank,
		        "%llu,%llu,%llu,%llu,%" PRIi64 ",%llu,%llu,%llu,%" PRIi64 ",%" PRIi64 ",%" PRIi64 ",%llu,%" PRIi64 "\n",
		        (unsigned long long)consumed_ns, (unsigned long long)displaylink_callback_ns,
		        (unsigned long long)displaylink_now_host_ns, (unsigned long long)displaylink_output_host_ns,
		        host_to_monotonic_offset_ns, (unsigned long long)displaylink_now_ns,
		        (unsigned long long)displaylink_output_ns, (unsigned long long)vblank_ns,
		        (int64_t)displaylink_output_ns - (int64_t)displaylink_now_ns,
		        (int64_t)displaylink_callback_ns - (int64_t)displaylink_output_ns,
		        (int64_t)displaylink_callback_ns - (int64_t)vblank_ns,
		        previous_vblank_ns != 0 && vblank_ns > previous_vblank_ns
		            ? (unsigned long long)(vblank_ns - previous_vblank_ns)
		            : 0ULL,
		        cwm->display_period_ns);
		cwm->trace_vblank_rows++;
		if (cwm->trace_vblank_rows % 256 == 0) {
			fflush(cwm->trace_vblank);
		}
	}

	if (debug_get_bool_option_macos_cvdisplaylink_pacing()) {
		u_pc_update_vblank_from_display_control(cwm->base.upc, (int64_t)vblank_ns);
	}
	if (cwm->last_vblank_ns != 0 && vblank_ns > cwm->last_vblank_ns) {
		uint64_t interval_ns = vblank_ns - cwm->last_vblank_ns;
		if (cwm->display_period_ns > 0 && interval_ns > (uint64_t)cwm->display_period_ns * 4) {
			cwm->last_vblank_ns = vblank_ns;
			return VK_SUCCESS;
		}
		cwm->cadence_total_ns += interval_ns;
		cwm->cadence_sample_count++;
		if (cwm->cadence_min_ns == 0 || interval_ns < cwm->cadence_min_ns) {
			cwm->cadence_min_ns = interval_ns;
		}
		if (interval_ns > cwm->cadence_max_ns) {
			cwm->cadence_max_ns = interval_ns;
		}

		if (cwm->cadence_sample_count == 240) {
			double average_ms = (double)cwm->cadence_total_ns / (double)cwm->cadence_sample_count / 1000000.0;
			COMP_INFO(ct->c, "PS VR2 display-link cadence: average %.3fms, min %.3fms, max %.3fms", average_ms,
			          (double)cwm->cadence_min_ns / 1000000.0, (double)cwm->cadence_max_ns / 1000000.0);
			cwm->cadence_sample_count = 0;
			cwm->cadence_total_ns = 0;
			cwm->cadence_min_ns = 0;
			cwm->cadence_max_ns = 0;
		}
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
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	@autoreleasepool {
		[CATransaction flush];
		if (!cwm->logged_layer_state) {
			CGSize drawable_size = [cwm->metal_layer drawableSize];
			COMP_INFO(ct->c, "macOS presentation: window visible=%s layer device=%s format=%lu drawable=%.0fx%.0f",
			          [cwm->window isVisible] ? "true" : "false", [cwm->metal_layer device] != nil ? "set" : "nil",
			          (unsigned long)[cwm->metal_layer pixelFormat], drawable_size.width, drawable_size.height);
			cwm->logged_layer_state = true;
		}
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
	if (cwm->display_link != NULL) {
		CVDisplayLinkStop(cwm->display_link);
		CVDisplayLinkRelease(cwm->display_link);
		cwm->display_link = NULL;
	}
	macos_timing_trace_close(cwm);
	comp_window_macos_free_images(cwm);
	if (ct->semaphores.render_complete != VK_NULL_HANDLE) {
		struct vk_bundle *vk = get_vk(cwm);
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
	free(cwm);
}

struct comp_target *
comp_window_macos_create(struct comp_compositor *c)
{
	struct comp_window_macos *cwm = U_TYPED_CALLOC(struct comp_window_macos);
	macos_timing_trace_open(cwm);
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
