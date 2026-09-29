// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Registry for the macOS headset display host.
 * @ingroup aux_util
 */

#include "util/u_macos_display_host.h"

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
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


/*
 *
 * Client side.
 *
 */

static pthread_mutex_t g_client_mutex = PTHREAD_MUTEX_INITIALIZER;
static const struct u_macos_hosted_client_ops *g_client_ops = NULL;
static void *g_client_ctx = NULL;
//! Last visibility the service accepted; guarded by g_client_mutex.
static enum u_macos_display_host_visibility g_client_visibility = U_MACOS_DISPLAY_HOST_HIDDEN;
//! Set while shown, until the first presented frame asks for exclusive.
static atomic_bool g_client_awaiting_present = false;

void
u_macos_hosted_client_register(const struct u_macos_hosted_client_ops *ops, void *ctx)
{
	pthread_mutex_lock(&g_client_mutex);
	g_client_ops = ops;
	g_client_ctx = ctx;
	pthread_mutex_unlock(&g_client_mutex);
}

void
u_macos_hosted_client_unregister(void *ctx)
{
	pthread_mutex_lock(&g_client_mutex);
	if (g_client_ctx == ctx) {
		g_client_ops = NULL;
		g_client_ctx = NULL;
		g_client_visibility = U_MACOS_DISPLAY_HOST_HIDDEN;
		atomic_store(&g_client_awaiting_present, false);
	}
	pthread_mutex_unlock(&g_client_mutex);
}

bool
u_macos_hosted_client_available(void)
{
	pthread_mutex_lock(&g_client_mutex);
	bool available = g_client_ops != NULL;
	pthread_mutex_unlock(&g_client_mutex);
	return available;
}

xrt_result_t
u_macos_hosted_client_attach(uint32_t context_id)
{
	xrt_result_t xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	pthread_mutex_lock(&g_client_mutex);
	if (g_client_ops != NULL) {
		xret = g_client_ops->attach(g_client_ctx, context_id);
	}
	pthread_mutex_unlock(&g_client_mutex);
	return xret;
}

xrt_result_t
u_macos_hosted_client_set_visibility(enum u_macos_display_host_visibility visibility)
{
	xrt_result_t xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	pthread_mutex_lock(&g_client_mutex);
	if (g_client_ops != NULL) {
		xret = g_client_ops->set_visibility(g_client_ctx, visibility);
		if (xret == XRT_SUCCESS) {
			g_client_visibility = visibility;
		}
		atomic_store(&g_client_awaiting_present, xret == XRT_SUCCESS && visibility == U_MACOS_DISPLAY_HOST_SHOWN);
	}
	pthread_mutex_unlock(&g_client_mutex);
	return xret;
}

void
u_macos_hosted_client_detach(void)
{
	pthread_mutex_lock(&g_client_mutex);
	atomic_store(&g_client_awaiting_present, false);
	g_client_visibility = U_MACOS_DISPLAY_HOST_HIDDEN;
	if (g_client_ops != NULL) {
		g_client_ops->detach(g_client_ctx);
	}
	pthread_mutex_unlock(&g_client_mutex);
}

static void
make_exclusive(void *unused)
{
	(void)unused;
	// Only if the layer is still just shown: not hidden or detached meanwhile.
	pthread_mutex_lock(&g_client_mutex);
	if (g_client_ops != NULL && g_client_visibility == U_MACOS_DISPLAY_HOST_SHOWN &&
	    g_client_ops->set_visibility(g_client_ctx, U_MACOS_DISPLAY_HOST_EXCLUSIVE) == XRT_SUCCESS) {
		g_client_visibility = U_MACOS_DISPLAY_HOST_EXCLUSIVE;
	}
	pthread_mutex_unlock(&g_client_mutex);
}

void
u_macos_hosted_client_note_presented(void)
{
	bool expected = true;
	if (!atomic_compare_exchange_strong(&g_client_awaiting_present, &expected, false)) {
		return;
	}
	// Not on the presented-handler thread: this is an IPC round trip.
	dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), NULL, make_exclusive);
}
