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
 * Only real-layer driven mode suppresses its callback. Hybrid is wake-only and
 * therefore deliberately keeps every native HMD timing/presentation mechanism on
 * the same path as legacy mode.
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
