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

/* Called by the CAMetalDisplayLink delegate. Returns true iff a compositor frame
 * consumed the tick and completed before the callback returns. */
bool
comp_multi_macos_displaylink_submit_tick(void *drawable,
                                         uint64_t callback_monotonic_ns,
                                         uint64_t target_monotonic_ns,
                                         uint64_t presentation_monotonic_ns);

/* Called from the Multi Client Module's predict-frame wrapper. Blocks until one
 * CAMetalDisplayLink tick is available. Returns false when the experiment is off. */
bool
comp_multi_macos_displaylink_wait_tick(uint64_t *out_callback_monotonic_ns,
                                       uint64_t *out_target_monotonic_ns,
                                       uint64_t *out_presentation_monotonic_ns);

/* Completes the currently consumed tick and releases the delegate callback. */
void
comp_multi_macos_displaylink_complete_tick(void);

/* Opaque CAMetalDrawable pointer valid from wait_tick() through complete_tick(). */
void *
comp_multi_macos_displaylink_current_drawable(void);

#ifdef __cplusplus
}
#endif
