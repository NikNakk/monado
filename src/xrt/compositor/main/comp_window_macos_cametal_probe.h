// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Opt-in CAMetalDisplayLink cadence probe for the macOS PS VR2 compositor.
 *
 * This diagnostic intentionally does not drive the compositor and does not attach
 * a second display link to the real compositor CAMetalLayer. Instead it creates a
 * tiny child CAMetalLayer, so the probe has its own drawable pool while inheriting
 * the real layer's display association. The existing compositor pacing and full-spin
 * behaviour are therefore left alone.
 *
 * Enable with XRT_MACOS_CAMETALDISPLAYLINK_PROBE=1.
 * Optionally set XRT_MACOS_CAMETALDISPLAYLINK_TRACE_PATH to choose the CSV path.
 */

#pragma once

#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static inline bool
macos_cametal_probe_enabled(void)
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

static inline uint64_t
macos_cametal_probe_monotonic_ns(void)
{
	struct timespec ts = {0};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

@interface MonadoCAMetalDisplayLinkProbe : NSObject <CAMetalDisplayLinkDelegate>
{
@private
	CAMetalLayer *_probeLayer;
	CAMetalDisplayLink *_displayLink;
	NSThread *_thread;
	FILE *_trace;
	uint64_t _lastCallbackNs;
	CFTimeInterval _lastTargetTimestamp;
	CFTimeInterval _lastTargetPresentationTimestamp;
	uint64_t _sampleCount;
}
- (instancetype)initWithParentLayer:(CAMetalLayer *)parentLayer;
- (void)start;
- (void)probeThreadMain;
@end

static MonadoCAMetalDisplayLinkProbe *g_macos_cametal_probe = nil;
static atomic_bool g_macos_cametal_probe_started = ATOMIC_VAR_INIT(false);

@implementation MonadoCAMetalDisplayLinkProbe

- (instancetype)initWithParentLayer:(CAMetalLayer *)parentLayer
{
	self = [super init];
	if (self == nil) {
		return nil;
	}

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

	const char *requested_path = getenv("XRT_MACOS_CAMETALDISPLAYLINK_TRACE_PATH");
	char default_path[256] = {0};
	if (requested_path == NULL || requested_path[0] == '\0') {
		snprintf(default_path, sizeof(default_path), "/tmp/monado-cametallink-probe-%d.csv", (int)getpid());
		requested_path = default_path;
	}
	_trace = fopen(requested_path, "w");
	if (_trace == NULL) {
		fprintf(stderr, "macOS CAMetalDisplayLink probe: could not open '%s'\n", requested_path);
		[_probeLayer removeFromSuperlayer];
		[_probeLayer release];
		_probeLayer = nil;
		[self release];
		return nil;
	}
	setvbuf(_trace, NULL, _IOFBF, 4u * 1024u * 1024u);
	fputs("sample,callback_monotonic_ns,callback_media_s,callback_delta_ms,target_timestamp_s,target_delta_ms,"
	      "target_presentation_timestamp_s,presentation_delta_ms,target_minus_callback_ms,"
	      "presentation_minus_callback_ms,presentation_minus_target_ms\n",
	      _trace);
	/* Make probe creation distinguishable from 'file opened but callback never fired'. */
	fflush(_trace);

	fprintf(stderr,
	        "macOS CAMetalDisplayLink probe enabled on an independent 1x1 child layer; trace: %s\n",
	        requested_path);
	return self;
}

- (void)start
{
	_thread = [[NSThread alloc] initWithTarget:self selector:@selector(probeThreadMain) object:nil];
	[_thread setName:@"Monado CAMetalDisplayLink probe"];
	[_thread setQualityOfService:NSQualityOfServiceUserInteractive];
	[_thread start];
}

- (void)probeThreadMain
{
	@autoreleasepool {
		if (@available(macOS 14.0, *)) {
			_displayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:_probeLayer];
			[_displayLink setDelegate:self];
			[_displayLink setPreferredFrameLatency:1.0f];
			[_displayLink setPreferredFrameRateRange:CAFrameRateRangeMake(120.0f, 120.0f, 120.0f)];
			[_displayLink addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
			fprintf(stderr,
			        "macOS CAMetalDisplayLink probe requesting 120 Hz, preferredFrameLatency=1\n");

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
			fprintf(stderr, "macOS CAMetalDisplayLink probe requires macOS 14 or later\n");
		}
	}
}

- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update
{
	(void)link;
	uint64_t callback_ns = macos_cametal_probe_monotonic_ns();
	CFTimeInterval callback_media_s = CACurrentMediaTime();
	CFTimeInterval target_s = [update targetTimestamp];
	CFTimeInterval presentation_s = [update targetPresentationTimestamp];

	double callback_delta_ms =
	    _lastCallbackNs != 0 && callback_ns > _lastCallbackNs ? (double)(callback_ns - _lastCallbackNs) / 1000000.0 : 0.0;
	double target_delta_ms =
	    _lastTargetTimestamp > 0.0 ? (target_s - _lastTargetTimestamp) * 1000.0 : 0.0;
	double presentation_delta_ms = _lastTargetPresentationTimestamp > 0.0
	                                   ? (presentation_s - _lastTargetPresentationTimestamp) * 1000.0
	                                   : 0.0;

	_sampleCount++;
	fprintf(_trace, "%llu,%llu,%.9f,%.6f,%.9f,%.6f,%.9f,%.6f,%.6f,%.6f,%.6f\n",
	        (unsigned long long)_sampleCount, (unsigned long long)callback_ns, callback_media_s, callback_delta_ms,
	        target_s, target_delta_ms, presentation_s, presentation_delta_ms,
	        (target_s - callback_media_s) * 1000.0, (presentation_s - callback_media_s) * 1000.0,
	        (presentation_s - target_s) * 1000.0);

	/*
	 * CAMetalDisplayLink supplies this drawable specifically for the callback and
	 * expects present() before the callback deadline. Presenting the independent
	 * 1x1 probe drawable keeps the probe's own drawable pool flowing without
	 * touching the real compositor layer or changing HMD presentation behaviour.
	 */
	id<CAMetalDrawable> drawable = [update drawable];
	if (drawable != nil) {
		[drawable present];
	}

	if (_sampleCount == 1) {
		fprintf(stderr,
		        "macOS CAMetalDisplayLink probe received first callback: target lead %.3fms, presentation lead %.3fms\n",
		        (target_s - callback_media_s) * 1000.0, (presentation_s - callback_media_s) * 1000.0);
	}

	_lastCallbackNs = callback_ns;
	_lastTargetTimestamp = target_s;
	_lastTargetPresentationTimestamp = presentation_s;

	/* Flush once per nominal second: low overhead, but useful during/after a failed run. */
	if ((_sampleCount % 120ULL) == 0) {
		fflush(_trace);
	}
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
		[g_macos_cametal_probe start];
	} else {
		fprintf(stderr, "macOS CAMetalDisplayLink probe requires macOS 14 or later\n");
	}
}

/*
 * Narrow source-level interception point: the real PS VR2 layer's drawable size
 * is configured once during target init. Calling the real selector first preserves
 * existing behaviour, then starts the independent probe layer when opted in.
 */
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

/* Defined last so the real selector references above are not rewritten recursively. */
#define setDrawableSize monadoProbeSetDrawableSize
