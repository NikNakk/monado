// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "os/os_threading.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Experimental macOS-only cadence bridge.
 *
 * The CAMetalDisplayLink delegate publishes one drawable/timing opportunity and
 * waits synchronously while the existing Multi Client Module thread performs one
 * compositor iteration. This keeps Vulkan/Monado rendering on its established
 * thread while making CAMetalDisplayLink the sole frame trigger.
 */
bool
comp_multi_macos_displaylink_enabled(void);

/* Only the local Metal target activates the bridge. Null and other compositor
 * targets keep normal pacing even when CAMetalDisplayLink is the macOS default. */
void
comp_multi_macos_displaylink_set_active(bool active);

bool
comp_multi_macos_displaylink_active(void);

/* Called by the CAMetalDisplayLink delegate. Returns true iff a compositor frame
 * consumed the tick and presentation was scheduled before the callback returns. */
bool
comp_multi_macos_displaylink_submit_tick(void *drawable,
                                         uint64_t callback_monotonic_ns,
                                         uint64_t target_monotonic_ns,
                                         uint64_t presentation_monotonic_ns);

/* Called from the Multi Client Module's predict-frame wrapper. Blocks until one
 * CAMetalDisplayLink tick is available. Returns false on the bounded failure/
 * teardown escape path or when the experiment is off. */
bool
comp_multi_macos_displaylink_wait_tick(uint64_t *out_callback_monotonic_ns,
                                       uint64_t *out_target_monotonic_ns,
                                       uint64_t *out_presentation_monotonic_ns);

/* Snapshot the consumed, not-yet-completed callback in Monado's monotonic
 * clock domain. Never returns an unconsumed, cancelled or previous tick. */
struct comp_multi_macos_displaylink_timing
{
	int64_t callback_ns;
	int64_t deadline_ns;
	int64_t presentation_ns;
};

bool
comp_multi_macos_displaylink_current_timing(struct comp_multi_macos_displaylink_timing *out_timing);

/* Called once Metal has scheduled the supplied drawable for presentation. */
void
comp_multi_macos_displaylink_complete_tick(void);

/* Teardown escape: release any delegate callback currently waiting for a
 * presentation that can no longer complete. */
void
comp_multi_macos_displaylink_cancel_pending_tick(void);

/* Opaque CAMetalDrawable pointer valid while the current callback-owned tick is active. */
void *
comp_multi_macos_displaylink_current_drawable(void);

#ifdef __cplusplus
}
#endif
