// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Probe the macOS IPC swapchain path using a running monado-service.
 */

#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "ipc_client_generated.h"

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_session.h"
#include "xrt/xrt_system.h"

#include <stdio.h>
#include <string.h>

static int
fail_xret(const char *what, xrt_result_t xret)
{
	fprintf(stderr, "%s failed: %d\n", what, xret);
	return 1;
}

int
main(void)
{
	struct ipc_connection ipc_c = {0};
	struct xrt_system_compositor *xsysc = NULL;
	struct xrt_system *xsys = NULL;
	struct xrt_session *xs = NULL;
	struct xrt_compositor_native *xcn = NULL;
	struct xrt_swapchain_native *xscn = NULL;
	struct xrt_swapchain *imported_xsc = NULL;
	int ret = 1;

	struct xrt_instance_info info = {0};
	snprintf(info.app_info.application_name, sizeof(info.app_info.application_name), "%s",
	         "tests_macos_ipc_swapchain_probe");

	xrt_result_t xret = ipc_client_connection_init(&ipc_c, U_LOGGING_INFO, &info);
	if (xret != XRT_SUCCESS) {
		return fail_xret("ipc_client_connection_init", xret);
	}

	bool is_system_available = false;
	xret = ipc_call_instance_is_system_available(&ipc_c, &is_system_available);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("ipc_call_instance_is_system_available", xret);
		goto out;
	}
	if (!is_system_available) {
		fprintf(stderr, "IPC service reported no system available.\n");
		goto out;
	}

	xret = ipc_client_create_system_compositor(&ipc_c, NULL, NULL, &xsysc);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("ipc_client_create_system_compositor", xret);
		goto out;
	}

	xsys = ipc_client_system_create(&ipc_c, xsysc);
	if (xsys == NULL) {
		fprintf(stderr, "ipc_client_system_create failed.\n");
		goto out;
	}

	const struct xrt_session_info xsi = {0};
	xret = xrt_system_create_session(xsys, &xsi, &xs, &xcn);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_system_create_session", xret);
		goto out;
	}

	const struct xrt_view_config *view_config = &xsysc->info.view_configs[0];
	if (xsysc->info.view_config_count == 0 || view_config->view_count < 2) {
		fprintf(stderr, "No stereo view configuration available.\n");
		goto out;
	}

	const struct xrt_begin_session_info begin_info = {
	    .view_type = view_config->view_type,
	};
	xret = xrt_comp_begin_session(&xcn->base, &begin_info);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_comp_begin_session", xret);
		goto out;
	}

	struct xrt_swapchain_create_info xsci = {0};
	xsci.bits = (enum xrt_swapchain_usage_bits)(XRT_SWAPCHAIN_USAGE_COLOR | XRT_SWAPCHAIN_USAGE_SAMPLED);
	xsci.format = xcn->base.info.formats[0];
	xsci.sample_count = view_config->views[0].recommended.sample_count;
	xsci.width = view_config->views[0].recommended.width_pixels;
	xsci.height = view_config->views[0].recommended.height_pixels;
	xsci.face_count = 1;
	xsci.array_size = 1;
	xsci.mip_count = 1;

	xret = xrt_comp_native_create_swapchain(xcn, &xsci, &xscn);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_comp_native_create_swapchain", xret);
		goto out;
	}

	fprintf(stdout, "IPC native swapchain with %u images.\n", xscn->base.image_count);
	for (uint32_t i = 0; i < xscn->base.image_count; ++i) {
		fprintf(stdout, "  native[%u]: handle=%p size=%llu dedicated=%d\n", i, (void *)xscn->images[i].handle,
		        (unsigned long long)xscn->images[i].size, xscn->images[i].use_dedicated_allocation ? 1 : 0);
	}

	struct xrt_image_native imported_images[XRT_MAX_SWAPCHAIN_IMAGES] = {0};
	for (uint32_t i = 0; i < xscn->base.image_count; ++i) {
		imported_images[i] = xscn->images[i];
	}

	xret = xrt_comp_import_swapchain(&xcn->base, &xsci, imported_images, xscn->base.image_count, &imported_xsc);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_comp_import_swapchain", xret);
		goto out;
	}

	uint32_t image_index = 0;
	xret = xrt_swapchain_acquire_image(imported_xsc, &image_index);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_swapchain_acquire_image(imported)", xret);
		goto out;
	}

	xret = xrt_swapchain_wait_image(imported_xsc, 1000000000ll, image_index);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_swapchain_wait_image(imported)", xret);
		goto out;
	}

	xret = xrt_swapchain_release_image(imported_xsc, image_index);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_swapchain_release_image(imported)", xret);
		goto out;
	}

	fprintf(stdout, "Verified IPC IOSurface swapchain round-trip successfully.\n");
	ret = 0;

out:
	if (xcn != NULL) {
		xrt_comp_end_session(&xcn->base);
	}
	xrt_swapchain_reference(&imported_xsc, NULL);
	xrt_swapchain_native_reference(&xscn, NULL);
	xrt_comp_native_destroy(&xcn);
	xrt_session_destroy(&xs);
	xrt_system_destroy(&xsys);
	xrt_syscomp_destroy(&xsysc);
	ipc_client_connection_fini(&ipc_c);
	return ret;
}
