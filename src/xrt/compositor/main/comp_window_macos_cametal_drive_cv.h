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
 * The driven header normally returns the callback-owned drawable. Its bridge has
 * a bounded 100-ms wait solely so service teardown or a failed display link cannot
 * deadlock the Multi Client Module. If such a fallback frame occurs there is no
 * callback-owned drawable, so let that exceptional frame use the real nextDrawable
 * path rather than fail with a null drawable. Healthy driven frames never take it.
 */
#ifdef nextDrawable
#undef nextDrawable
#endif

@interface NSObject (MonadoCAMetalDisplayLinkDriveDrawableFallback)
- (id<CAMetalDrawable>)monadoCAMetalDrivenNextDrawableWithFallback;
@end

@implementation NSObject (MonadoCAMetalDisplayLinkDriveDrawableFallback)
- (id<CAMetalDrawable>)monadoCAMetalDrivenNextDrawableWithFallback
{
	if (macos_cametal_drive_enabled() && self == g_macos_cametal_driver_layer) {
		id<CAMetalDrawable> supplied = (id<CAMetalDrawable>)comp_multi_macos_displaylink_current_drawable();
		if (supplied != nil) {
			return supplied;
		}
	}
	return [(CAMetalLayer *)self nextDrawable];
}
@end

#define nextDrawable monadoCAMetalDrivenNextDrawableWithFallback
