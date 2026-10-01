// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "util/u_macos_hosted_client.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

struct u_macos_hosted_client
{
	pthread_mutex_t mutex;
	atomic_uint references;
	const struct u_macos_hosted_client_ops *ops;
	void *ctx;
	bool follow_focus;
	bool attached;
	bool active;
	bool reported_active;
	enum u_macos_display_host_visibility visibility;
	uint64_t generation;
};

struct u_macos_hosted_client *
u_macos_hosted_client_create(const struct u_macos_hosted_client_ops *ops, void *ctx, bool follow_focus)
{
	struct u_macos_hosted_client *client = calloc(1, sizeof(*client));
	if (client == NULL) {
		return NULL;
	}
	if (pthread_mutex_init(&client->mutex, NULL) != 0) {
		free(client);
		return NULL;
	}
	atomic_init(&client->references, 1);
	client->ops = ops;
	client->ctx = ctx;
	client->follow_focus = follow_focus;
	client->generation = 1;
	return client;
}

void
u_macos_hosted_client_reference(struct u_macos_hosted_client *client)
{
	if (client != NULL) {
		atomic_fetch_add(&client->references, 1);
	}
}

void
u_macos_hosted_client_release(struct u_macos_hosted_client *client)
{
	if (client != NULL && atomic_fetch_sub(&client->references, 1) == 1) {
		pthread_mutex_destroy(&client->mutex);
		free(client);
	}
}

// All connection callbacks are serialized with close, which disarms the binding.
static xrt_result_t
set_visibility_locked(struct u_macos_hosted_client *client, enum u_macos_display_host_visibility visibility)
{
	if (visibility == U_MACOS_DISPLAY_HOST_HIDDEN) {
		++client->generation;
		client->visibility = visibility;
	} else if (!client->active) {
		return XRT_ERROR_INVALID_ARGUMENT;
	}
	if (client->ops == NULL || !client->attached) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	// Repeated focus events must not re-arm an already exclusive layer.
	if (visibility == U_MACOS_DISPLAY_HOST_SHOWN && client->visibility != U_MACOS_DISPLAY_HOST_HIDDEN) {
		return XRT_SUCCESS;
	}
	xrt_result_t xret = client->ops->set_visibility(client->ctx, visibility);
	if (xret == XRT_SUCCESS) {
		client->visibility = visibility;
	}
	return xret;
}

static void
detach_locked(struct u_macos_hosted_client *client)
{
	++client->generation;
	client->visibility = U_MACOS_DISPLAY_HOST_HIDDEN;
	if (client->attached && client->ops != NULL) {
		client->ops->detach(client->ctx);
	}
	client->attached = false;
}

void
u_macos_hosted_client_close(struct u_macos_hosted_client *client)
{
	if (client == NULL) {
		return;
	}
	pthread_mutex_lock(&client->mutex);
	detach_locked(client);
	if (client->reported_active && client->ops != NULL) {
		client->ops->set_active(client->ctx, false);
	}
	client->active = false;
	client->ops = NULL;
	client->ctx = NULL;
	pthread_mutex_unlock(&client->mutex);
}

bool
u_macos_hosted_client_follows_service_focus(struct u_macos_hosted_client *client)
{
	return client->follow_focus;
}

xrt_result_t
u_macos_hosted_client_attach(struct u_macos_hosted_client *client, uint32_t context_id)
{
	pthread_mutex_lock(&client->mutex);
	detach_locked(client);
	xrt_result_t xret =
	    client->ops == NULL ? XRT_ERROR_FEATURE_NOT_SUPPORTED : client->ops->attach(client->ctx, context_id);
	client->attached = xret == XRT_SUCCESS;
	pthread_mutex_unlock(&client->mutex);
	return xret;
}

xrt_result_t
u_macos_hosted_client_set_active(struct u_macos_hosted_client *client, bool active)
{
	pthread_mutex_lock(&client->mutex);
	if (!active) {
		client->active = false;
		set_visibility_locked(client, U_MACOS_DISPLAY_HOST_HIDDEN);
	}
	xrt_result_t xret = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	if (client->ops != NULL) {
		xret = client->reported_active == active ? XRT_SUCCESS : client->ops->set_active(client->ctx, active);
		if (xret == XRT_SUCCESS) {
			client->active = active;
			client->reported_active = active;
			if (active && !client->follow_focus) {
				xret = set_visibility_locked(client, U_MACOS_DISPLAY_HOST_SHOWN);
			}
		}
	}
	pthread_mutex_unlock(&client->mutex);
	return xret;
}

xrt_result_t
u_macos_hosted_client_set_visibility(struct u_macos_hosted_client *client,
                                     enum u_macos_display_host_visibility visibility)
{
	pthread_mutex_lock(&client->mutex);
	xrt_result_t xret = set_visibility_locked(client, visibility);
	pthread_mutex_unlock(&client->mutex);
	return xret;
}

void
u_macos_hosted_client_detach(struct u_macos_hosted_client *client)
{
	pthread_mutex_lock(&client->mutex);
	detach_locked(client);
	pthread_mutex_unlock(&client->mutex);
}

uint64_t
u_macos_hosted_client_present_ticket(struct u_macos_hosted_client *client)
{
	if (client == NULL) {
		return 0;
	}
	pthread_mutex_lock(&client->mutex);
	uint64_t ticket = client->attached && client->active && client->visibility == U_MACOS_DISPLAY_HOST_SHOWN
	                      ? client->generation
	                      : 0;
	pthread_mutex_unlock(&client->mutex);
	return ticket;
}

void
u_macos_hosted_client_note_presented(struct u_macos_hosted_client *client, uint64_t ticket)
{
	pthread_mutex_lock(&client->mutex);
	if (ticket != 0 && ticket == client->generation && client->visibility == U_MACOS_DISPLAY_HOST_SHOWN) {
		set_visibility_locked(client, U_MACOS_DISPLAY_HOST_EXCLUSIVE);
	}
	pthread_mutex_unlock(&client->mutex);
}
