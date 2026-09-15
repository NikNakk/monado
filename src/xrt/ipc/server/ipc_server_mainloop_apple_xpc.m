// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Add direct Metal XPC lifecycle to the macOS IPC server main loop.
 * @ingroup ipc_server
 */

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <dispatch/dispatch.h>

#include "server/ipc_server.h"
#include "shared/ipc_metal_xpc_service.h"
#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_logging.h"

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

DEBUG_GET_ONCE_BOOL_OPTION(macos_exit_on_display_loss, "XRT_MACOS_EXIT_ON_DISPLAY_LOSS", false)
DEBUG_GET_ONCE_NUM_OPTION(macos_display_loss_delay_ms, "XRT_MACOS_DISPLAY_LOSS_DELAY_MS", 3000)
DEBUG_GET_ONCE_NUM_OPTION(macos_display_loss_shutdown_watchdog_ms, "XRT_MACOS_DISPLAY_LOSS_SHUTDOWN_WATCHDOG_MS", 5000)

/*
 * ipc_server_mainloop_apple.c is compiled with source-local symbol redirects
 * for init/poll/deinit. Keep the established socket, signal, and AppKit event
 * implementation there and add only launchd/XPC lifecycle policy here.
 */
int
ipc_server_mainloop_init_apple_vanilla(struct ipc_server_mainloop *ml, bool no_stdin);

void
ipc_server_mainloop_poll_apple_vanilla(struct ipc_server *vs, struct ipc_server_mainloop *ml);

void
ipc_server_mainloop_deinit_apple_vanilla(struct ipc_server_mainloop *ml);

/*
 * The macOS compositor window is intentionally distinctive: borderless,
 * ignores input, and sits above the menu-bar window level. Track that window's
 * original display rather than any HMD- or driver-specific identity so display
 * loss handling stays generic for direct-display HMDs.
 */
static NSWindow *g_compositor_window = nil;
static CGDirectDisplayID g_compositor_display_id = kCGNullDirectDisplay;
static uint64_t g_display_loss_since_ns = 0;
static bool g_display_loss_hidden = false;
static bool g_display_loss_shutdown_requested = false;

static CGDirectDisplayID
screen_display_id(NSScreen *screen)
{
	if (screen == nil) {
		return kCGNullDirectDisplay;
	}
	NSNumber *number = [[screen deviceDescription] objectForKey:@"NSScreenNumber"];
	return number != nil ? (CGDirectDisplayID)[number unsignedIntValue] : kCGNullDirectDisplay;
}

static NSScreen *
find_screen_for_display_id(CGDirectDisplayID display_id)
{
	for (NSScreen *screen in [NSScreen screens]) {
		if (screen_display_id(screen) == display_id) {
			return screen;
		}
	}
	return nil;
}

static bool
is_compositor_window(NSWindow *window)
{
	if (window == nil) {
		return false;
	}

	return [window styleMask] == NSWindowStyleMaskBorderless && [window ignoresMouseEvents] &&
	       [window level] == NSMainMenuWindowLevel + 1;
}

static void
reset_compositor_window_tracking(void)
{
	[g_compositor_window release];
	g_compositor_window = nil;
	g_compositor_display_id = kCGNullDirectDisplay;
	g_display_loss_since_ns = 0;
	g_display_loss_hidden = false;
	g_display_loss_shutdown_requested = false;
}

static void
discover_compositor_window(void)
{
	if (NSApp == nil) {
		return;
	}

	if (g_compositor_window != nil) {
		if ([[NSApp windows] containsObject:g_compositor_window]) {
			return;
		}
		reset_compositor_window_tracking();
	}

	for (NSWindow *window in [NSApp windows]) {
		if (!is_compositor_window(window)) {
			continue;
		}

		CGDirectDisplayID display_id = screen_display_id([window screen]);
		if (display_id == kCGNullDirectDisplay) {
			continue;
		}

		g_compositor_window = [window retain];
		g_compositor_display_id = display_id;
		U_LOG_I("Tracking macOS compositor window on display id=%u (service pid=%d)", (unsigned)display_id,
		        (int)getpid());
		return;
	}
}

static bool
compositor_display_is_available(void)
{
	if (g_compositor_display_id == kCGNullDirectDisplay) {
		return true;
	}

	/*
	 * Online catches hot-unplug. Asleep catches the common HMD sleep case where
	 * the display is still enumerated but is no longer drawable. We deliberately
	 * do not use device presence/wear state: taking a headset off must not stop
	 * the runtime.
	 */
	return CGDisplayIsOnline(g_compositor_display_id) && !CGDisplayIsAsleep(g_compositor_display_id);
}

static uint64_t
get_display_loss_delay_ns(void)
{
	int64_t delay_ms = debug_get_num_option_macos_display_loss_delay_ms();
	if (delay_ms < 0) {
		delay_ms = 0;
	}
	return (uint64_t)delay_ms * 1000000ULL;
}

static void
schedule_display_loss_shutdown_watchdog(void)
{
	int64_t watchdog_ms = debug_get_num_option_macos_display_loss_shutdown_watchdog_ms();
	if (watchdog_ms <= 0) {
		return;
	}

	pid_t pid = getpid();
	dispatch_time_t when = dispatch_time(DISPATCH_TIME_NOW, watchdog_ms * (int64_t)NSEC_PER_MSEC);
	dispatch_after(when, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		/*
		 * Reaching this block means normal shutdown did not terminate the
		 * process in time. The service is launchd-disposable, and process exit
		 * is safer than leaving a Metal/CAMetalLayer teardown indefinitely
		 * wedged after its original display has disappeared.
		 */
		fprintf(stderr,
		        "ERROR: macOS display-loss shutdown watchdog expired after %lld ms for monado-service pid=%d; "
		        "forcing process exit\n",
		        (long long)watchdog_ms, (int)pid);
		fflush(stderr);
		_exit(0);
	});
}

static void
handle_display_recovered(void)
{
	if (!g_display_loss_hidden || g_compositor_window == nil) {
		g_display_loss_since_ns = 0;
		g_display_loss_shutdown_requested = false;
		return;
	}

	NSScreen *screen = find_screen_for_display_id(g_compositor_display_id);
	if (screen != nil) {
		[g_compositor_window setFrame:[screen frame] display:YES];
		[g_compositor_window orderFrontRegardless];
		U_LOG_I("Selected macOS compositor display id=%u recovered before shutdown; restoring compositor window",
		        (unsigned)g_compositor_display_id);
	}

	g_display_loss_since_ns = 0;
	g_display_loss_hidden = false;
	g_display_loss_shutdown_requested = false;
}

static void
poll_compositor_display_lifecycle(struct ipc_server *vs)
{
	@autoreleasepool {
		discover_compositor_window();
		if (g_compositor_window == nil || g_compositor_display_id == kCGNullDirectDisplay) {
			return;
		}

		if (compositor_display_is_available()) {
			handle_display_recovered();
			return;
		}

		uint64_t now_ns = os_monotonic_get_ns();
		if (g_display_loss_since_ns == 0) {
			g_display_loss_since_ns = now_ns;
			[g_compositor_window orderOut:nil];
			g_display_loss_hidden = true;
			U_LOG_W("Selected macOS compositor display id=%u is unavailable; hiding compositor window immediately",
			        (unsigned)g_compositor_display_id);
		}

		if (!debug_get_bool_option_macos_exit_on_display_loss() || g_display_loss_shutdown_requested) {
			return;
		}

		uint64_t delay_ns = get_display_loss_delay_ns();
		if (now_ns - g_display_loss_since_ns < delay_ns) {
			return;
		}

		/*
		 * If there are no clients, the ordinary IPC_EXIT_WHEN_IDLE policy owns
		 * shutdown. Avoid racing its delayed-exit thread during teardown. With a
		 * connected launcher/game, display loss is the independent reason to stop.
		 */
		os_mutex_lock(&vs->global_state.lock);
		uint32_t connected_clients = vs->global_state.connected_client_count;
		bool idle_exit_enabled = vs->exit_when_idle;
		os_mutex_unlock(&vs->global_state.lock);

		g_display_loss_shutdown_requested = true;
		if (connected_clients == 0 && idle_exit_enabled) {
			U_LOG_I("Selected macOS compositor display remains unavailable; idle-exit policy will stop the service");
			return;
		}

		U_LOG_I("Selected macOS compositor display remains unavailable for %llu ms; requesting service shutdown (pid=%d)",
		        (unsigned long long)(delay_ns / 1000000ULL), (int)getpid());
		schedule_display_loss_shutdown_watchdog();
		ipc_server_handle_shutdown_signal(vs);
	}
}

int
ipc_server_mainloop_init(struct ipc_server_mainloop *ml, bool no_stdin)
{
	reset_compositor_window_tracking();

	int ret = ipc_server_mainloop_init_apple_vanilla(ml, no_stdin);
	if (ret < 0) {
		return ret;
	}

	/*
	 * The Unix socket is already listening at this point. The XPC activation
	 * reply therefore acts as a readiness barrier before a client retries its
	 * ordinary Monado IPC connection.
	 *
	 * Keep failure non-fatal so manually-started development services can still
	 * use the legacy standalone broker while this direct path is being tested.
	 */
	xrt_result_t xret = ipc_metal_xpc_service_start();
	if (xret != XRT_SUCCESS) {
		U_LOG_W("Direct Metal XPC endpoint unavailable; continuing with ordinary Monado IPC");
	}

	return ret;
}

void
ipc_server_mainloop_poll(struct ipc_server *vs, struct ipc_server_mainloop *ml)
{
	ipc_server_mainloop_poll_apple_vanilla(vs, ml);
	poll_compositor_display_lifecycle(vs);
}

void
ipc_server_mainloop_deinit(struct ipc_server_mainloop *ml)
{
	reset_compositor_window_tracking();
	ipc_metal_xpc_service_stop();
	ipc_server_mainloop_deinit_apple_vanilla(ml);
}
