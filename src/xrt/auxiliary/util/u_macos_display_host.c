// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Registry for the macOS headset display host.
 * @ingroup aux_util
 */

#include "util/u_macos_display_host.h"

#include <pthread.h>
#include <stddef.h>

static pthread_mutex_t g_host_mutex = PTHREAD_MUTEX_INITIALIZER;
static const struct u_macos_display_host_ops *g_host_ops = NULL;
static void *g_host_ctx = NULL;

void
u_macos_display_host_register(const struct u_macos_display_host_ops *ops, void *ctx)
{
	pthread_mutex_lock(&g_host_mutex);
	g_host_ops = ops;
	g_host_ctx = ctx;
	pthread_mutex_unlock(&g_host_mutex);
}

void
u_macos_display_host_unregister(void *ctx)
{
	pthread_mutex_lock(&g_host_mutex);
	if (g_host_ctx == ctx) {
		g_host_ops = NULL;
		g_host_ctx = NULL;
	}
	pthread_mutex_unlock(&g_host_mutex);
}

/*
 * The host functions run with the registry lock held, so the host cannot be
 * unregistered and destroyed while a call is in progress.
 */

xrt_result_t
u_macos_display_host_attach(uint32_t client_id, uint32_t context_id)
{
	xrt_result_t xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	pthread_mutex_lock(&g_host_mutex);
	if (g_host_ops != NULL) {
		xret = g_host_ops->attach(g_host_ctx, client_id, context_id);
	}
	pthread_mutex_unlock(&g_host_mutex);
	return xret;
}

xrt_result_t
u_macos_display_host_set_visibility(uint32_t client_id, enum u_macos_display_host_visibility visibility)
{
	xrt_result_t xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	pthread_mutex_lock(&g_host_mutex);
	if (g_host_ops != NULL) {
		xret = g_host_ops->set_visibility(g_host_ctx, client_id, visibility);
	}
	pthread_mutex_unlock(&g_host_mutex);
	return xret;
}

void
u_macos_display_host_detach(uint32_t client_id)
{
	pthread_mutex_lock(&g_host_mutex);
	if (g_host_ops != NULL) {
		g_host_ops->detach(g_host_ctx, client_id);
	}
	pthread_mutex_unlock(&g_host_mutex);
}
