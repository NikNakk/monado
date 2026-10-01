// Copyright 2020-2023, Collabora, Ltd.
// Copyright 2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Client side wrapper of @ref xrt_system.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup ipc_client
 */

#include "xrt/xrt_defines.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_session.h"

#include "util/u_misc.h"
#include "b_session.h"

#include "ipc_client_generated.h"

#ifdef XRT_IPC_MACOS_HOSTED_COMPOSITOR
#include "client/ipc_client_macos_hosted.h"
#include "util/u_macos_hosted_client.h"
#include "multi/comp_multi_interface.h"
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

/*!
 * IPC client implementation of @ref xrt_system.
 *
 * @implements xrt_system
 * @ingroup ipc_client
 */
struct ipc_client_system
{
	struct xrt_system base;

	struct ipc_connection *ipc_c;

	struct xrt_system_compositor *xsysc;

	struct ipc_client_local_session *local_session;
	struct u_macos_hosted_client *hosted_client;
};


/*
 *
 * Helpers
 *
 */

static inline struct ipc_client_system *
ipc_system(struct xrt_system *xsys)
{
	return (struct ipc_client_system *)xsys;
}

static inline xrt_result_t
create_headless(struct ipc_client_system *icsys, const struct xrt_session_info *xsi, struct xrt_session **out_xs)
{
	xrt_result_t xret = XRT_SUCCESS;

	// We create the session ourselves.
	xret = ipc_call_session_create( //
	    icsys->ipc_c,               // ipc_c
	    xsi,                        // xsi
	    false);                     // create_native_compositor
	IPC_CHK_AND_RET(icsys->ipc_c, xret, "ipc_call_session_create");

	struct xrt_session *xs = ipc_client_session_create(icsys->ipc_c);
	assert(xs != NULL);

	*out_xs = xs;

	return XRT_SUCCESS;
}

static inline xrt_result_t
create_with_comp(struct ipc_client_system *icsys,
                 const struct xrt_session_info *xsi,
                 struct xrt_session **out_xs,
                 struct xrt_compositor_native **out_xcn)
{
	xrt_result_t xret = XRT_SUCCESS;

	assert(icsys->xsysc != NULL);

	// The native compositor creates the session.
	xret = ipc_client_create_native_compositor( //
	    icsys->xsysc,                           //
	    xsi,                                    //
	    out_xcn);                               //
	IPC_CHK_AND_RET(icsys->ipc_c, xret, "ipc_client_create_native_compositor");

	struct xrt_session *xs = ipc_client_session_create(icsys->ipc_c);
	assert(xs != NULL);

	*out_xs = xs;

	return XRT_SUCCESS;
}


#ifdef XRT_IPC_MACOS_HOSTED_COMPOSITOR
/*!
 * Session of a client that composites in-process: headless on the service
 * side, with the local native compositor's events queued here.
 */
struct ipc_client_local_session
{
	struct xrt_session base;

	struct ipc_connection *ipc_c;

	//! The service-side, headless, session.
	struct xrt_session *remote;

	//! Receives the local compositor's events.
	struct b_session *local;

	//! The local system compositor and this session's native compositor.
	struct xrt_system_compositor *xsysc;
	struct xrt_compositor_native *xcn;

	struct ipc_client_system *owner;
	struct u_macos_hosted_client *hosted_client;
};

static inline struct ipc_client_local_session *
ipc_local_session(struct xrt_session *xs)
{
	return (struct ipc_client_local_session *)xs;
}

static void
report_active(struct ipc_client_local_session *ils, bool active)
{
	ipc_client_macos_hosted_session_active(ils->hosted_client, active);
}

static void
local_comp_set_active(void *ctx, bool active)
{
	report_active(ctx, active);
}

static void
local_comp_destroyed(void *ctx)
{
	struct ipc_client_local_session *ils = ctx;
	report_active(ils, false);
	ils->xcn = NULL;
}

static const struct comp_multi_lifecycle_callbacks local_lifecycle_callbacks = {
    .set_active = local_comp_set_active,
    .destroyed = local_comp_destroyed,
};

//! Apply the service's decision on whether this application is visible and focused.
static void
apply_service_state(struct ipc_client_local_session *ils, const struct xrt_session_event_state_change *state)
{
	if (!ipc_client_macos_hosted_follows_service_focus(ils->hosted_client) || ils->xcn == NULL) {
		return;
	}
	// The local compositor then sends the application the matching event.
	xrt_syscomp_set_state(ils->xsysc, &ils->xcn->base, state->visible, state->focused, state->timestamp_ns);
	ipc_client_macos_hosted_set_visible(ils->hosted_client, state->visible);
}

static xrt_result_t
local_session_poll_events(struct xrt_session *xs, union xrt_session_event *out_xse)
{
	struct ipc_client_local_session *ils = ipc_local_session(xs);

	xrt_result_t xret = xrt_session_poll_events(&ils->local->base, out_xse);
	if (xret != XRT_SUCCESS || out_xse->type != XRT_SESSION_EVENT_NONE) {
		return xret;
	}

	/*
	 * The service creates a native compositor for every session, headless
	 * or not. Its compositor events describe that unused compositor, so the
	 * local compositor's events replace them. Its visible/focused state is
	 * the service's focus decision, which is applied to the local compositor.
	 */
	while (true) {
		xret = xrt_session_poll_events(ils->remote, out_xse);
		if (xret != XRT_SUCCESS) {
			return xret;
		}

		switch (out_xse->type) {
		case XRT_SESSION_EVENT_STATE_CHANGE: apply_service_state(ils, &out_xse->state); continue;
		case XRT_SESSION_EVENT_OVERLAY_CHANGE:
		case XRT_SESSION_EVENT_LOSS_PENDING:
		case XRT_SESSION_EVENT_LOST:
		case XRT_SESSION_EVENT_DISPLAY_REFRESH_RATE_CHANGE: continue;
		default: return XRT_SUCCESS;
		}
	}
}

static xrt_result_t
local_session_request_exit(struct xrt_session *xs)
{
	struct ipc_client_local_session *ils = ipc_local_session(xs);

	return xrt_session_request_exit(&ils->local->base);
}

static void
local_session_destroy(struct xrt_session *xs)
{
	struct ipc_client_local_session *ils = ipc_local_session(xs);

	// Disarm callbacks before freeing the observer context.
	if (ils->xcn != NULL) {
		comp_multi_compositor_set_lifecycle_callbacks(ils->xcn, NULL, NULL);
	}
	// An application may destroy the session without ending it.
	report_active(ils, false);

	struct xrt_session *local = &ils->local->base;
	xrt_session_destroy(&local);
	xrt_session_destroy(&ils->remote);

	if (ils->owner->local_session == ils) {
		ils->owner->local_session = NULL;
	}
	u_macos_hosted_client_release(ils->hosted_client);
	free(ils);
}

static inline xrt_result_t
create_with_local_comp(struct ipc_client_system *icsys,
                       const struct xrt_session_info *xsi,
                       struct xrt_session **out_xs,
                       struct xrt_compositor_native **out_xcn)
{
	if (icsys->local_session != NULL) {
		return XRT_ERROR_MULTI_SESSION_NOT_IMPLEMENTED;
	}
	struct xrt_session *remote = NULL;
	xrt_result_t xret = create_headless(icsys, xsi, &remote);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	struct ipc_client_local_session *ils = U_TYPED_CALLOC(struct ipc_client_local_session);
	if (ils == NULL) {
		xrt_session_destroy(&remote);
		return XRT_ERROR_ALLOCATION;
	}
	ils->base.poll_events = local_session_poll_events;
	ils->base.request_exit = local_session_request_exit;
	ils->base.destroy = local_session_destroy;
	ils->ipc_c = icsys->ipc_c;
	ils->owner = icsys;
	ils->hosted_client = icsys->hosted_client;
	u_macos_hosted_client_reference(ils->hosted_client);
	ils->remote = remote;
	ils->local = b_session_create(NULL);
	ils->xsysc = icsys->xsysc;

	xret = xrt_syscomp_create_native_compositor( //
	    icsys->xsysc,                            //
	    xsi,                                     //
	    &ils->local->sink,                       //
	    out_xcn);                                //
	if (xret != XRT_SUCCESS) {
		struct xrt_session *xs = &ils->base;
		xrt_session_destroy(&xs);
		return xret;
	}

	ils->xcn = *out_xcn;
	comp_multi_compositor_set_lifecycle_callbacks(ils->xcn, &local_lifecycle_callbacks, ils);
	icsys->local_session = ils;

	*out_xs = &ils->base;

	return XRT_SUCCESS;
}

#endif

/*
 *
 * Member functions.
 *
 */

static xrt_result_t
ipc_client_system_create_session(struct xrt_system *xsys,
                                 const struct xrt_session_info *xsi,
                                 struct xrt_session **out_xs,
                                 struct xrt_compositor_native **out_xcn)
{
	struct ipc_client_system *icsys = ipc_system(xsys);

	if (out_xcn != NULL && icsys->xsysc == NULL) {
		U_LOG_E("No system compositor in system, can't create native compositor.");
		return XRT_ERROR_COMPOSITOR_NOT_SUPPORTED;
	}

	// Skip making a native compositor if not asked for.
	if (out_xcn == NULL) {
		return create_headless(icsys, xsi, out_xs);
#ifdef XRT_IPC_MACOS_HOSTED_COMPOSITOR
	} else if (icsys->hosted_client != NULL) {
		return create_with_local_comp(icsys, xsi, out_xs, out_xcn);
#endif
	} else {
		return create_with_comp(icsys, xsi, out_xs, out_xcn);
	}
}

static void
ipc_client_system_destroy(struct xrt_system *xsys)
{
	struct ipc_client_system *icsys = ipc_system(xsys);

	free(icsys);
}


/*
 *
 * 'Exported' functions.
 *
 */

struct xrt_system *
ipc_client_system_create(struct ipc_connection *ipc_c, struct xrt_system_compositor *xsysc)
{
	struct ipc_client_system *icsys = U_TYPED_CALLOC(struct ipc_client_system);

	xrt_result_t xret = ipc_call_system_get_properties(ipc_c, &icsys->base.properties);
	IPC_CHK_ONLY_PRINT(ipc_c, xret, "ipc_call_system_get_properties");
	if (xret != XRT_SUCCESS) {
		free(icsys);
		return NULL;
	}

	icsys->base.create_session = ipc_client_system_create_session;
	icsys->base.destroy = ipc_client_system_destroy;
	icsys->ipc_c = ipc_c;
	icsys->xsysc = xsysc;

	return &icsys->base;
}

struct xrt_system *
ipc_client_system_create_with_local_compositor(struct ipc_connection *ipc_c,
                                               struct xrt_system_compositor *xsysc,
                                               struct u_macos_hosted_client *client)
{
	struct xrt_system *xsys = ipc_client_system_create(ipc_c, xsysc);
	if (xsys != NULL) {
		ipc_system(xsys)->hosted_client = client;
	}

	return xsys;
}
