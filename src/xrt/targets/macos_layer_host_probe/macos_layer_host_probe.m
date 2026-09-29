// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Standalone probe: does cross-process CALayerHost hosting change
 *        CAMetalLayer present timing on the headset display, and does it
 *        keep that timing when Game Mode throttles the hosting process?
 *
 * All modes present the same way as Monado's legacy headset window
 * (CVDisplayLink pacing, three drawables, afterMinimumDuration by default).
 *
 * Timing modes, run from a terminal:
 *
 *  - direct:       this process renders into its own window's CAMetalLayer.
 *  - hosted-local: this process renders into a CAContext and shows it through
 *                  a CALayerHost in its own window (hosting cost only).
 *  - hosted:       a spawned child process renders into a CAContext; this
 *                  process shows it through a CALayerHost (cross-process).
 *
 * Game Mode modes, with the host started by launchd (bootstrap-host) like
 * monado-service and the game half run as a games-category app bundle:
 *
 *  - game-direct:  the host renders to the headset, as monado-service does
 *                  today; the game only goes fullscreen and loads the CPU.
 *  - game-hosted:  the game renders into a CAContext; the host shows it
 *                  through a CALayerHost.
 *
 * Handoff mode, run from a terminal:
 *
 *  - handoff:      two spawned clients render continuously into their own
 *                  CAContexts; the host swaps which CALayerHost is shown every
 *                  few seconds in one CATransaction, optionally while the host
 *                  is Darwin-backgrounded like monado-service under Game Mode,
 *                  and measures the on-screen gap at each swap. With
 *                  --swap-method client the host never commits after setup:
 *                  the clients show and hide their own layers.
 *
 * In the Game Mode modes the host also runs a realtime canary thread that
 * logs its own scheduling priority and both processes' Darwin-background
 * state, so throttling of the host is visible directly.
 *
 * The rendering process records presentedTime for every drawable against the
 * CVDisplayLink vblank it was paced to, writes a CSV and prints a summary.
 *
 * CAContext and CALayerHost are private QuartzCore API. They are declared and
 * checked at runtime exactly as Chromium does in ui/base/cocoa/remote_layer_api.
 */

#import <AppKit/AppKit.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <math.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <objc/runtime.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

// Private libproc / getpriority values, from XNU's proc_info_private.h and
// resource_private.h. Only read, never set.
#ifndef PROC_FLAG_DARWINBG
#define PROC_FLAG_DARWINBG 0x8000
#endif
#ifndef PROC_FLAG_EXT_DARWINBG
#define PROC_FLAG_EXT_DARWINBG 0x10000
#endif
#ifndef PROC_FLAG_ADAPTIVE
#define PROC_FLAG_ADAPTIVE 0x100000
#endif
#ifndef PROC_FLAG_ADAPTIVE_IMPORTANT
#define PROC_FLAG_ADAPTIVE_IMPORTANT 0x200000
#endif
#ifndef PROC_FLAG_APPLICATION
#define PROC_FLAG_APPLICATION 0x1000000
#endif
#ifndef PRIO_DARWIN_ROLE
#define PRIO_DARWIN_ROLE 6
#endif
#ifndef PRIO_DARWIN_GAME_MODE
#define PRIO_DARWIN_GAME_MODE 7
#endif

#define PROBE_SOCKET_PATH "/tmp/monado-layer-host-probe.sock"
#define PROBE_LAUNCHD_LABEL "org.freedesktop.monado.layer-host-probe"
#define PROBE_HOST_LOG "/tmp/layer_host_probe_host.log"
#define PROBE_GAME_LOG "/tmp/layer_host_probe_game.log"
#define PROBE_MAGIC 0x4d4c4850u


/*
 *
 * Private QuartzCore interfaces, as declared by Chromium.
 *
 */

typedef uint32_t CGSConnectionID;
typedef uint32_t CAContextID;

@interface CAContext : NSObject
+ (instancetype)contextWithCGSConnection:(CAContextID)contextId options:(NSDictionary *)optionsDict;
@property(readonly) CAContextID contextId;
@property(retain) CALayer *layer;
@end

@interface CALayerHost : CALayer
@property CAContextID contextId;
@end

typedef CGSConnectionID (*cgs_main_connection_id_fn)(void);

//! Mirrors Chromium's ui::RemoteLayerAPISupported().
static bool
remote_layer_api_supported(void)
{
	Class context_class = NSClassFromString(@"CAContext");
	if (context_class == nil ||
	    ![context_class respondsToSelector:@selector(contextWithCGSConnection:options:)] ||
	    class_getProperty(context_class, "contextId") == NULL || class_getProperty(context_class, "layer") == NULL) {
		return false;
	}

	Class host_class = NSClassFromString(@"CALayerHost");
	if (host_class == nil || ![host_class instancesRespondToSelector:@selector(contextId)] ||
	    ![host_class instancesRespondToSelector:@selector(setContextId:)]) {
		return false;
	}

	return dlsym(RTLD_DEFAULT, "CGSMainConnectionID") != NULL;
}

static CAContext *
create_remote_context(CALayer *layer)
{
	cgs_main_connection_id_fn main_connection_id =
	    (cgs_main_connection_id_fn)dlsym(RTLD_DEFAULT, "CGSMainConnectionID");
	Class context_class = NSClassFromString(@"CAContext");
	CAContext *context = [context_class contextWithCGSConnection:main_connection_id() options:@{}];
	context.layer = layer;
	return context;
}

static CALayerHost *
create_layer_host(CAContextID context_id)
{
	CALayerHost *host = [[NSClassFromString(@"CALayerHost") alloc] init];
	host.anchorPoint = CGPointZero;
	host.position = CGPointZero;
	host.contextId = context_id;
	return host;
}


/*
 *
 * Options.
 *
 */

enum probe_role
{
	PROBE_ROLE_HOST,
	PROBE_ROLE_CLIENT,
	PROBE_ROLE_GAME,
	PROBE_ROLE_BOOTSTRAP_HOST,
	PROBE_ROLE_BOOTOUT_HOST,
	PROBE_ROLE_QUERY,
};

enum probe_mode
{
	PROBE_MODE_DIRECT,
	PROBE_MODE_HOSTED_LOCAL,
	PROBE_MODE_HOSTED,
	PROBE_MODE_GAME_DIRECT,
	PROBE_MODE_GAME_HOSTED,
	PROBE_MODE_HANDOFF,
};

enum swap_method
{
	SWAP_REPARENT,
	SWAP_HIDDEN,
	SWAP_CLIENT,
};

enum present_mode
{
	PRESENT_MIN_DURATION,
	PRESENT_AT_TIME,
	PRESENT_IMMEDIATE,
};

struct probe_options
{
	enum probe_role role;
	enum probe_mode mode;
	enum present_mode present;
	int display_index;
	double seconds;
	double min_duration_us;
	const char *out_prefix;
	bool realtime;

	// Latency guard: drop a frame to drain a present queue stuck deep.
	bool latency_guard;
	double guard_window_ms;
	double guard_min_interval_ms;

	// Game Mode test.
	int cpu_load;
	double warmup;
	const char *process_type;
	pid_t query_pid;

	// Handoff test.
	double swap_every;
	enum swap_method swap_method;
	bool host_background;
	int tint;        //!< Client only: 1 = client A (red), 2 = client B (blue).
	int peer_fd;     //!< Client only, SWAP_CLIENT: socket to the other client.
	double epoch_s;  //!< Client only, SWAP_CLIENT: swap k is at epoch + (k + 1) * swap_every.

	// Passed from host to client only.
	CGDirectDisplayID display_id;
	double width_points;
	double height_points;
	double scale;
	int context_fd;
};

static const char *
mode_name(enum probe_mode mode)
{
	switch (mode) {
	case PROBE_MODE_DIRECT: return "direct";
	case PROBE_MODE_HOSTED_LOCAL: return "hosted-local";
	case PROBE_MODE_HOSTED: return "hosted";
	case PROBE_MODE_GAME_DIRECT: return "game-direct";
	case PROBE_MODE_GAME_HOSTED: return "game-hosted";
	case PROBE_MODE_HANDOFF: return "handoff";
	}
	return "unknown";
}

static bool
mode_is_game(enum probe_mode mode)
{
	return mode == PROBE_MODE_GAME_DIRECT || mode == PROBE_MODE_GAME_HOSTED;
}

static const char *
present_name(enum present_mode present)
{
	switch (present) {
	case PRESENT_MIN_DURATION: return "min-duration";
	case PRESENT_AT_TIME: return "at-time";
	case PRESENT_IMMEDIATE: return "immediate";
	}
	return "unknown";
}

static const char *
swap_method_name(enum swap_method method)
{
	switch (method) {
	case SWAP_REPARENT: return "reparent";
	case SWAP_HIDDEN: return "hidden";
	case SWAP_CLIENT: return "client";
	}
	return "unknown";
}

static void
print_usage(const char *argv0)
{
	fprintf(stderr,
	        "Usage: %s [--mode MODE] [--display N] [--seconds S]\n"
	        "          [--present min-duration|at-time|immediate] [--min-duration-us US]\n"
	        "          [--out PREFIX] [--rt 0|1]\n"
	        "       %s --role bootstrap-host --mode game-direct|game-hosted [host options]\n"
	        "          [--process-type Interactive|Adaptive]\n"
	        "       %s --role bootout-host\n"
	        "       %s --role query --pid PID\n"
	        "\n"
	        "  MODE          direct, hosted-local, hosted (timing, run from a terminal)\n"
	        "                handoff (swap between two hosted clients, run from a terminal)\n"
	        "                game-direct, game-hosted (Game Mode test, host via bootstrap-host)\n"
	        "  --display N   index into NSScreen.screens (default: last screen, usually the headset)\n"
	        "  --seconds S   measurement length (default 20); in the Game Mode test the host\n"
	        "                renders for at most S and should outlast the game\n"
	        "  --out PREFIX  CSV path prefix (default /tmp/layer_host_probe)\n"
	        "  --rt 0|1      realtime (time-constraint) render thread, as Monado's compositor (default 1)\n"
	        "  --latency-guard 0|1        skip a frame when presents have been 2+ periods late for\n"
	        "                             --guard-window-ms (default 250), at most every\n"
	        "                             --guard-min-interval-ms (default 500) (default 0)\n"
	        "\n"
	        "Handoff:\n"
	        "  --swap-every S           seconds between swaps (default 2)\n"
	        "  --swap-method M          reparent (host removes/adds, as Chromium), hidden (host toggles\n"
	        "                           hidden) or client (clients show/hide their own layers; the host\n"
	        "                           never commits after setup) (default reparent)\n"
	        "  --host-background 0|1    Darwin-background the host, as Game Mode does (default 0)\n"
	        "  --cpu-load N             busy threads in each client (default 0)\n"
	        "\n"
	        "Game half (macos-layer-host-probe-game.app, launched with open --args):\n"
	        "  --seconds S   how long to render or load the CPU (default 20)\n"
	        "  --warmup S    wait before connecting, so Game Mode can engage (default 5)\n"
	        "  --cpu-load N  busy threads at user-interactive QoS (default 0)\n",
	        argv0, argv0, argv0, argv0);
}

static bool
parse_options(int argc, char **argv, struct probe_options *opts)
{
	*opts = (struct probe_options){
#ifdef LAYER_HOST_PROBE_GAME_BUNDLE
	    .role = PROBE_ROLE_GAME,
#else
	    .role = PROBE_ROLE_HOST,
#endif
	    .mode = PROBE_MODE_DIRECT,
	    .present = PRESENT_MIN_DURATION,
	    .display_index = -1,
	    .seconds = 20.0,
	    .min_duration_us = 8000.0,
	    .out_prefix = "/tmp/layer_host_probe",
	    .realtime = true,
	    .guard_window_ms = 250.0,
	    .guard_min_interval_ms = 500.0,
	    .warmup = 5.0,
	    .process_type = "Interactive",
	    .swap_every = 2.0,
	    .swap_method = SWAP_REPARENT,
	    .peer_fd = -1,
	    .context_fd = -1,
	};

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;

		// LaunchServices may pass a process serial number to bundled apps.
		if (strncmp(arg, "-psn_", 5) == 0) {
			continue;
		}
		if (value == NULL) {
			return false;
		}

		if (strcmp(arg, "--role") == 0) {
			if (strcmp(value, "host") == 0) {
				opts->role = PROBE_ROLE_HOST;
			} else if (strcmp(value, "client") == 0) {
				opts->role = PROBE_ROLE_CLIENT;
			} else if (strcmp(value, "game") == 0) {
				opts->role = PROBE_ROLE_GAME;
			} else if (strcmp(value, "bootstrap-host") == 0) {
				opts->role = PROBE_ROLE_BOOTSTRAP_HOST;
			} else if (strcmp(value, "bootout-host") == 0) {
				opts->role = PROBE_ROLE_BOOTOUT_HOST;
			} else if (strcmp(value, "query") == 0) {
				opts->role = PROBE_ROLE_QUERY;
			} else {
				return false;
			}
		} else if (strcmp(arg, "--mode") == 0) {
			if (strcmp(value, "direct") == 0) {
				opts->mode = PROBE_MODE_DIRECT;
			} else if (strcmp(value, "hosted-local") == 0) {
				opts->mode = PROBE_MODE_HOSTED_LOCAL;
			} else if (strcmp(value, "hosted") == 0) {
				opts->mode = PROBE_MODE_HOSTED;
			} else if (strcmp(value, "game-direct") == 0) {
				opts->mode = PROBE_MODE_GAME_DIRECT;
			} else if (strcmp(value, "game-hosted") == 0) {
				opts->mode = PROBE_MODE_GAME_HOSTED;
			} else if (strcmp(value, "handoff") == 0) {
				opts->mode = PROBE_MODE_HANDOFF;
			} else {
				return false;
			}
		} else if (strcmp(arg, "--present") == 0) {
			if (strcmp(value, "min-duration") == 0) {
				opts->present = PRESENT_MIN_DURATION;
			} else if (strcmp(value, "at-time") == 0) {
				opts->present = PRESENT_AT_TIME;
			} else if (strcmp(value, "immediate") == 0) {
				opts->present = PRESENT_IMMEDIATE;
			} else {
				return false;
			}
		} else if (strcmp(arg, "--display") == 0) {
			opts->display_index = atoi(value);
		} else if (strcmp(arg, "--seconds") == 0) {
			opts->seconds = atof(value);
		} else if (strcmp(arg, "--min-duration-us") == 0) {
			opts->min_duration_us = atof(value);
		} else if (strcmp(arg, "--out") == 0) {
			opts->out_prefix = value;
		} else if (strcmp(arg, "--rt") == 0) {
			opts->realtime = atoi(value) != 0;
		} else if (strcmp(arg, "--latency-guard") == 0) {
			opts->latency_guard = atoi(value) != 0;
		} else if (strcmp(arg, "--guard-window-ms") == 0) {
			opts->guard_window_ms = atof(value);
		} else if (strcmp(arg, "--guard-min-interval-ms") == 0) {
			opts->guard_min_interval_ms = atof(value);
		} else if (strcmp(arg, "--cpu-load") == 0) {
			opts->cpu_load = atoi(value);
		} else if (strcmp(arg, "--warmup") == 0) {
			opts->warmup = atof(value);
		} else if (strcmp(arg, "--process-type") == 0) {
			if (strcmp(value, "Interactive") != 0 && strcmp(value, "Adaptive") != 0) {
				return false;
			}
			opts->process_type = value;
		} else if (strcmp(arg, "--swap-every") == 0) {
			opts->swap_every = atof(value);
		} else if (strcmp(arg, "--swap-method") == 0) {
			if (strcmp(value, "reparent") == 0) {
				opts->swap_method = SWAP_REPARENT;
			} else if (strcmp(value, "hidden") == 0) {
				opts->swap_method = SWAP_HIDDEN;
			} else if (strcmp(value, "client") == 0) {
				opts->swap_method = SWAP_CLIENT;
			} else {
				return false;
			}
		} else if (strcmp(arg, "--host-background") == 0) {
			opts->host_background = atoi(value) != 0;
		} else if (strcmp(arg, "--tint") == 0) {
			opts->tint = atoi(value);
		} else if (strcmp(arg, "--peer-fd") == 0) {
			opts->peer_fd = atoi(value);
		} else if (strcmp(arg, "--epoch") == 0) {
			opts->epoch_s = atof(value);
		} else if (strcmp(arg, "--pid") == 0) {
			opts->query_pid = (pid_t)atoi(value);
		} else if (strcmp(arg, "--display-id") == 0) {
			opts->display_id = (CGDirectDisplayID)strtoul(value, NULL, 10);
		} else if (strcmp(arg, "--width") == 0) {
			opts->width_points = atof(value);
		} else if (strcmp(arg, "--height") == 0) {
			opts->height_points = atof(value);
		} else if (strcmp(arg, "--scale") == 0) {
			opts->scale = atof(value);
		} else if (strcmp(arg, "--context-fd") == 0) {
			opts->context_fd = atoi(value);
		} else {
			return false;
		}

		// Every option takes a value.
		i++;
	}

	return opts->seconds > 0.0 && opts->cpu_load >= 0 && opts->warmup >= 0.0 && opts->swap_every > 0.0;
}


/*
 *
 * Timing helpers.
 *
 */

static mach_timebase_info_data_t g_timebase;

static double
mach_to_seconds(uint64_t ticks)
{
	return (double)ticks * (double)g_timebase.numer / (double)g_timebase.denom / 1e9;
}

static uint64_t
seconds_to_mach(double seconds)
{
	return (uint64_t)(seconds * 1e9 * (double)g_timebase.denom / (double)g_timebase.numer);
}

static double
now_seconds(void)
{
	// Same clock as CACurrentMediaTime() and MTLDrawable.presentedTime.
	return mach_to_seconds(mach_absolute_time());
}

static int
compare_doubles(const void *a, const void *b)
{
	double da = *(const double *)a;
	double db = *(const double *)b;
	return (da > db) - (da < db);
}

static double
percentile(double *sorted, size_t count, double pct)
{
	if (count == 0) {
		return 0.0;
	}
	size_t index = (size_t)(pct / 100.0 * (double)(count - 1) + 0.5);
	return sorted[index < count ? index : count - 1];
}


/*
 *
 * Scheduling and process-policy helpers.
 *
 */

//! Same time-constraint shape as Monado's compositor thread (35 % / 70 %).
static bool
set_thread_realtime(double period_s)
{
	uint64_t period = seconds_to_mach(period_s);
	thread_time_constraint_policy_data_t policy = {
	    .period = (uint32_t)period,
	    .computation = (uint32_t)(period * 35 / 100),
	    .constraint = (uint32_t)(period * 70 / 100),
	    .preemptible = TRUE,
	};
	kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
	                                     (thread_policy_t)&policy, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	return kr == KERN_SUCCESS;
}

//! Current scheduled priority: 97 when realtime, 4 when Darwin-background.
static int
thread_current_priority(void)
{
	thread_extended_info_data_t info;
	mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
	kern_return_t kr =
	    thread_info(pthread_mach_thread_np(pthread_self()), THREAD_EXTENDED_INFO, (thread_info_t)&info, &count);
	return kr == KERN_SUCCESS ? info.pth_curpri : -1;
}

static const char *
priority_class(int priority)
{
	if (priority < 0) {
		return "unknown";
	}
	if (priority >= 80) {
		return "realtime";
	}
	if (priority <= 4) {
		return "throttled";
	}
	return "timeshare";
}

struct policy_snapshot
{
	bool flags_valid;
	uint32_t flags;
	int role;      //!< PRIO_DARWIN_ROLE_*, or -1 if unreadable.
	int game_mode; //!< 1 on, 0 off, -1 unreadable (needs root).
};

static struct policy_snapshot
snapshot_policy(pid_t pid)
{
	struct policy_snapshot snap = {.role = -1, .game_mode = -1};

	struct proc_bsdinfo info;
	if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == (int)sizeof(info)) {
		snap.flags_valid = true;
		snap.flags = info.pbi_flags;
	}

	errno = 0;
	int role = getpriority(PRIO_DARWIN_ROLE, (id_t)pid);
	if (errno == 0) {
		snap.role = role;
	}

	errno = 0;
	int game_mode = getpriority(PRIO_DARWIN_GAME_MODE, (id_t)pid);
	if (errno == 0) {
		snap.game_mode = game_mode;
	}

	return snap;
}

static const char *
darwin_role_name(int role)
{
	switch (role) {
	case -1: return "unreadable";
	case 0: return "default";
	case 1: return "ui-focal";
	case 2: return "ui";
	case 3: return "non-ui";
	case 4: return "ui-non-focal";
	case 5: return "tal-launch";
	case 6: return "darwin-bg";
	case 7: return "user-init";
	}
	return "other";
}

static void
format_policy(char *buf, size_t size, struct policy_snapshot snap)
{
	if (!snap.flags_valid) {
		snprintf(buf, size, "flags=unreadable role=%s", darwin_role_name(snap.role));
		return;
	}
	snprintf(buf, size, "darwinbg=%d ext_darwinbg=%d adaptive=%d adaptive_important=%d app=%d role=%s game_mode=%s",
	         (snap.flags & PROC_FLAG_DARWINBG) != 0, (snap.flags & PROC_FLAG_EXT_DARWINBG) != 0,
	         (snap.flags & PROC_FLAG_ADAPTIVE) != 0, (snap.flags & PROC_FLAG_ADAPTIVE_IMPORTANT) != 0,
	         (snap.flags & PROC_FLAG_APPLICATION) != 0, darwin_role_name(snap.role),
	         snap.game_mode < 0 ? "unreadable" : (snap.game_mode ? "on" : "off"));
}


/*
 *
 * Renderer: paces to CVDisplayLink and presents into a CAMetalLayer.
 *
 */

struct frame_record
{
	double submit_s;
	double target_s;
	_Atomic double presented_s;
	int priority;
};

@interface ProbeRenderer : NSObject
- (instancetype)initWithLayer:(CAMetalLayer *)layer
                    displayID:(CGDirectDisplayID)displayID
                      options:(const struct probe_options *)opts
                         role:(const char *)role;
//! Renders on the calling thread, which must be a dedicated thread.
- (void)runForSeconds:(double)seconds;
- (void)requestStop;
//! presentedTime of the most recently presented drawable, or 0.
- (double)latestPresented;
@end

static CVReturn
display_link_callback(CVDisplayLinkRef link,
                      const CVTimeStamp *now,
                      const CVTimeStamp *output_time,
                      CVOptionFlags flags_in,
                      CVOptionFlags *flags_out,
                      void *ctx);

@implementation ProbeRenderer
{
	CAMetalLayer *_layer;
	id<MTLCommandQueue> _queue;
	id<MTLTexture> _bar;
	CVDisplayLinkRef _displayLink;
	dispatch_semaphore_t _vblank;
	_Atomic uint64_t _nextVblankHostTime;
	_Atomic bool _stopRequested;
	_Atomic double _latestPresented;

	/*
	 * Latency guard. A present normally lands one period after the display
	 * link's output time. After any stall the queue can be left a frame or two
	 * deeper, and one-frame-per-vblank rendering never drains it. The guard
	 * counts consecutive presents at 2+ periods and skips one vblank to drain
	 * the queue once that has lasted long enough. The same idea as GAV's
	 * player (FramePacer / notePresentDelay), implemented independently.
	 */
	_Atomic int _deepPresents;
	double _lastDrainS;
	size_t _drains;
	double _periodSeconds;
	bool _realtime;

	struct probe_options _opts;
	const char *_role;

	struct frame_record *_records;
	size_t _capacity;
	size_t _count;
	size_t _nilDrawables;
}

- (instancetype)initWithLayer:(CAMetalLayer *)layer
                    displayID:(CGDirectDisplayID)displayID
                      options:(const struct probe_options *)opts
                         role:(const char *)role
{
	self = [super init];
	if (self == nil) {
		return nil;
	}

	_layer = layer;
	_opts = *opts;
	_role = role;
	_queue = [layer.device newCommandQueue];
	_vblank = dispatch_semaphore_create(0);

	// A white vertical bar that moves one step per frame makes judder visible.
	NSUInteger bar_width = 64;
	NSUInteger bar_height = (NSUInteger)layer.drawableSize.height;
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                                                width:bar_width
	                                                                               height:bar_height
	                                                                            mipmapped:NO];
	desc.usage = MTLTextureUsageShaderRead;
	desc.storageMode = MTLStorageModeManaged;
	_bar = [layer.device newTextureWithDescriptor:desc];
	size_t bytes = bar_width * bar_height * 4;
	uint8_t *white = malloc(bytes);
	memset(white, 0xff, bytes);
	[_bar replaceRegion:MTLRegionMake2D(0, 0, bar_width, bar_height)
	        mipmapLevel:0
	          withBytes:white
	        bytesPerRow:bar_width * 4];
	free(white);

	CVDisplayLinkCreateWithCGDisplay(displayID, &_displayLink);
	CVDisplayLinkSetOutputCallback(_displayLink, display_link_callback, (__bridge void *)self);
	CVTime period = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(_displayLink);
	_periodSeconds = (period.flags & kCVTimeIsIndefinite) || period.timeScale == 0
	                     ? 1.0 / 120.0
	                     : (double)period.timeValue / (double)period.timeScale;

	_capacity = (size_t)(opts->seconds / _periodSeconds * 1.5) + 64;
	_records = calloc(_capacity, sizeof(struct frame_record));

	return self;
}

- (void)dealloc
{
	if (_displayLink != NULL) {
		CVDisplayLinkRelease(_displayLink);
	}
	// _records is deliberately not freed: presented handlers for drawables that
	// never reached the screen can still fire after rendering stops.
}

- (void)onVblank:(uint64_t)outputHostTime
{
	atomic_store(&_nextVblankHostTime, outputHostTime);
	dispatch_semaphore_signal(_vblank);
}

- (void)requestStop
{
	atomic_store(&_stopRequested, true);
}

- (double)latestPresented
{
	return atomic_load(&_latestPresented);
}

- (void)renderFrame
{
	id<CAMetalDrawable> drawable = [_layer nextDrawable];
	if (drawable == nil) {
		_nilDrawables++;
		return;
	}
	if (_count >= _capacity) {
		return;
	}

	size_t index = _count++;
	struct frame_record *record = &_records[index];
	record->target_s = mach_to_seconds(atomic_load(&_nextVblankHostTime));
	record->priority = thread_current_priority();

	id<MTLCommandBuffer> cmd = [_queue commandBuffer];

	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = drawable.texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	switch (_opts.tint) {
	case 1: pass.colorAttachments[0].clearColor = MTLClearColorMake(0.35, 0.05, 0.05, 1.0); break;
	case 2: pass.colorAttachments[0].clearColor = MTLClearColorMake(0.05, 0.08, 0.35, 1.0); break;
	default: pass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.12, 1.0); break;
	}
	[[cmd renderCommandEncoderWithDescriptor:pass] endEncoding];

	NSUInteger width = drawable.texture.width;
	NSUInteger height = MIN(drawable.texture.height, _bar.height);
	NSUInteger travel = width > _bar.width ? width - _bar.width : 1;
	NSUInteger x = (index * 16) % travel;
	id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
	[blit copyFromTexture:_bar
	          sourceSlice:0
	          sourceLevel:0
	         sourceOrigin:MTLOriginMake(0, 0, 0)
	           sourceSize:MTLSizeMake(_bar.width, height, 1)
	            toTexture:drawable.texture
	     destinationSlice:0
	     destinationLevel:0
	    destinationOrigin:MTLOriginMake(x, 0, 0)];
	[blit endEncoding];

	_Atomic double *latest = &_latestPresented;
	_Atomic int *deep = &_deepPresents;
	double period = _periodSeconds;
	[drawable addPresentedHandler:^(id<MTLDrawable> presented) {
		double t = presented.presentedTime;
		atomic_store(&record->presented_s, t);
		if (t > 0.0) {
			atomic_store(latest, t);
			long periods = lround((t - record->target_s) / period);
			if (periods >= 2) {
				atomic_fetch_add(deep, 1);
			} else {
				atomic_store(deep, 0);
			}
		}
	}];

	record->submit_s = now_seconds();
	switch (_opts.present) {
	case PRESENT_MIN_DURATION: [cmd presentDrawable:drawable afterMinimumDuration:_opts.min_duration_us / 1e6]; break;
	case PRESENT_AT_TIME: [cmd presentDrawable:drawable atTime:record->target_s]; break;
	case PRESENT_IMMEDIATE: [cmd presentDrawable:drawable]; break;
	}
	[cmd commit];
}

- (void)runForSeconds:(double)seconds
{
	if (_opts.realtime) {
		_realtime = set_thread_realtime(_periodSeconds);
		if (!_realtime) {
			fprintf(stderr, "LAYER_HOST_PROBE %s: could not make the render thread realtime\n", _role);
		}
	}

	CVDisplayLinkStart(_displayLink);

	double end = now_seconds() + seconds;
	while (now_seconds() < end && !atomic_load(&_stopRequested)) {
		dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.1 * NSEC_PER_SEC));
		if (dispatch_semaphore_wait(_vblank, timeout) != 0) {
			continue;
		}
		if ([self shouldDrainQueue]) {
			continue;
		}
		@autoreleasepool {
			[self renderFrame];
		}
	}

	CVDisplayLinkStop(_displayLink);

	// Let the last presented handlers arrive.
	[NSThread sleepForTimeInterval:0.5];

	[self writeCsv];
	[self printSummary];
}

//! Latency guard: true to skip this vblank so the present queue drains by one.
- (bool)shouldDrainQueue
{
	if (!_opts.latency_guard) {
		return false;
	}
	int window_frames = (int)lround(_opts.guard_window_ms / 1e3 / _periodSeconds);
	double now = now_seconds();
	if (atomic_load(&_deepPresents) < (window_frames > 1 ? window_frames : 1) ||
	    now - _lastDrainS < _opts.guard_min_interval_ms / 1e3) {
		return false;
	}
	// Presents already in flight still report the old depth; start counting again.
	atomic_store(&_deepPresents, 0);
	_lastDrainS = now;
	_drains++;
	return true;
}

- (void)writeCsv
{
	char path[1024];
	snprintf(path, sizeof(path), "%s_%s_%s_%d.csv", _opts.out_prefix, mode_name(_opts.mode), _role, (int)getpid());
	FILE *file = fopen(path, "w");
	if (file == NULL) {
		fprintf(stderr, "LAYER_HOST_PROBE could not write %s\n", path);
		return;
	}

	fprintf(file,
	        "frame,submit_s,target_vblank_s,presented_s,present_minus_target_ms,present_minus_submit_ms,"
	        "thread_priority\n");
	for (size_t i = 0; i < _count; i++) {
		double presented = atomic_load(&_records[i].presented_s);
		double to_target = presented > 0.0 ? (presented - _records[i].target_s) * 1e3 : 0.0;
		double to_submit = presented > 0.0 ? (presented - _records[i].submit_s) * 1e3 : 0.0;
		fprintf(file, "%zu,%.9f,%.9f,%.9f,%.4f,%.4f,%d\n", i, _records[i].submit_s, _records[i].target_s,
		        presented, to_target, to_submit, _records[i].priority);
	}
	fclose(file);
	fprintf(stderr, "LAYER_HOST_PROBE wrote %s\n", path);
}

- (void)printSummary
{
	double *intervals = calloc(_count + 1, sizeof(double));
	double *to_target = calloc(_count + 1, sizeof(double));
	double *to_submit = calloc(_count + 1, sizeof(double));
	double *priorities = calloc(_count + 1, sizeof(double));
	size_t presented_count = 0;
	size_t interval_count = 0;
	size_t long_intervals = 0;
	size_t throttled_frames = 0;
	double previous = 0.0;

	for (size_t i = 0; i < _count; i++) {
		priorities[i] = _records[i].priority;
		if (_records[i].priority >= 0 && _records[i].priority <= 4) {
			throttled_frames++;
		}

		double presented = atomic_load(&_records[i].presented_s);
		if (presented <= 0.0) {
			continue;
		}
		to_target[presented_count] = (presented - _records[i].target_s) * 1e3;
		to_submit[presented_count] = (presented - _records[i].submit_s) * 1e3;
		presented_count++;

		if (previous > 0.0) {
			double interval = presented - previous;
			intervals[interval_count++] = interval * 1e3;
			if (interval > 1.5 * _periodSeconds) {
				long_intervals++;
			}
		}
		previous = presented;
	}

	qsort(intervals, interval_count, sizeof(double), compare_doubles);
	qsort(to_target, presented_count, sizeof(double), compare_doubles);
	qsort(to_submit, presented_count, sizeof(double), compare_doubles);
	qsort(priorities, _count, sizeof(double), compare_doubles);

	fprintf(stderr,
	        "LAYER_HOST_PROBE summary role=%s mode=%s present=%s min_duration_us=%.0f refresh_hz=%.3f\n"
	        "  frames submitted=%zu presented=%zu not_presented=%zu nil_drawables=%zu\n"
	        "  present interval ms: median=%.3f p95=%.3f p99=%.3f  >1.5x period=%.2f%%\n"
	        "  presented - vblank target ms: median=%.3f p95=%.3f\n"
	        "  presented - CPU submit ms: median=%.3f p95=%.3f\n"
	        "  render thread: realtime=%s priority min=%.0f median=%.0f  frames throttled (<=4)=%.2f%%\n"
	        "  latency guard: %s drains=%zu\n",
	        _role, mode_name(_opts.mode), present_name(_opts.present), _opts.min_duration_us,
	        1.0 / _periodSeconds, _count, presented_count, _count - presented_count, _nilDrawables,
	        percentile(intervals, interval_count, 50), percentile(intervals, interval_count, 95),
	        percentile(intervals, interval_count, 99),
	        interval_count > 0 ? 100.0 * (double)long_intervals / (double)interval_count : 0.0,
	        percentile(to_target, presented_count, 50), percentile(to_target, presented_count, 95),
	        percentile(to_submit, presented_count, 50), percentile(to_submit, presented_count, 95),
	        _realtime ? "yes" : "no", _count > 0 ? priorities[0] : -1.0, percentile(priorities, _count, 50),
	        _count > 0 ? 100.0 * (double)throttled_frames / (double)_count : 0.0,
	        _opts.latency_guard ? "on" : "off", _drains);

	free(intervals);
	free(to_target);
	free(to_submit);
	free(priorities);
}

@end

static CVReturn
display_link_callback(CVDisplayLinkRef link,
                      const CVTimeStamp *now,
                      const CVTimeStamp *output_time,
                      CVOptionFlags flags_in,
                      CVOptionFlags *flags_out,
                      void *ctx)
{
	ProbeRenderer *renderer = (__bridge ProbeRenderer *)ctx;
	[renderer onVblank:output_time->hostTime];
	return kCVReturnSuccess;
}

static CAMetalLayer *
create_metal_layer(CGSize points, double scale)
{
	// Same configuration as Monado's legacy headset window.
	CAMetalLayer *layer = [CAMetalLayer layer];
	layer.device = MTLCreateSystemDefaultDevice();
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = NO;
	layer.opaque = YES;
	layer.displaySyncEnabled = YES;
	layer.allowsNextDrawableTimeout = YES;
	layer.maximumDrawableCount = 3;
	layer.anchorPoint = CGPointZero;
	layer.frame = CGRectMake(0, 0, points.width, points.height);
	layer.contentsScale = scale;
	layer.drawableSize = CGSizeMake(points.width * scale, points.height * scale);
	return layer;
}


/*
 *
 * Canary: a realtime thread in the host that wakes every refresh period and
 * records how late it woke and at what priority it ran.
 *
 */

struct canary_sample
{
	double t_s;
	float lateness_us;
	int16_t priority;
};

struct canary
{
	pthread_t thread;
	_Atomic bool stop;
	_Atomic int watch_pid;
	double period_s;
	bool realtime;
	struct canary_sample *samples;
	size_t capacity;
	size_t count;
	size_t transitions;
};

static void
canary_log_state(struct canary *c, double t, int priority)
{
	char host_policy[256];
	char game_policy[256] = "none";
	format_policy(host_policy, sizeof(host_policy), snapshot_policy(getpid()));
	int watch_pid = atomic_load(&c->watch_pid);
	if (watch_pid > 0) {
		format_policy(game_policy, sizeof(game_policy), snapshot_policy(watch_pid));
	}
	fprintf(stderr, "LAYER_HOST_PROBE canary t=%.3f priority=%d class=%s host{%s} game{pid=%d %s}\n", t, priority,
	        priority_class(priority), host_policy, watch_pid, game_policy);
}

static void *
canary_main(void *ptr)
{
	struct canary *c = ptr;
	pthread_setname_np("layer-host-probe-canary");
	c->realtime = set_thread_realtime(c->period_s);

	uint64_t period_ticks = seconds_to_mach(c->period_s);
	uint64_t next = mach_absolute_time() + period_ticks;
	const char *last_class = NULL;
	double last_log = 0.0;

	while (!atomic_load(&c->stop)) {
		mach_wait_until(next);
		uint64_t now = mach_absolute_time();
		double t = mach_to_seconds(now);
		int priority = thread_current_priority();

		if (c->count < c->capacity) {
			struct canary_sample *s = &c->samples[c->count++];
			s->t_s = t;
			s->lateness_us = now > next ? (float)(mach_to_seconds(now - next) * 1e6) : 0.0f;
			s->priority = (int16_t)priority;
		}

		// Log every class change immediately, and the full state every 5 s.
		const char *cls = priority_class(priority);
		bool changed = last_class != NULL && strcmp(cls, last_class) != 0;
		if (changed) {
			c->transitions++;
		}
		if (last_class == NULL || changed || t - last_log >= 5.0) {
			canary_log_state(c, t, priority);
			last_log = t;
		}
		last_class = cls;

		next += period_ticks;
		if (next <= now) {
			next = now + period_ticks;
		}
	}
	return NULL;
}

static void
canary_start(struct canary *c, double period_s, double max_seconds)
{
	*c = (struct canary){.period_s = period_s};
	c->capacity = (size_t)(max_seconds / period_s) + 64;
	c->samples = calloc(c->capacity, sizeof(struct canary_sample));
	pthread_create(&c->thread, NULL, canary_main, c);
}

static void
canary_stop(struct canary *c, const struct probe_options *opts)
{
	atomic_store(&c->stop, true);
	pthread_join(c->thread, NULL);

	char path[1024];
	snprintf(path, sizeof(path), "%s_%s_canary_%d.csv", opts->out_prefix, mode_name(opts->mode), (int)getpid());
	FILE *file = fopen(path, "w");
	if (file != NULL) {
		fprintf(file, "t_s,lateness_us,priority\n");
		for (size_t i = 0; i < c->count; i++) {
			fprintf(file, "%.6f,%.1f,%d\n", c->samples[i].t_s, c->samples[i].lateness_us,
			        c->samples[i].priority);
		}
		fclose(file);
		fprintf(stderr, "LAYER_HOST_PROBE wrote %s\n", path);
	}

	/*
	 * Weight by time, not samples: a starved thread takes few samples, so a
	 * per-sample share would understate throttling. Each gap is attributed to
	 * the priority observed at the wake that ended it.
	 */
	double *lateness = calloc(c->count + 1, sizeof(double));
	double throttled_s = 0.0;
	double total_s = 0.0;
	for (size_t i = 0; i < c->count; i++) {
		lateness[i] = c->samples[i].lateness_us;
		if (i == 0) {
			continue;
		}
		double gap = c->samples[i].t_s - c->samples[i - 1].t_s;
		total_s += gap;
		if (c->samples[i].priority >= 0 && c->samples[i].priority <= 4) {
			throttled_s += gap;
		}
	}
	qsort(lateness, c->count, sizeof(double), compare_doubles);
	fprintf(stderr,
	        "LAYER_HOST_PROBE canary summary realtime=%s samples=%zu class_transitions=%zu "
	        "time throttled (<=4)=%.1fs of %.1fs (%.2f%%)\n"
	        "  wake lateness us: median=%.0f p95=%.0f p99=%.0f max=%.0f\n",
	        c->realtime ? "yes" : "no", c->count, c->transitions, throttled_s, total_s,
	        total_s > 0.0 ? 100.0 * throttled_s / total_s : 0.0, percentile(lateness, c->count, 50),
	        percentile(lateness, c->count, 95), percentile(lateness, c->count, 99),
	        c->count > 0 ? lateness[c->count - 1] : 0.0);
	free(lateness);
	free(c->samples);
	c->samples = NULL;
}


/*
 *
 * CPU load for the game half, standing in for a game's worker threads.
 *
 */

struct cpu_load
{
	_Atomic bool stop;
	int count;
	pthread_t *threads;
};

static void *
cpu_load_main(void *ptr)
{
	struct cpu_load *load = ptr;
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	volatile double x = 1.0;
	while (!atomic_load_explicit(&load->stop, memory_order_relaxed)) {
		for (int i = 0; i < 100000; i++) {
			x = x * 1.0000001 + 0.0000001;
		}
	}
	return NULL;
}

static struct cpu_load *
cpu_load_start(int count)
{
	struct cpu_load *load = calloc(1, sizeof(*load));
	load->count = count;
	load->threads = calloc((size_t)count + 1, sizeof(pthread_t));
	for (int i = 0; i < count; i++) {
		pthread_create(&load->threads[i], NULL, cpu_load_main, load);
	}
	return load;
}

static void
cpu_load_stop(struct cpu_load *load)
{
	atomic_store(&load->stop, true);
	for (int i = 0; i < load->count; i++) {
		pthread_join(load->threads[i], NULL);
	}
	free(load->threads);
	free(load);
}


/*
 *
 * Host <-> game rendezvous over a Unix socket.
 *
 */

struct probe_hello
{
	uint32_t magic;
	uint32_t mode;
	uint32_t display_id;
	uint32_t reserved;
	double width_points;
	double height_points;
	double scale;
};

struct probe_attach
{
	uint32_t magic;
	uint32_t context_id; //!< 0 in game-direct.
	int32_t pid;
	uint32_t reserved;
};

static bool
write_full(int fd, const void *data, size_t size)
{
	const uint8_t *p = data;
	while (size > 0) {
		ssize_t n = write(fd, p, size);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		p += n;
		size -= (size_t)n;
	}
	return true;
}

static bool
read_full(int fd, void *data, size_t size)
{
	uint8_t *p = data;
	while (size > 0) {
		ssize_t n = read(fd, p, size);
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n <= 0) {
			return false;
		}
		p += n;
		size -= (size_t)n;
	}
	return true;
}

static struct sockaddr_un
probe_socket_address(void)
{
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	strlcpy(addr.sun_path, PROBE_SOCKET_PATH, sizeof(addr.sun_path));
	return addr;
}

static int
listen_probe_socket(void)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	unlink(PROBE_SOCKET_PATH);
	struct sockaddr_un addr = probe_socket_address();
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int
connect_probe_socket(double timeout_s)
{
	double end = now_seconds() + timeout_s;
	do {
		int fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd < 0) {
			return -1;
		}
		struct sockaddr_un addr = probe_socket_address();
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			return fd;
		}
		close(fd);
		[NSThread sleepForTimeInterval:0.25];
	} while (now_seconds() < end);
	return -1;
}


/*
 *
 * Shared AppKit helpers.
 *
 */

static void
stop_app(void)
{
	// -[NSApp stop:] only takes effect once another event is processed.
	[NSApp stop:nil];
	NSEvent *wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
	                                   location:NSZeroPoint
	                              modifierFlags:0
	                                  timestamp:0
	                               windowNumber:0
	                                    context:nil
	                                    subtype:0
	                                      data1:0
	                                      data2:0];
	[NSApp postEvent:wake atStart:YES];
}

static NSScreen *
select_screen(int index)
{
	NSArray<NSScreen *> *screens = [NSScreen screens];
	if (screens.count == 0) {
		return nil;
	}
	if (index < 0 || (NSUInteger)index >= screens.count) {
		return screens.lastObject;
	}
	return screens[(NSUInteger)index];
}

static NSWindow *
create_headset_window(NSScreen *screen)
{
	// Same window configuration as Monado's legacy headset window.
	NSWindow *window = [[NSWindow alloc] initWithContentRect:screen.frame
	                                               styleMask:NSWindowStyleMaskBorderless
	                                                 backing:NSBackingStoreBuffered
	                                                   defer:NO
	                                                  screen:screen];
	window.backgroundColor = [NSColor blackColor];
	window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
	                            NSWindowCollectionBehaviorFullScreenAuxiliary |
	                            NSWindowCollectionBehaviorStationary;
	window.hasShadow = NO;
	window.hidesOnDeactivate = NO;
	window.ignoresMouseEvents = YES;
	window.level = NSMainMenuWindowLevel + 1;
	window.releasedWhenClosed = NO;
	return window;
}


/*
 *
 * Client role: renders into a CAContext and reports its id to the host.
 *
 */

/*
 * Client-driven visibility (--swap-method client). The host stacks B above A
 * and never commits again. At swap k the incoming client shows its own layer;
 * once one of its frames has been presented it tells the outgoing client over
 * the peer socket, and only then does the outgoing client hide. One valid
 * layer is always on screen, so the two commits need not be atomic.
 */

struct visibility_event
{
	uint32_t k;
	bool show;
	double scheduled_s;
	double request_s;
	double commit_s;
	double first_present_s; //!< Show only: first own present after the commit.
};

static void
set_layer_visible(CALayer *layer, bool visible)
{
	// Explicit transaction: this runs on a secondary thread.
	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	layer.hidden = !visible;
	[CATransaction commit];
	[CATransaction flush];
}

static size_t
run_visibility_controller(const struct probe_options *opts,
                          CALayer *container,
                          ProbeRenderer *renderer,
                          struct visibility_event *events,
                          size_t capacity)
{
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
	int me = opts->tint - 1; // 0 = A, 1 = B
	size_t count = 0;

	for (uint32_t k = 0; count < capacity; k++) {
		double scheduled = opts->epoch_s + (double)(k + 1) * opts->swap_every;
		/*
		 * Renderers start at spawn and run for --seconds; the epoch is about
		 * 1.5 s after spawn. Stop 2 s before epoch + seconds so the last swap
		 * lands while both still render. Both clients compute the same k.
		 */
		if (scheduled > opts->epoch_s + opts->seconds - 2.0) {
			break;
		}
		int incoming = (k % 2 == 0) ? 1 : 0; // B is shown first.
		struct visibility_event ev = {.k = k, .scheduled_s = scheduled};

		if (me == incoming) {
			mach_wait_until(seconds_to_mach(scheduled));
			ev.show = true;
			ev.request_s = now_seconds();
			set_layer_visible(container, true);
			ev.commit_s = now_seconds();

			// Wait until one of our frames is on screen, then release the peer.
			double deadline = ev.commit_s + 1.0;
			while (now_seconds() < deadline) {
				double latest = [renderer latestPresented];
				if (latest > ev.commit_s) {
					ev.first_present_s = latest;
					break;
				}
				usleep(500);
			}
			if (!write_full(opts->peer_fd, &k, sizeof(k))) {
				fprintf(stderr, "LAYER_HOST_PROBE client: peer gone at swap %u\n", k);
				events[count++] = ev;
				break;
			}
		} else {
			uint32_t shown = UINT32_MAX;
			struct pollfd pfd = {.fd = opts->peer_fd, .events = POLLIN};
			int timeout_ms = (int)((scheduled - now_seconds() + 2.0) * 1000.0);
			if (poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 0) != 1 ||
			    !read_full(opts->peer_fd, &shown, sizeof(shown)) || shown != k) {
				fprintf(stderr, "LAYER_HOST_PROBE client: no show message for swap %u\n", k);
				break;
			}
			ev.show = false;
			ev.request_s = now_seconds();
			set_layer_visible(container, false);
			ev.commit_s = now_seconds();
		}
		events[count++] = ev;
	}
	return count;
}

static void
write_visibility_log(const struct probe_options *opts,
                     const char *role,
                     const struct visibility_event *events,
                     size_t count)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s_%s_%s_vis_%d.csv", opts->out_prefix, mode_name(opts->mode), role,
	         (int)getpid());
	FILE *file = fopen(path, "w");
	if (file == NULL) {
		return;
	}
	fprintf(file, "swap,action,scheduled_s,request_s,commit_s,first_present_s\n");
	for (size_t i = 0; i < count; i++) {
		fprintf(file, "%u,%s,%.9f,%.9f,%.9f,%.9f\n", events[i].k, events[i].show ? "show" : "hide",
		        events[i].scheduled_s, events[i].request_s, events[i].commit_s, events[i].first_present_s);
	}
	fclose(file);
	fprintf(stderr, "LAYER_HOST_PROBE wrote %s\n", path);
}

static int
run_client(const struct probe_options *opts)
{
	if (!remote_layer_api_supported()) {
		fprintf(stderr, "LAYER_HOST_PROBE client: remote layer API not available\n");
		return 1;
	}

	bool client_swaps = opts->swap_method == SWAP_CLIENT && opts->peer_fd >= 0;
	if (client_swaps) {
		signal(SIGPIPE, SIG_IGN);
	}
	CAMetalLayer *layer = create_metal_layer(CGSizeMake(opts->width_points, opts->height_points), opts->scale);

	// With client swaps the context holds a container this client can hide.
	CALayer *container = nil;
	[CATransaction begin];
	CAContext *context = nil;
	if (client_swaps) {
		container = [CALayer layer];
		container.anchorPoint = CGPointZero;
		container.frame = layer.frame;
		[container addSublayer:layer];
		container.hidden = opts->tint == 2; // A is shown first.
		context = create_remote_context(container);
	} else {
		context = create_remote_context(layer);
	}
	[CATransaction commit];
	[CATransaction flush];

	CAContextID context_id = context.contextId;
	fprintf(stderr, "LAYER_HOST_PROBE client pid=%d context_id=%u\n", (int)getpid(), context_id);
	if (write(opts->context_fd, &context_id, sizeof(context_id)) != sizeof(context_id)) {
		fprintf(stderr, "LAYER_HOST_PROBE client: could not send context id\n");
		return 1;
	}
	close(opts->context_fd);

	const char *role = opts->tint == 1 ? "client-a" : opts->tint == 2 ? "client-b" : "client";
	ProbeRenderer *renderer = [[ProbeRenderer alloc] initWithLayer:layer
	                                                     displayID:opts->display_id
	                                                       options:opts
	                                                          role:role];

	size_t event_capacity = (size_t)(opts->seconds / opts->swap_every) + 8;
	struct visibility_event *events = calloc(event_capacity, sizeof(struct visibility_event));
	__block size_t event_count = 0;
	dispatch_semaphore_t controller_done = dispatch_semaphore_create(0);
	if (client_swaps) {
		[NSThread detachNewThreadWithBlock:^{
			event_count = run_visibility_controller(opts, container, renderer, events, event_capacity);
			dispatch_semaphore_signal(controller_done);
		}];
	}

	struct cpu_load *load = cpu_load_start(opts->cpu_load);
	[renderer runForSeconds:opts->seconds];
	cpu_load_stop(load);

	if (client_swaps) {
		dispatch_semaphore_wait(controller_done, DISPATCH_TIME_FOREVER);
		write_visibility_log(opts, role, events, event_count);
		close(opts->peer_fd);
	}
	free(events);

	// Keep the context alive until rendering is finished.
	(void)context;
	return 0;
}


/*
 *
 * Game role: a games-category app that goes fullscreen on the Mac display so
 * Game Mode engages, then either renders to the headset through a CAContext
 * (game-hosted) or only loads the CPU while the host renders (game-direct).
 *
 */

static int
game_session(const struct probe_options *opts)
{
	fprintf(stderr, "LAYER_HOST_PROBE game pid=%d warmup=%.1fs seconds=%.1fs cpu_load=%d\n", (int)getpid(),
	        opts->warmup, opts->seconds, opts->cpu_load);

	// Give the fullscreen transition time to finish and Game Mode time to engage.
	[NSThread sleepForTimeInterval:opts->warmup];

	int fd = connect_probe_socket(10.0);
	if (fd < 0) {
		fprintf(stderr, "LAYER_HOST_PROBE game: no host on %s (run --role bootstrap-host first)\n",
		        PROBE_SOCKET_PATH);
		return 1;
	}

	struct probe_hello hello;
	if (!read_full(fd, &hello, sizeof(hello)) || hello.magic != PROBE_MAGIC) {
		fprintf(stderr, "LAYER_HOST_PROBE game: bad hello from host\n");
		close(fd);
		return 1;
	}
	enum probe_mode mode = (enum probe_mode)hello.mode;
	fprintf(stderr, "LAYER_HOST_PROBE game connected mode=%s display_id=%u %.0fx%.0f pt\n", mode_name(mode),
	        hello.display_id, hello.width_points, hello.height_points);

	__block CAMetalLayer *layer = nil;
	__block CAContext *context = nil;
	if (mode == PROBE_MODE_GAME_HOSTED) {
		if (!remote_layer_api_supported()) {
			fprintf(stderr, "LAYER_HOST_PROBE game: remote layer API not available\n");
			close(fd);
			return 1;
		}
		dispatch_sync(dispatch_get_main_queue(), ^{
			layer = create_metal_layer(CGSizeMake(hello.width_points, hello.height_points), hello.scale);
			[CATransaction begin];
			context = create_remote_context(layer);
			[CATransaction commit];
			[CATransaction flush];
		});
	}

	struct probe_attach attach = {
	    .magic = PROBE_MAGIC,
	    .context_id = context != nil ? context.contextId : 0,
	    .pid = (int32_t)getpid(),
	};
	if (!write_full(fd, &attach, sizeof(attach))) {
		fprintf(stderr, "LAYER_HOST_PROBE game: could not attach to host\n");
		close(fd);
		return 1;
	}

	char policy[256];
	format_policy(policy, sizeof(policy), snapshot_policy(getpid()));
	fprintf(stderr, "LAYER_HOST_PROBE game policy at start: %s\n", policy);

	struct cpu_load *load = cpu_load_start(opts->cpu_load);

	if (mode == PROBE_MODE_GAME_HOSTED) {
		struct probe_options render_opts = *opts;
		render_opts.mode = mode;
		ProbeRenderer *renderer = [[ProbeRenderer alloc] initWithLayer:layer
		                                                     displayID:hello.display_id
		                                                       options:&render_opts
		                                                          role:"game"];
		[renderer runForSeconds:opts->seconds];
	} else {
		[NSThread sleepForTimeInterval:opts->seconds];
	}

	format_policy(policy, sizeof(policy), snapshot_policy(getpid()));
	fprintf(stderr, "LAYER_HOST_PROBE game policy at end: %s\n", policy);

	cpu_load_stop(load);

	// Closing the socket tells the host the session is over.
	close(fd);
	(void)context;
	return 0;
}

static int
run_game(const struct probe_options *opts)
{
	// Launched through open(1), stderr goes nowhere; keep a log instead.
	if (!isatty(STDERR_FILENO) && freopen(PROBE_GAME_LOG, "a", stderr) != NULL) {
		setvbuf(stderr, NULL, _IOLBF, 0);
	}

	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

	// Game Mode needs a fullscreen window on the Mac's own display.
	NSScreen *screen = [NSScreen screens].firstObject;
	NSWindow *window = [[NSWindow alloc]
	    initWithContentRect:NSMakeRect(0, 0, 1280, 720)
	              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
	                backing:NSBackingStoreBuffered
	                  defer:NO
	                 screen:screen];
	window.title = @"Monado layer-host probe (game)";
	window.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
	window.releasedWhenClosed = NO;
	NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 1280, 720)];
	CALayer *root = [CALayer layer];
	root.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
	view.layer = root;
	view.wantsLayer = YES;
	window.contentView = view;
	[window center];
	[window makeKeyAndOrderFront:nil];
	[NSApp activateIgnoringOtherApps:YES];

	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		[window toggleFullScreen:nil];
	});

	__block int exit_code = 0;
	[NSThread detachNewThreadWithBlock:^{
		exit_code = game_session(opts);
		dispatch_async(dispatch_get_main_queue(), ^{
			stop_app();
		});
	}];

	[NSApp run];
	return exit_code;
}


/*
 *
 * Host role: owns the fullscreen window on the headset display.
 *
 */

static pid_t
spawn_client(const char *self_path,
             const struct probe_options *opts,
             int tint,
             int peer_fd,
             double epoch_s,
             CGDirectDisplayID display_id,
             CGSize points,
             double scale,
             int *out_read_fd)
{
	int fds[2];
	if (pipe(fds) != 0) {
		return -1;
	}
	// Neither end may leak into later children.
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);

	/*
	 * The child gets the pipe as fd 3 and the peer socket as fd 4. Duplicate
	 * both sources above 10 first: a source already at 3 or 4 would either be
	 * overwritten by the other dup2, or be dup2'd onto itself, which keeps its
	 * close-on-exec flag and closes it in the child.
	 */
	int context_src = fcntl(fds[1], F_DUPFD_CLOEXEC, 10);
	int peer_src = peer_fd >= 0 ? fcntl(peer_fd, F_DUPFD_CLOEXEC, 10) : -1;
	if (context_src < 0 || (peer_fd >= 0 && peer_src < 0)) {
		close(fds[0]);
		close(fds[1]);
		if (context_src >= 0) {
			close(context_src);
		}
		return -1;
	}

	char display_id_str[32], width_str[32], height_str[32], scale_str[32], seconds_str[32], min_us_str[32];
	char tint_str[16], cpu_load_str[16];
	snprintf(tint_str, sizeof(tint_str), "%d", tint);
	snprintf(cpu_load_str, sizeof(cpu_load_str), "%d", opts->cpu_load);
	snprintf(display_id_str, sizeof(display_id_str), "%u", display_id);
	snprintf(width_str, sizeof(width_str), "%.3f", points.width);
	snprintf(height_str, sizeof(height_str), "%.3f", points.height);
	snprintf(scale_str, sizeof(scale_str), "%.3f", scale);
	snprintf(seconds_str, sizeof(seconds_str), "%.3f", opts->seconds);
	snprintf(min_us_str, sizeof(min_us_str), "%.0f", opts->min_duration_us);
	char epoch_str[48], swap_every_str[32];
	snprintf(epoch_str, sizeof(epoch_str), "%.9f", epoch_s);
	snprintf(swap_every_str, sizeof(swap_every_str), "%.6f", opts->swap_every);

	char *argv[48];
	int argc = 0;
#define PUSH_ARG(value) argv[argc++] = (char *)(value)
	PUSH_ARG(self_path);
	PUSH_ARG("--role"), PUSH_ARG("client");
	PUSH_ARG("--mode"), PUSH_ARG(mode_name(opts->mode));
	PUSH_ARG("--present"), PUSH_ARG(present_name(opts->present));
	PUSH_ARG("--min-duration-us"), PUSH_ARG(min_us_str);
	PUSH_ARG("--seconds"), PUSH_ARG(seconds_str);
	PUSH_ARG("--out"), PUSH_ARG(opts->out_prefix);
	PUSH_ARG("--rt"), PUSH_ARG(opts->realtime ? "1" : "0");
	char guard_window_str[32], guard_interval_str[32];
	snprintf(guard_window_str, sizeof(guard_window_str), "%.3f", opts->guard_window_ms);
	snprintf(guard_interval_str, sizeof(guard_interval_str), "%.3f", opts->guard_min_interval_ms);
	PUSH_ARG("--latency-guard"), PUSH_ARG(opts->latency_guard ? "1" : "0");
	PUSH_ARG("--guard-window-ms"), PUSH_ARG(guard_window_str);
	PUSH_ARG("--guard-min-interval-ms"), PUSH_ARG(guard_interval_str);
	PUSH_ARG("--tint"), PUSH_ARG(tint_str);
	PUSH_ARG("--cpu-load"), PUSH_ARG(cpu_load_str);
	PUSH_ARG("--display-id"), PUSH_ARG(display_id_str);
	PUSH_ARG("--width"), PUSH_ARG(width_str);
	PUSH_ARG("--height"), PUSH_ARG(height_str);
	PUSH_ARG("--scale"), PUSH_ARG(scale_str);
	PUSH_ARG("--context-fd"), PUSH_ARG("3");
	if (peer_fd >= 0) {
		PUSH_ARG("--swap-method"), PUSH_ARG(swap_method_name(opts->swap_method));
		PUSH_ARG("--swap-every"), PUSH_ARG(swap_every_str);
		PUSH_ARG("--epoch"), PUSH_ARG(epoch_str);
		PUSH_ARG("--peer-fd"), PUSH_ARG("4");
	}
	argv[argc] = NULL;
#undef PUSH_ARG

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, context_src, 3);
	if (peer_src >= 0) {
		posix_spawn_file_actions_adddup2(&actions, peer_src, 4);
	}

	pid_t pid = -1;
	int ret = posix_spawn(&pid, self_path, &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(fds[1]);
	close(context_src);
	if (peer_src >= 0) {
		close(peer_src);
	}

	if (ret != 0) {
		close(fds[0]);
		return -1;
	}

	*out_read_fd = fds[0];
	return pid;
}

static bool
read_context_id(int fd, CAContextID *out_id)
{
	struct pollfd pfd = {.fd = fd, .events = POLLIN};
	if (poll(&pfd, 1, 5000) != 1) {
		return false;
	}
	return read(fd, out_id, sizeof(*out_id)) == sizeof(*out_id);
}

/*
 *
 * Handoff analysis: rebuilds what was on screen from both clients' CSVs and
 * the host's swap log.
 *
 */

struct handoff_swap
{
	double scheduled_s;
	double request_s;
	double commit_s;
	int visible;         //!< Client shown after this swap: 0 = A, 1 = B.
	double hide_commit_s; //!< Client swaps: when the outgoing client hid; 0 otherwise.
};

struct handoff_log
{
	double attach_s; //!< Client A shown from here.
	struct handoff_swap *swaps;
	size_t capacity;
	size_t count;
};

struct client_frames
{
	double *submit_s;
	double *presented_s; //!< 0 when the drawable was not presented.
	size_t count;
};

//! Reads submit and presented times for every frame of a ProbeRenderer CSV.
static bool
load_client_frames(const char *path, struct client_frames *out)
{
	*out = (struct client_frames){0};
	FILE *file = fopen(path, "r");
	if (file == NULL) {
		return false;
	}

	size_t capacity = 4096;
	out->submit_s = malloc(capacity * sizeof(double));
	out->presented_s = malloc(capacity * sizeof(double));
	char line[512];
	(void)fgets(line, sizeof(line), file); // header
	while (fgets(line, sizeof(line), file) != NULL) {
		size_t frame;
		double submit, target, presented;
		if (sscanf(line, "%zu,%lf,%lf,%lf", &frame, &submit, &target, &presented) != 4) {
			continue;
		}
		if (out->count == capacity) {
			capacity *= 2;
			out->submit_s = realloc(out->submit_s, capacity * sizeof(double));
			out->presented_s = realloc(out->presented_s, capacity * sizeof(double));
		}
		out->submit_s[out->count] = submit;
		out->presented_s[out->count] = presented;
		out->count++;
	}
	fclose(file);
	return true;
}

static int
visible_at(const struct handoff_log *log, double t)
{
	int visible = 0;
	for (size_t k = 0; k < log->count && log->swaps[k].commit_s <= t; k++) {
		visible = log->swaps[k].visible;
	}
	return visible;
}

/*
 * Whether client c's layer was meant to be hidden at time t. For host swaps
 * that is simply "not the shown client". For client swaps the outgoing client
 * stays visible, under or over the incoming one, until its own hide commit.
 */
static bool
client_hidden_at(const struct handoff_log *log, enum swap_method method, int c, double t)
{
	if (method != SWAP_CLIENT) {
		return visible_at(log, t) != c;
	}
	bool hidden = c == 1; // A is shown first.
	double latest = -1.0;
	for (size_t k = 0; k < log->count; k++) {
		const struct handoff_swap *sw = &log->swaps[k];
		if (sw->visible == c && sw->commit_s <= t && sw->commit_s > latest) {
			hidden = false;
			latest = sw->commit_s;
		}
		if (sw->visible != c && sw->hide_commit_s > 0.0 && sw->hide_commit_s <= t && sw->hide_commit_s > latest) {
			hidden = true;
			latest = sw->hide_commit_s;
		}
	}
	return hidden;
}

/*
 * Client swaps: rebuild the swap log from both clients' visibility logs. The
 * incoming client's show commit is the swap; the outgoing client's hide
 * commit is recorded alongside.
 */
static void
load_client_swaps(const struct probe_options *opts, pid_t pids[2], struct handoff_log *log)
{
	const char *roles[2] = {"client-a", "client-b"};
	for (int c = 0; c < 2; c++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s_%s_%s_vis_%d.csv", opts->out_prefix, mode_name(opts->mode), roles[c],
		         (int)pids[c]);
		FILE *file = fopen(path, "r");
		if (file == NULL) {
			fprintf(stderr, "LAYER_HOST_PROBE handoff: could not read %s\n", path);
			continue;
		}
		char line[512];
		(void)fgets(line, sizeof(line), file); // header
		while (fgets(line, sizeof(line), file) != NULL) {
			unsigned k;
			char action[8];
			double scheduled, request, commit, first_present;
			if (sscanf(line, "%u,%7[^,],%lf,%lf,%lf,%lf", &k, action, &scheduled, &request, &commit,
			           &first_present) != 6 ||
			    k >= log->capacity) {
				continue;
			}
			struct handoff_swap *sw = &log->swaps[k];
			if (strcmp(action, "show") == 0) {
				sw->scheduled_s = scheduled;
				sw->request_s = request;
				sw->commit_s = commit;
				sw->visible = c;
			} else {
				sw->hide_commit_s = commit;
			}
		}
		fclose(file);
	}

	// Swaps are consecutive from 0; stop at the first one never shown.
	log->count = 0;
	while (log->count < log->capacity && log->swaps[log->count].commit_s > 0.0) {
		log->count++;
	}
}

static bool
near_swap(const struct handoff_log *log, double t, double before_s, double after_s)
{
	for (size_t k = 0; k < log->count; k++) {
		if (t >= log->swaps[k].commit_s - before_s && t <= log->swaps[k].commit_s + after_s) {
			return true;
		}
	}
	return false;
}

static void
analyze_handoff(const struct probe_options *opts, pid_t pids[2], const struct handoff_log *log, double period_s)
{
	const char *roles[2] = {"client-a", "client-b"};
	struct client_frames frames[2];
	for (int c = 0; c < 2; c++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s_%s_%s_%d.csv", opts->out_prefix, mode_name(opts->mode), roles[c],
		         (int)pids[c]);
		if (!load_client_frames(path, &frames[c])) {
			fprintf(stderr, "LAYER_HOST_PROBE handoff: could not read %s\n", path);
		}
	}

	/*
	 * Classify each frame by whether its client was shown when it was
	 * submitted, skipping frames submitted within a few periods of a swap.
	 * Hidden frames should not report presentedTime. If they do,
	 * presentedTime cannot tell what was on screen and the gap figures are
	 * unreliable.
	 *
	 * Since only on-screen drawables report presentedTime, the on-screen
	 * timeline is every presented frame from both clients. It must not be
	 * split by the host's commit time: WindowServer applies the swap some
	 * frames after the commit, and the old client stays on screen until then.
	 */
	size_t hidden_presented = 0, hidden_total = 0;
	size_t merged_count = 0;
	double *merged = malloc((frames[0].count + frames[1].count + 1) * sizeof(double));
	for (int c = 0; c < 2; c++) {
		for (size_t i = 0; i < frames[c].count; i++) {
			double submit = frames[c].submit_s[i];
			double p = frames[c].presented_s[i];
			if (submit < log->attach_s) {
				continue;
			}
			if (client_hidden_at(log, opts->swap_method, c, submit) &&
			    !near_swap(log, submit, 3.0 * period_s, 3.0 * period_s)) {
				hidden_total++;
				if (p > 0.0) {
					hidden_presented++;
				}
			}
			if (p > 0.0) {
				merged[merged_count++] = p;
			}
		}
	}
	qsort(merged, merged_count, sizeof(double), compare_doubles);

	// Steady state: long intervals away from any swap.
	size_t steady_intervals = 0, steady_long = 0;
	for (size_t i = 1; i < merged_count; i++) {
		if (near_swap(log, merged[i], 2.0 * period_s, 10.0 * period_s)) {
			continue;
		}
		steady_intervals++;
		if (merged[i] - merged[i - 1] > 1.5 * period_s) {
			steady_long++;
		}
	}

	char path[1024];
	snprintf(path, sizeof(path), "%s_handoff_swaps_%d.csv", opts->out_prefix, (int)getpid());
	FILE *file = fopen(path, "w");
	if (file != NULL) {
		fprintf(file,
		        "swap,visible,scheduled_s,commit_s,swap_late_ms,commit_ms,max_gap_ms,first_new_present_ms,"
		        "switch_gap_ms,old_after_commit_ms,hide_lag_ms\n");
	}

	double *gaps = calloc(log->count + 1, sizeof(double));
	double *switch_gaps = calloc(log->count + 1, sizeof(double));
	double *old_after = calloc(log->count + 1, sizeof(double));
	double *first_new = calloc(log->count + 1, sizeof(double));
	double *late = calloc(log->count + 1, sizeof(double));
	double *hide_lags = calloc(log->count + 1, sizeof(double));
	size_t measured = 0, long_gaps = 0, long_switches = 0, hide_measured = 0;
	for (size_t k = 0; k < log->count; k++) {
		const struct handoff_swap *sw = &log->swaps[k];
		double lo = sw->commit_s - 2.0 * period_s;
		double hi = sw->commit_s + 10.0 * period_s;

		double max_gap = 0.0;
		for (size_t i = 1; i < merged_count; i++) {
			if (merged[i] >= lo && merged[i] <= hi) {
				double gap = merged[i] - merged[i - 1];
				max_gap = gap > max_gap ? gap : max_gap;
			}
		}

		double first = -1.0;
		int c = sw->visible;
		for (size_t i = 0; i < frames[c].count; i++) {
			double p = frames[c].presented_s[i];
			if (p >= sw->commit_s && (first < 0.0 || p < first)) {
				first = p;
			}
		}

		/*
		 * The real switch: the old client's last present before the next swap,
		 * then the new client's first present after that. With client swaps
		 * the new layer may already be presenting while stacked under the old
		 * one; this still finds any gap left when the old one disappears.
		 */
		double next_commit = k + 1 < log->count ? log->swaps[k + 1].commit_s : 1e300;
		double last_old = -1.0;
		int o = !c;
		for (size_t i = 0; i < frames[o].count; i++) {
			double p = frames[o].presented_s[i];
			if (p >= sw->commit_s - period_s && p < next_commit && p > last_old) {
				last_old = p;
			}
		}
		double first_after_old = -1.0;
		for (size_t i = 0; last_old >= 0.0 && i < frames[c].count; i++) {
			double p = frames[c].presented_s[i];
			if (p > last_old && (first_after_old < 0.0 || p < first_after_old)) {
				first_after_old = p;
			}
		}

		double swap_late_ms = (sw->request_s - sw->scheduled_s) * 1e3;
		double commit_ms = (sw->commit_s - sw->request_s) * 1e3;
		double first_ms = first >= 0.0 ? (first - sw->commit_s) * 1e3 : -1.0;
		double switch_gap_ms = first_after_old >= 0.0 ? (first_after_old - last_old) * 1e3 : -1.0;
		double hide_lag_ms = sw->hide_commit_s > 0.0 ? (sw->hide_commit_s - sw->commit_s) * 1e3 : -1.0;
		double old_after_ms = last_old >= 0.0 ? (last_old - sw->commit_s) * 1e3 : -1.0;
		if (file != NULL) {
			fprintf(file, "%zu,%s,%.6f,%.6f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", k, c ? "b" : "a",
			        sw->scheduled_s, sw->commit_s, swap_late_ms, commit_ms, max_gap * 1e3, first_ms, switch_gap_ms,
			        old_after_ms, hide_lag_ms);
		}

		// The last swap can land after the clients stopped; skip it.
		if (first < 0.0) {
			continue;
		}
		gaps[measured] = max_gap * 1e3;
		switch_gaps[measured] = switch_gap_ms;
		old_after[measured] = old_after_ms;
		first_new[measured] = first_ms;
		late[measured] = swap_late_ms;
		if (hide_lag_ms >= 0.0) {
			hide_lags[hide_measured++] = hide_lag_ms;
		}
		measured++;
		if (max_gap > 1.5 * period_s) {
			long_gaps++;
		}
		if (switch_gap_ms > 1.5 * period_s * 1e3) {
			long_switches++;
		}
	}
	if (file != NULL) {
		fclose(file);
		fprintf(stderr, "LAYER_HOST_PROBE wrote %s\n", path);
	}

	qsort(gaps, measured, sizeof(double), compare_doubles);
	qsort(switch_gaps, measured, sizeof(double), compare_doubles);
	qsort(old_after, measured, sizeof(double), compare_doubles);
	qsort(first_new, measured, sizeof(double), compare_doubles);
	qsort(late, measured, sizeof(double), compare_doubles);
	qsort(hide_lags, hide_measured, sizeof(double), compare_doubles);

	/*
	 * Each client's present latency before the first swap and after it, to
	 * separate the effect of the host's state from the effect of swapping.
	 */
	char latency[2][160];
	double first_commit = log->count > 0 ? log->swaps[0].commit_s : 1e300;
	for (int c = 0; c < 2; c++) {
		double *before = calloc(frames[c].count + 1, sizeof(double));
		double *after = calloc(frames[c].count + 1, sizeof(double));
		size_t nb = 0, na = 0;
		for (size_t i = 0; i < frames[c].count; i++) {
			double p = frames[c].presented_s[i];
			if (p <= 0.0 || frames[c].submit_s[i] < log->attach_s) {
				continue;
			}
			double ms = (p - frames[c].submit_s[i]) * 1e3;
			if (frames[c].submit_s[i] < first_commit) {
				before[nb++] = ms;
			} else {
				after[na++] = ms;
			}
		}
		qsort(before, nb, sizeof(double), compare_doubles);
		qsort(after, na, sizeof(double), compare_doubles);
		snprintf(latency[c], sizeof(latency[c]), "before first swap median=%.3f (n=%zu), after median=%.3f p95=%.3f (n=%zu)",
		         percentile(before, nb, 50), nb, percentile(after, na, 50), percentile(after, na, 95), na);
		free(before);
		free(after);
	}

	double hidden_share = hidden_total > 0 ? (double)hidden_presented / (double)hidden_total : 0.0;
	fprintf(stderr,
	        "LAYER_HOST_PROBE handoff summary method=%s host_background=%s swaps=%zu measured=%zu period_ms=%.3f\n"
	        "  switch gap (old last present -> new first present) ms: median=%.3f p95=%.3f max=%.3f  "
	        ">1.5x period=%zu\n"
	        "  max on-screen interval around swap ms: median=%.3f p95=%.3f max=%.3f  >1.5x period=%zu\n"
	        "  old client still shown after commit ms: median=%.3f p95=%.3f max=%.3f\n"
	        "  swap commit -> first new-client present ms: median=%.3f p95=%.3f max=%.3f\n"
	        "  swap timer lateness ms: median=%.3f p95=%.3f max=%.3f\n"
	        "  outgoing hide commit after incoming show commit ms (client swaps): median=%.3f p95=%.3f max=%.3f\n"
	        "  steady state (away from swaps) intervals >1.5x period=%.2f%%\n"
	        "  client-a presented - submit ms: %s\n"
	        "  client-b presented - submit ms: %s\n"
	        "  hidden-client frames reporting presentedTime: %zu of %zu (%.1f%%)%s\n",
	        swap_method_name(opts->swap_method), opts->host_background ? "yes" : "no",
	        log->count, measured, period_s * 1e3, percentile(switch_gaps, measured, 50),
	        percentile(switch_gaps, measured, 95), measured > 0 ? switch_gaps[measured - 1] : 0.0, long_switches,
	        percentile(gaps, measured, 50), percentile(gaps, measured, 95),
	        measured > 0 ? gaps[measured - 1] : 0.0, long_gaps, percentile(old_after, measured, 50),
	        percentile(old_after, measured, 95), measured > 0 ? old_after[measured - 1] : 0.0,
	        percentile(first_new, measured, 50),
	        percentile(first_new, measured, 95), measured > 0 ? first_new[measured - 1] : 0.0,
	        percentile(late, measured, 50), percentile(late, measured, 95), measured > 0 ? late[measured - 1] : 0.0,
	        percentile(hide_lags, hide_measured, 50), percentile(hide_lags, hide_measured, 95),
	        hide_measured > 0 ? hide_lags[hide_measured - 1] : 0.0,
	        steady_intervals > 0 ? 100.0 * (double)steady_long / (double)steady_intervals : 0.0, latency[0],
	        latency[1], hidden_presented, hidden_total, 100.0 * hidden_share,
	        hidden_share > 0.05 ? "\n  WARNING: hidden layers report presentedTime, so the gap figures may count "
	                              "frames that were not visible; check visually"
	                            : "");

	free(gaps);
	free(switch_gaps);
	free(old_after);
	free(hide_lags);
	free(first_new);
	free(late);
	free(merged);
	for (int c = 0; c < 2; c++) {
		free(frames[c].submit_s);
		free(frames[c].presented_s);
	}
}

//! Runs on a dedicated thread: waits for the game, then serves one session.
static int
host_game_session(const struct probe_options *opts,
                  CALayer *root,
                  CGDirectDisplayID display_id,
                  CGSize points,
                  double scale,
                  double period_s)
{
	int listen_fd = listen_probe_socket();
	if (listen_fd < 0) {
		fprintf(stderr, "LAYER_HOST_PROBE host: could not listen on %s: %s\n", PROBE_SOCKET_PATH,
		        strerror(errno));
		return 1;
	}

	// Canary covers the wait too, so there is a baseline before Game Mode.
	struct canary canary;
	canary_start(&canary, period_s, opts->seconds + 600.0);

	fprintf(stderr, "LAYER_HOST_PROBE host waiting for the game on %s\n", PROBE_SOCKET_PATH);
	int fd = accept(listen_fd, NULL, NULL);
	close(listen_fd);
	unlink(PROBE_SOCKET_PATH);
	if (fd < 0) {
		canary_stop(&canary, opts);
		return 1;
	}

	struct probe_hello hello = {
	    .magic = PROBE_MAGIC,
	    .mode = (uint32_t)opts->mode,
	    .display_id = display_id,
	    .width_points = points.width,
	    .height_points = points.height,
	    .scale = scale,
	};
	struct probe_attach attach;
	if (!write_full(fd, &hello, sizeof(hello)) || !read_full(fd, &attach, sizeof(attach)) ||
	    attach.magic != PROBE_MAGIC) {
		fprintf(stderr, "LAYER_HOST_PROBE host: game handshake failed\n");
		close(fd);
		canary_stop(&canary, opts);
		return 1;
	}
	atomic_store(&canary.watch_pid, attach.pid);
	fprintf(stderr, "LAYER_HOST_PROBE host attached game pid=%d context_id=%u\n", attach.pid, attach.context_id);

	ProbeRenderer *renderer = nil;
	dispatch_semaphore_t renderer_done = dispatch_semaphore_create(0);

	if (opts->mode == PROBE_MODE_GAME_HOSTED) {
		if (attach.context_id == 0) {
			fprintf(stderr, "LAYER_HOST_PROBE host: game sent no context id\n");
			close(fd);
			canary_stop(&canary, opts);
			return 1;
		}
		dispatch_sync(dispatch_get_main_queue(), ^{
			[CATransaction begin];
			[CATransaction setDisableActions:YES];
			[root addSublayer:create_layer_host(attach.context_id)];
			[CATransaction commit];
			[CATransaction flush];
		});
	} else {
		__block CAMetalLayer *layer = nil;
		dispatch_sync(dispatch_get_main_queue(), ^{
			[CATransaction begin];
			[CATransaction setDisableActions:YES];
			layer = create_metal_layer(points, scale);
			[root addSublayer:layer];
			[CATransaction commit];
			[CATransaction flush];
		});
		renderer = [[ProbeRenderer alloc] initWithLayer:layer displayID:display_id options:opts role:"host"];
		ProbeRenderer *thread_renderer = renderer;
		[NSThread detachNewThreadWithBlock:^{
			[thread_renderer runForSeconds:opts->seconds];
			dispatch_semaphore_signal(renderer_done);
		}];
	}

	// The game closes the socket when it is done.
	uint8_t byte;
	while (read(fd, &byte, 1) > 0) {
	}
	close(fd);
	fprintf(stderr, "LAYER_HOST_PROBE host: game disconnected\n");

	if (renderer != nil) {
		[renderer requestStop];
		dispatch_semaphore_wait(renderer_done, DISPATCH_TIME_FOREVER);
	}

	canary_log_state(&canary, now_seconds(), -1);
	canary_stop(&canary, opts);
	return 0;
}

static int
run_host(const struct probe_options *opts, const char *self_path)
{
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

	if (opts->mode != PROBE_MODE_DIRECT && opts->mode != PROBE_MODE_GAME_DIRECT && !remote_layer_api_supported()) {
		fprintf(stderr, "LAYER_HOST_PROBE: remote layer API not available on this macOS\n");
		return 1;
	}

	NSScreen *screen = select_screen(opts->display_index);
	if (screen == nil) {
		fprintf(stderr, "LAYER_HOST_PROBE: no screens\n");
		return 1;
	}
	CGDirectDisplayID display_id = [[screen.deviceDescription objectForKey:@"NSScreenNumber"] unsignedIntValue];
	CGSize points = screen.frame.size;
	double scale = screen.backingScaleFactor;
	double period_s = screen.maximumFramesPerSecond > 0 ? 1.0 / (double)screen.maximumFramesPerSecond : 1.0 / 120.0;
	fprintf(stderr, "LAYER_HOST_PROBE host pid=%d mode=%s screen='%s' display_id=%u %.0fx%.0f pt scale=%.1f\n",
	        (int)getpid(), mode_name(opts->mode), screen.localizedName.UTF8String, display_id, points.width,
	        points.height, scale);

	NSWindow *window = create_headset_window(screen);
	NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, points.width, points.height)];
	CALayer *root = [CALayer layer];
	root.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
	view.layer = root;
	view.wantsLayer = YES;
	window.contentView = view;

	CAMetalLayer *local_layer = nil;
	CAContext *local_context = nil;
	pid_t child = -1;
	pid_t handoff_pids[2] = {-1, -1};
	CALayerHost *handoff_hosts[2] = {nil, nil};
	double handoff_epoch_s = 0.0;

	[CATransaction begin];
	[CATransaction setDisableActions:YES];
	switch (opts->mode) {
	case PROBE_MODE_DIRECT:
		local_layer = create_metal_layer(points, scale);
		[root addSublayer:local_layer];
		break;
	case PROBE_MODE_HOSTED_LOCAL:
		local_layer = create_metal_layer(points, scale);
		local_context = create_remote_context(local_layer);
		[root addSublayer:create_layer_host(local_context.contextId)];
		break;
	case PROBE_MODE_HOSTED: {
		int read_fd = -1;
		child = spawn_client(self_path, opts, 0, -1, 0.0, display_id, points, scale, &read_fd);
		CAContextID context_id = 0;
		if (child < 0 || !read_context_id(read_fd, &context_id)) {
			fprintf(stderr, "LAYER_HOST_PROBE: client did not report a context id\n");
			[CATransaction commit];
			if (child > 0) {
				kill(child, SIGTERM);
			}
			return 1;
		}
		close(read_fd);
		fprintf(stderr, "LAYER_HOST_PROBE host hosting client pid=%d context_id=%u\n", (int)child, context_id);
		[root addSublayer:create_layer_host(context_id)];
		break;
	}
	case PROBE_MODE_HANDOFF: {
		// Client swaps: the clients talk directly over a socket pair.
		int peer[2] = {-1, -1};
		if (opts->swap_method == SWAP_CLIENT) {
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, peer) != 0) {
				fprintf(stderr, "LAYER_HOST_PROBE: socketpair failed: %s\n", strerror(errno));
				[CATransaction commit];
				return 1;
			}
			fcntl(peer[0], F_SETFD, FD_CLOEXEC);
			fcntl(peer[1], F_SETFD, FD_CLOEXEC);
			handoff_epoch_s = now_seconds() + 1.5;
		}
		for (int c = 0; c < 2; c++) {
			int read_fd = -1;
			handoff_pids[c] = spawn_client(self_path, opts, c + 1, peer[c], handoff_epoch_s, display_id, points,
			                               scale, &read_fd);
			CAContextID context_id = 0;
			if (handoff_pids[c] < 0 || !read_context_id(read_fd, &context_id)) {
				fprintf(stderr, "LAYER_HOST_PROBE: client %c did not report a context id\n", 'a' + c);
				[CATransaction commit];
				for (int k = 0; k <= c; k++) {
					if (handoff_pids[k] > 0) {
						kill(handoff_pids[k], SIGTERM);
					}
				}
				return 1;
			}
			close(read_fd);
			handoff_hosts[c] = create_layer_host(context_id);
			fprintf(stderr, "LAYER_HOST_PROBE host hosting client %c pid=%d context_id=%u\n", 'a' + c,
			        (int)handoff_pids[c], context_id);
		}
		if (peer[0] >= 0) {
			close(peer[0]);
			close(peer[1]);
		}
		// Client A is shown first. With the hidden method both stay attached;
		// with client swaps both stay attached and visible, B stacked above A,
		// and each client hides its own content.
		[root addSublayer:handoff_hosts[0]];
		if (opts->swap_method == SWAP_HIDDEN) {
			handoff_hosts[1].hidden = YES;
			[root addSublayer:handoff_hosts[1]];
		} else if (opts->swap_method == SWAP_CLIENT) {
			[root addSublayer:handoff_hosts[1]];
		}
		break;
	}
	case PROBE_MODE_GAME_DIRECT:
	case PROBE_MODE_GAME_HOSTED:
		// Content is added once the game connects.
		break;
	}
	[CATransaction commit];

	[window setFrame:screen.frame display:YES];
	[window orderFrontRegardless];
	[CATransaction flush];

	__block int exit_code = 0;
	if (mode_is_game(opts->mode)) {
		[NSThread detachNewThreadWithBlock:^{
			exit_code = host_game_session(opts, root, display_id, points, scale, period_s);
			dispatch_async(dispatch_get_main_queue(), ^{
				stop_app();
			});
		}];
	} else if (opts->mode == PROBE_MODE_HANDOFF) {
		if (opts->host_background) {
			// The same clamp Game Mode applies to monado-service. The clients
			// were spawned first, so they are not affected.
			if (setpriority(PRIO_DARWIN_PROCESS, 0, PRIO_DARWIN_BG) != 0) {
				fprintf(stderr, "LAYER_HOST_PROBE host: could not enter Darwin background: %s\n",
				        strerror(errno));
			}
			char policy[256];
			format_policy(policy, sizeof(policy), snapshot_policy(getpid()));
			fprintf(stderr, "LAYER_HOST_PROBE host policy: %s\n", policy);
		}

		struct handoff_log *log = calloc(1, sizeof(*log));
		log->attach_s = now_seconds();
		log->capacity = (size_t)(opts->seconds / opts->swap_every) + 8;
		log->swaps = calloc(log->capacity, sizeof(struct handoff_swap));

		CALayerHost *host_a = handoff_hosts[0];
		CALayerHost *host_b = handoff_hosts[1];
		double first_s = log->attach_s + opts->swap_every;
		// With client swaps the host never commits again; the clients swap.
		dispatch_source_t timer = opts->swap_method == SWAP_CLIENT
		                              ? nil
		                              : dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
		                                                       dispatch_get_main_queue());
		if (timer != nil) {
			dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(opts->swap_every * NSEC_PER_SEC)),
			                          (uint64_t)(opts->swap_every * NSEC_PER_SEC), 0);
			dispatch_source_set_event_handler(timer, ^{
				if (log->count >= log->capacity) {
					return;
				}
				struct handoff_swap *sw = &log->swaps[log->count];
				sw->scheduled_s = first_s + (double)log->count * opts->swap_every;
				sw->request_s = now_seconds();
				sw->visible = log->count == 0 ? 1 : !log->swaps[log->count - 1].visible;

				CALayerHost *show = sw->visible ? host_b : host_a;
				CALayerHost *hide = sw->visible ? host_a : host_b;
				[CATransaction begin];
				[CATransaction setDisableActions:YES];
				if (opts->swap_method == SWAP_HIDDEN) {
					show.hidden = NO;
					hide.hidden = YES;
				} else {
					[root addSublayer:show];
					[hide removeFromSuperlayer];
				}
				[CATransaction commit];
				[CATransaction flush];
				sw->commit_s = now_seconds();
				log->count++;
			});
			dispatch_resume(timer);
		}

		pid_t pid_a = handoff_pids[0];
		pid_t pid_b = handoff_pids[1];
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
			int status_a = 0, status_b = 0;
			waitpid(pid_a, &status_a, 0);
			waitpid(pid_b, &status_b, 0);
			int statuses[2] = {status_a, status_b};
			for (int c = 0; c < 2; c++) {
				if (WIFSIGNALED(statuses[c])) {
					fprintf(stderr, "LAYER_HOST_PROBE host: client %c killed by signal %d (%s)\n", 'a' + c,
					        WTERMSIG(statuses[c]), strsignal(WTERMSIG(statuses[c])));
				} else if (WIFEXITED(statuses[c]) && WEXITSTATUS(statuses[c]) != 0) {
					fprintf(stderr, "LAYER_HOST_PROBE host: client %c exited with status %d\n", 'a' + c,
					        WEXITSTATUS(statuses[c]));
				}
			}
			exit_code = (WIFEXITED(status_a) && WEXITSTATUS(status_a) == 0 && WIFEXITED(status_b) &&
			             WEXITSTATUS(status_b) == 0)
			                ? 0
			                : 1;
			dispatch_async(dispatch_get_main_queue(), ^{
				pid_t pids[2] = {pid_a, pid_b};
				if (timer != nil) {
					dispatch_source_cancel(timer);
				} else {
					load_client_swaps(opts, pids, log);
				}
				analyze_handoff(opts, pids, log, period_s);
				stop_app();
			});
		});
	} else if (opts->mode == PROBE_MODE_HOSTED) {
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
			int status = 0;
			waitpid(child, &status, 0);
			exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
			dispatch_async(dispatch_get_main_queue(), ^{
				stop_app();
			});
		});
	} else {
		ProbeRenderer *renderer = [[ProbeRenderer alloc] initWithLayer:local_layer
		                                                     displayID:display_id
		                                                       options:opts
		                                                          role:"host"];
		// A dedicated thread, since the renderer may make it realtime.
		[NSThread detachNewThreadWithBlock:^{
			[renderer runForSeconds:opts->seconds];
			dispatch_async(dispatch_get_main_queue(), ^{
				stop_app();
			});
		}];
	}

	[NSApp run];

	(void)local_context;
	[window orderOut:nil];
	return exit_code;
}


/*
 *
 * launchd registration, so the host runs in its own coalition like
 * monado-service rather than inside the terminal's.
 *
 */

static int
run_launchctl(NSArray<NSString *> *args, bool quiet)
{
	char **argv = calloc(args.count + 2, sizeof(char *));
	argv[0] = "launchctl";
	for (NSUInteger i = 0; i < args.count; i++) {
		argv[i + 1] = (char *)args[i].UTF8String;
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	if (quiet) {
		posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
	}

	pid_t pid = -1;
	int ret = posix_spawnp(&pid, "launchctl", &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	free(argv);
	if (ret != 0) {
		return -1;
	}

	int status = 0;
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static NSString *
launchd_target(void)
{
	return [NSString stringWithFormat:@"gui/%u/%s", getuid(), PROBE_LAUNCHD_LABEL];
}

static int
run_bootout_host(void)
{
	int ret = run_launchctl(@[ @"bootout", launchd_target() ], true);
	fprintf(stderr, "LAYER_HOST_PROBE bootout %s: %s\n", PROBE_LAUNCHD_LABEL, ret == 0 ? "done" : "not loaded");
	unlink(PROBE_SOCKET_PATH);
	return 0;
}

static int
run_bootstrap_host(const struct probe_options *opts, const char *self_path)
{
	if (!mode_is_game(opts->mode)) {
		fprintf(stderr, "LAYER_HOST_PROBE: bootstrap-host needs --mode game-direct or game-hosted\n");
		return 2;
	}

	NSMutableArray<NSString *> *args = [NSMutableArray arrayWithArray:@[
		@(self_path),
		@"--role",
		@"host",
		@"--mode",
		@(mode_name(opts->mode)),
		@"--seconds",
		[NSString stringWithFormat:@"%.3f", opts->seconds],
		@"--present",
		@(present_name(opts->present)),
		@"--min-duration-us",
		[NSString stringWithFormat:@"%.0f", opts->min_duration_us],
		@"--out",
		@(opts->out_prefix),
		@"--rt",
		opts->realtime ? @"1" : @"0",
		@"--latency-guard",
		opts->latency_guard ? @"1" : @"0",
		@"--guard-window-ms",
		[NSString stringWithFormat:@"%.3f", opts->guard_window_ms],
		@"--guard-min-interval-ms",
		[NSString stringWithFormat:@"%.3f", opts->guard_min_interval_ms],
	]];
	if (opts->display_index >= 0) {
		[args addObjectsFromArray:@[ @"--display", [NSString stringWithFormat:@"%d", opts->display_index] ]];
	}

	NSDictionary *plist = @{
		@"Label" : @PROBE_LAUNCHD_LABEL,
		@"ProgramArguments" : args,
		@"RunAtLoad" : @YES,
		@"KeepAlive" : @NO,
		@"LimitLoadToSessionType" : @"Aqua",
		@"ProcessType" : @(opts->process_type),
		@"StandardOutPath" : @PROBE_HOST_LOG,
		@"StandardErrorPath" : @PROBE_HOST_LOG,
	};

	NSString *path = @"/tmp/" PROBE_LAUNCHD_LABEL ".plist";
	NSError *error = nil;
	if (![plist writeToURL:[NSURL fileURLWithPath:path] error:&error]) {
		fprintf(stderr, "LAYER_HOST_PROBE: could not write %s: %s\n", path.UTF8String,
		        error.localizedDescription.UTF8String);
		return 1;
	}

	// Replace any earlier run, then start the host under launchd.
	run_launchctl(@[ @"bootout", launchd_target() ], true);
	unlink(PROBE_SOCKET_PATH);
	NSString *domain = [NSString stringWithFormat:@"gui/%u", getuid()];
	int ret = run_launchctl(@[ @"bootstrap", domain, path ], false);
	if (ret != 0) {
		fprintf(stderr, "LAYER_HOST_PROBE: launchctl bootstrap failed (%d)\n", ret);
		return 1;
	}

	fprintf(stderr,
	        "LAYER_HOST_PROBE host started by launchd: label=%s mode=%s ProcessType=%s\n"
	        "  log: %s\n"
	        "  now launch the game half, e.g.\n"
	        "    open macos-layer-host-probe-game.app --args --seconds 60 --cpu-load 8\n",
	        PROBE_LAUNCHD_LABEL, mode_name(opts->mode), opts->process_type, PROBE_HOST_LOG);
	return 0;
}

static int
run_query(pid_t pid)
{
	if (pid <= 0) {
		fprintf(stderr, "LAYER_HOST_PROBE: query needs --pid PID\n");
		return 2;
	}
	char policy[256];
	format_policy(policy, sizeof(policy), snapshot_policy(pid));
	printf("pid=%d %s\n", (int)pid, policy);
	return 0;
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		mach_timebase_info(&g_timebase);

		struct probe_options opts;
		if (!parse_options(argc, argv, &opts)) {
			print_usage(argv[0]);
			return 2;
		}

		char self_path[PATH_MAX];
		uint32_t self_path_size = sizeof(self_path);
		if (_NSGetExecutablePath(self_path, &self_path_size) != 0) {
			strlcpy(self_path, argv[0], sizeof(self_path));
		}

		switch (opts.role) {
		case PROBE_ROLE_CLIENT: return run_client(&opts);
		case PROBE_ROLE_GAME: return run_game(&opts);
		case PROBE_ROLE_BOOTSTRAP_HOST: return run_bootstrap_host(&opts, self_path);
		case PROBE_ROLE_BOOTOUT_HOST: return run_bootout_host();
		case PROBE_ROLE_QUERY: return run_query(opts.query_pid);
		case PROBE_ROLE_HOST: break;
		}
		return run_host(&opts, self_path);
	}
}
