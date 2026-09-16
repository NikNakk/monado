// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experimental real-layer CAMetalDisplayLink compositor driver.
 *
 * Force-included after comp_window_macos_trace_buffer.h and the independent
 * timing probe. XRT_MACOS_CAMETALDISPLAYLINK_DRIVE=1 attaches CAMetalDisplayLink
 * to the real PS VR2 CAMetalLayer, hands update.drawable synchronously to the
 * Multi Client Module, and makes the target consume that supplied drawable.
 *
 * In this mode:
 *  - no target nextDrawable acquisition is performed;
 *  - presentDrawable:atTime: is converted to plain presentDrawable:;
 *  - the callback remains alive until Metal has scheduled the supplied drawable;
 *  - stale callbacks whose target/presentation deadline is already past are
 *    drained without triggering a compositor frame.
 */
#pragma once

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "multi/comp_multi_macos_displaylink.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static inline bool
macos_cametal_drive_enabled(void)
{
	return comp_multi_macos_displaylink_enabled();
}

static inline uint64_t
macos_cametal_drive_monotonic_ns(void)
{
	struct timespec ts = {0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t
macos_cametal_drive_delta_to_monotonic_ns(uint64_t callback_ns,
                                          CFTimeInterval callback_media_s,
                                          CFTimeInterval timestamp_s)
{
	double delta_ns = (timestamp_s - callback_media_s) * 1000000000.0;
	if (!isfinite(delta_ns)) {
		return callback_ns;
	}
	if (delta_ns >= 0.0) {
		double result = (double)callback_ns + delta_ns;
		return result >= (double)UINT64_MAX ? UINT64_MAX : (uint64_t)llround(result);
	}
	double magnitude = -delta_ns;
	return magnitude >= (double)callback_ns ? 0 : callback_ns - (uint64_t)llround(magnitude);
}

enum macos_cametal_drive_thread_priority
{
	MACOS_CAMETAL_DRIVE_THREAD_NORMAL = 0,
	MACOS_CAMETAL_DRIVE_THREAD_INTERACTIVE,
	MACOS_CAMETAL_DRIVE_THREAD_REALTIME,
};

static inline int
macos_cametal_drive_preferred_frame_latency(void)
{
	const char *value = getenv("XRT_MACOS_CAMETALDISPLAYLINK_LATENCY");
	if (value == NULL || value[0] == '\0') {
		return 1;
	}

	char *end = NULL;
	long latency = strtol(value, &end, 10);
	if (end == value || *end != '\0' || (latency != 1 && latency != 2)) {
		fprintf(stderr,
		        "WARN: XRT_MACOS_CAMETALDISPLAYLINK_LATENCY='%s' is invalid; expected 1 or 2, using 1\n",
		        value);
		return 1;
	}
	return (int)latency;
}

static inline enum macos_cametal_drive_thread_priority
macos_cametal_drive_thread_priority(void)
{
	const char *value = getenv("XRT_MACOS_CAMETALDISPLAYLINK_THREAD_PRIORITY");
	if (value == NULL || value[0] == '\0' || strcmp(value, "interactive") == 0) {
		return MACOS_CAMETAL_DRIVE_THREAD_INTERACTIVE;
	}
	if (strcmp(value, "normal") == 0) {
		return MACOS_CAMETAL_DRIVE_THREAD_NORMAL;
	}
	if (strcmp(value, "realtime") == 0) {
		return MACOS_CAMETAL_DRIVE_THREAD_REALTIME;
	}
	fprintf(stderr,
	        "WARN: XRT_MACOS_CAMETALDISPLAYLINK_THREAD_PRIORITY='%s' is invalid; expected normal, interactive, or realtime; using interactive\n",
	        value);
	return MACOS_CAMETAL_DRIVE_THREAD_INTERACTIVE;
}

static inline const char *
macos_cametal_drive_thread_priority_name(enum macos_cametal_drive_thread_priority priority)
{
	switch (priority) {
	case MACOS_CAMETAL_DRIVE_THREAD_NORMAL: return "normal";
	case MACOS_CAMETAL_DRIVE_THREAD_REALTIME: return "realtime";
	case MACOS_CAMETAL_DRIVE_THREAD_INTERACTIVE:
	default: return "interactive";
	}
}

static inline uint32_t
macos_cametal_drive_ns_to_absolute_time(uint64_t ns)
{
	mach_timebase_info_data_t timebase = {0};
	if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) {
		return 0;
	}
	__uint128_t ticks = (__uint128_t)ns * timebase.denom / timebase.numer;
	return ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks;
}

static inline bool
macos_cametal_drive_apply_realtime_policy(void)
{
	thread_time_constraint_policy_data_t policy = {
	    .period = macos_cametal_drive_ns_to_absolute_time(8333333ULL),
	    .computation = macos_cametal_drive_ns_to_absolute_time(1000000ULL),
	    .constraint = macos_cametal_drive_ns_to_absolute_time(2000000ULL),
	    .preemptible = TRUE,
	};
	if (policy.period == 0 || policy.computation == 0 || policy.constraint == 0) {
		fprintf(stderr, "WARN: CAMetalDisplayLink realtime scheduling could not convert Mach time units\n");
		return false;
	}

	thread_port_t thread = pthread_mach_thread_np(pthread_self());
	kern_return_t ret = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy,
	                                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	if (ret != KERN_SUCCESS) {
		fprintf(stderr, "WARN: CAMetalDisplayLink realtime scheduling failed: %d\n", ret);
		return false;
	}

	fprintf(stderr,
	        "macOS CAMetalDisplayLink thread: Mach realtime period=8.333ms computation=1.000ms constraint=2.000ms\n");
	return true;
}

@interface MonadoCAMetalDisplayLinkDriver : NSObject <CAMetalDisplayLinkDelegate>
{
@private
	CAMetalLayer *_layer;
	CAMetalDisplayLink *_displayLink;
	NSThread *_thread;
	FILE *_trace;
	atomic_bool _stopping;
	int _preferredFrameLatency;
	enum macos_cametal_drive_thread_priority _threadPriority;
	uint64_t _callbackCount;
	uint64_t _consumedCount;
	uint64_t _staleCount;
	uint64_t _unconsumedCount;
	uint64_t _lastCallbackNs;
	CFTimeInterval _lastTargetTimestamp;
	CFTimeInterval _lastPresentationTimestamp;
}
- (instancetype)initWithLayer:(CAMetalLayer *)layer;
- (void)start;
- (void)stop;
- (void)threadMain;
@end

static MonadoCAMetalDisplayLinkDriver *g_macos_cametal_driver = nil;
static CAMetalLayer *g_macos_cametal_driver_layer = nil;
static atomic_bool g_macos_cametal_driver_started = ATOMIC_VAR_INIT(false);
static atomic_bool g_macos_cametal_driver_atexit_registered = ATOMIC_VAR_INIT(false);

static inline void
macos_cametal_drive_stop(void)
{
	@autoreleasepool {
		if (g_macos_cametal_driver != nil) {
			[g_macos_cametal_driver stop];
			[g_macos_cametal_driver release];
			g_macos_cametal_driver = nil;
		}
		comp_multi_macos_displaylink_set_active(false);
		g_macos_cametal_driver_layer = nil;
		atomic_store_explicit(&g_macos_cametal_driver_started, false, memory_order_release);
	}
}

static void
macos_cametal_drive_teardown(void)
{
	macos_cametal_drive_stop();
}

@implementation MonadoCAMetalDisplayLinkDriver

- (instancetype)initWithLayer:(CAMetalLayer *)layer
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	_layer = [layer retain];
	atomic_init(&_stopping, false);
	_preferredFrameLatency = macos_cametal_drive_preferred_frame_latency();
	_threadPriority = macos_cametal_drive_thread_priority();

#ifdef XRT_FEATURE_MACOS_TIMING_DIAGNOSTICS
	const char *trace_enabled = getenv("PSVR2_TIMING_TRACE");
	const char *requested_path = getenv("XRT_MACOS_CAMETALDISPLAYLINK_DRIVE_TRACE_PATH");
	if ((trace_enabled == NULL || strcmp(trace_enabled, "1") != 0) &&
	    (requested_path == NULL || requested_path[0] == '\0')) {
		return self;
	}
	char default_path[256] = {0};
	if (requested_path == NULL || requested_path[0] == '\0') {
		snprintf(default_path, sizeof(default_path), "/tmp/monado-cametallink-drive-%d.csv", (int)getpid());
		requested_path = default_path;
	}
	_trace = fopen(requested_path, "w");
	if (_trace != NULL) {
		if (macos_trace_fully_buffered_enabled()) {
			setvbuf(_trace, NULL, _IOFBF, 4u * 1024u * 1024u);
		} else {
			setvbuf(_trace, NULL, _IOLBF, 0);
		}
		fputs("sample,callback_monotonic_ns,callback_media_s,callback_delta_ms,target_timestamp_s,target_delta_ms,"
		      "target_presentation_timestamp_s,presentation_delta_ms,target_minus_callback_ms,"
		      "presentation_minus_callback_ms,presentation_minus_target_ms,outcome\n",
		      _trace);
		fflush(_trace);
		fprintf(stderr, "macOS CAMetalDisplayLink driver trace: %s%s\n", requested_path,
		        macos_trace_fully_buffered_enabled() ? " (fully buffered; written at teardown)" : "");
	} else {
		fprintf(stderr, "WARN: macOS CAMetalDisplayLink driver could not open trace '%s'\n", requested_path);
	}

#endif
	return self;
}

- (void)start
{
	_thread = [[NSThread alloc] initWithTarget:self selector:@selector(threadMain) object:nil];
	[_thread setName:@"Monado CAMetalDisplayLink compositor"];
	if (_threadPriority == MACOS_CAMETAL_DRIVE_THREAD_NORMAL) {
		[_thread setQualityOfService:NSQualityOfServiceDefault];
	} else {
		[_thread setQualityOfService:NSQualityOfServiceUserInteractive];
	}
	[_thread start];
}

- (void)stop
{
	if (atomic_exchange_explicit(&_stopping, true, memory_order_acq_rel)) {
		return;
	}

	/*
	 * A callback may be synchronously waiting for the compositor/present worker to
	 * schedule its supplied drawable. Release that wait before joining the NSThread
	 * so service/process teardown can always reach fclose() for fully buffered traces.
	 */
	comp_multi_macos_displaylink_cancel_pending_tick();

	if (_thread != nil) {
		[_thread cancel];
		if ([NSThread currentThread] != _thread) {
			while (![_thread isFinished]) {
				[NSThread sleepForTimeInterval:0.001];
			}
		}
		[_thread release];
		_thread = nil;
	}
	if (_trace != NULL) {
		/* fclose is intentionally not wrapped by the fully-buffered trace shim. */
		fclose(_trace);
		_trace = NULL;
	}
	fprintf(stderr,
	        "macOS CAMetalDisplayLink driver stopped: callbacks=%llu consumed=%llu stale=%llu unconsumed=%llu\n",
	        (unsigned long long)_callbackCount, (unsigned long long)_consumedCount,
	        (unsigned long long)_staleCount, (unsigned long long)_unconsumedCount);
	if (_layer != nil) {
		[_layer release];
		_layer = nil;
	}
}

- (void)dealloc
{
	[self stop];
	[super dealloc];
}

- (void)threadMain
{
	@autoreleasepool {
		if (_threadPriority == MACOS_CAMETAL_DRIVE_THREAD_REALTIME) {
			(void)macos_cametal_drive_apply_realtime_policy();
		}
		if (@available(macOS 14.0, *)) {
			_displayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:_layer];
			[_displayLink setDelegate:self];
			[_displayLink setPreferredFrameLatency:(float)_preferredFrameLatency];
			[_displayLink setPreferredFrameRateRange:CAFrameRateRangeMake(120.0f, 120.0f, 120.0f)];
			[_displayLink addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
			fprintf(stderr,
			        "macOS CAMetalDisplayLink driver attached to real compositor layer; requesting 120 Hz, "
			        "preferredFrameLatency=%d threadPriority=%s\n",
			        _preferredFrameLatency, macos_cametal_drive_thread_priority_name(_threadPriority));
			while (![[NSThread currentThread] isCancelled]) {
				@autoreleasepool {
					[[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
					                            beforeDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
				}
			}
			[_displayLink invalidate];
			[_displayLink release];
			_displayLink = nil;
		} else {
			fprintf(stderr, "macOS CAMetalDisplayLink driver requires macOS 14 or later\n");
		}
	}
}

- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update
{
	(void)link;
	if (atomic_load_explicit(&_stopping, memory_order_acquire)) {
		return;
	}

	id<CAMetalDrawable> drawable = [update drawable];
	if (drawable == nil) {
		return;
	}
	[drawable retain];

	uint64_t callback_ns = macos_cametal_drive_monotonic_ns();
	CFTimeInterval callback_media_s = CACurrentMediaTime();
	CFTimeInterval target_s = [update targetTimestamp];
	CFTimeInterval presentation_s = [update targetPresentationTimestamp];
	uint64_t target_ns =
	    macos_cametal_drive_delta_to_monotonic_ns(callback_ns, callback_media_s, target_s);
	uint64_t presentation_ns =
	    macos_cametal_drive_delta_to_monotonic_ns(callback_ns, callback_media_s, presentation_s);

	double callback_delta_ms =
	    _lastCallbackNs != 0 && callback_ns > _lastCallbackNs ? (double)(callback_ns - _lastCallbackNs) / 1000000.0 : 0.0;
	double target_delta_ms =
	    _lastTargetTimestamp > 0.0 ? (target_s - _lastTargetTimestamp) * 1000.0 : 0.0;
	double presentation_delta_ms =
	    _lastPresentationTimestamp > 0.0 ? (presentation_s - _lastPresentationTimestamp) * 1000.0 : 0.0;

	_callbackCount++;
	const char *outcome = "unconsumed";

	/*
	 * The probe found rare catch-up callbacks that arrived after their own target
	 * timestamp. Rendering such an update would intentionally start a stale frame,
	 * even if Core Animation still reports a later presentation timestamp.
	 */
	if (target_s <= callback_media_s || presentation_s <= callback_media_s || target_ns <= callback_ns ||
	    presentation_ns <= callback_ns) {
		_staleCount++;
		outcome = "stale";
		[drawable present];
	} else {
		bool consumed =
		    comp_multi_macos_displaylink_submit_tick((void *)drawable, callback_ns, target_ns, presentation_ns);
		if (consumed) {
			_consumedCount++;
			outcome = "consumed";
		} else {
			/* No active compositor consumer before the CA deadline: keep its pool flowing. */
			_unconsumedCount++;
			if (!atomic_load_explicit(&_stopping, memory_order_acquire)) {
				[drawable present];
			}
		}
	}

	if (_trace != NULL) {
		fprintf(_trace, "%llu,%llu,%.9f,%.6f,%.9f,%.6f,%.9f,%.6f,%.6f,%.6f,%.6f,%s\n",
		        (unsigned long long)_callbackCount, (unsigned long long)callback_ns, callback_media_s,
		        callback_delta_ms, target_s, target_delta_ms, presentation_s, presentation_delta_ms,
		        (target_s - callback_media_s) * 1000.0, (presentation_s - callback_media_s) * 1000.0,
		        (presentation_s - target_s) * 1000.0, outcome);
		if ((_callbackCount % 120ULL) == 0) {
			fflush(_trace);
		}
	}

	_lastCallbackNs = callback_ns;
	_lastTargetTimestamp = target_s;
	_lastPresentationTimestamp = presentation_s;
	[drawable release];
}

@end

static inline void
macos_cametal_drive_start_for_layer(CAMetalLayer *layer)
{
	if (!macos_cametal_drive_enabled() || layer == nil) {
		return;
	}
	bool expected = false;
	if (!atomic_compare_exchange_strong_explicit(&g_macos_cametal_driver_started, &expected, true,
	                                             memory_order_acq_rel, memory_order_acquire)) {
		return;
	}
	if (@available(macOS 14.0, *)) {
		g_macos_cametal_driver_layer = layer;
		g_macos_cametal_driver = [[MonadoCAMetalDisplayLinkDriver alloc] initWithLayer:layer];
		if (g_macos_cametal_driver != nil) {
			bool atexit_expected = false;
			if (atomic_compare_exchange_strong_explicit(&g_macos_cametal_driver_atexit_registered, &atexit_expected,
			                                             true, memory_order_acq_rel, memory_order_acquire)) {
				atexit(macos_cametal_drive_teardown);
			}
			comp_multi_macos_displaylink_set_active(true);
			[g_macos_cametal_driver start];
		} else {
			atomic_store_explicit(&g_macos_cametal_driver_started, false, memory_order_release);
		}
	} else {
		atomic_store_explicit(&g_macos_cametal_driver_started, false, memory_order_release);
		fprintf(stderr, "macOS CAMetalDisplayLink driver requires macOS 14 or later\n");
	}
}

/*
 * Chain after the independent probe's setDrawableSize interception. That method
 * calls the real selector first and starts only the child-layer probe when its
 * own PROBE environment flag is enabled.
 */
#ifdef setDrawableSize
#undef setDrawableSize
#endif

@interface NSObject (MonadoCAMetalDisplayLinkDriveAttach)
- (void)monadoDriveSetDrawableSize:(CGSize)size;
@end

@implementation NSObject (MonadoCAMetalDisplayLinkDriveAttach)
- (void)monadoDriveSetDrawableSize:(CGSize)size
{
	[self monadoProbeSetDrawableSize:size];
	if (size.width >= 1000.0 && size.height >= 1000.0 && [self isKindOfClass:[CAMetalLayer class]]) {
		macos_cametal_drive_start_for_layer((CAMetalLayer *)self);
	}
}
@end

#define setDrawableSize monadoDriveSetDrawableSize

/*
 * Replace target nextDrawable only for the real CAMetalLayer in drive mode. The
 * CAMetalDisplayLink delegate retains update.drawable until Metal has scheduled
 * its presentation, so this borrowed reference remains valid across an existing
 * async present-worker handoff.
 */
@interface NSObject (MonadoCAMetalDisplayLinkDriveDrawable)
- (id<CAMetalDrawable>)monadoCAMetalDrivenNextDrawable;
@end

@implementation NSObject (MonadoCAMetalDisplayLinkDriveDrawable)
- (id<CAMetalDrawable>)monadoCAMetalDrivenNextDrawable
{
	if (macos_cametal_drive_enabled() && self == g_macos_cametal_driver_layer) {
		return (id<CAMetalDrawable>)comp_multi_macos_displaylink_current_drawable();
	}
	return [(CAMetalLayer *)self nextDrawable];
}
@end

#define nextDrawable monadoCAMetalDrivenNextDrawable

/*
 * The trace-buffer header has already installed monadoPresentDrawable wrappers.
 * Re-wrap source-level calls so drive mode discards absolute atTime scheduling,
 * while every non-drive path retains the previous diagnostic behaviour exactly.
 *
 * Crucially, a driven tick is completed only when the Metal command buffer that
 * owns the supplied drawable reaches the scheduled state. At that point Metal
 * owns the presentation request and the CAMetalDisplayLink delegate can safely
 * return/release its extra drawable retain, even when the target used its normal
 * asynchronous present-worker path.
 */
#ifdef presentDrawable
#undef presentDrawable
#endif

static inline void
macos_cametal_drive_complete_when_scheduled(id<MTLCommandBuffer> command_buffer, id<MTLDrawable> drawable)
{
	if (!macos_cametal_drive_enabled() || command_buffer == nil || drawable == nil) {
		return;
	}
	void *current = comp_multi_macos_displaylink_current_drawable();
	if (current == NULL || current != (void *)drawable) {
		return;
	}
	[command_buffer addScheduledHandler:^(id<MTLCommandBuffer> scheduled_buffer) {
		(void)scheduled_buffer;
		comp_multi_macos_displaylink_complete_tick();
	}];
}

@interface NSObject (MonadoCAMetalDisplayLinkDrivePresent)
- (void)monadoCAMetalDrivenPresentDrawable:(id<MTLDrawable>)drawable;
- (void)monadoCAMetalDrivenPresentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)presentationTime;
@end

@implementation NSObject (MonadoCAMetalDisplayLinkDrivePresent)
- (void)monadoCAMetalDrivenPresentDrawable:(id<MTLDrawable>)drawable
{
	id<MTLCommandBuffer> command_buffer = (id<MTLCommandBuffer>)self;
	[command_buffer monadoPresentDrawable:drawable];
	macos_cametal_drive_complete_when_scheduled(command_buffer, drawable);
}

- (void)monadoCAMetalDrivenPresentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)presentationTime
{
	id<MTLCommandBuffer> command_buffer = (id<MTLCommandBuffer>)self;
	if (macos_cametal_drive_enabled()) {
		/* CAMetalDisplayLink supplies the timing; never schedule this drawable at an absolute time. */
		[command_buffer monadoPresentDrawable:drawable];
		macos_cametal_drive_complete_when_scheduled(command_buffer, drawable);
		return;
	}
	[command_buffer monadoPresentDrawable:drawable atTime:presentationTime];
}
@end

#define presentDrawable monadoCAMetalDrivenPresentDrawable