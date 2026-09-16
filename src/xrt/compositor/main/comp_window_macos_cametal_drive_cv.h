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
 * Keep the normal target's CVDisplayLink object/nominal-period discovery intact,
 * but do not start its callback when CAMetalDisplayLink owns compositor cadence.
 * This leaves one display cadence trigger in the experiment without modifying the
 * established macOS target source.
 */
static inline CVReturn
macos_cametal_drive_cvdisplaylink_start(CVDisplayLinkRef display_link)
{
	if (comp_multi_macos_displaylink_enabled()) {
		static bool logged = false;
		if (!logged) {
			fprintf(stderr,
			        "INFO: macOS CAMetalDisplayLink-driven compositor: legacy CVDisplayLink callback suppressed\n");
			logged = true;
		}
		(void)display_link;
		return kCVReturnSuccess;
	}
	return CVDisplayLinkStart(display_link);
}

#define CVDisplayLinkStart(display_link) macos_cametal_drive_cvdisplaylink_start((display_link))

/*
 * Strict experiment invariant: while CAMetalDisplayLink drive mode is enabled,
 * the real compositor layer may consume only update.drawable from the active
 * callback. Do not silently fall back to CAMetalLayer nextDrawable if the bridge
 * has no supplied drawable: returning nil makes that exceptional frame fail/drop
 * visibly instead of contaminating the A/B with the legacy acquisition path.
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
	if (macos_cametal_drive_enabled() && self == g_macos_cametal_driver_layer) {
		return (id<CAMetalDrawable>)comp_multi_macos_displaylink_current_drawable();
	}
	return [(CAMetalLayer *)self nextDrawable];
}
@end

#define nextDrawable monadoCAMetalDrivenNextDrawableStrict
