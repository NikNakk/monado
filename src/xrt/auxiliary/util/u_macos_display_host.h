// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Registry for the macOS headset display host.
 *
 * The service's macOS presenter owns the headset window. Clients that composite
 * in their own process present into a CAContext, and the service shows it in
 * that window through a CALayerHost. The presenter registers its host
 * functions here, so the IPC server can reach them without linking against the
 * compositor. See doc/macos-client-compositor-design.md.
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_results.h"

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * How a hosted client's layer is shown, following the handoff order in the
 * design: arm (shown, service layer still underneath), then exclusive.
 */
enum u_macos_display_host_visibility
{
	//! The client's host layer is hidden.
	U_MACOS_DISPLAY_HOST_HIDDEN = 0,
	//! The client's host layer is shown above the service's own layer.
	U_MACOS_DISPLAY_HOST_SHOWN = 1,
	//! Shown, and the service's own layer is hidden: one layer on screen.
	U_MACOS_DISPLAY_HOST_EXCLUSIVE = 2,
};

struct u_macos_display_host_ops
{
	xrt_result_t (*attach)(void *ctx, uint32_t client_id, uint32_t context_id);
	xrt_result_t (*set_visibility)(void *ctx, uint32_t client_id, enum u_macos_display_host_visibility visibility);
	void (*detach)(void *ctx, uint32_t client_id);
};

//! Called by the presenter once its window exists. One host at a time.
void
u_macos_display_host_register(const struct u_macos_display_host_ops *ops, void *ctx);

//! Called by the presenter before it destroys the window.
void
u_macos_display_host_unregister(void *ctx);

/*!
 * Show the CAContext @p context_id of client @p client_id in the headset
 * window, hidden until u_macos_display_host_set_visibility. Replaces any
 * earlier context of the same client.
 *
 * @return XRT_ERROR_FEATURE_NOT_SUPPORTED if no host is registered or the
 *         remote layer API is unavailable.
 */
xrt_result_t
u_macos_display_host_attach(uint32_t client_id, uint32_t context_id);

xrt_result_t
u_macos_display_host_set_visibility(uint32_t client_id, enum u_macos_display_host_visibility visibility);

//! Remove the client's host layer, restoring the service's own layer if needed.
void
u_macos_display_host_detach(uint32_t client_id);


#ifdef __cplusplus
}
#endif
