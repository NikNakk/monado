// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "multi/comp_multi_macos_displaylink.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Keep the normal target's CVDisplayLink object/nominal-period discovery intact.
 * Only real-layer driven mode suppresses its callback: hybrid deliberately keeps
 * the real display's CVDisplayLink running for independent phase/diagnostic data.
 */
static inline CVReturn
macos_cametal_drive_cvdisplaylink_start(CVDisplayLinkRef display_link)
{
	if (comp_multi_macos_displaylink_driven_mode()) {
		static bool logged = false;
		if (!logged) {
			fprintf(stderr,
			        "INFO: macOS CAMetalDisplayLink driven mode: legacy CVDisplayLink callback suppressed\n");
			logged = true;
		}
		(void)display_link;
		return kCVReturnSuccess;
	}
	return CVDisplayLinkStart(display_link);
}

#define CVDisplayLinkStart(display_link) macos_cametal_drive_cvdisplaylink_start((display_link))

/*
 * comp_compositor.c needs the stable consumed CAMetal timing in both driven and
 * hybrid modes so pose prediction follows the callback that woke that frame.
 *
 * The HMD target needs a deliberately different interpretation:
 *   - driven: return true, causing comp_window_macos.m to use the callback-owned
 *     drawable and plain presentDrawable: semantics;
 *   - hybrid: recover the timing belonging to this present job's captured
 *     desired_present_time_ns (the callback target/deadline), copy that frame's
 *     presentation timestamp into target_output_ns, then return false. The source
 *     therefore continues through its proven pre-latch + presentDrawable:atTime:
 *     path while remaining immune to newer callbacks consumed before an async
 *     presentation worker gets to this frame.
 *
 * This macro is force-included only into the macOS target translation unit. The
 * call site in macos_execute_present_job() has target_output_ns and
 * desired_present_time_ns locals, intentionally captured here. This preserves the
 * source's existing host-clock conversion and timing trace fields.
 */
static inline bool
macos_target_displaylink_current_timing(uint64_t *target_output_ns,
                                        int64_t desired_present_time_ns,
                                        struct comp_multi_macos_displaylink_timing *out_timing)
{
	if (comp_multi_macos_displaylink_hybrid_mode()) {
		bool valid = comp_multi_macos_displaylink_timing_for_deadline(desired_present_time_ns, out_timing);
		if (valid && target_output_ns != NULL && out_timing->presentation_ns > 0) {
			*target_output_ns = (uint64_t)out_timing->presentation_ns;
		}
		/* Always retain the legacy real-layer timed-presentation branch. */
		return false;
	}

	if (!comp_multi_macos_displaylink_driven_mode()) {
		return false;
	}
	return comp_multi_macos_displaylink_current_timing(out_timing);
}

#define comp_multi_macos_displaylink_current_timing(out_timing) \
	macos_target_displaylink_current_timing(&target_output_ns, desired_present_time_ns, (out_timing))

/*
 * Strict driven-mode invariant: the real compositor layer may consume only
 * update.drawable from its active CAMetalDisplayLink callback. Hybrid and legacy
 * modes pass through to the real CAMetalLayer nextDrawable implementation.
 */
#ifdef nextDrawable
#undef nextDrawable
#endif

@interface NSObject (MonadoCAMetalDisplayLinkDriveDrawableStrict)
- (id<CAMetalDrawable>)monadoCAMetalDrivenNextDrawableStrict;
@end

@implementation NSObject (MonadoCAMetalDisplayLinkDriveDrawableStrict)
- (id<CAMetalDrawable>)monadoCAMetalDrivenNextDrawableStrict
{
	if (comp_multi_macos_displaylink_driven_mode() && self == g_macos_cametal_driver_layer) {
		return (id<CAMetalDrawable>)comp_multi_macos_displaylink_current_drawable();
	}
	return [(CAMetalLayer *)self nextDrawable];
}
@end

#define nextDrawable monadoCAMetalDrivenNextDrawableStrict
