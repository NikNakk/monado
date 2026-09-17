// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "multi/comp_multi_macos_displaylink.h"

#include <stdbool.h>
#include <stdio.h>

/*
 * Keep the normal target's CVDisplayLink object/nominal-period discovery intact.
 * Only real-layer driven mode suppresses its callback: hybrid deliberately keeps
 * the real display's CVDisplayLink running so normal timed HMD presentation keeps
 * the same phase source as the proven legacy path.
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
 * comp_compositor.c needs the consumed CAMetal timing in both driven and hybrid
 * modes so renderer prediction follows the callback that woke the frame. The HMD
 * target is different: only driven mode may use that timing as the callback-owned
 * drawable's presentation target. Hybrid must keep the normal real-CVDisplayLink
 * target selection and presentDrawable:atTime: path.
 *
 * This force-included wrapper is local to the macOS target translation unit, so
 * it hides CAMetal timing from comp_window_macos.m in hybrid mode without changing
 * what comp_compositor.c sees.
 */
static inline bool
macos_target_displaylink_current_timing(struct comp_multi_macos_displaylink_timing *out_timing)
{
	if (!comp_multi_macos_displaylink_driven_mode()) {
		return false;
	}
	return comp_multi_macos_displaylink_current_timing(out_timing);
}

#define comp_multi_macos_displaylink_current_timing(out_timing) \
	macos_target_displaylink_current_timing((out_timing))

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
