// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS PS VR2 display target using CAMetalLayer and VK_EXT_metal_surface.
 * @ingroup comp_main
 */

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/QuartzCore.h>

#include "main/comp_window.h"
#include "util/u_misc.h"


struct comp_window_macos
{
	struct comp_target_swapchain base;

	NSScreen *screen;
	NSWindow *window;
	CAMetalLayer *metal_layer;
	CGDirectDisplayID display_id;
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
		[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
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

		CAMetalLayer *metal_layer = [[CAMetalLayer layer] retain];
		if (metal_layer == nil) {
			[window release];
			COMP_ERROR(ct->c, "Failed to create the PS VR2 CAMetalLayer");
			return false;
		}

		NSView *content_view = [window contentView];
		[content_view setWantsLayer:YES];
		[content_view setLayer:metal_layer];
		[metal_layer setDelegate:(id<CALayerDelegate>)content_view];
		[metal_layer setFrame:[content_view bounds]];
		[metal_layer setContentsScale:[screen backingScaleFactor]];
		[metal_layer setDrawableSize:CGSizeMake(pixel_width, pixel_height)];
		[metal_layer setAutoresizingMask:kCALayerWidthSizable | kCALayerHeightSizable];
		[metal_layer setOpaque:YES];
		[metal_layer setDisplaySyncEnabled:YES];

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
		[CATransaction flush];

		cwm->screen = [screen retain];
		cwm->window = window;
		cwm->metal_layer = metal_layer;
		cwm->display_id = display_id;

		VkExtent2D extent = {
		    .width = (uint32_t)pixel_width,
		    .height = (uint32_t)pixel_height,
		};
		comp_target_swapchain_override_extents(&cwm->base, extent);

		COMP_INFO(ct->c, "Selected macOS display '%s' at %ux%u", [[screen localizedName] UTF8String], extent.width,
		          extent.height);
	}

	return true;
}

static bool
comp_window_macos_init_swapchain(struct comp_target *ct, uint32_t width, uint32_t height)
{
	struct comp_window_macos *cwm = (struct comp_window_macos *)ct;
	struct vk_bundle *vk = get_vk(cwm);

	if (vk->vkCreateMetalSurfaceEXT == NULL) {
		COMP_ERROR(ct->c, "vkCreateMetalSurfaceEXT was not loaded");
		return false;
	}

	VkMetalSurfaceCreateInfoEXT surface_info = {
	    .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
	    .pLayer = cwm->metal_layer,
	};

	VkResult ret = vk->vkCreateMetalSurfaceEXT(vk->instance, &surface_info, NULL, &cwm->base.surface.handle);
	if (ret != VK_SUCCESS) {
		COMP_ERROR(ct->c, "vkCreateMetalSurfaceEXT: %s", vk_result_string(ret));
		return false;
	}

	VK_NAME_SURFACE(vk, cwm->base.surface.handle, "comp_window_macos surface");
	return true;
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

	comp_target_swapchain_cleanup(&cwm->base);

	@autoreleasepool {
		[cwm->window orderOut:nil];
		[cwm->window close];
		[cwm->window release];
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

	cwm->base.base.name = "macOS Metal";
	cwm->base.display = VK_NULL_HANDLE;
	cwm->base.base.destroy = comp_window_macos_destroy;
	cwm->base.base.flush = comp_window_macos_flush;
	cwm->base.base.init_pre_vulkan = comp_window_macos_init;
	cwm->base.base.init_post_vulkan = comp_window_macos_init_swapchain;
	cwm->base.base.set_title = comp_window_macos_set_title;
	cwm->base.base.c = c;

	return &cwm->base.base;
}

static const char *instance_extensions[] = {
	VK_EXT_METAL_SURFACE_EXTENSION_NAME,
};

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
	.required_instance_extensions = instance_extensions,
	.required_instance_extension_count = ARRAY_SIZE(instance_extensions),
	.optional_device_extensions = NULL,
	.optional_device_extension_count = 0,
	.detect = detect,
	.create_target = create_target,
};
