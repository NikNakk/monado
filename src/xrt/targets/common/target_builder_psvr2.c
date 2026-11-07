// Copyright 2025, Beyley Cardellio
// Copyright 2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PS VR2 prober code.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup xrt_iface
 */

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_prober.h"
#include "xrt/xrt_system.h"

#include "util/u_debug.h"
#include "util/u_misc.h"
#include "util/u_logging.h"
#include "util/u_trace_marker.h"
#include "util/u_builder_search.h"

#include "target_builder_helpers.h"

#include "psvr2/psvr2_interface.h"

#ifdef XRT_BUILD_DRIVER_REMOTE
#include "remote/r_interface.h"
#endif

#ifdef XRT_BUILD_DRIVER_PSSENSE
#include "pssense/pssense_interface.h"
#include "target_psvr2_sense_tracking.h"
#endif

/*
 *
 * Internal structures
 *
 */

struct psvr2_builder
{
	struct t_builder base;

	enum u_logging_level log_level;
};

static struct psvr2_builder *
psvr2_builder(struct xrt_builder *xb)
{
	return (struct psvr2_builder *)xb;
}

/*
 *
 * Misc stuff.
 *
 */

DEBUG_GET_ONCE_LOG_OPTION(psvr2_log, "PSVR2_LOG", U_LOGGING_WARN)

#define PSVR2_ERROR(p, ...) U_LOG_IFL_E(p->log_level, __VA_ARGS__)
#define PSVR2_DEBUG(p, ...) U_LOG_IFL_D(p->log_level, __VA_ARGS__)

/*!
 * Replay on the real headset display: when set, the remote driver listens on this port (as with
 * P_OVERRIDE_ACTIVE_CONFIG=remote) and supplies the head pose and both controllers, while the PS VR2 keeps its display,
 * distortion and refresh. Sense controllers are not opened. Used for repeatable pacing comparisons.
 */
DEBUG_GET_ONCE_NUM_OPTION(psvr2_replay_remote_port, "PSVR2_REPLAY_REMOTE_PORT", 0)

#ifdef XRT_BUILD_DRIVER_REMOTE
static void
psvr2_replay_teardown(void *data)
{
	struct xrt_system_devices *remote = data;
	xrt_system_devices_destroy(&remote);
}

/*!
 * Feed @p head_xdev's pose from a remote hub and put the hub's controllers into @p xsysd. The hub keeps its own HMD
 * (pose source only) and is destroyed by the headset's teardown hook; the controllers move to @p xsysd and do not use
 * the hub when destroyed.
 */
static bool
psvr2_replay_open(struct psvr2_builder *pb,
                  uint16_t port,
                  struct xrt_device *head_xdev,
                  struct xrt_system_devices *xsysd,
                  struct xrt_device **out_left,
                  struct xrt_device **out_right)
{
	struct xrt_system_devices *remote = NULL;
	struct xrt_hmd_parts *parts = head_xdev->hmd;
	uint32_t w = parts != NULL && parts->views[0].display.w_pixels > 0 ? parts->views[0].display.w_pixels : 2000;
	uint32_t h = parts != NULL && parts->views[0].display.h_pixels > 0 ? parts->views[0].display.h_pixels : 2040;
	if (r_create_devices(port, 2, w, h, 0.13f, 0.07f, &remote) != XRT_SUCCESS || remote == NULL) {
		PSVR2_ERROR(pb, "PSVR2_REPLAY_REMOTE_PORT: could not start the remote hub on port %u", port);
		return false;
	}

	struct xrt_device *remote_head = remote->static_roles.head;
	struct xrt_device *left = remote->static_roles.hand_tracking.conforming.left;
	struct xrt_device *right = remote->static_roles.hand_tracking.conforming.right;
	if (remote_head == NULL || left == NULL || right == NULL ||
	    xsysd->static_xdev_count + 2 > ARRAY_SIZE(xsysd->static_xdevs) ||
	    !psvr2_set_teardown_hook(head_xdev, psvr2_replay_teardown, remote)) {
		PSVR2_ERROR(pb, "PSVR2_REPLAY_REMOTE_PORT: could not attach the remote hub");
		xrt_system_devices_destroy(&remote);
		return false;
	}

	// Hand the controllers over; the hub must not destroy them.
	for (uint32_t i = 0; i < remote->static_xdev_count; i++) {
		if (remote->static_xdevs[i] == left || remote->static_xdevs[i] == right) {
			remote->static_xdevs[i] = NULL;
		}
	}

	// One tracking origin, so replayed head and controller poses stay in the same space.
	left->tracking_origin = head_xdev->tracking_origin;
	right->tracking_origin = head_xdev->tracking_origin;
	xsysd->static_xdevs[xsysd->static_xdev_count++] = left;
	xsysd->static_xdevs[xsysd->static_xdev_count++] = right;

	(void)psvr2_set_replay_head(head_xdev, remote_head);
	U_LOG_W("PSVR2_REPLAY_REMOTE_PORT=%u: PS VR2 display with replayed head and controller poses; Sense and gaze off",
	        port);

	*out_left = left;
	*out_right = right;
	return true;
}
#endif

static const char *driver_list[] = {
    "psvr2",
    "pssense",
};


/*
 *
 * Member functions.
 *
 */

static xrt_result_t
psvr2_estimate_system(struct xrt_builder *xb,
                      cJSON *config,
                      struct xrt_prober *xp,
                      struct xrt_builder_estimate *estimate)
{
	struct psvr2_builder *pb = psvr2_builder(xb);

	struct xrt_prober_device **xpdevs = NULL;
	size_t xpdev_count = 0;
	xrt_result_t xret = XRT_SUCCESS;

	U_ZERO(estimate);

	xret = xrt_prober_lock_list(xp, &xpdevs, &xpdev_count);
	U_LOG_CHK_AND_RET(pb->log_level, xret, "xrt_prober_lock_list");

	struct xrt_prober_device *dev =
	    u_builder_find_prober_device(xpdevs, xpdev_count, PSVR2_VID, PSVR2_PID, XRT_BUS_TYPE_USB);
	if (dev != NULL) {
		estimate->certain.head = true;
		estimate->certain.dof6 = true;
	}

#ifdef XRT_BUILD_DRIVER_PSSENSE
	struct xrt_prober_device *dev_controller_left =
	    u_builder_find_prober_device(xpdevs, xpdev_count, PSSENSE_VID, PSSENSE_PID_LEFT, XRT_BUS_TYPE_ANY);
	if (dev_controller_left != NULL) {
		estimate->certain.left = true;
	}

	struct xrt_prober_device *dev_controller_right =
	    u_builder_find_prober_device(xpdevs, xpdev_count, PSSENSE_VID, PSSENSE_PID_RIGHT, XRT_BUS_TYPE_ANY);
	if (dev_controller_right != NULL) {
		estimate->certain.right = true;
	}
#endif

	PSVR2_DEBUG(pb, "PSVR2 builder estimate: head %d, left %d, right %d", estimate->certain.head,
	            estimate->certain.left, estimate->certain.right);

	xret = xrt_prober_unlock_list(xp, &xpdevs);
	U_LOG_CHK_AND_RET(pb->log_level, xret, "xrt_prober_unlock_list");

	return xret;
}

static xrt_result_t
psvr2_open_system_impl(struct xrt_builder *xb,
                       cJSON *config,
                       struct xrt_prober *xp,
                       struct xrt_tracking_origin *origin,
                       struct xrt_system_devices *xsysd,
                       struct xrt_frame_context *xfctx,
                       struct t_builder_options *tbo)
{
	struct xrt_prober_device **xpdevs = NULL;
	size_t xpdev_count = 0;
	xrt_result_t xret = XRT_SUCCESS;

	DRV_TRACE_MARKER();

	xret = xrt_prober_lock_list(xp, &xpdevs, &xpdev_count);
	if (xret != XRT_SUCCESS) {
		goto unlock_and_fail;
	}

#ifdef XRT_BUILD_DRIVER_PSSENSE
	bool sense_tracking = psvr2_sense_tracking_requested();
#endif
	struct xrt_device *head_xdev = NULL;
	struct xrt_prober_device *head_xpdev =
	    u_builder_find_prober_device(xpdevs, xpdev_count, PSVR2_VID, PSVR2_PID, XRT_BUS_TYPE_USB);
	if (head_xpdev != NULL) {
		head_xdev = psvr2_hmd_create(head_xpdev);
		if (head_xdev == NULL) {
			PSVR2_ERROR(psvr2_builder(xb), "PSVR2 HMD device creation failed");
			goto unlock_and_fail;
		}

		xsysd->static_xdevs[xsysd->static_xdev_count++] = head_xdev;
	}

	tbo->head = head_xdev;
	tbo->eyes = head_xdev;
	if (head_xdev != NULL && head_xdev->supported.face_tracking) {
		tbo->face = head_xdev;
	}

	int64_t replay_port = debug_get_num_option_psvr2_replay_remote_port();
	if (replay_port != 0) {
#ifdef XRT_BUILD_DRIVER_REMOTE
		struct xrt_device *left = NULL;
		struct xrt_device *right = NULL;
		if (replay_port < 1 || replay_port > 65535 || head_xdev == NULL ||
		    !psvr2_replay_open(psvr2_builder(xb), (uint16_t)replay_port, head_xdev, xsysd, &left, &right)) {
			PSVR2_ERROR(psvr2_builder(xb), "PSVR2_REPLAY_REMOTE_PORT=%lld needs a PS VR2 and a free port",
			            (long long)replay_port);
			goto unlock_and_fail;
		}
		tbo->left = left;
		tbo->right = right;
		xret = xrt_prober_unlock_list(xp, &xpdevs);
		return xret == XRT_SUCCESS ? XRT_SUCCESS : XRT_ERROR_DEVICE_CREATION_FAILED;
#else
		PSVR2_ERROR(psvr2_builder(xb), "PSVR2_REPLAY_REMOTE_PORT needs the remote driver");
		goto unlock_and_fail;
#endif
	}

#ifdef XRT_BUILD_DRIVER_PSSENSE
	struct xrt_device *left_xdev = NULL;
	struct xrt_prober_device *left_xpdev =
	    u_builder_find_prober_device(xpdevs, xpdev_count, PSSENSE_VID, PSSENSE_PID_LEFT, XRT_BUS_TYPE_ANY);
	// PSSENSE_RECONNECT: a controller not connected yet is created anyway, and attaches when it connects.
	if (left_xpdev != NULL || pssense_reconnect_requested()) {
		struct t_timing_event_sink *timing_sink = NULL;
		left_xdev = left_xpdev != NULL ? pssense_create(xp, left_xpdev, xfctx, &timing_sink)
		                               : pssense_create_disconnected(PSSENSE_PID_LEFT, xfctx, &timing_sink);
		if (left_xdev == NULL) {
			PSVR2_ERROR(psvr2_builder(xb), "PS Sense left controller device creation failed");
		} else {
			xsysd->static_xdevs[xsysd->static_xdev_count++] = left_xdev;
			struct t_timing_event_source *timing_source = psvr2_get_timing_event_source(head_xdev);
			if (timing_source != NULL && timing_sink != NULL &&
			    t_timing_event_source_add_sink(timing_source, timing_sink) != 0) {
				PSVR2_ERROR(psvr2_builder(xb), "Failed to connect left Sense camera timing");
			}
		}
	}

	struct xrt_device *right_xdev = NULL;
	struct xrt_prober_device *right_xpdev =
	    u_builder_find_prober_device(xpdevs, xpdev_count, PSSENSE_VID, PSSENSE_PID_RIGHT, XRT_BUS_TYPE_ANY);
	// PSSENSE_RECONNECT: a controller not connected yet is created anyway, and attaches when it connects.
	if (right_xpdev != NULL || pssense_reconnect_requested()) {
		struct t_timing_event_sink *timing_sink = NULL;
		right_xdev = right_xpdev != NULL ? pssense_create(xp, right_xpdev, xfctx, &timing_sink)
		                                 : pssense_create_disconnected(PSSENSE_PID_RIGHT, xfctx, &timing_sink);
		if (right_xdev == NULL) {
			PSVR2_ERROR(psvr2_builder(xb), "PS Sense right controller device creation failed");
		} else {
			xsysd->static_xdevs[xsysd->static_xdev_count++] = right_xdev;
			struct t_timing_event_source *timing_source = psvr2_get_timing_event_source(head_xdev);
			if (timing_source != NULL && timing_sink != NULL &&
			    t_timing_event_source_add_sink(timing_source, timing_sink) != 0) {
				PSVR2_ERROR(psvr2_builder(xb), "Failed to connect right Sense camera timing");
			}
		}
	}

	if (left_xdev != NULL) {
		pssense_set_head_device(left_xdev, head_xdev);
	}
	if (right_xdev != NULL) {
		pssense_set_head_device(right_xdev, head_xdev);
	}

	tbo->left = left_xdev;
	tbo->right = right_xdev;
	if (sense_tracking && head_xdev != NULL) {
		(void)psvr2_sense_tracking_start(head_xdev, left_xdev, right_xdev);
	}
#endif

	xret = xrt_prober_unlock_list(xp, &xpdevs);
	if (xret != XRT_SUCCESS) {
		goto fail;
	}

	return XRT_SUCCESS;


unlock_and_fail:
	xret = xrt_prober_unlock_list(xp, &xpdevs);
	if (xret != XRT_SUCCESS) {
		return xret;
	}

	/* Fallthrough */
fail:
	return XRT_ERROR_DEVICE_CREATION_FAILED;
}

static void
psvr2_destroy(struct xrt_builder *xb)
{
	free(xb);
}


/*
 *
 * 'Exported' functions.
 *
 */

struct xrt_builder *
t_builder_psvr2_create(void)
{
	struct psvr2_builder *pb = U_TYPED_CALLOC(struct psvr2_builder);

	pb->log_level = debug_get_log_option_psvr2_log();

	// xrt_builder fields.
	pb->base.base.estimate_system = psvr2_estimate_system;
	pb->base.base.open_system = t_builder_open_system_static_roles;
	pb->base.base.destroy = psvr2_destroy;
	pb->base.base.identifier = "psvr2";
	pb->base.base.name = "PlayStation VR 2";
	pb->base.base.driver_identifiers = driver_list;
	pb->base.base.driver_identifier_count = ARRAY_SIZE(driver_list);

	// t_builder fields.
	pb->base.open_system_static_roles = psvr2_open_system_impl;

	return &pb->base.base;
}
