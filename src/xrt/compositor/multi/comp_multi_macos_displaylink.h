// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "os/os_threading.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum comp_multi_macos_displaylink_mode
{
	COMP_MULTI_MACOS_DISPLAYLINK_LEGACY = 0,
	COMP_MULTI_MACOS_DISPLAYLINK_DRIVEN,
	COMP_MULTI_MACOS_DISPLAYLINK_HYBRID,
};

/*
 * Experimental macOS-only cadence bridge.
 *
 * XRT_MACOS_CAMETALDISPLAYLINK_MODE selects one of three architectures:
 *
 *   legacy  - no CAMetalDisplayLink cadence bridge; retain the existing Monado
 *             pacing and normal CAMetalLayer nextDrawable/timed presentation.
 *   driven  - CAMetalDisplayLink is attached to the real HMD layer. Its callback
 *             drawable is synchronously consumed and presented by Monado.
 *   hybrid  - CAMetalDisplayLink runs on an independent child layer and publishes
 *             timing only. The callback returns immediately; the real HMD layer
 *             retains normal nextDrawable/timed presentation semantics.
 *
 * The default remains driven for compatibility with the current branch. If MODE
 * is unset, the older XRT_MACOS_CAMETALDISPLAYLINK_DRIVE=0 escape hatch still
 * selects legacy mode.
 */
enum comp_multi_macos_displaylink_mode
comp_multi_macos_displaylink_get_mode(void);

bool
comp_multi_macos_displaylink_enabled(void);

bool
comp_multi_macos_displaylink_driven_mode(void);

bool
comp_multi_macos_displaylink_hybrid_mode(void);

/* Only the local Metal target activates the bridge. Null and other compositor
 * targets keep normal pacing even when CAMetalDisplayLink cadence is selected. */
void
comp_multi_macos_displaylink_set_active(bool active);

bool
comp_multi_macos_displaylink_active(void);

/*
 * Called by the CAMetalDisplayLink delegate.
 *
 * Driven mode requires a non-NULL callback-owned drawable and returns only after
 * the compositor has consumed the tick and Metal has scheduled that drawable.
 * Hybrid mode ignores drawable, publishes the newest timing opportunity, signals
 * the compositor and returns immediately. Therefore hybrid callback execution is
 * never coupled to compositor or presentation-worker scheduling latency.
 */
bool
comp_multi_macos_displaylink_submit_tick(void *drawable,
                                         uint64_t callback_monotonic_ns,
                                         uint64_t target_monotonic_ns,
                                         uint64_t presentation_monotonic_ns);

/* Called from the Multi Client Module's predict-frame wrapper. Blocks until one
 * CAMetalDisplayLink tick is available. In hybrid mode, if the compositor fell
 * behind, the newest published tick wins. Returns false on the bounded failure/
 * teardown escape path or when CAMetalDisplayLink cadence is disabled. */
bool
comp_multi_macos_displaylink_wait_tick(uint64_t *out_callback_monotonic_ns,
                                       uint64_t *out_target_monotonic_ns,
                                       uint64_t *out_presentation_monotonic_ns);

/* Snapshot the last consumed callback timing. This is stable until wait_tick()
 * consumes another callback, so native compositor prediction can use it in both
 * driven and hybrid modes without racing a newer published callback. The macOS
 * HMD target locally restricts use of this timing to driven mode; hybrid keeps
 * real-CVDisplayLink timed presentation semantics. */
struct comp_multi_macos_displaylink_timing
{
	int64_t callback_ns;
	int64_t deadline_ns;
	int64_t presentation_ns;
};

bool
comp_multi_macos_displaylink_current_timing(struct comp_multi_macos_displaylink_timing *out_timing);

/* Driven mode only: called once Metal has scheduled the callback-supplied drawable. */
void
comp_multi_macos_displaylink_complete_tick(void);

/* Driven-mode teardown escape for a delegate callback waiting on presentation. */
void
comp_multi_macos_displaylink_cancel_pending_tick(void);

/* Driven mode only: callback-owned CAMetalDrawable for the currently consumed tick. */
void *
comp_multi_macos_displaylink_current_drawable(void);

#ifdef __cplusplus
}
#endif
