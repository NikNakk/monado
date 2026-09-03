// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS PS VR2 display target using IOSurface-backed Vulkan images and Metal presentation.
 * @ingroup comp_main
 */

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>

#include "main/comp_window.h"
#include "util/u_handles.h"
#include "util/u_misc.h"
#include "util/u_pacing.h"
#include "vk/vk_image_allocator.h"

#define MACOS_TARGET_IMAGE_COUNT 3

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
	uint32_t pixel_width;
	uint32_t pixel_height;
	uint32_t next_image;
	bool logged_layer_state;
};

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
	(void)timeline_semaphore_value;
	(void)desired_present_time_ns;
	(void)present_slop_ns;
	assert(present_queue != NULL);
	if (index >= ct->image_count || cwm->metal_images[index] == nil) {
		return VK_ERROR_INITIALIZATION_FAILED;
	}

	vk_queue_lock(present_queue);
	VkResult ret = vk->vkQueueWaitIdle(present_queue->queue);
	vk_queue_unlock(present_queue);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "vkQueueWaitIdle before Metal presentation: %s", vk_result_string(ret));
		return ret;
	}

	@autoreleasepool {
		id<CAMetalDrawable> drawable = [cwm->metal_layer nextDrawable];
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
		[command_buffer presentDrawable:drawable];
		[command_buffer commit];
		[command_buffer waitUntilCompleted];
		if ([command_buffer status] == MTLCommandBufferStatusError) {
			COMP_ERROR(ct->c, "Metal presentation failed: %s",
			           [[[command_buffer error] localizedDescription] UTF8String]);
			return VK_ERROR_DEVICE_LOST;
		}
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
	comp_window_macos_free_images(cwm);
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
	cwm->base.base.queue_supports_present = comp_window_macos_queue_supports_present;
	cwm->base.base.set_title = comp_window_macos_set_title;
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
