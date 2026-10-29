// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief One-shot frame capture requests for session marks.
 * @ingroup aux_util
 */

#include "util/u_session_mark.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_path[1024];
static bool g_pending = false;

void
u_session_mark_request_capture(const char *path)
{
	pthread_mutex_lock(&g_lock);
	snprintf(g_path, sizeof(g_path), "%s", path);
	g_pending = true;
	pthread_mutex_unlock(&g_lock);
}

bool
u_session_mark_capture_pending(void)
{
	pthread_mutex_lock(&g_lock);
	bool pending = g_pending;
	pthread_mutex_unlock(&g_lock);
	return pending;
}

bool
u_session_mark_take_capture(char *out_path, size_t out_size)
{
	pthread_mutex_lock(&g_lock);
	bool pending = g_pending;
	if (pending) {
		snprintf(out_path, out_size, "%s", g_path);
		g_pending = false;
	}
	pthread_mutex_unlock(&g_lock);
	return pending;
}

static pthread_mutex_t g_tick_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tick_cond = PTHREAD_COND_INITIALIZER;
static uint64_t g_tick = 0;

void
u_session_mark_frame_tick(void)
{
	pthread_mutex_lock(&g_tick_lock);
	g_tick++;
	pthread_cond_broadcast(&g_tick_cond);
	pthread_mutex_unlock(&g_tick_lock);
}

bool
u_session_mark_wait_frame(int64_t timeout_ns)
{
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	int64_t ns = deadline.tv_nsec + timeout_ns;
	deadline.tv_sec += ns / 1000000000;
	deadline.tv_nsec = ns % 1000000000;

	pthread_mutex_lock(&g_tick_lock);
	uint64_t seen = g_tick;
	int ret = 0;
	while (g_tick == seen && ret == 0) {
		ret = pthread_cond_timedwait(&g_tick_cond, &g_tick_lock, &deadline);
	}
	bool ticked = g_tick != seen;
	pthread_mutex_unlock(&g_tick_lock);
	return ticked;
}
