// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "util/u_macos_display_host.h"

#ifdef __cplusplus
extern "C" {
#endif

struct u_macos_hosted_client;
struct u_macos_hosted_client_ops
{
	xrt_result_t (*attach)(void *ctx, uint32_t context_id);
	xrt_result_t (*set_visibility)(void *ctx, enum u_macos_display_host_visibility visibility);
	xrt_result_t (*set_active)(void *ctx, bool active);
	void (*detach)(void *ctx);
};

//! An owned connection binding, independent of other instances in this process.
struct u_macos_hosted_client *
u_macos_hosted_client_create(const struct u_macos_hosted_client_ops *ops, void *ctx, bool follow_focus);

void
u_macos_hosted_client_reference(struct u_macos_hosted_client *client);
void
u_macos_hosted_client_release(struct u_macos_hosted_client *client);
//! Disarm callbacks and release the connection binding before destroying IPC.
void
u_macos_hosted_client_close(struct u_macos_hosted_client *client);

bool
u_macos_hosted_client_follows_service_focus(struct u_macos_hosted_client *client);
xrt_result_t
u_macos_hosted_client_attach(struct u_macos_hosted_client *client, uint32_t context_id);
xrt_result_t
u_macos_hosted_client_set_active(struct u_macos_hosted_client *client, bool active);
xrt_result_t
u_macos_hosted_client_set_visibility(struct u_macos_hosted_client *client,
                                     enum u_macos_display_host_visibility visibility);
void
u_macos_hosted_client_detach(struct u_macos_hosted_client *client);

//! Capture at submission. Zero means this drawable must not promote the layer.
uint64_t
u_macos_hosted_client_present_ticket(struct u_macos_hosted_client *client);
//! Run off the presentation thread. Stale tickets and closed bindings do nothing.
void
u_macos_hosted_client_note_presented(struct u_macos_hosted_client *client, uint64_t ticket);

#ifdef __cplusplus
}
#endif
