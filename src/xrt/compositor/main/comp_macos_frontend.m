// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "main/comp_macos_frontend.h"
#include "main/comp_compositor.h"
#include "main/comp_macos_remote_layer.h"
#include "xrt/xrt_device.h"
#include "util/u_desktop_pose.h"
#include "util/u_macos_display_host.h"
#include "util/u_macos_hosted_client.h"
#include "util/u_misc.h"

#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <pthread.h>

struct macos_frontend_ops;
#define MACOS_MAX_HOSTED_LAYERS 8

//! A client's CAContext shown in the headset window (window front-end).
struct macos_hosted_layer
{
	uint32_t client_id;
	CALayerHost *host;
	enum u_macos_display_host_visibility visibility;
};

struct comp_macos_frontend
{
	struct comp_compositor *c;
	NSScreen *screen;
	NSWindow *window;
	/* Window front-end: root of the window's layer tree; metal_layer is a sublayer. */
	CALayer *root_layer;
	/* Window front-end: layers hosted from client processes, above metal_layer. */
	pthread_mutex_t host_mutex;
	struct macos_hosted_layer hosted[MACOS_MAX_HOSTED_LAYERS];
	bool host_registered;
	/* Hosted front-end: container around metal_layer, shown by the service. */
	CALayer *hosted_container;
	CAContext *hosted_context;
	/* The headset display, fixed for the session. */
	CGDirectDisplayID display_id;
	/* Simulated HMD in a desktop window (SIMULATED_DESKTOP=1), not full screen. */
	bool desktop;
	/* Layer size in points: the screen, or the desktop window's content. */
	NSSize size;
	char display_name[128];
	/* Owns the layer's container; the presenter only uses metal_layer. */
	const struct macos_frontend_ops *frontend;
	struct u_macos_hosted_client *hosted_client;
	CAMetalLayer *metal_layer;
	uint32_t pixel_width, pixel_height;
};

CGDirectDisplayID
comp_macos_frontend_display_id(NSScreen *screen)
{
	NSNumber *number = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
	return number != nil ? (CGDirectDisplayID)[number unsignedIntValue] : kCGNullDirectDisplay;
}

static NSScreen *
find_psvr2_screen(struct comp_compositor *c)
{
	NSScreen *width_fallback = nil;
	for (NSScreen *screen in [NSScreen screens]) {
		CGDirectDisplayID display_id = comp_macos_frontend_display_id(screen);
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

//! The built-in panel, else the main screen.
static NSScreen *
find_desktop_screen(void)
{
	for (NSScreen *screen in [NSScreen screens]) {
		CGDirectDisplayID display_id = comp_macos_frontend_display_id(screen);
		if (display_id != kCGNullDirectDisplay && CGDisplayIsBuiltin(display_id)) {
			return screen;
		}
	}
	return [NSScreen mainScreen];
}

/*!
 * The desktop window's content size in points: the simulated HMD's panel
 * aspect (so nothing is stretched), as large as fits 90% of the visible area.
 * A hosted client computes the same size from the same screen.
 */
static NSSize
desktop_window_size(struct comp_compositor *c, NSScreen *screen)
{
	double aspect = 16.0 / 9.0;
	if (c != NULL && c->xdev != NULL && c->xdev->hmd != NULL && c->xdev->hmd->screens[0].w_pixels > 0 &&
	    c->xdev->hmd->screens[0].h_pixels > 0) {
		aspect = (double)c->xdev->hmd->screens[0].w_pixels / (double)c->xdev->hmd->screens[0].h_pixels;
	}
	NSSize visible = [screen visibleFrame].size;
	double width = fmin(visible.width * 0.9, visible.height * 0.9 * aspect);
	return NSMakeSize(floor(width), floor(width / aspect));
}

/*
 *
 * Front-ends: where the presenter's CAMetalLayer lives.
 *
 * The presenter only needs a CAMetalLayer, the headset's display ID and its
 * pixel size. A front-end provides them and owns whatever the layer sits in.
 * Today that is a borderless window on the headset display.
 *
 */

@interface MonadoHostedPresentNotification : NSObject {
	struct u_macos_hosted_client *_client;
	uint64_t _ticket;
}
- (instancetype)initWithClient:(struct u_macos_hosted_client *)client ticket:(uint64_t)ticket;
- (void)presented;
@end

@implementation MonadoHostedPresentNotification
- (instancetype)initWithClient:(struct u_macos_hosted_client *)client ticket:(uint64_t)ticket
{
	self = [super init];
	if (self) {
		_client = client;
		_ticket = ticket;
		u_macos_hosted_client_reference(client);
	}
	return self;
}
- (void)presented
{
	u_macos_hosted_client_note_presented(_client, _ticket);
}
- (void)dealloc
{
	u_macos_hosted_client_release(_client);
	[super dealloc];
}
@end

/*
 *
 * Desktop window input (SIMULATED_DESKTOP=1).
 *
 * Click captures the pointer; then moving one finger on the trackpad turns the
 * head (yaw and pitch) and two fingers move it across the floor. Esc frees the
 * pointer and R returns to the starting pose. The service's main loop pumps
 * AppKit events, so these handlers run on the main thread.
 *
 */

//! Radians of head rotation per point of pointer movement (0.15 degrees).
#define DESKTOP_RADIANS_PER_POINT 0.0026179939f
//! Metres of head movement per point of two-finger scrolling.
#define DESKTOP_METRES_PER_POINT 0.002f

@interface MonadoDesktopView : NSView {
	BOOL _captured;
}
- (void)releasePointer;
@end

@implementation MonadoDesktopView
- (BOOL)acceptsFirstResponder
{
	return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
	return YES;
}
- (void)viewDidMoveToWindow
{
	[[NSNotificationCenter defaultCenter] removeObserver:self];
	if ([self window] != nil) {
		// Never keep the pointer captured behind another window.
		[[NSNotificationCenter defaultCenter] addObserver:self
		                                         selector:@selector(windowDidResignKey:)
		                                             name:NSWindowDidResignKeyNotification
		                                           object:[self window]];
	}
}
- (void)windowDidResignKey:(NSNotification *)notification
{
	[self releasePointer];
}
- (void)capturePointer
{
	if (_captured) {
		return;
	}
	_captured = YES;
	CGAssociateMouseAndMouseCursorPosition(false);
	[NSCursor hide];
}
- (void)releasePointer
{
	if (!_captured) {
		return;
	}
	_captured = NO;
	CGAssociateMouseAndMouseCursorPosition(true);
	[NSCursor unhide];
}
- (void)mouseDown:(NSEvent *)event
{
	[self capturePointer];
}
- (void)look:(NSEvent *)event
{
	if (!_captured) {
		return;
	}
	// Pointer right turns right (negative yaw); pointer up (negative deltaY) looks up.
	u_desktop_pose_rotate(-(float)[event deltaX] * DESKTOP_RADIANS_PER_POINT,
	                      -(float)[event deltaY] * DESKTOP_RADIANS_PER_POINT);
}
- (void)mouseMoved:(NSEvent *)event
{
	[self look:event];
}
- (void)mouseDragged:(NSEvent *)event
{
	[self look:event];
}
- (void)scrollWheel:(NSEvent *)event
{
	// Ignore inertial scrolling: movement stops when the fingers lift.
	if ([event momentumPhase] != NSEventPhaseNone) {
		return;
	}
	CGFloat dx = [event scrollingDeltaX];
	CGFloat dy = [event scrollingDeltaY];
	if (![event hasPreciseScrollingDeltas]) {
		// A mouse wheel reports lines, not points.
		dx *= 10.0;
		dy *= 10.0;
	}
	// Recover the fingers' direction whatever the natural-scrolling setting.
	CGFloat fingers_right = [event isDirectionInvertedFromDevice] ? dx : -dx;
	CGFloat fingers_up = [event isDirectionInvertedFromDevice] ? -dy : dy;
	// Fingers away from you move forward, as on a joystick.
	u_desktop_pose_move((float)fingers_right * DESKTOP_METRES_PER_POINT,
	                    (float)fingers_up * DESKTOP_METRES_PER_POINT);
}
- (void)keyDown:(NSEvent *)event
{
	if ([event keyCode] == 53) { // Escape
		[self releasePointer];
		return;
	}
	NSString *chars = [event charactersIgnoringModifiers];
	if ([chars caseInsensitiveCompare:@"r"] == NSOrderedSame) {
		u_desktop_pose_reset();
	}
}
- (void)dealloc
{
	[[NSNotificationCenter defaultCenter] removeObserver:self];
	[self releasePointer];
	[super dealloc];
}
@end

struct macos_frontend_ops
{
	const char *name;
	/*!
	 * Set cwm->metal_layer (retained, contentsScale set), cwm->display_id,
	 * cwm->pixel_width/height and cwm->display_name. Nothing is on screen yet.
	 */
	bool (*create)(struct comp_macos_frontend *cwm);
	//! Put the layer on screen, once the presenter has configured it.
	void (*show)(struct comp_macos_frontend *cwm);
	void (*set_title)(struct comp_macos_frontend *cwm, const char *title);
	bool (*is_visible)(struct comp_macos_frontend *cwm);
	//! Release what create made, except cwm->metal_layer. Safe after a failed create.
	void (*destroy)(struct comp_macos_frontend *cwm);
};

static NSScreen *
macos_frontend_select_display(struct comp_macos_frontend *cwm)
{
	struct comp_compositor *c = cwm->c;
	if (u_desktop_pose_enabled()) {
		NSScreen *screen = find_desktop_screen();
		CGDirectDisplayID display_id =
		    screen != nil ? comp_macos_frontend_display_id(screen) : kCGNullDirectDisplay;
		if (display_id == kCGNullDirectDisplay) {
			COMP_ERROR(c, "SIMULATED_DESKTOP: no usable desktop display");
			return nil;
		}
		NSSize size = desktop_window_size(c, screen);
		double scale = [screen backingScaleFactor];
		cwm->desktop = true;
		cwm->size = size;
		cwm->display_id = display_id;
		cwm->pixel_width = (uint32_t)lround(size.width * scale);
		cwm->pixel_height = (uint32_t)lround(size.height * scale);
		snprintf(cwm->display_name, sizeof(cwm->display_name), "%s (simulated HMD window)",
		         [[screen localizedName] UTF8String]);
		COMP_INFO(c, "SIMULATED_DESKTOP: %ux%u pixel window on '%s'", cwm->pixel_width, cwm->pixel_height,
		          [[screen localizedName] UTF8String]);
		return screen;
	}
	NSScreen *screen = find_psvr2_screen(c);
	if (screen == nil) {
		COMP_ERROR(c, "Could not find a display named 'PS VR2' or a 4000-pixel-wide fallback");
		return nil;
	}
	CGDirectDisplayID display_id = comp_macos_frontend_display_id(screen);
	size_t width = display_id != kCGNullDirectDisplay ? CGDisplayPixelsWide(display_id) : 0;
	size_t height = display_id != kCGNullDirectDisplay ? CGDisplayPixelsHigh(display_id) : 0;
	if (width == 0 || height == 0) {
		COMP_ERROR(c, "Selected macOS display has no usable display ID or pixel size");
		return nil;
	}
	cwm->display_id = display_id;
	cwm->size = [screen frame].size;
	cwm->pixel_width = (uint32_t)width;
	cwm->pixel_height = (uint32_t)height;
	snprintf(cwm->display_name, sizeof(cwm->display_name), "%s", [[screen localizedName] UTF8String]);
	return screen;
}

static CAMetalLayer *
macos_frontend_create_metal_layer(struct comp_macos_frontend *cwm, NSScreen *screen)
{
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (device == nil) {
		COMP_ERROR(cwm->c, "Failed to create the default Metal device");
		return nil;
	}
	NSSize size = cwm->size;
	CAMetalLayer *layer = [[CAMetalLayer alloc] init];
	[layer setDevice:device];
	[device release];
	[layer setPixelFormat:MTLPixelFormatBGRA8Unorm];
	[layer setFramebufferOnly:NO];
	[layer setContentsScale:[screen backingScaleFactor]];
	[layer setAnchorPoint:CGPointZero];
	[layer setFrame:CGRectMake(0, 0, size.width, size.height)];
	[layer setAutoresizingMask:kCALayerWidthSizable | kCALayerHeightSizable];
	return layer;
}

static bool
macos_window_frontend_create(struct comp_macos_frontend *cwm)
{
	struct comp_compositor *c = cwm->c;

	NSScreen *screen = macos_frontend_select_display(cwm);
	if (screen == nil) {
		return false;
	}
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
	[NSApp finishLaunching];

	NSRect content_rect = [screen frame];
	NSWindowStyleMask style = NSWindowStyleMaskBorderless;
	if (cwm->desktop) {
		// An ordinary, fixed-size window: the target size follows it.
		NSRect visible = [screen visibleFrame];
		content_rect = NSMakeRect(NSMidX(visible) - cwm->size.width / 2, NSMidY(visible) - cwm->size.height / 2,
		                          cwm->size.width, cwm->size.height);
		style = NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable;
	}
	NSWindow *window = [[NSWindow alloc] initWithContentRect:content_rect
	                                               styleMask:style
	                                                 backing:NSBackingStoreBuffered
	                                                   defer:NO
	                                                  screen:screen];
	if (window == nil) {
		COMP_ERROR(c, "Failed to create the macOS PS VR2 window");
		return false;
	}
	// Teardown closes then releases; a default close would release it too.
	[window setReleasedWhenClosed:NO];

	CAMetalLayer *metal_layer = macos_frontend_create_metal_layer(cwm, screen);
	if (metal_layer == nil) {
		[window release];
		return false;
	}

	/*
	 * The window hosts its own layer tree: a root layer holding the
	 * presenter's CAMetalLayer. Layers hosted from client processes can then
	 * sit beside it, and the presenter's layer can be hidden on its own while
	 * a client is shown. This replaces an MTKView, whose layer AppKit owns.
	 */
	NSSize size = cwm->size;
	NSRect content_frame = NSMakeRect(0, 0, size.width, size.height);
	NSView *content_view = cwm->desktop ? [[MonadoDesktopView alloc] initWithFrame:content_frame]
	                                    : [[NSView alloc] initWithFrame:content_frame];
	CALayer *root_layer = [[CALayer alloc] init];
	CGColorRef black = CGColorCreateGenericRGB(0.0, 0.0, 0.0, 1.0);
	[root_layer setBackgroundColor:black];
	CGColorRelease(black);
	[root_layer setFrame:CGRectMake(0, 0, size.width, size.height)];
	[content_view setLayer:root_layer];
	[content_view setWantsLayer:YES];
	[window setContentView:content_view];
	[content_view release];

	[root_layer addSublayer:metal_layer];

	cwm->screen = [screen retain];
	cwm->window = window;
	cwm->root_layer = root_layer;
	cwm->metal_layer = metal_layer;
	return true;
}


/*
 *
 * Window front-end: hosting client layers.
 *
 * A client that composites in its own process presents into a CAContext and
 * the service shows it here through a CALayerHost, above the presenter's own
 * layer. Registered with u_macos_display_host so the IPC server can reach it.
 *
 */

//! Hide the presenter's own layer while any client is exclusive.
static void
macos_host_update_service_layer_locked(struct comp_macos_frontend *cwm)
{
	bool exclusive = false;
	for (uint32_t i = 0; i < MACOS_MAX_HOSTED_LAYERS; i++) {
		if (cwm->hosted[i].host != nil && cwm->hosted[i].visibility == U_MACOS_DISPLAY_HOST_EXCLUSIVE) {
			exclusive = true;
		}
	}
	[cwm->metal_layer setHidden:exclusive];
}

static struct macos_hosted_layer *
macos_host_find_locked(struct comp_macos_frontend *cwm, uint32_t client_id, bool allocate)
{
	struct macos_hosted_layer *free_entry = NULL;
	for (uint32_t i = 0; i < MACOS_MAX_HOSTED_LAYERS; i++) {
		struct macos_hosted_layer *entry = &cwm->hosted[i];
		if (entry->host != nil && entry->client_id == client_id) {
			return entry;
		}
		if (entry->host == nil && free_entry == NULL) {
			free_entry = entry;
		}
	}
	return allocate ? free_entry : NULL;
}

static void
macos_host_remove_locked(struct macos_hosted_layer *entry)
{
	[entry->host removeFromSuperlayer];
	[entry->host release];
	entry->host = nil;
	entry->client_id = 0;
	entry->visibility = U_MACOS_DISPLAY_HOST_HIDDEN;
}

static xrt_result_t
macos_host_attach(void *ctx, uint32_t client_id, uint32_t context_id)
{
	struct comp_macos_frontend *cwm = (struct comp_macos_frontend *)ctx;
	if (!comp_macos_remote_layer_supported()) {
		COMP_WARN(cwm->c, "Client %u asked to be hosted, but the remote layer API is unavailable", client_id);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	xrt_result_t xret = XRT_SUCCESS;
	@autoreleasepool {
		pthread_mutex_lock(&cwm->host_mutex);
		struct macos_hosted_layer *entry = macos_host_find_locked(cwm, client_id, true);
		CALayerHost *host = entry != NULL ? comp_macos_remote_layer_create_host(context_id) : nil;
		if (entry == NULL || host == nil) {
			xret = XRT_ERROR_ALLOCATION;
		} else {
			[CATransaction begin];
			[CATransaction setDisableActions:YES];
			if (entry->host != nil) {
				macos_host_remove_locked(entry);
			}
			host.hidden = YES;
			// Added after the presenter's layer, so it is drawn above it.
			[cwm->root_layer addSublayer:host];
			entry->client_id = client_id;
			entry->host = host;
			entry->visibility = U_MACOS_DISPLAY_HOST_HIDDEN;
			macos_host_update_service_layer_locked(cwm);
			[CATransaction commit];
			[CATransaction flush];
		}
		pthread_mutex_unlock(&cwm->host_mutex);
	}

	if (xret == XRT_SUCCESS) {
		COMP_INFO(cwm->c, "Hosting client %u (CAContext %u) in the headset window", client_id, context_id);
	} else {
		COMP_ERROR(cwm->c, "Could not host client %u (CAContext %u)", client_id, context_id);
	}
	return xret;
}

static xrt_result_t
macos_host_set_visibility(void *ctx, uint32_t client_id, enum u_macos_display_host_visibility visibility)
{
	struct comp_macos_frontend *cwm = (struct comp_macos_frontend *)ctx;
	xrt_result_t xret = XRT_SUCCESS;
	@autoreleasepool {
		pthread_mutex_lock(&cwm->host_mutex);
		struct macos_hosted_layer *entry = macos_host_find_locked(cwm, client_id, false);
		if (entry == NULL) {
			xret = XRT_ERROR_INVALID_ARGUMENT;
		} else {
			[CATransaction begin];
			[CATransaction setDisableActions:YES];
			if (entry->visibility == U_MACOS_DISPLAY_HOST_HIDDEN &&
			    visibility != U_MACOS_DISPLAY_HOST_HIDDEN) {
				// The client being shown is the one taking over: draw it above any other.
				[entry->host retain];
				[entry->host removeFromSuperlayer];
				[cwm->root_layer addSublayer:entry->host];
				[entry->host release];
			}
			if (entry->visibility != visibility) {
				COMP_INFO(cwm->c, "Hosted client %u visibility %d -> %d", client_id,
				          (int)entry->visibility, (int)visibility);
			}
			entry->host.hidden = visibility == U_MACOS_DISPLAY_HOST_HIDDEN;
			entry->visibility = visibility;
			macos_host_update_service_layer_locked(cwm);
			[CATransaction commit];
			[CATransaction flush];
		}
		pthread_mutex_unlock(&cwm->host_mutex);
	}
	return xret;
}

static void
macos_host_detach(void *ctx, uint32_t client_id)
{
	struct comp_macos_frontend *cwm = (struct comp_macos_frontend *)ctx;
	@autoreleasepool {
		pthread_mutex_lock(&cwm->host_mutex);
		struct macos_hosted_layer *entry = macos_host_find_locked(cwm, client_id, false);
		if (entry != NULL) {
			[CATransaction begin];
			[CATransaction setDisableActions:YES];
			macos_host_remove_locked(entry);
			macos_host_update_service_layer_locked(cwm);
			[CATransaction commit];
			[CATransaction flush];
			COMP_INFO(cwm->c, "Stopped hosting client %u", client_id);
		}
		pthread_mutex_unlock(&cwm->host_mutex);
	}
}

static const struct u_macos_display_host_ops macos_display_host_ops = {
    .attach = macos_host_attach,
    .set_visibility = macos_host_set_visibility,
    .detach = macos_host_detach,
};

static void
macos_window_frontend_show(struct comp_macos_frontend *cwm)
{
	NSWindow *window = cwm->window;
	if (cwm->desktop) {
		// An ordinary window that takes the trackpad and keyboard.
		[window setAcceptsMouseMovedEvents:YES];
		[window makeKeyAndOrderFront:nil];
		[window makeFirstResponder:[window contentView]];
		[NSApp activateIgnoringOtherApps:YES];
		[CATransaction flush];
		u_macos_display_host_register(&macos_display_host_ops, cwm);
		cwm->host_registered = true;
		return;
	}
	[window setBackgroundColor:[NSColor blackColor]];
	[window setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
	                              NSWindowCollectionBehaviorFullScreenAuxiliary |
	                              NSWindowCollectionBehaviorStationary];
	[window setHasShadow:NO];
	[window setHidesOnDeactivate:NO];
	[window setIgnoresMouseEvents:YES];
	[window setLevel:NSMainMenuWindowLevel + 1];
	[window setFrame:[cwm->screen frame] display:YES];
	[window orderFrontRegardless];
	[NSApp activateIgnoringOtherApps:YES];
	[CATransaction flush];

	// The window can now show layers hosted from client processes.
	u_macos_display_host_register(&macos_display_host_ops, cwm);
	cwm->host_registered = true;
}

static void
macos_window_frontend_set_title(struct comp_macos_frontend *cwm, const char *title)
{
	NSString *text = [NSString stringWithUTF8String:title];
	if (cwm->desktop) {
		text = [text
		    stringByAppendingString:@" - click to look, two fingers to move, R resets, Esc frees the pointer"];
	}
	[cwm->window setTitle:text];
}

static bool
macos_window_frontend_is_visible(struct comp_macos_frontend *cwm)
{
	return [cwm->window isVisible];
}

static void
macos_window_frontend_destroy(struct comp_macos_frontend *cwm)
{
	if (cwm->host_registered) {
		// Waits for any host call in progress.
		u_macos_display_host_unregister(cwm);
		cwm->host_registered = false;
	}
	pthread_mutex_lock(&cwm->host_mutex);
	for (uint32_t i = 0; i < MACOS_MAX_HOSTED_LAYERS; i++) {
		if (cwm->hosted[i].host != nil) {
			macos_host_remove_locked(&cwm->hosted[i]);
		}
	}
	pthread_mutex_unlock(&cwm->host_mutex);

	if (cwm->desktop) {
		[(MonadoDesktopView *)[cwm->window contentView] releasePointer];
	}
	[cwm->window orderOut:nil];
	[cwm->window close];
	[cwm->window release];
	cwm->window = nil;
	[cwm->root_layer release];
	cwm->root_layer = nil;
	[cwm->screen release];
	cwm->screen = nil;
}

static const struct macos_frontend_ops macos_window_frontend = {
    .name = "window",
    .create = macos_window_frontend_create,
    .show = macos_window_frontend_show,
    .set_title = macos_window_frontend_set_title,
    .is_visible = macos_window_frontend_is_visible,
    .destroy = macos_window_frontend_destroy,
};


/*
 *
 * Hosted front-end: the presenter runs in a client process and its layer is
 * shown by the service in the headset window.
 *
 * The layer sits in a container on a CAContext; the service shows the context
 * through a CALayerHost (u_macos_display_host). This process's NSApp belongs
 * to the application, so nothing here touches it. Selected when the IPC client
 * passes an owned u_macos_hosted_client. See doc/macos-client-compositor-design.md.
 *
 */

static bool
macos_hosted_frontend_create(struct comp_macos_frontend *cwm)
{
	struct comp_compositor *c = cwm->c;

	if (!comp_macos_remote_layer_supported()) {
		COMP_ERROR(c, "Hosted presentation needs the remote layer API, which is unavailable");
		return false;
	}

	NSScreen *screen = macos_frontend_select_display(cwm);
	if (screen == nil) {
		return false;
	}
	CAMetalLayer *metal_layer = macos_frontend_create_metal_layer(cwm, screen);
	if (metal_layer == nil) {
		return false;
	}
	NSSize size = cwm->size;
	CGRect bounds = CGRectMake(0, 0, size.width, size.height);

	[CATransaction begin];
	[CATransaction setDisableActions:YES];

	// Hidden until the service has unhidden its host layer (arm, then show).
	CALayer *container = [[CALayer alloc] init];
	[container setAnchorPoint:CGPointZero];
	[container setFrame:bounds];
	[container setHidden:YES];
	[container addSublayer:metal_layer];

	CAContext *context = comp_macos_remote_layer_create_context(container);

	[CATransaction commit];
	[CATransaction flush];

	if (context == nil) {
		[container release];
		[metal_layer release];
		COMP_ERROR(c, "Could not create a CAContext for hosted presentation");
		return false;
	}

	xrt_result_t xret = u_macos_hosted_client_attach(cwm->hosted_client, context.contextId);
	if (xret != XRT_SUCCESS) {
		context.layer = nil;
		[context release];
		[container release];
		[metal_layer release];
		COMP_ERROR(c, "The service did not accept the hosted layer (%d)", (int)xret);
		return false;
	}

	cwm->hosted_container = container;
	cwm->hosted_context = context;
	cwm->metal_layer = metal_layer;
	COMP_INFO(c, "Presenting through the service's headset window (CAContext %u)", context.contextId);
	return true;
}

static void
macos_hosted_frontend_show(struct comp_macos_frontend *cwm)
{
	// The service shows this content only while its session is active.
	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	[cwm->hosted_container setHidden:NO];
	[CATransaction commit];
	[CATransaction flush];
}

static void
macos_hosted_frontend_set_title(struct comp_macos_frontend *cwm, const char *title)
{
	// The service owns the window.
	(void)cwm;
	(void)title;
}

static bool
macos_hosted_frontend_is_visible(struct comp_macos_frontend *cwm)
{
	return cwm->hosted_container != nil && ![cwm->hosted_container isHidden];
}

static void
macos_hosted_frontend_destroy(struct comp_macos_frontend *cwm)
{
	if (cwm->hosted_context == nil) {
		return;
	}
	// The service restores its own layer when the host goes.
	u_macos_hosted_client_detach(cwm->hosted_client);
	cwm->hosted_context.layer = nil;
	[cwm->hosted_context release];
	cwm->hosted_context = nil;
	[cwm->hosted_container release];
	cwm->hosted_container = nil;
}

static const struct macos_frontend_ops macos_hosted_frontend = {
    .name = "hosted",
    .create = macos_hosted_frontend_create,
    .show = macos_hosted_frontend_show,
    .set_title = macos_hosted_frontend_set_title,
    .is_visible = macos_hosted_frontend_is_visible,
    .destroy = macos_hosted_frontend_destroy,
};


/*
 *
 * Presenter setup: configures whatever layer the front-end provides.
 *
 */

struct comp_macos_frontend *
comp_macos_frontend_create(struct comp_compositor *c,
                           struct u_macos_hosted_client *client,
                           struct comp_macos_frontend_info *out_info)
{
	struct comp_macos_frontend *frontend = U_TYPED_CALLOC(struct comp_macos_frontend);
	if (frontend == NULL) {
		return NULL;
	}
	if (pthread_mutex_init(&frontend->host_mutex, NULL) != 0) {
		free(frontend);
		return NULL;
	}
	frontend->c = c;
	frontend->hosted_client = client;
	u_macos_hosted_client_reference(client);
	frontend->frontend = client != NULL ? &macos_hosted_frontend : &macos_window_frontend;
	if (!frontend->frontend->create(frontend)) {
		comp_macos_frontend_destroy(&frontend);
		return NULL;
	}
	out_info->metal_layer = frontend->metal_layer;
	out_info->display_id = frontend->display_id;
	out_info->pixel_width = frontend->pixel_width;
	out_info->pixel_height = frontend->pixel_height;
	snprintf(out_info->display_name, sizeof(out_info->display_name), "%s", frontend->display_name);
	return frontend;
}

void
comp_macos_frontend_destroy(struct comp_macos_frontend **frontend_ptr)
{
	struct comp_macos_frontend *frontend = *frontend_ptr;
	if (frontend == NULL) {
		return;
	}
	frontend->frontend->destroy(frontend);
	[frontend->metal_layer release];
	u_macos_hosted_client_release(frontend->hosted_client);
	pthread_mutex_destroy(&frontend->host_mutex);
	free(frontend);
	*frontend_ptr = NULL;
}

void
comp_macos_frontend_show(struct comp_macos_frontend *frontend)
{
	frontend->frontend->show(frontend);
}

void
comp_macos_frontend_set_title(struct comp_macos_frontend *frontend, const char *title)
{
	frontend->frontend->set_title(frontend, title);
}

bool
comp_macos_frontend_is_visible(struct comp_macos_frontend *frontend)
{
	return frontend->frontend->is_visible(frontend);
}

const char *
comp_macos_frontend_name(struct comp_macos_frontend *frontend)
{
	return frontend->frontend->name;
}

uint64_t
comp_macos_frontend_present_ticket(struct comp_macos_frontend *frontend)
{
	return u_macos_hosted_client_present_ticket(frontend->hosted_client);
}

void
comp_macos_frontend_note_presented(struct comp_macos_frontend *frontend, uint64_t ticket)
{
	if (ticket == 0) {
		return;
	}
	MonadoHostedPresentNotification *notification =
	    [[MonadoHostedPresentNotification alloc] initWithClient:frontend->hosted_client ticket:ticket];
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
	  [notification presented];
	});
	[notification release];
}

void
comp_macos_frontend_note_present(struct comp_macos_frontend *frontend, id<MTLDrawable> drawable)
{
	uint64_t ticket = comp_macos_frontend_present_ticket(frontend);
	if (ticket == 0) {
		return;
	}
	// Do not capture frontend: the drawable callback may outlive target teardown.
	MonadoHostedPresentNotification *notification =
	    [[MonadoHostedPresentNotification alloc] initWithClient:frontend->hosted_client ticket:ticket];
	[drawable addPresentedHandler:^(id<MTLDrawable> presented_drawable) {
	  if ([presented_drawable presentedTime] > 0.0) {
		  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		    [notification presented];
		  });
	  }
	}];
	[notification release];
}

bool
comp_macos_frontend_detect(void)
{
	@autoreleasepool {
		return u_desktop_pose_enabled() || find_psvr2_screen(NULL) != nil;
	}
}

bool
comp_macos_frontend_is_desktop(struct comp_macos_frontend *frontend)
{
	return frontend->desktop;
}
