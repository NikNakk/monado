// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Minimal in-process runtime probe for the macOS bring-up branch.
 */

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_session.h"
#include "xrt/xrt_system.h"

#include "os/os_time.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int
fail_xret(const char *what, xrt_result_t xret)
{
	fprintf(stderr, "%s failed: %d\n", what, xret);
	return 1;
}

int
main(void)
{
	struct xrt_instance *xinst = NULL;
	struct xrt_system *xsys = NULL;
	struct xrt_system_devices *xsysd = NULL;
	struct xrt_space_overseer *xso = NULL;
	struct xrt_system_compositor *xsysc = NULL;
	struct xrt_session *xs = NULL;
	struct xrt_compositor_native *xcn = NULL;
	struct xrt_swapchain_native *xscn = NULL;
	struct xrt_swapchain *imported_xsc = NULL;
	int ret = 1;

	xrt_result_t xret = xrt_instance_create(NULL, &xinst);
	if (xret != XRT_SUCCESS) {
		return fail_xret("xrt_instance_create", xret);
	}

	xret = xrt_instance_create_system(xinst, &xsys, &xsysd, &xso, &xsysc);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_instance_create_system", xret);
		goto out;
	}

	if (xsysd == NULL || xsysd->static_roles.head == NULL) {
		fprintf(stderr, "No head device available.\n");
		goto out;
	}

	if (xsysc == NULL) {
		fprintf(stderr, "No system compositor available.\n");
		goto out;
	}

	const struct xrt_view_config *view_config = &xsysc->info.view_configs[0];
	if (xsysc->info.view_config_count == 0 || view_config->view_count < 2) {
		fprintf(stderr, "No stereo view configuration available.\n");
		goto out;
	}

	fprintf(stdout, "Head device: %s\n", xsysd->static_roles.head->str);
	fprintf(stdout, "View config count: %u\n", xsysc->info.view_config_count);
	fprintf(stdout, "Recommended view size: %ux%u\n",
	        view_config->views[0].recommended.width_pixels,
	        view_config->views[0].recommended.height_pixels);

	const struct xrt_session_info xsi = {0};
	xret = xrt_system_create_session(xsys, &xsi, &xs, &xcn);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_system_create_session", xret);
		goto out;
	}

	if (xsysc->xmcc != NULL) {
		xrt_syscomp_set_state(xsysc, &xcn->base, true, true, os_monotonic_get_ns());
		xrt_syscomp_set_z_order(xsysc, &xcn->base, 0);
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
	int64_t chosen_format = xcn->base.info.formats[0];
	if (chosen_format == 0) {
		// VK_FORMAT_R8G8B8A8_UNORM. Keep this probe self-contained instead of depending on Vulkan headers.
		chosen_format = 37;
		fprintf(stdout, "Runtime reported no native-exportable color formats, using fallback format %lld.\n",
		        (long long)chosen_format);
	}

	xsci.bits = (enum xrt_swapchain_usage_bits)(XRT_SWAPCHAIN_USAGE_COLOR | XRT_SWAPCHAIN_USAGE_SAMPLED);
	xsci.format = chosen_format;
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

	fprintf(stdout, "Created native swapchain with %u images.\n", xscn->base.image_count);
	for (uint32_t i = 0; i < xscn->base.image_count; ++i) {
		fprintf(stdout, "  native[%u]: handle=%p size=%llu dedicated=%d\n", i, (void *)xscn->images[i].handle,
		        (unsigned long long)xscn->images[i].size, xscn->images[i].use_dedicated_allocation ? 1 : 0);
	}
	fflush(stdout);

	struct xrt_image_native imported_images[XRT_MAX_SWAPCHAIN_IMAGES] = {0};
	for (uint32_t i = 0; i < xscn->base.image_count; ++i) {
		imported_images[i] = xscn->images[i];
	}

	fprintf(stdout, "Importing native swapchain back into the compositor.\n");
	fflush(stdout);
	xret = xrt_comp_import_swapchain(&xcn->base, &xsci, imported_images, xscn->base.image_count, &imported_xsc);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_comp_import_swapchain", xret);
		goto out;
	}
	fprintf(stdout, "Imported native swapchain successfully.\n");
	fflush(stdout);

	uint32_t image_index = 0;
	xret = xrt_swapchain_acquire_image(&xscn->base, &image_index);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_swapchain_acquire_image", xret);
		goto out;
	}

	xret = xrt_swapchain_wait_image(&xscn->base, 1000000000ll, image_index);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_swapchain_wait_image", xret);
		goto out;
	}

	xret = xrt_swapchain_release_image(&xscn->base, image_index);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_swapchain_release_image", xret);
		goto out;
	}

	uint32_t imported_image_index = 0;
	xret = xrt_swapchain_acquire_image(imported_xsc, &imported_image_index);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_swapchain_acquire_image(imported)", xret);
		goto out;
	}

	xret = xrt_swapchain_wait_image(imported_xsc, 1000000000ll, imported_image_index);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_swapchain_wait_image(imported)", xret);
		goto out;
	}

	xret = xrt_swapchain_release_image(imported_xsc, imported_image_index);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_swapchain_release_image(imported)", xret);
		goto out;
	}

	fprintf(stdout, "Verified native IOSurface swapchain round-trip successfully.\n");
	fflush(stdout);

	if (getenv("MACOS_RUNTIME_PROBE_SUBMIT_FRAME") == NULL) {
		return 0;
	}

	int64_t frame_id = -1;
	int64_t predicted_display_time_ns = 0;
	int64_t predicted_display_period_ns = 0;
	xret = xrt_comp_wait_frame(&xcn->base, &frame_id, &predicted_display_time_ns, &predicted_display_period_ns);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_comp_wait_frame", xret);
		goto out;
	}

	xret = xrt_comp_begin_frame(&xcn->base, frame_id);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_comp_begin_frame", xret);
		goto out;
	}

	struct xrt_layer_frame_data frame_data = {
	    .frame_id = frame_id,
	    .display_time_ns = predicted_display_time_ns,
	    .env_blend_mode = xsysc->info.supported_blend_modes[0],
	};
	xret = xrt_comp_layer_begin(&xcn->base, &frame_data);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_comp_layer_begin", xret);
		goto out;
	}

	struct xrt_vec3 default_eye_relation = {0.063f, 0.0f, 0.0f};
	struct xrt_space_relation head_relation = XRT_SPACE_RELATION_ZERO;
	struct xrt_fov fovs[XRT_MAX_VIEWS] = {0};
	struct xrt_pose eye_poses[XRT_MAX_VIEWS] = {0};
	xret = xrt_device_get_view_poses(xsysd->static_roles.head,
	                                 &default_eye_relation,
	                                 predicted_display_time_ns,
	                                 view_config->view_type,
	                                 view_config->view_count,
	                                 &head_relation,
	                                 fovs,
	                                 eye_poses);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_device_get_view_poses", xret);
		goto out;
	}

	struct xrt_swapchain *projection_swapchains[XRT_MAX_VIEWS] = {&xscn->base, &xscn->base};
	struct xrt_layer_data projection = {0};
	projection.type = XRT_LAYER_PROJECTION;
	projection.name = XRT_INPUT_GENERIC_HEAD_POSE;
	projection.timestamp = predicted_display_time_ns;
	projection.view_count = view_config->view_count;

	for (uint32_t i = 0; i < view_config->view_count; ++i) {
		projection.proj.v[i].sub.image_index = image_index;
		projection.proj.v[i].sub.array_index = 0;
		projection.proj.v[i].sub.rect.offset.w = 0;
		projection.proj.v[i].sub.rect.offset.h = 0;
		projection.proj.v[i].sub.rect.extent.w = xsci.width;
		projection.proj.v[i].sub.rect.extent.h = xsci.height;
		projection.proj.v[i].sub.norm_rect.x = 0.0f;
		projection.proj.v[i].sub.norm_rect.y = 0.0f;
		projection.proj.v[i].sub.norm_rect.w = 1.0f;
		projection.proj.v[i].sub.norm_rect.h = 1.0f;
		projection.proj.v[i].fov = fovs[i];
		projection.proj.v[i].pose = eye_poses[i];
	}

	xret = xrt_comp_layer_projection(&xcn->base, xsysd->static_roles.head, projection_swapchains, &projection);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_comp_layer_projection", xret);
		goto out;
	}

	struct xrt_layer_data quad = {0};
	quad.type = XRT_LAYER_QUAD;
	quad.name = XRT_INPUT_GENERIC_HEAD_POSE;
	quad.timestamp = predicted_display_time_ns;
	quad.flags = XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT;
	quad.view_count = view_config->view_count;
	quad.quad.visibility = XRT_LAYER_EYE_VISIBILITY_BOTH;
	quad.quad.sub.image_index = image_index;
	quad.quad.sub.array_index = 0;
	quad.quad.sub.rect.offset.w = 0;
	quad.quad.sub.rect.offset.h = 0;
	quad.quad.sub.rect.extent.w = xsci.width;
	quad.quad.sub.rect.extent.h = xsci.height;
	quad.quad.sub.norm_rect.x = 0.0f;
	quad.quad.sub.norm_rect.y = 0.0f;
	quad.quad.sub.norm_rect.w = 1.0f;
	quad.quad.sub.norm_rect.h = 1.0f;
	quad.quad.pose.orientation.w = 1.0f;
	quad.quad.pose.position.z = -1.0f;
	quad.quad.size.x = 1.0f;
	quad.quad.size.y = 1.0f;

	xret = xrt_comp_layer_quad(&xcn->base, xsysd->static_roles.head, &xscn->base, &quad);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_comp_layer_quad", xret);
		goto out;
	}

	xret = xrt_comp_layer_commit(&xcn->base, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
	if (xret != XRT_SUCCESS) {
		ret = fail_xret("xrt_comp_layer_commit", xret);
		goto out;
	}

	// The multi-compositor session path queues layers for a separate render thread.
	usleep(100000);

	fprintf(stdout, "Submitted a non-fast-path frame successfully.\n");
	ret = 0;

out:
	if (xcn != NULL) {
		xrt_comp_end_session(&xcn->base);
	}
	xrt_swapchain_reference(&imported_xsc, NULL);
	xrt_swapchain_native_reference(&xscn, NULL);
	xrt_comp_native_destroy(&xcn);
	xrt_session_destroy(&xs);
	xrt_syscomp_destroy(&xsysc);
	xrt_space_overseer_destroy(&xso);
	xrt_system_devices_destroy(&xsysd);
	xrt_system_destroy(&xsys);
	xrt_instance_destroy(&xinst);
	return ret;
}
