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
	uint64_t callback_monotonic_ns;
	uint64_t target_monotonic_ns;
	uint64_t presentation_monotonic_ns;
};

static struct macos_displaylink_bridge g_bridge = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static pthread_once_t g_enabled_once = PTHREAD_ONCE_INIT;
static bool g_enabled = false;
static bool g_active = false;

static void
macos_displaylink_init_enabled(void)
{
	/* Older macOS versions retain the legacy target instead of waiting for callbacks
	 * from an API they cannot create. */
	if (__builtin_available(macOS 14.0, *)) {
		const char *value = getenv("XRT_MACOS_CAMETALDISPLAYLINK_DRIVE");
		g_enabled = value == NULL || strcmp(value, "1") == 0;
	}
}

bool
comp_multi_macos_displaylink_enabled(void)
{
	pthread_once(&g_enabled_once, macos_displaylink_init_enabled);
	return g_enabled;
}

void
comp_multi_macos_displaylink_set_active(bool active)
{
	pthread_mutex_lock(&g_bridge.mutex);
	g_active = active;
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

/*
 * pthread_cond_timedwait() uses CLOCK_REALTIME for the statically initialized
 * condition variable. Convert CAMetalDisplayLink's monotonic target deadline to
 * a realtime absolute deadline without making realtime itself a cadence source.
 */
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
	if (!comp_multi_macos_displaylink_enabled() || drawable == NULL) {
		return false;
	}

	pthread_mutex_lock(&g_bridge.mutex);
	uint64_t serial = ++g_bridge.next_serial;
	g_bridge.published_serial = serial;
	g_bridge.drawable = drawable;
	g_bridge.callback_monotonic_ns = callback_monotonic_ns;
	g_bridge.target_monotonic_ns = target_monotonic_ns;
	g_bridge.presentation_monotonic_ns = presentation_monotonic_ns;
	pthread_cond_broadcast(&g_bridge.cond);

	/*
	 * In steady state the Multi Client Module is already blocked in wait_tick().
	 * Do not use the old arbitrary 2 ms hand-off timeout: under UE load that could
	 * discard a perfectly usable callback merely because the compositor thread was
	 * briefly descheduled. Give it the whole CAMetalDisplayLink target window. If
	 * there is no active native session, the callback is drained at that deadline.
	 */
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

	/*
	 * Once consumed, keep the delegate callback and its retained update.drawable
	 * alive until the macOS target has actually scheduled that exact drawable for
	 * presentation. In particular, xrt_comp_layer_commit() is not sufficient here:
	 * the existing async-present worker may return from layer_commit before it has
	 * touched the drawable. The target calls complete_tick() from its plain present
	 * path once Metal owns the presentation request.
	 */
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
		/*
		 * Do not make service shutdown depend forever on another CA callback. This
		 * timeout is only a failure/teardown escape hatch; healthy driven cadence is
		 * released exclusively by CAMetalDisplayLink callbacks.
		 */
		while (g_bridge.published_serial <= g_bridge.consumed_serial) {
			struct timespec deadline = realtime_deadline_ms(100);
			int ret = pthread_cond_timedwait(&g_bridge.cond, &g_bridge.mutex, &deadline);
			if (ret == ETIMEDOUT && g_bridge.published_serial <= g_bridge.consumed_serial) {
				pthread_mutex_unlock(&g_bridge.mutex);
				return false;
			}
		}

		uint64_t serial = g_bridge.published_serial;
		if (g_bridge.cancelled_serial >= serial) {
			g_bridge.consumed_serial = serial;
			continue;
		}

		g_bridge.consumed_serial = serial;
		if (out_callback_monotonic_ns != NULL) {
			*out_callback_monotonic_ns = g_bridge.callback_monotonic_ns;
		}
		if (out_target_monotonic_ns != NULL) {
			*out_target_monotonic_ns = g_bridge.target_monotonic_ns;
		}
		if (out_presentation_monotonic_ns != NULL) {
			*out_presentation_monotonic_ns = g_bridge.presentation_monotonic_ns;
		}
		pthread_cond_broadcast(&g_bridge.cond);
		pthread_mutex_unlock(&g_bridge.mutex);
		return true;
	}
}

bool
comp_multi_macos_displaylink_current_timing(struct comp_multi_macos_displaylink_timing *out_timing)
{
	pthread_mutex_lock(&g_bridge.mutex);
	bool valid = g_active && g_bridge.drawable != NULL &&
	             g_bridge.published_serial == g_bridge.consumed_serial &&
	             g_bridge.consumed_serial > g_bridge.completed_serial &&
	             g_bridge.consumed_serial > g_bridge.cancelled_serial &&
	             g_bridge.callback_monotonic_ns > 0 &&
	             g_bridge.target_monotonic_ns > g_bridge.callback_monotonic_ns &&
	             g_bridge.presentation_monotonic_ns >= g_bridge.target_monotonic_ns &&
	             g_bridge.presentation_monotonic_ns <= INT64_MAX;
	if (valid) {
		*out_timing = (struct comp_multi_macos_displaylink_timing){
		    .callback_ns = (int64_t)g_bridge.callback_monotonic_ns,
		    .deadline_ns = (int64_t)g_bridge.target_monotonic_ns,
		    .presentation_ns = (int64_t)g_bridge.presentation_monotonic_ns,
		};
	}
	pthread_mutex_unlock(&g_bridge.mutex);
	return valid;
}

void
comp_multi_macos_displaylink_complete_tick(void)
{
	if (!comp_multi_macos_displaylink_enabled()) {
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
	if (!comp_multi_macos_displaylink_enabled()) {
		return;
	}
	pthread_mutex_lock(&g_bridge.mutex);
	/*
	 * Only teardown calls this. If a callback is blocked after its tick was
	 * consumed but before Metal could schedule presentation, release that callback
	 * so the display-link NSThread can invalidate and the fully buffered trace can
	 * be closed. The callback itself still owns an explicit retain on the drawable.
	 */
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
	if (!comp_multi_macos_displaylink_enabled()) {
		return NULL;
	}
	pthread_mutex_lock(&g_bridge.mutex);
	void *drawable = g_bridge.drawable;
	pthread_mutex_unlock(&g_bridge.mutex);
	return drawable;
}

#else

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
comp_multi_macos_displaylink_enabled(void)
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
