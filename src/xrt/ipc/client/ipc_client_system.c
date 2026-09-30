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
#include "util/u_session.h"

#include "ipc_client_generated.h"

#ifdef XRT_IPC_MACOS_HOSTED_COMPOSITOR
#include "client/ipc_client_macos_hosted.h"
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

	//! The system compositor runs in this process, not in the service.
	bool local_compositor;
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
	struct u_session *local;

	//! The local system compositor and this session's native compositor.
	struct xrt_system_compositor *xsysc;
	struct xrt_compositor_native *xcn;

	//! The native compositor's own functions, wrapped below.
	xrt_result_t (*begin_session)(struct xrt_compositor *xc, const struct xrt_begin_session_info *info);
	xrt_result_t (*end_session)(struct xrt_compositor *xc);
	void (*destroy)(struct xrt_compositor *xc);

	//! The service has been told the session is running.
	bool reported_active;
};

/*!
 * The wrapped compositor functions only get the compositor, and OpenXR has
 * one session per instance, so the session is found here.
 */
static struct ipc_client_local_session *g_local_session;

static inline struct ipc_client_local_session *
ipc_local_session(struct xrt_session *xs)
{
	return (struct ipc_client_local_session *)xs;
}

static void
report_active(struct ipc_client_local_session *ils, bool active)
{
#ifdef XRT_IPC_MACOS_HOSTED_COMPOSITOR
	if (ils->reported_active != active) {
		ipc_client_macos_hosted_session_active(ils->ipc_c, active);
		ils->reported_active = active;
	}
#else
	(void)ils;
	(void)active;
#endif
}

static xrt_result_t
local_comp_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	struct ipc_client_local_session *ils = g_local_session;
	assert(ils != NULL && xc == &ils->xcn->base);

	xrt_result_t xret = ils->begin_session(xc, info);
	if (xret == XRT_SUCCESS) {
		// Lets the service's focus logic make this application the visible one.
		report_active(ils, true);
	}
	return xret;
}

static xrt_result_t
local_comp_end_session(struct xrt_compositor *xc)
{
	struct ipc_client_local_session *ils = g_local_session;
	assert(ils != NULL && xc == &ils->xcn->base);

	report_active(ils, false);
	return ils->end_session(xc);
}

static void
local_comp_destroy(struct xrt_compositor *xc)
{
	struct ipc_client_local_session *ils = g_local_session;
	assert(ils != NULL && xc == &ils->xcn->base);

	ils->xcn = NULL;
	ils->destroy(xc);
}

//! Apply the service's decision on whether this application is visible and focused.
static void
apply_service_state(struct ipc_client_local_session *ils, const struct xrt_session_event_state_change *state)
{
#ifdef XRT_IPC_MACOS_HOSTED_COMPOSITOR
	if (!ipc_client_macos_hosted_follows_service_focus() || ils->xcn == NULL) {
		return;
	}
	// The local compositor then sends the application the matching event.
	xrt_syscomp_set_state(ils->xsysc, &ils->xcn->base, state->visible, state->focused, state->timestamp_ns);
	ipc_client_macos_hosted_set_visible(state->visible);
#else
	(void)ils;
	(void)state;
#endif
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

	// An application may destroy the session without ending it.
	report_active(ils, false);

	struct xrt_session *local = &ils->local->base;
	xrt_session_destroy(&local);
	xrt_session_destroy(&ils->remote);

	if (g_local_session == ils) {
		g_local_session = NULL;
	}
	free(ils);
}

static inline xrt_result_t
create_with_local_comp(struct ipc_client_system *icsys,
                       const struct xrt_session_info *xsi,
                       struct xrt_session **out_xs,
                       struct xrt_compositor_native **out_xcn)
{
	struct xrt_session *remote = NULL;
	xrt_result_t xret = create_headless(icsys, xsi, &remote);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	struct ipc_client_local_session *ils = U_TYPED_CALLOC(struct ipc_client_local_session);
	ils->base.poll_events = local_session_poll_events;
	ils->base.request_exit = local_session_request_exit;
	ils->base.destroy = local_session_destroy;
	ils->ipc_c = icsys->ipc_c;
	ils->remote = remote;
	ils->local = u_session_create(NULL);
	ils->xsysc = icsys->xsysc;

	if (g_local_session != NULL) {
		struct xrt_session *xs = &ils->base;
		xrt_session_destroy(&xs);
		return XRT_ERROR_MULTI_SESSION_NOT_IMPLEMENTED;
	}

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

	// Tell the service when the session runs, and notice the compositor going.
	struct xrt_compositor *xc = &(*out_xcn)->base;
	ils->xcn = *out_xcn;
	ils->begin_session = xc->begin_session;
	ils->end_session = xc->end_session;
	ils->destroy = xc->destroy;
	xc->begin_session = local_comp_begin_session;
	xc->end_session = local_comp_end_session;
	xc->destroy = local_comp_destroy;
	g_local_session = ils;

	*out_xs = &ils->base;

	return XRT_SUCCESS;
}


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
	} else if (icsys->local_compositor) {
		return create_with_local_comp(icsys, xsi, out_xs, out_xcn);
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
ipc_client_system_create_with_local_compositor(struct ipc_connection *ipc_c, struct xrt_system_compositor *xsysc)
{
	struct xrt_system *xsys = ipc_client_system_create(ipc_c, xsysc);
	if (xsys != NULL) {
		ipc_system(xsys)->local_compositor = true;
	}

	return xsys;
}
