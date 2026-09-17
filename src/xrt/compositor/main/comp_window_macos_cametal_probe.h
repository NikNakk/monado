// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Independent child-layer CAMetalDisplayLink probe and hybrid cadence source.
 *
 * In ordinary probe mode this creates a tiny child CAMetalLayer and records timing
 * without changing compositor pacing. In XRT_MACOS_CAMETALDISPLAYLINK_MODE=hybrid
 * the same independent display link becomes a timing-only compositor cadence
 * source: it publishes callback timestamps and immediately returns, while the real
 * HMD CAMetalLayer continues to use normal nextDrawable/timed presentation.
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
macos_cametal_probe_explicitly_enabled(void)
{
	static int initialized = 0;
	static bool enabled = false;
	if (!initialized) {
		const char *value = getenv("XRT_MACOS_CAMETALDISPLAYLINK_PROBE");
		enabled = value != NULL && strcmp(value, "1") == 0;
		initialized = 1;
	}
	return enabled;
}

static inline bool
macos_cametal_probe_enabled(void)
{
	return macos_cametal_probe_explicitly_enabled() || comp_multi_macos_displaylink_hybrid_mode();
}

static inline uint64_t
macos_cametal_probe_monotonic_ns(void)
{
	struct timespec ts = {0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline uint64_t
macos_cametal_probe_delta_to_monotonic_ns(uint64_t callback_ns,
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

enum macos_cametal_probe_thread_priority
{
	MACOS_CAMETAL_PROBE_THREAD_NORMAL = 0,
	MACOS_CAMETAL_PROBE_THREAD_INTERACTIVE,
	MACOS_CAMETAL_PROBE_THREAD_REALTIME,
};

static inline int
macos_cametal_probe_preferred_frame_latency(void)
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

static inline enum macos_cametal_probe_thread_priority
macos_cametal_probe_thread_priority(void)
{
	const char *value = getenv("XRT_MACOS_CAMETALDISPLAYLINK_THREAD_PRIORITY");
	if (value == NULL || value[0] == '\0' || strcmp(value, "interactive") == 0) {
		return MACOS_CAMETAL_PROBE_THREAD_INTERACTIVE;
	}
	if (strcmp(value, "normal") == 0) {
		return MACOS_CAMETAL_PROBE_THREAD_NORMAL;
	}
	if (strcmp(value, "realtime") == 0) {
		return MACOS_CAMETAL_PROBE_THREAD_REALTIME;
	}
	fprintf(stderr,
	        "WARN: XRT_MACOS_CAMETALDISPLAYLINK_THREAD_PRIORITY='%s' is invalid; expected normal, interactive, or realtime; using interactive\n",
		        value);
	return MACOS_CAMETAL_PROBE_THREAD_INTERACTIVE;
}

static inline const char *
macos_cametal_probe_thread_priority_name(enum macos_cametal_probe_thread_priority priority)
{
	switch (priority) {
	case MACOS_CAMETAL_PROBE_THREAD_NORMAL: return "normal";
	case MACOS_CAMETAL_PROBE_THREAD_REALTIME: return "realtime";
	case MACOS_CAMETAL_PROBE_THREAD_INTERACTIVE:
	default: return "interactive";
	}
}

static inline uint32_t
macos_cametal_probe_ns_to_absolute_time(uint64_t ns)
{
	mach_timebase_info_data_t timebase = {0};
	if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0) {
		return 0;
	}
	__uint128_t ticks = (__uint128_t)ns * timebase.denom / timebase.numer;
	return ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks;
}

static inline bool
macos_cametal_probe_apply_realtime_policy(void)
{
	thread_time_constraint_policy_data_t policy = {
	    .period = macos_cametal_probe_ns_to_absolute_time(8333333ULL),
	    .computation = macos_cametal_probe_ns_to_absolute_time(1000000ULL),
	    .constraint = macos_cametal_probe_ns_to_absolute_time(2000000ULL),
	    .preemptible = TRUE,
	};
	if (policy.period == 0 || policy.computation == 0 || policy.constraint == 0) {
		fprintf(stderr, "WARN: CAMetalDisplayLink hybrid/probe realtime scheduling could not convert Mach time units\n");
		return false;
	}
	thread_port_t thread = pthread_mach_thread_np(pthread_self());
	kern_return_t ret = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy,
	                                      THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	if (ret != KERN_SUCCESS) {
		fprintf(stderr, "WARN: CAMetalDisplayLink hybrid/probe realtime scheduling failed: %d\n", ret);
		return false;
	}
	fprintf(stderr,
	        "macOS CAMetalDisplayLink hybrid/probe thread: Mach realtime period=8.333ms computation=1.000ms constraint=2.000ms\n");
	return true;
}

@interface MonadoCAMetalDisplayLinkProbe : NSObject <CAMetalDisplayLinkDelegate>
{
@private
	CAMetalLayer *_probeLayer;
	CAMetalDisplayLink *_displayLink;
	NSThread *_thread;
	FILE *_trace;
	atomic_bool _stopping;
	int _preferredFrameLatency;
	enum macos_cametal_probe_thread_priority _threadPriority;
	uint64_t _lastCallbackNs;
	CFTimeInterval _lastTargetTimestamp;
	CFTimeInterval _lastTargetPresentationTimestamp;
	uint64_t _sampleCount;
	uint64_t _publishedCount;
	uint64_t _staleCount;
}
- (instancetype)initWithParentLayer:(CAMetalLayer *)parentLayer;
- (void)start;
- (void)stop;
- (void)probeThreadMain;
@end

static MonadoCAMetalDisplayLinkProbe *g_macos_cametal_probe = nil;
static atomic_bool g_macos_cametal_probe_started = ATOMIC_VAR_INIT(false);
static atomic_bool g_macos_cametal_probe_atexit_registered = ATOMIC_VAR_INIT(false);

static void
macos_cametal_probe_teardown(void)
{
	@autoreleasepool {
		if (g_macos_cametal_probe != nil) {
			[g_macos_cametal_probe stop];
			[g_macos_cametal_probe release];
			g_macos_cametal_probe = nil;
		}
		if (comp_multi_macos_displaylink_hybrid_mode()) {
			comp_multi_macos_displaylink_set_active(false);
		}
		atomic_store_explicit(&g_macos_cametal_probe_started, false, memory_order_release);
	}
}

@implementation MonadoCAMetalDisplayLinkProbe

- (instancetype)initWithParentLayer:(CAMetalLayer *)parentLayer
{
	self = [super init];
	if (self == nil) {
		return nil;
	}
	atomic_init(&_stopping, false);
	_preferredFrameLatency = macos_cametal_probe_preferred_frame_latency();
	_threadPriority = macos_cametal_probe_thread_priority();

	_probeLayer = [[CAMetalLayer alloc] init];
	[_probeLayer setDevice:[parentLayer device]];
	[_probeLayer setPixelFormat:MTLPixelFormatBGRA8Unorm];
	[_probeLayer setFramebufferOnly:YES];
	[_probeLayer setOpaque:NO];
	[_probeLayer setBackgroundColor:[[NSColor clearColor] CGColor]];
	[_probeLayer setFrame:CGRectMake(0.0, 0.0, 1.0, 1.0)];
	[_probeLayer setDrawableSize:CGSizeMake(1.0, 1.0)];
	[_probeLayer setContentsScale:1.0];
	[_probeLayer setDisplaySyncEnabled:YES];
	[_probeLayer setAllowsNextDrawableTimeout:YES];
	[_probeLayer setMaximumDrawableCount:2];
	[parentLayer addSublayer:_probeLayer];

	const char *trace_enabled = getenv("PSVR2_TIMING_TRACE");
	const char *requested_path = getenv("XRT_MACOS_CAMETALDISPLAYLINK_TRACE_PATH");
	bool want_trace = (trace_enabled != NULL && strcmp(trace_enabled, "1") == 0) ||
	                  (requested_path != NULL && requested_path[0] != '\0');
	if (want_trace) {
		char default_path[256] = {0};
		if (requested_path == NULL || requested_path[0] == '\0') {
			snprintf(default_path, sizeof(default_path), "/tmp/monado-cametallink-probe-%d.csv", (int)getpid());
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
			      "presentation_minus_callback_ms,presentation_minus_target_ms,cadence_published\n",
			      _trace);
			fflush(_trace);
			fprintf(stderr, "macOS CAMetalDisplayLink child-layer trace: %s%s\n", requested_path,
			        macos_trace_fully_buffered_enabled() ? " (fully buffered; written at teardown)" : "");
		} else {
			fprintf(stderr, "WARN: macOS CAMetalDisplayLink child-layer trace could not open '%s'\n", requested_path);
		}
	}
	return self;
}

- (void)start
{
	_thread = [[NSThread alloc] initWithTarget:self selector:@selector(probeThreadMain) object:nil];
	[_thread setName:comp_multi_macos_displaylink_hybrid_mode() ? @"Monado CAMetalDisplayLink hybrid cadence"
	                                                       : @"Monado CAMetalDisplayLink probe"];
	if (_threadPriority == MACOS_CAMETAL_PROBE_THREAD_NORMAL) {
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
		fclose(_trace);
		_trace = NULL;
	}
	fprintf(stderr,
	        "macOS CAMetalDisplayLink child-layer stopped: callbacks=%llu published=%llu stale=%llu\n",
	        (unsigned long long)_sampleCount, (unsigned long long)_publishedCount,
	        (unsigned long long)_staleCount);
	if (_probeLayer != nil) {
		[_probeLayer removeFromSuperlayer];
		[_probeLayer release];
		_probeLayer = nil;
	}
}

- (void)dealloc
{
	[self stop];
	[super dealloc];
}

- (void)probeThreadMain
{
	@autoreleasepool {
		if (_threadPriority == MACOS_CAMETAL_PROBE_THREAD_REALTIME) {
			(void)macos_cametal_probe_apply_realtime_policy();
		}
		if (@available(macOS 14.0, *)) {
			_displayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:_probeLayer];
			[_displayLink setDelegate:self];
			[_displayLink setPreferredFrameLatency:(float)_preferredFrameLatency];
			[_displayLink setPreferredFrameRateRange:CAFrameRateRangeMake(120.0f, 120.0f, 120.0f)];
			[_displayLink addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
			fprintf(stderr,
			        "macOS CAMetalDisplayLink %s on independent 1x1 child layer; requesting 120 Hz, preferredFrameLatency=%d threadPriority=%s\n",
			        comp_multi_macos_displaylink_hybrid_mode() ? "hybrid cadence" : "probe",
			        _preferredFrameLatency, macos_cametal_probe_thread_priority_name(_threadPriority));

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
			fprintf(stderr, "macOS CAMetalDisplayLink child-layer driver requires macOS 14 or later\n");
		}
	}
}

- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update
{
	(void)link;
	if (atomic_load_explicit(&_stopping, memory_order_acquire)) {
		return;
	}

	uint64_t callback_ns = macos_cametal_probe_monotonic_ns();
	CFTimeInterval callback_media_s = CACurrentMediaTime();
	CFTimeInterval target_s = [update targetTimestamp];
	CFTimeInterval presentation_s = [update targetPresentationTimestamp];
	uint64_t target_ns = macos_cametal_probe_delta_to_monotonic_ns(callback_ns, callback_media_s, target_s);
	uint64_t presentation_ns =
	    macos_cametal_probe_delta_to_monotonic_ns(callback_ns, callback_media_s, presentation_s);

	double callback_delta_ms =
	    _lastCallbackNs != 0 && callback_ns > _lastCallbackNs ? (double)(callback_ns - _lastCallbackNs) / 1000000.0 : 0.0;
	double target_delta_ms =
	    _lastTargetTimestamp > 0.0 ? (target_s - _lastTargetTimestamp) * 1000.0 : 0.0;
	double presentation_delta_ms = _lastTargetPresentationTimestamp > 0.0
	                                   ? (presentation_s - _lastTargetPresentationTimestamp) * 1000.0
	                                   : 0.0;

	_sampleCount++;
	bool cadence_published = false;
	if (comp_multi_macos_displaylink_hybrid_mode()) {
		if (target_s <= callback_media_s || presentation_s <= callback_media_s || target_ns <= callback_ns ||
		    presentation_ns <= callback_ns) {
			_staleCount++;
		} else {
			cadence_published = comp_multi_macos_displaylink_submit_tick(NULL, callback_ns, target_ns, presentation_ns);
			if (cadence_published) {
				_publishedCount++;
			}
		}
	}

	if (_trace != NULL) {
		fprintf(_trace, "%llu,%llu,%.9f,%.6f,%.9f,%.6f,%.9f,%.6f,%.6f,%.6f,%.6f,%u\n",
		        (unsigned long long)_sampleCount, (unsigned long long)callback_ns, callback_media_s, callback_delta_ms,
		        target_s, target_delta_ms, presentation_s, presentation_delta_ms,
		        (target_s - callback_media_s) * 1000.0, (presentation_s - callback_media_s) * 1000.0,
		        (presentation_s - target_s) * 1000.0, cadence_published ? 1u : 0u);
		if ((_sampleCount % 120ULL) == 0) {
			fflush(_trace);
		}
	}

	/* Always drain the independent layer's own drawable pool. This drawable is
	 * never handed to the real HMD compositor in hybrid mode. */
	id<CAMetalDrawable> drawable = [update drawable];
	if (drawable != nil) {
		[drawable present];
	}

	if (_sampleCount == 1) {
		fprintf(stderr,
		        "macOS CAMetalDisplayLink child layer received first callback: target lead %.3fms, presentation lead %.3fms\n",
		        (target_s - callback_media_s) * 1000.0, (presentation_s - callback_media_s) * 1000.0);
	}

	_lastCallbackNs = callback_ns;
	_lastTargetTimestamp = target_s;
	_lastTargetPresentationTimestamp = presentation_s;
}

@end

static inline void
macos_cametal_probe_start_for_layer(CAMetalLayer *parentLayer)
{
	if (!macos_cametal_probe_enabled() || parentLayer == nil) {
		return;
	}
	bool expected = false;
	if (!atomic_compare_exchange_strong_explicit(&g_macos_cametal_probe_started, &expected, true,
	                                             memory_order_acq_rel, memory_order_acquire)) {
		return;
	}
	if (@available(macOS 14.0, *)) {
		g_macos_cametal_probe = [[MonadoCAMetalDisplayLinkProbe alloc] initWithParentLayer:parentLayer];
		if (g_macos_cametal_probe != nil) {
			bool atexit_expected = false;
			if (atomic_compare_exchange_strong_explicit(&g_macos_cametal_probe_atexit_registered, &atexit_expected,
			                                             true, memory_order_acq_rel, memory_order_acquire)) {
				atexit(macos_cametal_probe_teardown);
			}
			if (comp_multi_macos_displaylink_hybrid_mode()) {
				comp_multi_macos_displaylink_set_active(true);
			}
			[g_macos_cametal_probe start];
		} else {
			atomic_store_explicit(&g_macos_cametal_probe_started, false, memory_order_release);
		}
	} else {
		atomic_store_explicit(&g_macos_cametal_probe_started, false, memory_order_release);
		fprintf(stderr, "macOS CAMetalDisplayLink child-layer driver requires macOS 14 or later\n");
	}
}

/* Narrow source-level interception point: the real PS VR2 layer's drawable size
 * is configured once during target init. Calling the real selector first preserves
 * existing behaviour, then starts the independent child layer when requested. */
@interface NSObject (MonadoCAMetalDisplayLinkProbeAttach)
- (void)monadoProbeSetDrawableSize:(CGSize)size;
@end

@implementation NSObject (MonadoCAMetalDisplayLinkProbeAttach)
- (void)monadoProbeSetDrawableSize:(CGSize)size
{
	[(CAMetalLayer *)self setDrawableSize:size];
	if (size.width >= 1000.0 && size.height >= 1000.0 && [self isKindOfClass:[CAMetalLayer class]]) {
		macos_cametal_probe_start_for_layer((CAMetalLayer *)self);
	}
}
@end

#define setDrawableSize monadoProbeSetDrawableSize
