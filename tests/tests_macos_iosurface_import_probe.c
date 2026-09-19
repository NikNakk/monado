// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Import externally-created IOSurface IDs as a real Monado service swapchain.
 */

#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "ipc_client_generated.h"

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_session.h"
#include "xrt/xrt_system.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define PROBE_WIDTH 64
#define PROBE_HEIGHT 64
#define PROBE_VK_FORMAT_BGRA8_UNORM 44

static int
fail_xret(const char *what, xrt_result_t xret)
{
	fprintf(stderr, "%s failed: %d\n", what, xret);
	return 1;
}

static bool
parse_id(const char *text, uint32_t *out)
{
	errno = 0;
	char *end = NULL;
	unsigned long value = strtoul(text, &end, 0);
	if (errno != 0 || end == text || *end != '\0' || value == 0 || value > UINT32_MAX) {
		return false;
	}
	*out = (uint32_t)value;
	return true;
}

static bool
format_supported(const struct xrt_compositor *xc, int64_t format)
{
	for (uint32_t i = 0; i < xc->info.format_count; i++) {
		if (xc->info.formats[i] == format) {
			return true;
		}
	}
	return false;
}

int
main(int argc, char **argv)
{
	if (argc < 2 || argc > (int)XRT_MAX_SWAPCHAIN_IMAGES + 1) {
		fprintf(stderr, "Usage: %s IOSURFACE_ID [IOSURFACE_ID ...]\n", argv[0]);
		return 2;
	}

	struct ipc_arg_swapchain_iosurface args = {0};
	args.image_count = (uint32_t)(argc - 1);
	for (uint32_t i = 0; i < args.image_count; i++) {
		if (!parse_id(argv[i + 1], &args.ids[i])) {
			fprintf(stderr, "Invalid IOSurface ID: %s\n", argv[i + 1]);
			return 2;
		}
	}

	struct ipc_connection ipc_c = {0};
	struct xrt_system_compositor *xsysc = NULL;
	struct xrt_system *xsys = NULL;
	struct xrt_session *xs = NULL;
	struct xrt_compositor_native *xcn = NULL;
	uint32_t swapchain_id = UINT32_MAX;
	bool session_begun = false;
	int ret = 1;

	struct xrt_instance_info info = {0};
	snprintf(info.app_info.application_name, sizeof(info.app_info.application_name), "%s",
	         "tests_macos_iosurface_import_probe");

	xrt_result_t xret = ipc_client_connection_init(&ipc_c, U_LOGGING_INFO, &info);
	if (xret != XRT_SUCCESS) {
		return fail_xret("ipc_client_connection_init", xret);
	}

	xret = ipc_client_create_system_compositor(&ipc_c, NULL, NULL, &xsysc);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("ipc_client_create_system_compositor", xret);
		goto out;
	}

	xsys = ipc_client_system_create(&ipc_c, xsysc);
	if (xsys == NULL) {
		fprintf(stderr, "ipc_client_system_create failed\n");
		goto out;
	}

	const struct xrt_session_info xsi = {0};
	xret = xrt_system_create_session(xsys, &xsi, &xs, &xcn);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_system_create_session", xret);
		goto out;
	}

	if (!format_supported(&xcn->base, PROBE_VK_FORMAT_BGRA8_UNORM)) {
		fprintf(stderr, "Service compositor does not advertise VK_FORMAT_B8G8R8A8_UNORM\n");
		goto out;
	}

	if (xsysc->info.view_config_count == 0) {
		fprintf(stderr, "No view configuration available\n");
		goto out;
	}
	const struct xrt_begin_session_info begin_info = {
	    .view_type = xsysc->info.view_configs[0].view_type,
	};
	xret = xrt_comp_begin_session(&xcn->base, &begin_info);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_comp_begin_session", xret);
		goto out;
	}
	session_begun = true;

	struct xrt_swapchain_create_info xsci = {0};
	xsci.bits = (enum xrt_swapchain_usage_bits)(XRT_SWAPCHAIN_USAGE_COLOR | XRT_SWAPCHAIN_USAGE_SAMPLED);
	xsci.format = PROBE_VK_FORMAT_BGRA8_UNORM;
	xsci.sample_count = 1;
	xsci.width = PROBE_WIDTH;
	xsci.height = PROBE_HEIGHT;
	xsci.face_count = 1;
	xsci.array_size = 1;
	xsci.mip_count = 1;

	xret = ipc_call_swapchain_import_iosurface(&ipc_c, &xsci, &args, &swapchain_id);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("ipc_call_swapchain_import_iosurface", xret);
		goto out;
	}

	printf("Imported %u external IOSurfaces as Monado swapchain id=%u\n", args.image_count, swapchain_id);
	for (uint32_t iteration = 0; iteration < args.image_count; iteration++) {
		uint32_t image_index = UINT32_MAX;
		xret = ipc_call_swapchain_acquire_image(&ipc_c, swapchain_id, &image_index);
		if (xret != XRT_SUCCESS) {
			ret = fail_xret("ipc_call_swapchain_acquire_image", xret);
			goto out;
		}
		xret = ipc_call_swapchain_wait_image(&ipc_c, swapchain_id, 1000000000ll, image_index);
		if (xret != XRT_SUCCESS) {
			ret = fail_xret("ipc_call_swapchain_wait_image", xret);
			goto out;
		}
		xret = ipc_call_swapchain_release_image(&ipc_c, swapchain_id, image_index);
		if (xret != XRT_SUCCESS) {
			ret = fail_xret("ipc_call_swapchain_release_image", xret);
			goto out;
		}
		printf("  acquire/wait/release iteration=%u image=%u OK\n", iteration, image_index);
	}

	xret = ipc_call_swapchain_destroy(&ipc_c, swapchain_id);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("ipc_call_swapchain_destroy", xret);
		goto out;
	}
	swapchain_id = UINT32_MAX;
	printf("External IOSurface -> MoltenVK MTLDevice -> VkImage swapchain import succeeded.\n");
	ret = 0;

out:
	if (swapchain_id != UINT32_MAX) {
		(void)ipc_call_swapchain_destroy(&ipc_c, swapchain_id);
	}
	if (session_begun && xcn != NULL) {
		xrt_comp_end_session(&xcn->base);
	}
	xrt_comp_native_destroy(&xcn);
	xrt_session_destroy(&xs);
	xrt_system_destroy(&xsys);
	xrt_syscomp_destroy(&xsysc);
	ipc_client_connection_fini(&ipc_c);
	return ret;
}
