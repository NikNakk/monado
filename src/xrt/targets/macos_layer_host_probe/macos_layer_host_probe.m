// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Standalone probe: does cross-process CALayerHost hosting change
 *        CAMetalLayer present timing on the headset display?
 *
 * Three modes, all presenting the same way as Monado's legacy headset window
 * (CVDisplayLink pacing, three drawables, afterMinimumDuration by default):
 *
 *  - direct:       this process renders into its own window's CAMetalLayer.
 *  - hosted-local: this process renders into a CAContext and shows it through
 *                  a CALayerHost in its own window (hosting cost only).
 *  - hosted:       a spawned child process renders into a CAContext; this
 *                  process shows it through a CALayerHost (cross-process).
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
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;


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

enum probe_mode
{
	PROBE_MODE_DIRECT,
	PROBE_MODE_HOSTED_LOCAL,
	PROBE_MODE_HOSTED,
};

enum present_mode
{
	PRESENT_MIN_DURATION,
	PRESENT_AT_TIME,
	PRESENT_IMMEDIATE,
};

struct probe_options
{
	bool is_client;
	enum probe_mode mode;
	enum present_mode present;
	int display_index;
	double seconds;
	double min_duration_us;
	const char *out_prefix;

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
	}
	return "unknown";
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

static void
print_usage(const char *argv0)
{
	fprintf(stderr,
	        "Usage: %s [--mode direct|hosted-local|hosted] [--display N] [--seconds S]\n"
	        "          [--present min-duration|at-time|immediate] [--min-duration-us US]\n"
	        "          [--out PREFIX]\n"
	        "\n"
	        "  --display N   index into NSScreen.screens (default: last screen, usually the headset)\n"
	        "  --seconds S   measurement length (default 20)\n"
	        "  --out PREFIX  CSV path prefix (default /tmp/layer_host_probe)\n",
	        argv0);
}

static bool
parse_options(int argc, char **argv, struct probe_options *opts)
{
	*opts = (struct probe_options){
	    .mode = PROBE_MODE_DIRECT,
	    .present = PRESENT_MIN_DURATION,
	    .display_index = -1,
	    .seconds = 20.0,
	    .min_duration_us = 8000.0,
	    .out_prefix = "/tmp/layer_host_probe",
	    .context_fd = -1,
	};

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];
		const char *value = (i + 1 < argc) ? argv[i + 1] : NULL;

		if (strcmp(arg, "--role") == 0 && value != NULL) {
			opts->is_client = strcmp(value, "client") == 0;
		} else if (strcmp(arg, "--mode") == 0 && value != NULL) {
			if (strcmp(value, "direct") == 0) {
				opts->mode = PROBE_MODE_DIRECT;
			} else if (strcmp(value, "hosted-local") == 0) {
				opts->mode = PROBE_MODE_HOSTED_LOCAL;
			} else if (strcmp(value, "hosted") == 0) {
				opts->mode = PROBE_MODE_HOSTED;
			} else {
				return false;
			}
		} else if (strcmp(arg, "--present") == 0 && value != NULL) {
			if (strcmp(value, "min-duration") == 0) {
				opts->present = PRESENT_MIN_DURATION;
			} else if (strcmp(value, "at-time") == 0) {
				opts->present = PRESENT_AT_TIME;
			} else if (strcmp(value, "immediate") == 0) {
				opts->present = PRESENT_IMMEDIATE;
			} else {
				return false;
			}
		} else if (strcmp(arg, "--display") == 0 && value != NULL) {
			opts->display_index = atoi(value);
		} else if (strcmp(arg, "--seconds") == 0 && value != NULL) {
			opts->seconds = atof(value);
		} else if (strcmp(arg, "--min-duration-us") == 0 && value != NULL) {
			opts->min_duration_us = atof(value);
		} else if (strcmp(arg, "--out") == 0 && value != NULL) {
			opts->out_prefix = value;
		} else if (strcmp(arg, "--display-id") == 0 && value != NULL) {
			opts->display_id = (CGDirectDisplayID)strtoul(value, NULL, 10);
		} else if (strcmp(arg, "--width") == 0 && value != NULL) {
			opts->width_points = atof(value);
		} else if (strcmp(arg, "--height") == 0 && value != NULL) {
			opts->height_points = atof(value);
		} else if (strcmp(arg, "--scale") == 0 && value != NULL) {
			opts->scale = atof(value);
		} else if (strcmp(arg, "--context-fd") == 0 && value != NULL) {
			opts->context_fd = atoi(value);
		} else {
			return false;
		}

		// Every option takes a value.
		i++;
	}

	return opts->seconds > 0.0;
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
 * Renderer: paces to CVDisplayLink and presents into a CAMetalLayer.
 *
 */

struct frame_record
{
	double submit_s;
	double target_s;
	_Atomic double presented_s;
};

@interface ProbeRenderer : NSObject
- (instancetype)initWithLayer:(CAMetalLayer *)layer
                    displayID:(CGDirectDisplayID)displayID
                      options:(const struct probe_options *)opts
                         role:(const char *)role;
- (void)runForSeconds:(double)seconds;
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
	double _periodSeconds;

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

	id<MTLCommandBuffer> cmd = [_queue commandBuffer];

	MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = drawable.texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.1, 0.1, 0.12, 1.0);
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

	[drawable addPresentedHandler:^(id<MTLDrawable> presented) {
		atomic_store(&record->presented_s, presented.presentedTime);
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
	CVDisplayLinkStart(_displayLink);

	double end = now_seconds() + seconds;
	while (now_seconds() < end) {
		dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.1 * NSEC_PER_SEC));
		if (dispatch_semaphore_wait(_vblank, timeout) != 0) {
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

- (void)writeCsv
{
	char path[1024];
	snprintf(path, sizeof(path), "%s_%s_%s_%d.csv", _opts.out_prefix, mode_name(_opts.mode), _role, (int)getpid());
	FILE *file = fopen(path, "w");
	if (file == NULL) {
		fprintf(stderr, "LAYER_HOST_PROBE could not write %s\n", path);
		return;
	}

	fprintf(file, "frame,submit_s,target_vblank_s,presented_s,present_minus_target_ms,present_minus_submit_ms\n");
	for (size_t i = 0; i < _count; i++) {
		double presented = atomic_load(&_records[i].presented_s);
		double to_target = presented > 0.0 ? (presented - _records[i].target_s) * 1e3 : 0.0;
		double to_submit = presented > 0.0 ? (presented - _records[i].submit_s) * 1e3 : 0.0;
		fprintf(file, "%zu,%.9f,%.9f,%.9f,%.4f,%.4f\n", i, _records[i].submit_s, _records[i].target_s, presented,
		        to_target, to_submit);
	}
	fclose(file);
	fprintf(stderr, "LAYER_HOST_PROBE wrote %s\n", path);
}

- (void)printSummary
{
	double *intervals = calloc(_count + 1, sizeof(double));
	double *to_target = calloc(_count + 1, sizeof(double));
	double *to_submit = calloc(_count + 1, sizeof(double));
	size_t presented_count = 0;
	size_t interval_count = 0;
	size_t long_intervals = 0;
	double previous = 0.0;

	for (size_t i = 0; i < _count; i++) {
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

	fprintf(stderr,
	        "LAYER_HOST_PROBE summary role=%s mode=%s present=%s min_duration_us=%.0f refresh_hz=%.3f\n"
	        "  frames submitted=%zu presented=%zu not_presented=%zu nil_drawables=%zu\n"
	        "  present interval ms: median=%.3f p95=%.3f p99=%.3f  >1.5x period=%.2f%%\n"
	        "  presented - vblank target ms: median=%.3f p95=%.3f\n"
	        "  presented - CPU submit ms: median=%.3f p95=%.3f\n",
	        _role, mode_name(_opts.mode), present_name(_opts.present), _opts.min_duration_us,
	        1.0 / _periodSeconds, _count, presented_count, _count - presented_count, _nilDrawables,
	        percentile(intervals, interval_count, 50), percentile(intervals, interval_count, 95),
	        percentile(intervals, interval_count, 99),
	        interval_count > 0 ? 100.0 * (double)long_intervals / (double)interval_count : 0.0,
	        percentile(to_target, presented_count, 50), percentile(to_target, presented_count, 95),
	        percentile(to_submit, presented_count, 50), percentile(to_submit, presented_count, 95));

	free(intervals);
	free(to_target);
	free(to_submit);
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
 * Client role: renders into a CAContext and reports its id to the host.
 *
 */

static int
run_client(const struct probe_options *opts)
{
	if (!remote_layer_api_supported()) {
		fprintf(stderr, "LAYER_HOST_PROBE client: remote layer API not available\n");
		return 1;
	}

	CAMetalLayer *layer = create_metal_layer(CGSizeMake(opts->width_points, opts->height_points), opts->scale);

	[CATransaction begin];
	CAContext *context = create_remote_context(layer);
	[CATransaction commit];
	[CATransaction flush];

	CAContextID context_id = context.contextId;
	fprintf(stderr, "LAYER_HOST_PROBE client pid=%d context_id=%u\n", (int)getpid(), context_id);
	if (write(opts->context_fd, &context_id, sizeof(context_id)) != sizeof(context_id)) {
		fprintf(stderr, "LAYER_HOST_PROBE client: could not send context id\n");
		return 1;
	}
	close(opts->context_fd);

	ProbeRenderer *renderer = [[ProbeRenderer alloc] initWithLayer:layer
	                                                     displayID:opts->display_id
	                                                       options:opts
	                                                          role:"client"];
	[renderer runForSeconds:opts->seconds];

	// Keep the context alive until rendering is finished.
	(void)context;
	return 0;
}


/*
 *
 * Host role: owns the fullscreen window on the headset display.
 *
 */

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

static pid_t
spawn_client(const char *self_path,
             const struct probe_options *opts,
             CGDirectDisplayID display_id,
             CGSize points,
             double scale,
             int *out_read_fd)
{
	int fds[2];
	if (pipe(fds) != 0) {
		return -1;
	}

	char display_id_str[32], width_str[32], height_str[32], scale_str[32], seconds_str[32], min_us_str[32];
	snprintf(display_id_str, sizeof(display_id_str), "%u", display_id);
	snprintf(width_str, sizeof(width_str), "%.3f", points.width);
	snprintf(height_str, sizeof(height_str), "%.3f", points.height);
	snprintf(scale_str, sizeof(scale_str), "%.3f", scale);
	snprintf(seconds_str, sizeof(seconds_str), "%.3f", opts->seconds);
	snprintf(min_us_str, sizeof(min_us_str), "%.0f", opts->min_duration_us);

	char *const argv[] = {
	    (char *)self_path,
	    "--role", "client",
	    "--mode", (char *)mode_name(opts->mode),
	    "--present", (char *)present_name(opts->present),
	    "--min-duration-us", min_us_str,
	    "--seconds", seconds_str,
	    "--out", (char *)opts->out_prefix,
	    "--display-id", display_id_str,
	    "--width", width_str,
	    "--height", height_str,
	    "--scale", scale_str,
	    "--context-fd", "3",
	    NULL,
	};

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, fds[1], 3);
	posix_spawn_file_actions_addclose(&actions, fds[0]);

	pid_t pid = -1;
	int ret = posix_spawn(&pid, self_path, &actions, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&actions);
	close(fds[1]);

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

static int
run_host(const struct probe_options *opts, const char *self_path)
{
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

	if (opts->mode != PROBE_MODE_DIRECT && !remote_layer_api_supported()) {
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
		child = spawn_client(self_path, opts, display_id, points, scale, &read_fd);
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
	}
	[CATransaction commit];

	[window setFrame:screen.frame display:YES];
	[window orderFrontRegardless];
	[CATransaction flush];

	__block int exit_code = 0;
	if (opts->mode == PROBE_MODE_HOSTED) {
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
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
			[renderer runForSeconds:opts->seconds];
			dispatch_async(dispatch_get_main_queue(), ^{
				stop_app();
			});
		});
	}

	[NSApp run];

	(void)local_context;
	[window orderOut:nil];
	return exit_code;
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

		if (opts.is_client) {
			return run_client(&opts);
		}

		char self_path[PATH_MAX];
		uint32_t self_path_size = sizeof(self_path);
		if (_NSGetExecutablePath(self_path, &self_path_size) != 0) {
			strlcpy(self_path, argv[0], sizeof(self_path));
		}
		return run_host(&opts, self_path);
	}
}
