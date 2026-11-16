// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Public AppKit/Metal surface for the generic Vulkan WSI compositor.
 * @ingroup comp_main
 */

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CATransaction.h>

#include "main/comp_window.h"
#include "util/u_debug.h"
#include "util/u_misc.h"

DEBUG_GET_ONCE_NUM_OPTION(macos_display_id, "XRT_MACOS_DISPLAY_ID", 0)
DEBUG_GET_ONCE_BOOL_OPTION(macos_windowed, "XRT_MACOS_WINDOWED", false)

struct comp_window_macos_wsi
{
	struct comp_target_swapchain base;
	NSWindow *window;
	CAMetalLayer *layer;
};

static bool
window_init(struct comp_target *ct)
{
	struct comp_window_macos_wsi *w = (struct comp_window_macos_wsi *)ct;
	// Dispatching synchronously to main here can deadlock an in-process client.
	if (![NSThread isMainThread]) {
		COMP_ERROR(ct->c, "Create the macOS in-process OpenXR system on the main thread");
		return false;
	}
	@autoreleasepool {
		[NSApplication sharedApplication];
		bool windowed = debug_get_bool_option_macos_windowed();
		uint32_t display_id = (uint32_t)debug_get_num_option_macos_display_id();
		NSScreen *screen = nil;
		for (NSScreen *candidate in [NSScreen screens]) {
			uint32_t candidate_id =
			    [[[candidate deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue];
			if (display_id != 0 && candidate_id == display_id) {
				screen = candidate;
				break;
			}
		}
		if (screen == nil && display_id == 0 && windowed) {
			screen = [NSScreen mainScreen];
		}
		if (screen == nil) {
			if (display_id == 0) {
				COMP_ERROR(ct->c,
				           "Set XRT_MACOS_DISPLAY_ID to the headset display's CoreGraphics ID, "
				           "or use XRT_MACOS_WINDOWED=1 for desktop output");
			} else {
				COMP_ERROR(ct->c, "No macOS display matches XRT_MACOS_DISPLAY_ID=%u", display_id);
			}
			for (NSScreen *candidate in [NSScreen screens]) {
				COMP_INFO(
				    ct->c, "Available macOS display: id=%u name=%s",
				    [[[candidate deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue],
				    [[candidate localizedName] UTF8String]);
			}
			return false;
		}
		// The initializer positions content relative to the selected screen.
		NSSize screen_size = [screen frame].size;
		NSRect rect =
		    windowed ? NSMakeRect(0, 0, 1280, 720) : NSMakeRect(0, 0, screen_size.width, screen_size.height);
		NSWindowStyleMask style = windowed ? NSWindowStyleMaskTitled : NSWindowStyleMaskBorderless;
		w->window = [[NSWindow alloc] initWithContentRect:rect
		                                        styleMask:style
		                                          backing:NSBackingStoreBuffered
		                                            defer:NO
		                                           screen:screen];
		[w->window setReleasedWhenClosed:NO];
		[w->window setTitle:@"Monado Vulkan WSI"];
		[w->window setLevel:windowed ? NSNormalWindowLevel : NSMainMenuWindowLevel + 1];
		w->layer = [[CAMetalLayer alloc] init];
		[w->layer setContentsScale:[screen backingScaleFactor]];
		[w->layer setOpaque:YES];
		NSView *view = [w->window contentView];
		// Set the layer before enabling it, making a layer-hosting view that
		// AppKit does not draw into or replace.
		[view setLayer:w->layer];
		[view setWantsLayer:YES];
		[w->layer setDelegate:(id<CALayerDelegate>)view];
		NSSize size = [view convertRectToBacking:[view bounds]].size;
		[w->layer setDrawableSize:CGSizeMake(size.width, size.height)];
		w->base.override.compositor_extent = true;
		w->base.override.extent = (VkExtent2D){(uint32_t)size.width, (uint32_t)size.height};
		[w->window makeKeyAndOrderFront:nil];
		// A command-line OpenXR client may not enter an AppKit run loop.
		// Commit the window's layer tree before Vulkan starts presenting to it.
		[w->window displayIfNeeded];
		[CATransaction flush];
		COMP_INFO(ct->c, "macOS Vulkan WSI display=%u extent=%ux%u",
		          [[[screen deviceDescription] objectForKey:@"NSScreenNumber"] unsignedIntValue],
		          w->base.override.extent.width, w->base.override.extent.height);
		COMP_DEBUG(ct->c, "macOS WSI screen=%s window=%s", [NSStringFromRect([screen frame]) UTF8String],
		           [NSStringFromRect([w->window frame]) UTF8String]);
		COMP_DEBUG(ct->c, "macOS WSI layer=%s committed=%s visible=%s",
		           [NSStringFromRect([w->layer frame]) UTF8String],
		           [w->layer presentationLayer] != nil ? "true" : "false",
		           [w->window isVisible] ? "true" : "false");
	}
	return w->window != nil && w->layer != nil;
}

static bool
window_init_vulkan(struct comp_target *ct, uint32_t width, uint32_t height)
{
	struct comp_window_macos_wsi *w = (struct comp_window_macos_wsi *)ct;
	struct vk_bundle *vk = &ct->c->base.vk;
	VkMetalSurfaceCreateInfoEXT info = {
	    .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
	    .pLayer = w->layer,
	};
	VkResult ret = vk->vkCreateMetalSurfaceEXT(vk->instance, &info, NULL, &w->base.surface.handle);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "vkCreateMetalSurfaceEXT: %s", vk_str_result(ret));
	}
	return ret == VK_SUCCESS;
}

static void
window_flush(struct comp_target *ct)
{
	// The application's main thread owns AppKit event processing.
}

static void
window_set_title(struct comp_target *ct, const char *title)
{
	struct comp_window_macos_wsi *w = (struct comp_window_macos_wsi *)ct;
	if (title == NULL || w->window == nil) {
		return;
	}
	@autoreleasepool {
		NSString *text = [NSString stringWithUTF8String:title];
		NSWindow *window = w->window;
		if ([NSThread isMainThread]) {
			[window setTitle:text];
			return;
		}
		// Applied once the application services the main queue; a client
		// without an AppKit run loop keeps the initial title.
		dispatch_async(dispatch_get_main_queue(), ^{
		  [window setTitle:text];
		});
	}
}

static void
window_destroy(struct comp_target *ct)
{
	struct comp_window_macos_wsi *w = (struct comp_window_macos_wsi *)ct;
	comp_target_swapchain_cleanup(&w->base);
	NSWindow *window = w->window;
	CAMetalLayer *layer = w->layer;
	void (^cleanup)(void) = ^{
	  [window orderOut:nil];
	  [window close];
	  [window release];
	  [layer release];
	};
	if ([NSThread isMainThread]) {
		cleanup();
	} else {
		// AppKit objects must be released on the main thread. Without a
		// serviced main queue the window stays visible until process exit.
		dispatch_async(dispatch_get_main_queue(), cleanup);
	}
	free(w);
}

static bool
window_detect(const struct comp_target_factory *ctf, struct comp_compositor *c)
{
	return true;
}

static bool
window_create(const struct comp_target_factory *ctf, struct comp_compositor *c, struct comp_target **out_ct)
{
	struct comp_window_macos_wsi *w = U_TYPED_CALLOC(struct comp_window_macos_wsi);
	if (w == NULL)
		return false;
	// The generic target supplies display-feedback and non-feedback pacing.
	comp_target_swapchain_init_and_set_fnptrs(&w->base, COMP_TARGET_USE_DISPLAY_IF_AVAILABLE);
	w->base.base.name = "macOS Vulkan WSI";
	w->base.base.c = c;
	w->base.base.init_pre_vulkan = window_init;
	w->base.base.init_post_vulkan = window_init_vulkan;
	w->base.base.flush = window_flush;
	w->base.base.set_title = window_set_title;
	w->base.base.destroy = window_destroy;
	*out_ct = &w->base.base;
	return true;
}

static const char *instance_extensions[] = {VK_EXT_METAL_SURFACE_EXTENSION_NAME};

const struct comp_target_factory comp_target_factory_macos_wsi = {
    .name = "macOS Vulkan WSI",
    .identifier = "wsi",
    .requires_vulkan_for_create = false,
    .is_deferred = false,
    .required_instance_version = VK_API_VERSION_1_1,
    .required_instance_extensions = instance_extensions,
    .required_instance_extension_count = ARRAY_SIZE(instance_extensions),
    .detect = window_detect,
    .create_target = window_create,
};
