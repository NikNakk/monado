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

bool
comp_multi_macos_displaylink_enabled(void)
{
	static int initialized = 0;
	static bool enabled = false;
	if (!initialized) {
		const char *value = getenv("XRT_MACOS_CAMETALDISPLAYLINK_DRIVE");
		enabled = value != NULL && strcmp(value, "1") == 0;
		if (enabled) {
			fprintf(stderr,
			        "INFO: macOS CAMetalDisplayLink-driven compositor experiment enabled; "
			        "Multi Client Module will consume one synchronous tick per callback\n");
		}
		initialized = 1;
	}
	return enabled;
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
	 * If there is no active native session, do not hold the CA callback indefinitely:
	 * give the render thread 2 ms to consume this opportunity, then drain it.
	 */
	struct timespec deadline = realtime_deadline_ms(2);
	while (g_bridge.consumed_serial < serial) {
		int ret = pthread_cond_timedwait(&g_bridge.cond, &g_bridge.mutex, &deadline);
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
	 * Once consumed, the delegate deliberately waits without a timeout. The
	 * supplied drawable must remain valid until the corresponding compositor
	 * iteration has called comp_target_present().
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
		 * Do not make service shutdown depend on another CA callback arriving.
		 * Normal callbacks are ~8.34 ms apart; 100 ms is therefore only a teardown/
		 * failure escape hatch, not a pacing source during a healthy run.
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

void
comp_multi_macos_displaylink_complete_tick(void)
{}

void *
comp_multi_macos_displaylink_current_drawable(void)
{
	return NULL;
}

#endif
