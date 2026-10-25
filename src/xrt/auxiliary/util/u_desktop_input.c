// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Keyboard and pointer input from a desktop window, for emulated devices.
 * @ingroup aux_util
 */

#include "util/u_desktop_input.h"

#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static u_desktop_input_handler_t g_handler = NULL;
static void *g_ctx = NULL;

void
u_desktop_input_register(u_desktop_input_handler_t handler, void *ctx)
{
	pthread_mutex_lock(&g_mutex);
	g_handler = handler;
	g_ctx = ctx;
	pthread_mutex_unlock(&g_mutex);
}

void
u_desktop_input_unregister(void *ctx)
{
	pthread_mutex_lock(&g_mutex);
	if (g_ctx == ctx) {
		g_handler = NULL;
		g_ctx = NULL;
	}
	pthread_mutex_unlock(&g_mutex);
}

bool
u_desktop_input_active(void)
{
	pthread_mutex_lock(&g_mutex);
	bool active = g_handler != NULL;
	pthread_mutex_unlock(&g_mutex);
	return active;
}

bool
u_desktop_input_push(const struct u_desktop_input_event *event)
{
	// The handler runs with the lock held, so it cannot be unregistered mid-call.
	pthread_mutex_lock(&g_mutex);
	bool handled = g_handler != NULL;
	if (handled) {
		g_handler(g_ctx, event);
	}
	pthread_mutex_unlock(&g_mutex);
	return handled;
}
