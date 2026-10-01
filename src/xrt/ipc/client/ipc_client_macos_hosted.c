// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS clients that composite in-process and are hosted by the service.
 *
 * The client runs the main compositor itself, with the service's devices over
 * IPC. Its presenter renders into a CAContext and the service shows that in
 * the headset window, so the service no longer composites this client's
 * frames. See doc/macos-client-compositor-design.md.
 *
 * @ingroup ipc_client
 */

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "util/u_macos_hosted_client.h"
#include "util/u_misc.h"
#include "main/comp_window_macos_hosted.h"

#include "main/comp_main_interface.h"

#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "client/ipc_client_macos_hosted.h"

#include "ipc_client_generated.h"


DEBUG_GET_ONCE_BOOL_OPTION(macos_client_compositor, "XRT_MACOS_CLIENT_COMPOSITOR", false)
DEBUG_GET_ONCE_BOOL_OPTION(macos_hosted_follow_service_focus, "XRT_MACOS_HOSTED_FOLLOW_SERVICE_FOCUS", true)

struct ipc_client_macos_hosted
{
	struct u_macos_hosted_client *client;
	struct comp_target_factory *factory;
};

static xrt_result_t
hosted_set_active(void *ctx, bool active)
{
	return ipc_call_compositor_hosted_session_active((struct ipc_connection *)ctx, active);
}



/*
 *
 * Host operations, reached from the presenter.
 *
 */

static xrt_result_t
hosted_attach(void *ctx, uint32_t context_id)
{
	return ipc_call_compositor_hosted_attach((struct ipc_connection *)ctx, context_id);
}

static xrt_result_t
hosted_set_visibility(void *ctx, enum u_macos_display_host_visibility visibility)
{
	return ipc_call_compositor_hosted_set_visibility((struct ipc_connection *)ctx, (uint32_t)visibility);
}

static void
hosted_detach(void *ctx)
{
	xrt_result_t xret = ipc_call_compositor_hosted_detach((struct ipc_connection *)ctx);
	IPC_CHK_ONLY_PRINT((struct ipc_connection *)ctx, xret, "ipc_call_compositor_hosted_detach");
}

static const struct u_macos_hosted_client_ops hosted_ops = {
    .attach = hosted_attach,
    .set_visibility = hosted_set_visibility,
    .detach = hosted_detach,
    .set_active = hosted_set_active,
};


/*
 *
 * 'Exported' functions.
 *
 */

xrt_result_t
ipc_client_macos_hosted_create_system_compositor(struct ipc_connection *ipc_c,
                                                 struct xrt_device *head,
                                                 struct xrt_system_compositor **out_xsysc,
                                                 struct ipc_client_macos_hosted **out_hosted)
{
	if (!debug_get_bool_option_macos_client_compositor()) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	struct ipc_client_macos_hosted *hosted = U_TYPED_CALLOC(struct ipc_client_macos_hosted);
	if (hosted == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	hosted->client =
	    u_macos_hosted_client_create(&hosted_ops, ipc_c, debug_get_bool_option_macos_hosted_follow_service_focus());
	if (hosted->client == NULL) {
		free(hosted);
		return XRT_ERROR_ALLOCATION;
	}
	hosted->factory = comp_window_macos_hosted_factory_create(hosted->client);
	if (hosted->factory == NULL) {
		ipc_client_macos_hosted_fini(hosted);
		return XRT_ERROR_ALLOCATION;
	}

	ipc_client_hmd_prepare_for_local_compositor(head);

	struct xrt_system_compositor *xsysc = NULL;
	xrt_result_t xret = comp_main_create_system_compositor(head, hosted->factory, NULL, &xsysc);
	if (xret != XRT_SUCCESS || xsysc == NULL) {
		U_LOG_W("In-process compositor unavailable (%d), using the service's compositor", (int)xret);
		ipc_client_macos_hosted_fini(hosted);
		return xret != XRT_SUCCESS ? xret : XRT_ERROR_IPC_FAILURE;
	}

	U_LOG_I("Compositing in-process; the service hosts this client's layer");
	*out_xsysc = xsysc;
	*out_hosted = hosted;

	return XRT_SUCCESS;
}

void
ipc_client_macos_hosted_fini(struct ipc_client_macos_hosted *hosted)
{
	if (hosted == NULL) {
		return;
	}
	u_macos_hosted_client_close(hosted->client);
	u_macos_hosted_client_release(hosted->client);
	comp_window_macos_hosted_factory_destroy(hosted->factory);
	free(hosted);
}

struct u_macos_hosted_client *
ipc_client_macos_hosted_get_client(struct ipc_client_macos_hosted *hosted)
{
	return hosted->client;
}

bool
ipc_client_macos_hosted_follows_service_focus(struct u_macos_hosted_client *client)
{
	return u_macos_hosted_client_follows_service_focus(client);
}

void
ipc_client_macos_hosted_session_active(struct u_macos_hosted_client *client, bool active)
{
	xrt_result_t xret = u_macos_hosted_client_set_active(client, active);
	if (xret != XRT_SUCCESS) {
		U_LOG_W("The service did not accept hosted session activity (%d)", (int)xret);
	}
}

void
ipc_client_macos_hosted_set_visible(struct u_macos_hosted_client *client, bool visible)
{
	xrt_result_t xret = u_macos_hosted_client_set_visibility(client, visible ? U_MACOS_DISPLAY_HOST_SHOWN
	                                                                         : U_MACOS_DISPLAY_HOST_HIDDEN);
	if (xret != XRT_SUCCESS) {
		U_LOG_W("The service did not %s the hosted layer (%d)", visible ? "show" : "hide", (int)xret);
	}
}
