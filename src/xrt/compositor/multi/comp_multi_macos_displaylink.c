// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0

#include "multi/comp_multi_macos_displaylink.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __APPLE__

struct macos_displaylink_bridge
{
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	uint64_t next_serial;
	uint64_t published_serial;
	uint64_t consumed_serial;
	uint64_t completed_serial;
	uint64_t cancelled_serial;
	void *drawable;
	uint64_t published_callback_monotonic_ns;
	uint64_t published_target_monotonic_ns;
	uint64_t published_presentation_monotonic_ns;
	uint64_t consumed_callback_monotonic_ns;
	uint64_t consumed_target_monotonic_ns;
	uint64_t consumed_presentation_monotonic_ns;
};

static struct macos_displaylink_bridge g_bridge = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static pthread_once_t g_mode_once = PTHREAD_ONCE_INIT;
static enum comp_multi_macos_displaylink_mode g_mode = COMP_MULTI_MACOS_DISPLAYLINK_LEGACY;
static bool g_active = false;

static void
macos_displaylink_init_mode(void)
{
	if (!__builtin_available(macOS 14.0, *)) {
		g_mode = COMP_MULTI_MACOS_DISPLAYLINK_LEGACY;
		return;
	}

	const char *mode = getenv("XRT_MACOS_CAMETALDISPLAYLINK_MODE");
	if (mode != NULL && mode[0] != '\0') {
		if (strcmp(mode, "legacy") == 0) {
			g_mode = COMP_MULTI_MACOS_DISPLAYLINK_LEGACY;
			return;
		}
		if (strcmp(mode, "driven") == 0) {
			g_mode = COMP_MULTI_MACOS_DISPLAYLINK_DRIVEN;
			return;
		}
		if (strcmp(mode, "hybrid") == 0) {
			g_mode = COMP_MULTI_MACOS_DISPLAYLINK_HYBRID;
			return;
		}
		fprintf(stderr,
		        "WARN: XRT_MACOS_CAMETALDISPLAYLINK_MODE='%s' is invalid; expected legacy, driven, or hybrid; using driven\n",
		        mode);
		g_mode = COMP_MULTI_MACOS_DISPLAYLINK_DRIVEN;
		return;
	}

	/* Backward-compatible escape hatch used by earlier revisions of this branch. */
	const char *drive = getenv("XRT_MACOS_CAMETALDISPLAYLINK_DRIVE");
	if (drive != NULL && strcmp(drive, "0") == 0) {
		g_mode = COMP_MULTI_MACOS_DISPLAYLINK_LEGACY;
	} else {
		g_mode = COMP_MULTI_MACOS_DISPLAYLINK_DRIVEN;
	}
}

enum comp_multi_macos_displaylink_mode
comp_multi_macos_displaylink_get_mode(void)
{
	pthread_once(&g_mode_once, macos_displaylink_init_mode);
	return g_mode;
}

bool
comp_multi_macos_displaylink_enabled(void)
{
	return comp_multi_macos_displaylink_get_mode() != COMP_MULTI_MACOS_DISPLAYLINK_LEGACY;
}

bool
comp_multi_macos_displaylink_driven_mode(void)
{
	return comp_multi_macos_displaylink_get_mode() == COMP_MULTI_MACOS_DISPLAYLINK_DRIVEN;
}

bool
comp_multi_macos_displaylink_hybrid_mode(void)
{
	return comp_multi_macos_displaylink_get_mode() == COMP_MULTI_MACOS_DISPLAYLINK_HYBRID;
}

void
comp_multi_macos_displaylink_set_active(bool active)
{
	pthread_mutex_lock(&g_bridge.mutex);
	g_active = active;
	pthread_cond_broadcast(&g_bridge.cond);
	pthread_mutex_unlock(&g_bridge.mutex);
}

bool
comp_multi_macos_displaylink_active(void)
{
	pthread_mutex_lock(&g_bridge.mutex);
	bool active = g_active;
	pthread_mutex_unlock(&g_bridge.mutex);
	return active;
}

static struct timespec
realtime_deadline_ms(long milliseconds)
{
	struct timespec ts = {0};
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += milliseconds / 1000;
	ts.tv_nsec += (milliseconds % 1000) * 1000 * 1000;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec += 1;
		ts.tv_nsec -= 1000000000L;
	}
	return ts;
}

/* pthread_cond_timedwait() uses CLOCK_REALTIME for this statically initialized
 * condition variable. Convert the CAMetalDisplayLink monotonic deadline without
 * making realtime itself a cadence source. */
static struct timespec
realtime_deadline_for_monotonic_ns(uint64_t target_monotonic_ns)
{
	struct timespec mono = {0};
	struct timespec real = {0};
	clock_gettime(CLOCK_MONOTONIC, &mono);
	clock_gettime(CLOCK_REALTIME, &real);

	uint64_t now_monotonic_ns = (uint64_t)mono.tv_sec * 1000000000ULL + (uint64_t)mono.tv_nsec;
	uint64_t remaining_ns = target_monotonic_ns > now_monotonic_ns ? target_monotonic_ns - now_monotonic_ns : 0;
	uint64_t real_ns = (uint64_t)real.tv_nsec + remaining_ns;
	real.tv_sec += (time_t)(real_ns / 1000000000ULL);
	real.tv_nsec = (long)(real_ns % 1000000000ULL);
	return real;
}

bool
comp_multi_macos_displaylink_submit_tick(void *drawable,
                                         uint64_t callback_monotonic_ns,
                                         uint64_t target_monotonic_ns,
                                         uint64_t presentation_monotonic_ns)
{
	if (!comp_multi_macos_displaylink_enabled()) {
		return false;
	}
	if (callback_monotonic_ns == 0 || target_monotonic_ns <= callback_monotonic_ns ||
	    presentation_monotonic_ns < target_monotonic_ns) {
		return false;
	}
	bool hybrid = comp_multi_macos_displaylink_hybrid_mode();
	if (!hybrid && drawable == NULL) {
		return false;
	}

	pthread_mutex_lock(&g_bridge.mutex);
	if (!g_active) {
		pthread_mutex_unlock(&g_bridge.mutex);
		return false;
	}

	uint64_t serial = ++g_bridge.next_serial;
	g_bridge.published_serial = serial;
	g_bridge.drawable = hybrid ? NULL : drawable;
	g_bridge.published_callback_monotonic_ns = callback_monotonic_ns;
	g_bridge.published_target_monotonic_ns = target_monotonic_ns;
	g_bridge.published_presentation_monotonic_ns = presentation_monotonic_ns;
	pthread_cond_broadcast(&g_bridge.cond);

	if (hybrid) {
		/* Cadence-only mode: never hold the CA callback open waiting for Monado. If
		 * the compositor is behind, wait_tick() will consume the newest published
		 * serial and intentionally skip older callbacks. */
		pthread_mutex_unlock(&g_bridge.mutex);
		return true;
	}

	/* Driven mode owns update.drawable, so retain the original synchronous handoff
	 * semantics until the compositor consumes this tick. */
	struct timespec consume_deadline = realtime_deadline_for_monotonic_ns(target_monotonic_ns);
	while (g_bridge.consumed_serial < serial) {
		int ret = pthread_cond_timedwait(&g_bridge.cond, &g_bridge.mutex, &consume_deadline);
		if (ret == ETIMEDOUT && g_bridge.consumed_serial < serial) {
			g_bridge.cancelled_serial = serial;
			if (g_bridge.published_serial == serial) {
				g_bridge.drawable = NULL;
			}
			pthread_cond_broadcast(&g_bridge.cond);
			pthread_mutex_unlock(&g_bridge.mutex);
			return false;
		}
	}

	/* Keep the callback-owned drawable alive until Metal has scheduled it. */
	while (g_bridge.completed_serial < serial) {
		pthread_cond_wait(&g_bridge.cond, &g_bridge.mutex);
	}

	if (g_bridge.published_serial == serial) {
		g_bridge.drawable = NULL;
	}
	pthread_mutex_unlock(&g_bridge.mutex);
	return true;
}

bool
comp_multi_macos_displaylink_wait_tick(uint64_t *out_callback_monotonic_ns,
                                       uint64_t *out_target_monotonic_ns,
                                       uint64_t *out_presentation_monotonic_ns)
{
	if (!comp_multi_macos_displaylink_enabled()) {
		return false;
	}

	pthread_mutex_lock(&g_bridge.mutex);
	for (;;) {
		while (g_active && g_bridge.published_serial <= g_bridge.consumed_serial) {
			struct timespec deadline = realtime_deadline_ms(100);
			int ret = pthread_cond_timedwait(&g_bridge.cond, &g_bridge.mutex, &deadline);
			if (ret == ETIMEDOUT && g_bridge.published_serial <= g_bridge.consumed_serial) {
				pthread_mutex_unlock(&g_bridge.mutex);
				return false;
			}
		}
		if (!g_active) {
			pthread_mutex_unlock(&g_bridge.mutex);
			return false;
		}

		/* In hybrid mode published_serial may have advanced several times while the
		 * compositor was busy. Consume the newest callback, not a stale queue. */
		uint64_t serial = g_bridge.published_serial;
		if (comp_multi_macos_displaylink_driven_mode() && g_bridge.cancelled_serial >= serial) {
			g_bridge.consumed_serial = serial;
			continue;
		}

		g_bridge.consumed_serial = serial;
		g_bridge.consumed_callback_monotonic_ns = g_bridge.published_callback_monotonic_ns;
		g_bridge.consumed_target_monotonic_ns = g_bridge.published_target_monotonic_ns;
		g_bridge.consumed_presentation_monotonic_ns = g_bridge.published_presentation_monotonic_ns;
		if (out_callback_monotonic_ns != NULL) {
			*out_callback_monotonic_ns = g_bridge.consumed_callback_monotonic_ns;
		}
		if (out_target_monotonic_ns != NULL) {
			*out_target_monotonic_ns = g_bridge.consumed_target_monotonic_ns;
		}
		if (out_presentation_monotonic_ns != NULL) {
			*out_presentation_monotonic_ns = g_bridge.consumed_presentation_monotonic_ns;
		}
		pthread_cond_broadcast(&g_bridge.cond);
		pthread_mutex_unlock(&g_bridge.mutex);
		return true;
	}
}

bool
comp_multi_macos_displaylink_current_timing(struct comp_multi_macos_displaylink_timing *out_timing)
{
	if (!comp_multi_macos_displaylink_driven_mode()) {
		return false;
	}
	pthread_mutex_lock(&g_bridge.mutex);
	bool valid = g_active && g_bridge.drawable != NULL &&
	             g_bridge.published_serial == g_bridge.consumed_serial &&
	             g_bridge.consumed_serial > g_bridge.completed_serial &&
	             g_bridge.consumed_serial > g_bridge.cancelled_serial &&
	             g_bridge.consumed_callback_monotonic_ns > 0 &&
	             g_bridge.consumed_target_monotonic_ns > g_bridge.consumed_callback_monotonic_ns &&
	             g_bridge.consumed_presentation_monotonic_ns >= g_bridge.consumed_target_monotonic_ns &&
	             g_bridge.consumed_presentation_monotonic_ns <= INT64_MAX;
	if (valid) {
		*out_timing = (struct comp_multi_macos_displaylink_timing){
		    .callback_ns = (int64_t)g_bridge.consumed_callback_monotonic_ns,
		    .deadline_ns = (int64_t)g_bridge.consumed_target_monotonic_ns,
		    .presentation_ns = (int64_t)g_bridge.consumed_presentation_monotonic_ns,
		};
	}
	pthread_mutex_unlock(&g_bridge.mutex);
	return valid;
}

void
comp_multi_macos_displaylink_complete_tick(void)
{
	if (!comp_multi_macos_displaylink_driven_mode()) {
		return;
	}
	pthread_mutex_lock(&g_bridge.mutex);
	if (g_bridge.completed_serial < g_bridge.consumed_serial) {
		g_bridge.completed_serial = g_bridge.consumed_serial;
		pthread_cond_broadcast(&g_bridge.cond);
	}
	pthread_mutex_unlock(&g_bridge.mutex);
}

void
comp_multi_macos_displaylink_cancel_pending_tick(void)
{
	if (!comp_multi_macos_displaylink_driven_mode()) {
		return;
	}
	pthread_mutex_lock(&g_bridge.mutex);
	if (g_bridge.completed_serial < g_bridge.published_serial) {
		g_bridge.completed_serial = g_bridge.published_serial;
		g_bridge.drawable = NULL;
	}
	pthread_cond_broadcast(&g_bridge.cond);
	pthread_mutex_unlock(&g_bridge.mutex);
}

void *
comp_multi_macos_displaylink_current_drawable(void)
{
	if (!comp_multi_macos_displaylink_driven_mode()) {
		return NULL;
	}
	pthread_mutex_lock(&g_bridge.mutex);
	void *drawable = g_bridge.drawable;
	pthread_mutex_unlock(&g_bridge.mutex);
	return drawable;
}

#else

enum comp_multi_macos_displaylink_mode
comp_multi_macos_displaylink_get_mode(void)
{
	return COMP_MULTI_MACOS_DISPLAYLINK_LEGACY;
}

bool
comp_multi_macos_displaylink_enabled(void)
{
	return false;
}

bool
comp_multi_macos_displaylink_driven_mode(void)
{
	return false;
}

bool
comp_multi_macos_displaylink_hybrid_mode(void)
{
	return false;
}

void
comp_multi_macos_displaylink_set_active(bool active)
{
	(void)active;
}

bool
comp_multi_macos_displaylink_active(void)
{
	return false;
}

bool
comp_multi_macos_displaylink_submit_tick(void *drawable,
                                         uint64_t callback_monotonic_ns,
                                         uint64_t target_monotonic_ns,
                                         uint64_t presentation_monotonic_ns)
{
	(void)drawable;
	(void)callback_monotonic_ns;
	(void)target_monotonic_ns;
	(void)presentation_monotonic_ns;
	return false;
}

bool
comp_multi_macos_displaylink_wait_tick(uint64_t *out_callback_monotonic_ns,
                                       uint64_t *out_target_monotonic_ns,
                                       uint64_t *out_presentation_monotonic_ns)
{
	(void)out_callback_monotonic_ns;
	(void)out_target_monotonic_ns;
	(void)out_presentation_monotonic_ns;
	return false;
}

bool
comp_multi_macos_displaylink_current_timing(struct comp_multi_macos_displaylink_timing *out_timing)
{
	(void)out_timing;
	return false;
}

void
comp_multi_macos_displaylink_complete_tick(void)
{}

void
comp_multi_macos_displaylink_cancel_pending_tick(void)
{}

void *
comp_multi_macos_displaylink_current_drawable(void)
{
	return NULL;
}

#endif
