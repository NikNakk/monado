// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>

struct comp_macos_display_link;
//! Callback timestamps are mach host time in nanoseconds; zero means invalid.
typedef void (*comp_macos_display_tick)(void *ctx, uint64_t now_host_ns, uint64_t output_host_ns);
struct comp_macos_display_link *
comp_macos_display_link_create(CGDirectDisplayID display, comp_macos_display_tick tick, void *ctx);
void
comp_macos_display_link_destroy(struct comp_macos_display_link **link);
void
comp_macos_display_link_stop(struct comp_macos_display_link *link);
bool
comp_macos_display_link_start(struct comp_macos_display_link *link);
bool
comp_macos_display_link_running(struct comp_macos_display_link *link);
int64_t
comp_macos_display_link_period(struct comp_macos_display_link *link);
uint32_t
comp_macos_display_rates(CGDirectDisplayID display, uint32_t capacity, float *rates);
//! Caller owns the returned same-geometry display mode. Rejects NaN/negative rates.
CGDisplayModeRef
comp_macos_display_copy_mode(CGDirectDisplayID display, float requested_hz, float *selected_hz);

//! Apply and wait for the requested physical mode (including transient display inactivity).
bool
comp_macos_display_set_mode(CGDirectDisplayID display, CGDisplayModeRef mode);
