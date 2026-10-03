// Copyright 2020-2024, Collabora, Ltd.
// Copyright 2025-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  IPC Client HMD device.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup ipc_client
 */


#include "xrt/xrt_device.h"

#include "os/os_time.h"

#include "math/m_api.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"

#include "client/ipc_client.h"
#include "client/ipc_client_xdev.h"
#include "client/ipc_client_connection.h"
#include "ipc_client_generated.h"

#ifdef XRT_IPC_MACOS_HOSTED_COMPOSITOR
#include "client/ipc_client_passthrough.h"
#include "client/ipc_client_tracking_share.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>


DEBUG_GET_ONCE_BOOL_OPTION(ipc_distortion_mesh_transfer, "XRT_IPC_DISTORTION_MESH_TRANSFER", true)
DEBUG_GET_ONCE_NUM_OPTION(ipc_distortion_grid_points, "XRT_IPC_DISTORTION_GRID_POINTS", 513)


/*
 *
 * Structs and defines.
 *
 */

/*!
 * An IPC client proxy for an HMD @ref xrt_device and @ref ipc_client_xdev.
 * Using a typedef reduce impact of refactor change.
 *
 * @implements ipc_client_xdev
 * @ingroup ipc_client
 */
typedef struct ipc_client_xdev ipc_client_hmd_t;


/*
 *
 * Helpers.
 *
 */

static inline ipc_client_hmd_t *
ipc_client_hmd(struct xrt_device *xdev)
{
	return (ipc_client_hmd_t *)xdev;
}

static xrt_result_t
call_get_view_poses_raw(ipc_client_hmd_t *ich,
                        const struct xrt_vec3 *default_eye_relation,
                        int64_t at_timestamp_ns,
                        enum xrt_view_type view_type,
                        uint32_t view_count,
                        struct xrt_space_relation *out_head_relation,
                        struct xrt_fov *out_fovs,
                        struct xrt_pose *out_poses)
{
	struct ipc_connection *ipc_c = ich->ipc_c;
	xrt_result_t xret;

	ipc_client_connection_lock(ipc_c);
	ipc_client_connection_send_lock(ipc_c);

	// Using the raw send helper is the only one that is required.
	xret = ipc_send_device_get_view_poses_locked( //
	    ipc_c,                                    //
	    ich->device_id,                           //
	    default_eye_relation,                     //
	    at_timestamp_ns,                          //
	    view_type,                                //
	    view_count);                              //
	if (xret != XRT_SUCCESS) {
		ipc_client_connection_send_unlock(ipc_c);
		goto out;
	}
	ipc_client_connection_send_unlock(ipc_c);

	// This is the data we get back in the provided reply.
	uint32_t returned_view_count = 0;
	struct xrt_space_relation head_relation = XRT_SPACE_RELATION_ZERO;

	// Get the reply, use the raw function helper.
	xret = ipc_receive_device_get_view_poses_locked( //
	    ipc_c,                                       //
	    &head_relation,                              //
	    &returned_view_count);                       //
	IPC_CHK_WITH_GOTO(ich->ipc_c, xret, "ipc_receive_device_get_view_poses_locked", out);

	if (view_count != returned_view_count) {
		IPC_ERROR(ich->ipc_c, "Wrong view counts (sent: %u != got: %u)", view_count, returned_view_count);
		assert(false);
	}

	// We can read directly to the output variables.
	xret = ipc_receive(&ipc_c->imc, out_fovs, sizeof(struct xrt_fov) * view_count);
	IPC_CHK_WITH_GOTO(ich->ipc_c, xret, "ipc_receive(1)", out);

	// We can read directly to the output variables.
	xret = ipc_receive(&ipc_c->imc, out_poses, sizeof(struct xrt_pose) * view_count);
	IPC_CHK_WITH_GOTO(ich->ipc_c, xret, "ipc_receive(2)", out);

	/*
	 * Finally set the head_relation that we got in the reply, mostly to
	 * demonstrate that you can use the reply struct in such a way.
	 */
	*out_head_relation = head_relation;

out:
	ipc_client_connection_unlock(ipc_c);
	return xret;
}


/*
 *
 * Member functions
 *
 */

static xrt_result_t
ipc_client_hmd_get_view_poses(struct xrt_device *xdev,
                              const struct xrt_vec3 *default_eye_relation,
                              int64_t at_timestamp_ns,
                              enum xrt_view_type view_type,
                              uint32_t view_count,
                              struct xrt_space_relation *out_head_relation,
                              struct xrt_fov *out_fovs,
                              struct xrt_pose *out_poses)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);

	// Artificial limit.
	if (view_count == 0 || view_count > IPC_MAX_RAW_VIEWS) {
		IPC_ERROR(ich->ipc_c, "Cannot handle %u view_count, %u or less supported.", view_count,
		          (uint32_t)IPC_MAX_RAW_VIEWS);
		return XRT_ERROR_IPC_FAILURE;
	}

#ifdef XRT_IPC_MACOS_HOSTED_COMPOSITOR
	float ipd_m = 0;
	if (view_count == 2 && ipc_client_tracking_share_pose(ich, at_timestamp_ns, out_head_relation, &ipd_m)) {
		struct xrt_vec3 eye_relation = *default_eye_relation;
		eye_relation.x = ipd_m;
		for (uint32_t i = 0; i < view_count; ++i) {
			out_fovs[i] = xdev->hmd->distortion.fov[i];
			u_device_get_view_pose(&eye_relation, i, &out_poses[i]);
		}
		return XRT_SUCCESS;
	}
#endif
	// Fast path.
	if (view_count == 2) {
		struct ipc_info_get_view_poses_2 info = {0};
		xrt_result_t xret = ipc_call_device_get_view_poses_2( //
		    ich->ipc_c,                                       //
		    ich->device_id,                                   //
		    default_eye_relation,                             //
		    at_timestamp_ns,                                  //
		    view_type,                                        //
		    view_count,                                       //
		    &info);                                           //
		IPC_CHK_AND_RET(ich->ipc_c, xret, "ipc_call_device_get_view_poses_2");

		*out_head_relation = info.head_relation;
		for (int i = 0; i < 2; i++) {
			out_fovs[i] = info.fovs[i];
			out_poses[i] = info.poses[i];
		}

		return xret;
	}

	return call_get_view_poses_raw( //
	    ich,                        //
	    default_eye_relation,       //
	    at_timestamp_ns,            //
	    view_type,                  //
	    view_count,                 //
	    out_head_relation,          //
	    out_fovs,                   //
	    out_poses);                 //
}

static xrt_result_t
ipc_client_hmd_compute_distortion(
    struct xrt_device *xdev, uint32_t view, float u, float v, struct xrt_uv_triplet *out_result)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	xrt_result_t xret;

	xret = ipc_call_device_compute_distortion( //
	    ich->ipc_c,                            //
	    ich->device_id,                        //
	    view,                                  //
	    u,                                     //
	    v,                                     //
	    out_result);                           //

	IPC_CHK_ALWAYS_RET(ich->ipc_c, xret, "ipc_call_device_compute_distortion");
}

/*!
 * Fetch the service device's distortion for @p view sampled on a grid, in one
 * call. Leaves nothing allocated on failure.
 */
static xrt_result_t
fetch_distortion_grid(ipc_client_hmd_t *ich, uint32_t view, uint32_t points)
{
	struct ipc_connection *ipc_c = ich->ipc_c;
	struct xrt_uv_triplet *grid = NULL;
	uint32_t grid_size = 0;
	xrt_result_t xret;

	ipc_client_connection_lock(ipc_c);
	ipc_client_connection_send_lock(ipc_c);
	xret = ipc_send_device_get_distortion_grid_locked(ipc_c, ich->device_id, view, points);
	ipc_client_connection_send_unlock(ipc_c);
	if (xret != XRT_SUCCESS) {
		goto out_unlock;
	}

	// Nothing follows an unsuccessful reply.
	xret = ipc_receive_device_get_distortion_grid_locked(ipc_c, &grid_size);
	if (xret != XRT_SUCCESS) {
		goto out_unlock;
	}

	grid = malloc(grid_size > 0 ? grid_size : 1);
	if (grid == NULL) {
		xret = XRT_ERROR_ALLOCATION;
		goto out_unlock;
	}

	// Receive even if the size is wrong, to keep the channel in step.
	xret = ipc_receive(&ipc_c->imc, grid, grid_size);
	if (xret == XRT_SUCCESS && grid_size != (size_t)points * points * sizeof(*grid)) {
		xret = XRT_ERROR_IPC_FAILURE;
	}
	if (xret != XRT_SUCCESS) {
		free(grid);
		goto out_unlock;
	}

	ich->distortion_grid[view] = grid;

out_unlock:
	ipc_client_connection_unlock(ipc_c);
	return xret;
}

static inline struct xrt_vec2
lerp_vec2(struct xrt_vec2 a, struct xrt_vec2 b, float t)
{
	return (struct xrt_vec2){a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

//! compute_distortion from the fetched grid, bilinearly interpolated.
static xrt_result_t
ipc_client_hmd_compute_distortion_from_grid(
    struct xrt_device *xdev, uint32_t view, float u, float v, struct xrt_uv_triplet *out_result)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	const uint32_t n = ich->distortion_grid_points;
	const struct xrt_uv_triplet *grid = view < XRT_MAX_VIEWS ? ich->distortion_grid[view] : NULL;
	if (grid == NULL || n < 2) {
		return ipc_client_hmd_compute_distortion(xdev, view, u, v, out_result);
	}

	// Clamp to the sampled square; callers sample inside [0, 1].
	float x = (u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u)) * (float)(n - 1);
	float y = (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v)) * (float)(n - 1);
	uint32_t x0 = (uint32_t)x < n - 1 ? (uint32_t)x : n - 2;
	uint32_t y0 = (uint32_t)y < n - 1 ? (uint32_t)y : n - 2;
	float tx = x - (float)x0;
	float ty = y - (float)y0;

	const struct xrt_uv_triplet *a = &grid[(size_t)y0 * n + x0];
	const struct xrt_uv_triplet *b = a + 1;
	const struct xrt_uv_triplet *c = a + n;
	const struct xrt_uv_triplet *d = c + 1;

	out_result->r = lerp_vec2(lerp_vec2(a->r, b->r, tx), lerp_vec2(c->r, d->r, tx), ty);
	out_result->g = lerp_vec2(lerp_vec2(a->g, b->g, tx), lerp_vec2(c->g, d->g, tx), ty);
	out_result->b = lerp_vec2(lerp_vec2(a->b, b->b, tx), lerp_vec2(c->b, d->b, tx), ty);

	return XRT_SUCCESS;
}

/*!
 * Copy the service device's finished distortion mesh in one call, instead of
 * one call per vertex. Leaves the mesh untouched on failure.
 */
static xrt_result_t
fetch_distortion_mesh(ipc_client_hmd_t *ich)
{
	struct ipc_connection *ipc_c = ich->ipc_c;
	struct xrt_hmd_parts *hmd = ich->base.hmd;
	struct ipc_distortion_mesh_info info = {0};
	float *vertices = NULL;
	int *indices = NULL;
	xrt_result_t xret;

	ipc_client_connection_lock(ipc_c);
	ipc_client_connection_send_lock(ipc_c);
	xret = ipc_send_device_get_distortion_mesh_locked(ipc_c, ich->device_id);
	ipc_client_connection_send_unlock(ipc_c);
	if (xret != XRT_SUCCESS) {
		goto out_unlock;
	}

	// Nothing follows an unsuccessful reply.
	xret = ipc_receive_device_get_distortion_mesh_locked(ipc_c, &info);
	if (xret != XRT_SUCCESS) {
		goto out_unlock;
	}

	size_t vertices_size = (size_t)info.vertex_count * info.stride;
	size_t indices_size = (size_t)info.index_count_total * sizeof(int);
	vertices = malloc(vertices_size);
	indices = malloc(indices_size);
	if (vertices == NULL || indices == NULL) {
		// The data is on its way regardless: drain it to keep the channel in step.
		free(vertices);
		free(indices);
		vertices = malloc(vertices_size > indices_size ? vertices_size : indices_size);
		indices = NULL;
		if (vertices == NULL) {
			xret = XRT_ERROR_IPC_FAILURE;
			goto out_unlock;
		}
		(void)ipc_receive(&ipc_c->imc, vertices, vertices_size);
		(void)ipc_receive(&ipc_c->imc, vertices, indices_size);
		free(vertices);
		xret = XRT_ERROR_ALLOCATION;
		goto out_unlock;
	}

	xret = ipc_receive(&ipc_c->imc, vertices, vertices_size);
	if (xret == XRT_SUCCESS) {
		xret = ipc_receive(&ipc_c->imc, indices, indices_size);
	}
	if (xret != XRT_SUCCESS) {
		free(vertices);
		free(indices);
		goto out_unlock;
	}

	hmd->distortion.mesh.vertices = vertices;
	hmd->distortion.mesh.vertex_count = info.vertex_count;
	hmd->distortion.mesh.stride = info.stride;
	hmd->distortion.mesh.uv_channels_count = info.uv_channels_count;
	hmd->distortion.mesh.indices = indices;
	hmd->distortion.mesh.index_count_total = info.index_count_total;
	for (uint32_t i = 0; i < XRT_MAX_VIEWS; i++) {
		hmd->distortion.mesh.index_counts[i] = info.index_counts[i];
		hmd->distortion.mesh.index_offsets[i] = info.index_offsets[i];
	}

out_unlock:
	ipc_client_connection_unlock(ipc_c);
	return xret;
}

#ifdef XRT_IPC_MACOS_HOSTED_COMPOSITOR
//! The service's camera frames, for this process's compositor.
static xrt_result_t
ipc_client_hmd_set_passthrough_sinks(struct xrt_device *xdev, struct xrt_frame_sink *left, struct xrt_frame_sink *right)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	if (ich->passthrough == NULL) {
		if (left == NULL && right == NULL) {
			return XRT_SUCCESS;
		}
		ich->passthrough = ipc_client_passthrough_create(ich->ipc_c, ich->device_id);
	}
	return ipc_client_passthrough_set_sinks(ich->passthrough, left, right);
}
#endif

void
ipc_client_hmd_prepare_for_local_compositor(struct xrt_device *xdev)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	struct ipc_shared_memory *ism = ich->ipc_c->ism;
	struct xrt_hmd_parts *hmd = xdev->hmd;

	hmd->screens[0].w_pixels = (int)ism->hmd.compositor.w_pixels;
	hmd->screens[0].h_pixels = (int)ism->hmd.compositor.h_pixels;
	hmd->screens[0].nominal_frame_interval_ns = ism->hmd.compositor.nominal_frame_interval_ns;
	for (uint32_t i = 0; i < hmd->view_count && i < ARRAY_SIZE(ism->hmd.compositor.views); ++i) {
		hmd->views[i].viewport.x_pixels = ism->hmd.compositor.views[i].x_pixels;
		hmd->views[i].viewport.y_pixels = ism->hmd.compositor.views[i].y_pixels;
		hmd->views[i].viewport.w_pixels = ism->hmd.compositor.views[i].w_pixels;
		hmd->views[i].viewport.h_pixels = ism->hmd.compositor.views[i].h_pixels;
		hmd->views[i].rot = ism->hmd.compositor.views[i].rot;
		hmd->distortion.fov[i] = ism->hmd.compositor.views[i].distortion_fov;
	}

#ifdef XRT_IPC_MACOS_HOSTED_COMPOSITOR
	ipc_client_tracking_share_create(ich);
	// Passthrough, if the service's compositor has camera frames to share.
	if (ism->hmd.passthrough_share_available != 0) {
		xdev->set_passthrough_sinks = ipc_client_hmd_set_passthrough_sinks;
	}
#endif

	// Replace the placeholder mesh with the service device's distortion.
	free(hmd->distortion.mesh.vertices);
	hmd->distortion.mesh.vertices = NULL;
	free(hmd->distortion.mesh.indices);
	hmd->distortion.mesh.indices = NULL;

	xdev->compute_distortion = ipc_client_hmd_compute_distortion;
	hmd->distortion.models = XRT_DISTORTION_MODEL_COMPUTE;
	hmd->distortion.preferred = XRT_DISTORTION_MODEL_COMPUTE;

	/*
	 * The compositor samples the distortion per texel as well (the compute
	 * path's distortion images, and on macOS the passthrough maps). Fetch a
	 * grid per view and interpolate it locally, instead of hundreds of
	 * thousands of calls to the service.
	 */
	int64_t grid_start_ns = os_monotonic_get_ns();
	int64_t points = debug_get_num_option_ipc_distortion_grid_points();
	if (points >= 2 && points <= 1025) {
		ich->distortion_grid_points = (uint32_t)points;
		bool all = true;
		for (uint32_t i = 0; i < hmd->view_count && i < XRT_MAX_VIEWS; i++) {
			xrt_result_t xret = fetch_distortion_grid(ich, i, (uint32_t)points);
			if (xret != XRT_SUCCESS) {
				IPC_WARN(ich->ipc_c, "Could not fetch the distortion grid for view %u (%d)", i, xret);
				all = false;
				break;
			}
		}
		if (all) {
			xdev->compute_distortion = ipc_client_hmd_compute_distortion_from_grid;
			IPC_INFO(ich->ipc_c, "Distortion grid copied from the service: %u x %u per view in %.1f ms",
			         (uint32_t)points, (uint32_t)points,
			         (double)(os_monotonic_get_ns() - grid_start_ns) / 1e6);
		} else {
			for (uint32_t i = 0; i < XRT_MAX_VIEWS; i++) {
				free(ich->distortion_grid[i]);
				ich->distortion_grid[i] = NULL;
			}
			ich->distortion_grid_points = 0;
		}
	}

	int64_t start_ns = os_monotonic_get_ns();
	const char *how = "computed point by point";
	if (debug_get_bool_option_ipc_distortion_mesh_transfer()) {
		xrt_result_t xret = fetch_distortion_mesh(ich);
		if (xret == XRT_SUCCESS) {
			how = "copied from the service";
		} else {
			IPC_WARN(ich->ipc_c, "Could not copy the distortion mesh from the service (%d), computing it",
			         xret);
		}
	}
	if (hmd->distortion.mesh.vertices == NULL) {
		u_distortion_mesh_fill_in_compute(xdev);
	}
	int64_t elapsed_ns = os_monotonic_get_ns() - start_ns;

	hmd->distortion.models |= XRT_DISTORTION_MODEL_MESHUV;
	hmd->distortion.preferred = XRT_DISTORTION_MODEL_MESHUV;

	IPC_INFO(ich->ipc_c, "Distortion mesh %s: %u vertices in %.1f ms", how, hmd->distortion.mesh.vertex_count,
	         (double)elapsed_ns / 1e6);
}

static xrt_result_t
ipc_client_hmd_is_form_factor_available(struct xrt_device *xdev, enum xrt_form_factor form_factor, bool *out_available)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	xrt_result_t xret;

	xret = ipc_call_device_is_form_factor_available( //
	    ich->ipc_c,                                  //
	    ich->device_id,                              //
	    form_factor,                                 //
	    out_available);                              //

	IPC_CHK_ALWAYS_RET(ich->ipc_c, xret, "ipc_call_device_is_form_factor_available");
}

static xrt_result_t
ipc_client_hmd_get_visibility_mask(struct xrt_device *xdev,
                                   enum xrt_visibility_mask_type type,
                                   uint32_t view_index,
                                   struct xrt_visibility_mask **out_mask)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	struct ipc_connection *ipc_c = ich->ipc_c;
	struct xrt_visibility_mask *mask = NULL;
	xrt_result_t xret;

	ipc_client_connection_lock(ipc_c);
	ipc_client_connection_send_lock(ipc_c);

	xret = ipc_send_device_get_visibility_mask_locked(ipc_c, ich->device_id, type, view_index);
	if (xret != XRT_SUCCESS) {
		ipc_client_connection_send_unlock(ipc_c);
		goto err_mask_unlock;
	}
	ipc_client_connection_send_unlock(ipc_c);

	uint32_t mask_size;
	xret = ipc_receive_device_get_visibility_mask_locked(ipc_c, &mask_size);
	IPC_CHK_WITH_GOTO(ipc_c, xret, "ipc_receive_device_get_visibility_mask_locked", err_mask_unlock);

	mask = U_CALLOC_WITH_CAST(struct xrt_visibility_mask, mask_size);
	if (mask == NULL) {
		IPC_ERROR(ich->ipc_c, "failed to allocate xrt_visibility_mask");
		goto err_mask_unlock;
	}

	xret = ipc_receive(&ipc_c->imc, mask, mask_size);
	IPC_CHK_WITH_GOTO(ipc_c, xret, "ipc_receive", err_mask_free);

	*out_mask = mask;
	ipc_client_connection_unlock(ipc_c);

	return XRT_SUCCESS;

err_mask_free:
	free(mask);
err_mask_unlock:
	ipc_client_connection_unlock(ipc_c);
	return XRT_ERROR_IPC_FAILURE;
}

static void
ipc_client_hmd_destroy(struct xrt_device *xdev)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);

	// Remove the variable tracking.
	u_var_remove_root(ich);

	for (uint32_t i = 0; i < XRT_MAX_VIEWS; i++) {
		free(ich->distortion_grid[i]);
		ich->distortion_grid[i] = NULL;
	}

#ifdef XRT_IPC_MACOS_HOSTED_COMPOSITOR
	ipc_client_passthrough_destroy(&ich->passthrough);
	ipc_client_tracking_share_destroy(&ich->tracking_share);
#endif

	// Free and de-init the shared things.
	ipc_client_xdev_fini(ich);

	// Free this device with the helper.
	u_device_free(&ich->base);
}

static xrt_result_t
ipc_client_hmd_get_brightness(struct xrt_device *xdev, float *out_brightness)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	struct ipc_connection *ipc_c = ich->ipc_c;
	xrt_result_t xret;

	ipc_client_connection_lock(ipc_c);

	xret = ipc_call_device_get_brightness(ipc_c, ich->device_id, out_brightness);
	IPC_CHK_ONLY_PRINT(ipc_c, xret, "ipc_call_device_get_brightness");

	ipc_client_connection_unlock(ipc_c);

	return xret;
}

static xrt_result_t
ipc_client_hmd_set_brightness(struct xrt_device *xdev, float brightness, bool relative)
{
	ipc_client_hmd_t *ich = ipc_client_hmd(xdev);
	struct ipc_connection *ipc_c = ich->ipc_c;
	xrt_result_t xret;

	ipc_client_connection_lock(ipc_c);

	xret = ipc_call_device_set_brightness(ipc_c, ich->device_id, brightness, relative);
	IPC_CHK_ONLY_PRINT(ipc_c, xret, "ipc_call_device_set_brightness");

	ipc_client_connection_unlock(ipc_c);

	return xret;
}

/*!
 * @public @memberof ipc_client_hmd
 */
struct xrt_device *
ipc_client_hmd_create(struct ipc_connection *ipc_c,
                      struct ipc_client_tracking_origin_manager *ictom,
                      uint32_t device_id)
{
	// Convenience helper.
	struct ipc_shared_memory *ism = ipc_c->ism;

	// Allocate a HMD device.
	enum u_device_alloc_flags flags = (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD);
	ipc_client_hmd_t *ich = U_DEVICE_ALLOCATE(ipc_client_hmd_t, flags, 0, 0);

	// Fills in almost everything a regular device needs.
	xrt_result_t xret = ipc_client_xdev_init(ich, ipc_c, ictom, device_id, ipc_client_hmd_destroy);
	if (xret != XRT_SUCCESS) {
		IPC_ERROR(ipc_c, "Failed to initialize IPC client HMD: %d", xret);
		u_device_free(&ich->base);
		return NULL;
	}

	// Fill in needed HMD functions.
	ich->base.get_view_poses = ipc_client_hmd_get_view_poses;
	ich->base.compute_distortion = ipc_client_hmd_compute_distortion;
	ich->base.is_form_factor_available = ipc_client_hmd_is_form_factor_available;
	ich->base.get_visibility_mask = ipc_client_hmd_get_visibility_mask;
	ich->base.get_brightness = ipc_client_hmd_get_brightness;
	ich->base.set_brightness = ipc_client_hmd_set_brightness;

	// Setup blend-modes.
	ich->base.hmd->blend_mode_count = ipc_c->ism->hmd.blend_mode_count;
	for (int i = 0; i < XRT_MAX_DEVICE_BLEND_MODES; i++) {
		ich->base.hmd->blend_modes[i] = ipc_c->ism->hmd.blend_modes[i];
	}

	// Setup the views.
	ich->base.hmd->view_count = ism->hmd.view_count;
	for (uint32_t i = 0; i < ich->base.hmd->view_count; ++i) {
		ich->base.hmd->views[i].display.w_pixels = ipc_c->ism->hmd.views[i].display.w_pixels;
		ich->base.hmd->views[i].display.h_pixels = ipc_c->ism->hmd.views[i].display.h_pixels;
	}

	// Distortion information, fills in xdev->compute_distortion().
	u_distortion_mesh_set_none(&ich->base);

	// Setup variable tracker.
	u_var_add_root(ich, ich->base.str, true);
	u_var_add_ro_u32(ich, &ich->device_id, "device_id");

	return &ich->base;
}
