// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Automatic floor calibration of the managed STAGE from eye height.
 * @ingroup aux_util
 */

#include "xrt/xrt_device.h"
#include "xrt/xrt_space.h"

#include "math/m_api.h"
#include "math/m_vec3.h"
#include "util/u_logging.h"

#include "util/u_floor_calibration.h"

#include <math.h>
#include <string.h>


static void
restart_window(struct u_floor_calibration *fc, const struct u_floor_calibration_sample *sample)
{
	fc->steady = true;
	fc->steady_since_ns = sample->timestamp_ns;
	fc->anchor = sample->head.position;
	fc->height_sum = 0;
	fc->height_count = 0;
}

static bool
head_is_level(const struct xrt_pose *head)
{
	struct xrt_vec3 forward = {0, 0, -1};
	struct xrt_vec3 rotated;
	math_quat_rotate_vec3(&head->orientation, &forward, &rotated);
	const float max = sinf(U_FLOOR_CALIBRATION_MAX_PITCH_DEG * (float)M_PI / 180.0f);
	return fabsf(rotated.y) <= max;
}

void
u_floor_calibration_init(struct u_floor_calibration *fc, float eye_height_m)
{
	memset(fc, 0, sizeof(*fc));
	fc->eye_height_m = eye_height_m;
}

bool
u_floor_calibration_push(struct u_floor_calibration *fc,
                         const struct u_floor_calibration_sample *sample,
                         float *out_floor_y)
{
	if (fc->done) {
		return false;
	}

	if (!sample->worn || !sample->tracked || !head_is_level(&sample->head)) {
		fc->steady = false;
		return false;
	}

	struct xrt_vec3 moved = m_vec3_sub(sample->head.position, fc->anchor);
	if (!fc->steady || m_vec3_len(moved) > U_FLOOR_CALIBRATION_MAX_MOTION_M) {
		restart_window(fc, sample);
	}

	fc->height_sum += sample->head.position.y;
	fc->height_count++;

	if (sample->timestamp_ns - fc->steady_since_ns < U_FLOOR_CALIBRATION_STEADY_NS) {
		return false;
	}

	fc->done = true;
	*out_floor_y = (float)(fc->height_sum / fc->height_count) - fc->eye_height_m;
	return true;
}

static bool
read_worn(struct xrt_device *head)
{
	if (!head->supported.presence) {
		return true; // Cannot tell; rely on tracked, level and steady.
	}

	if (xrt_device_update_inputs(head) != XRT_SUCCESS) {
		return false;
	}

	for (size_t i = 0; i < head->input_count; i++) {
		if (head->inputs[i].name == XRT_INPUT_GENERIC_HEAD_DETECT) {
			return head->inputs[i].value.boolean;
		}
	}

	return true;
}

bool
u_floor_calibration_poll(struct u_floor_calibration *fc,
                         struct xrt_space_overseer *xso,
                         struct xrt_device *head,
                         int64_t now_ns)
{
	if (fc->done) {
		return true;
	}

	struct u_floor_calibration_sample sample = {.timestamp_ns = now_ns};
	sample.worn = read_worn(head);

	// Where the head's poses are reported, then the head pose within that.
	struct xrt_pose identity = XRT_POSE_IDENTITY;
	struct xrt_space_relation origin = XRT_SPACE_RELATION_ZERO;
	struct xrt_space_relation pose = XRT_SPACE_RELATION_ZERO;
	xrt_result_t xret = xrt_space_overseer_locate_device(xso, xso->semantic.root, &identity, now_ns, head, &origin);
	if (xret == XRT_SUCCESS) {
		xret = xrt_device_get_tracked_pose(head, XRT_INPUT_GENERIC_HEAD_POSE, now_ns, &pose);
	}

	const enum xrt_space_relation_flags tracked =
	    XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT;
	sample.tracked = xret == XRT_SUCCESS && (origin.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0 &&
	                 (pose.relation_flags & tracked) == tracked;
	if (sample.tracked) {
		math_pose_transform(&origin.pose, &pose.pose, &sample.head);
	}

	float floor_y;
	if (!u_floor_calibration_push(fc, &sample, &floor_y)) {
		return false;
	}

	struct xrt_pose stage;
	xret = xrt_space_overseer_get_reference_space_offset(xso, XRT_SPACE_REFERENCE_TYPE_STAGE, &stage);
	if (xret != XRT_SUCCESS) {
		U_LOG_W("Floor calibration: cannot read the STAGE offset (%d); skipped.", xret);
		return true;
	}
	if (stage.position.y != 0.0f) {
		U_LOG_I("Floor calibration: STAGE already calibrated (y %.3f m); automatic calibration skipped.",
		        stage.position.y);
		return true;
	}

	// Only the floor height changes; the stage keeps its position and heading.
	stage.position.y = floor_y;
	xret = xrt_space_overseer_set_reference_space_offset(xso, XRT_SPACE_REFERENCE_TYPE_STAGE, &stage);
	if (xret != XRT_SUCCESS) {
		U_LOG_W("Floor calibration: cannot set the STAGE offset (%d); a driver-provided STAGE cannot be moved.",
		        xret);
		return true;
	}

	U_LOG_I("Floor calibration: STAGE floor set to y %.3f m, %.2f m below the steady head.", floor_y,
	        fc->eye_height_m);
	return true;
}
