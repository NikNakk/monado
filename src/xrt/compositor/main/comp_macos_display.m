// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "main/comp_macos_display.h"
#import <CoreVideo/CoreVideo.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdlib.h>
#include "os/os_time.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
struct comp_macos_display_link
{
	CVDisplayLinkRef cv;
	mach_timebase_info_data_t timebase;
	comp_macos_display_tick tick;
	void *ctx;
	int64_t period_ns;
};

static uint64_t
host_ns(struct comp_macos_display_link *link, uint64_t host)
{
	return (uint64_t)(((__uint128_t)host * link->timebase.numer) / link->timebase.denom);
}

static CVReturn
on_tick(CVDisplayLinkRef cv,
        const CVTimeStamp *now,
        const CVTimeStamp *output,
        CVOptionFlags flags_in,
        CVOptionFlags *flags_out,
        void *ctx)
{
	(void)cv;
	(void)flags_in;
	(void)flags_out;
	struct comp_macos_display_link *link = ctx;
	link->tick(link->ctx, (now->flags & kCVTimeStampHostTimeValid) ? host_ns(link, now->hostTime) : 0,
	           (output->flags & kCVTimeStampHostTimeValid) ? host_ns(link, output->hostTime) : 0);
	return kCVReturnSuccess;
}

struct comp_macos_display_link *
comp_macos_display_link_create(CGDirectDisplayID display, comp_macos_display_tick tick, void *ctx)
{
	if (display == kCGNullDirectDisplay || tick == NULL) {
		return NULL;
	}
	struct comp_macos_display_link *link = calloc(1, sizeof(*link));
	if (link == NULL) {
		return NULL;
	}
	mach_timebase_info(&link->timebase);
	link->tick = tick;
	link->ctx = ctx;
	if (CVDisplayLinkCreateWithCGDisplay(display, &link->cv) != kCVReturnSuccess ||
	    CVDisplayLinkSetOutputCallback(link->cv, on_tick, link) != kCVReturnSuccess) {
		comp_macos_display_link_destroy(&link);
		return NULL;
	}
	CVTime period = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link->cv);
	if (!(period.flags & kCVTimeIsIndefinite) && period.timeValue > 0 && period.timeScale > 0) {
		link->period_ns = (int64_t)(((__int128)period.timeValue * 1000000000) / period.timeScale);
	}
	return link;
}

void
comp_macos_display_link_stop(struct comp_macos_display_link *link)
{
	if (link != NULL && link->cv != NULL) {
		CVDisplayLinkStop(link->cv);
	}
}

void
comp_macos_display_link_destroy(struct comp_macos_display_link **ptr)
{
	struct comp_macos_display_link *link = *ptr;
	if (link == NULL) {
		return;
	}
	comp_macos_display_link_stop(link);
	if (link->cv != NULL) {
		CVDisplayLinkRelease(link->cv);
	}
	free(link);
	*ptr = NULL;
}

bool
comp_macos_display_link_start(struct comp_macos_display_link *link)
{
	return link != NULL && CVDisplayLinkStart(link->cv) == kCVReturnSuccess;
}

bool
comp_macos_display_link_running(struct comp_macos_display_link *link)
{
	return link != NULL && CVDisplayLinkIsRunning(link->cv);
}

int64_t
comp_macos_display_link_period(struct comp_macos_display_link *link)
{
	return link != NULL ? link->period_ns : 0;
}
#pragma clang diagnostic pop

uint32_t
comp_macos_display_rates(CGDirectDisplayID display_id, uint32_t capacity, float *out_rates)
{
	if (display_id == kCGNullDirectDisplay) {
		return 0;
	}
	CGDisplayModeRef current_mode = CGDisplayCopyDisplayMode(display_id);
	if (current_mode == NULL) {
		return 0;
	}
	size_t width = CGDisplayModeGetWidth(current_mode);
	size_t height = CGDisplayModeGetHeight(current_mode);
	size_t pixel_width = CGDisplayModeGetPixelWidth(current_mode);
	size_t pixel_height = CGDisplayModeGetPixelHeight(current_mode);
	CGDisplayModeRelease(current_mode);

	CFArrayRef modes = CGDisplayCopyAllDisplayModes(display_id, NULL);
	if (modes == NULL) {
		return 0;
	}
	uint32_t count = 0;
	for (CFIndex i = 0; i < CFArrayGetCount(modes); i++) {
		CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
		if (mode == NULL || CGDisplayModeGetWidth(mode) != width || CGDisplayModeGetHeight(mode) != height ||
		    CGDisplayModeGetPixelWidth(mode) != pixel_width ||
		    CGDisplayModeGetPixelHeight(mode) != pixel_height) {
			continue;
		}
		double refresh_hz = CGDisplayModeGetRefreshRate(mode);
		if (!(refresh_hz > 1.0)) {
			continue;
		}
		bool duplicate = false;
		for (uint32_t j = 0; j < count; j++) {
			if (fabs((double)out_rates[j] - refresh_hz) < 0.05) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate && count < capacity) {
			out_rates[count++] = (float)refresh_hz;
		}
	}
	CFRelease(modes);
	for (uint32_t i = 1; i < count; i++) {
		float value = out_rates[i];
		uint32_t j = i;
		while (j > 0 && out_rates[j - 1] > value) {
			out_rates[j] = out_rates[j - 1];
			j--;
		}
		out_rates[j] = value;
	}
	return count;
}

CGDisplayModeRef
comp_macos_display_copy_mode(CGDirectDisplayID display_id, float requested_hz, float *out_selected_hz)
{
	if (!isfinite(requested_hz) || requested_hz < 0.0f) {
		return NULL;
	}
	CGDisplayModeRef current_mode =
	    display_id != kCGNullDirectDisplay ? CGDisplayCopyDisplayMode(display_id) : NULL;
	if (current_mode == NULL) {
		return NULL;
	}
	size_t width = CGDisplayModeGetWidth(current_mode);
	size_t height = CGDisplayModeGetHeight(current_mode);
	size_t pixel_width = CGDisplayModeGetPixelWidth(current_mode);
	size_t pixel_height = CGDisplayModeGetPixelHeight(current_mode);
	CGDisplayModeRelease(current_mode);

	CFArrayRef modes = CGDisplayCopyAllDisplayModes(display_id, NULL);
	if (modes == NULL) {
		return NULL;
	}
	CGDisplayModeRef selected = NULL;
	double selected_hz = 0.0;
	double best_error = HUGE_VAL;
	for (CFIndex i = 0; i < CFArrayGetCount(modes); i++) {
		CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
		if (mode == NULL || CGDisplayModeGetWidth(mode) != width || CGDisplayModeGetHeight(mode) != height ||
		    CGDisplayModeGetPixelWidth(mode) != pixel_width ||
		    CGDisplayModeGetPixelHeight(mode) != pixel_height) {
			continue;
		}
		double refresh_hz = CGDisplayModeGetRefreshRate(mode);
		if (!(refresh_hz > 1.0)) {
			continue;
		}
		if (requested_hz <= 0.0f) {
			if (refresh_hz > selected_hz) {
				selected = mode;
				selected_hz = refresh_hz;
			}
		} else {
			double error = fabs(refresh_hz - (double)requested_hz);
			if (error < best_error) {
				best_error = error;
				selected = mode;
				selected_hz = refresh_hz;
			}
		}
	}
	if (selected != NULL && (requested_hz <= 0.0f || best_error < 0.5)) {
		CGDisplayModeRetain(selected);
	} else {
		selected = NULL;
	}
	CFRelease(modes);
	if (out_selected_hz != NULL) {
		*out_selected_hz = (float)selected_hz;
	}
	return selected;
}

bool
comp_macos_display_set_mode(CGDirectDisplayID display, CGDisplayModeRef mode)
{
	if (CGDisplaySetDisplayMode(display, mode, NULL) != kCGErrorSuccess) {
		return false;
	}
	int64_t deadline = os_monotonic_get_ns() + 2000000000LL;
	uint32_t stable = 0;
	do {
		CGDisplayModeRef current = CGDisplayIsActive(display) ? CGDisplayCopyDisplayMode(display) : NULL;
		bool matches = current != NULL &&
		               CGDisplayModeGetPixelWidth(current) == CGDisplayModeGetPixelWidth(mode) &&
		               CGDisplayModeGetPixelHeight(current) == CGDisplayModeGetPixelHeight(mode) &&
		               fabs(CGDisplayModeGetRefreshRate(current) - CGDisplayModeGetRefreshRate(mode)) < 0.05;
		if (current != NULL) {
			CGDisplayModeRelease(current);
		}
		stable = matches ? stable + 1 : 0;
		if (stable >= 3) {
			return true;
		}
		os_nanosleep(10000000);
	} while (os_monotonic_get_ns() < deadline);
	return false;
}
