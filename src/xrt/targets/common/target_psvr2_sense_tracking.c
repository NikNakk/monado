// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PS Sense optical (6DoF) tracking with the PS VR2's cameras.
 * @author Nick Kennedy
 * @ingroup xrt_iface
 */

#include "target_psvr2_sense_tracking.h"

#include "xrt/xrt_config_drivers.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_frame.h"

#include "constellation/t_rift_blobwatch.h"
#include "math/m_api.h"
#include "util/u_debug.h"
#include "util/u_file.h"
#include "util/u_json.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_sink.h"

#include "psvr2/psvr2_interface.h"
#ifdef XRT_BUILD_DRIVER_PSSENSE
#include "pssense/pssense_interface.h"
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>


DEBUG_GET_ONCE_BOOL_OPTION(psvr2_sense_6dof, "PSVR2_SENSE_6DOF", false)
DEBUG_GET_ONCE_OPTION(psvr2_sense_6dof_calibration, "PSVR2_SENSE_6DOF_CALIBRATION", NULL)
DEBUG_GET_ONCE_NUM_OPTION(psvr2_sense_blob_pixel_threshold, "PSVR2_BLOB_PIXEL_THRESHOLD", 0x50)
DEBUG_GET_ONCE_NUM_OPTION(psvr2_sense_blob_required_threshold, "PSVR2_BLOB_REQUIRED_THRESHOLD", 0xb4)
DEBUG_GET_ONCE_NUM_OPTION(psvr2_sense_blob_max_width, "PSVR2_BLOB_MAX_WIDTH", 50)

#define SENSE_TRACKING_CAMERAS 4


/*
 *
 * Head tracking origin.
 *
 */

static void
head_tracking_origin_get(struct t_constellation_tracker_tracking_source *source,
                         int64_t when_ns,
                         struct xrt_space_relation *out_relation)
{
	struct psvr2_head_tracking_origin *origin = (struct psvr2_head_tracking_origin *)source;
	struct xrt_space_relation head = XRT_SPACE_RELATION_ZERO;
	// Taken before the query, so a SLAM pose arriving meanwhile cannot make the recorded source look newer.
	struct psvr2_slam_timing slam = {0};
	bool have_slam = psvr2_get_slam_timing(origin->head, &slam) && slam.valid;
	if (xrt_device_get_tracked_pose(origin->head, XRT_INPUT_GENERIC_HEAD_POSE, when_ns, &head) != XRT_SUCCESS) {
		*out_relation = (struct xrt_space_relation)XRT_SPACE_RELATION_ZERO;
		return;
	}
	*out_relation = head;
	math_pose_transform(&head.pose, &origin->head_from_camera0, &out_relation->pose);

	if (origin->tracker != NULL) {
		int64_t source_ns = have_slam ? slam.slam_monotonic_ns : 0;
		uint32_t flags = have_slam && when_ns <= source_ns ? T_CONSTELLATION_HEAD_POSE_INTERPOLATED : 0;
		t_constellation_tracker_record_head_pose(origin->tracker, when_ns, &head, source_ns, flags);
	}
}

void
psvr2_head_tracking_origin_init(struct psvr2_head_tracking_origin *origin, struct xrt_device *head)
{
	U_ZERO(origin);
	origin->base.get_tracked_pose = head_tracking_origin_get;
	origin->head = head;
	origin->head_from_camera0 = (struct xrt_pose)XRT_POSE_IDENTITY;
}


/*
 *
 * Calibration.
 *
 */

static bool
get_number(const cJSON *object, const char *name, double *out_value)
{
	return object != NULL && u_json_get_double(u_json_get(object, name), out_value) && isfinite(*out_value);
}

static bool
load_camera(const cJSON *json, struct t_constellation_tracker_camera *out_camera)
{
	const cJSON *calibration = u_json_get(json, "calibration");
	const cJSON *resolution = u_json_get(calibration, "resolution");
	const cJSON *intrinsics = u_json_get(calibration, "intrinsics");
	const cJSON *distortion = u_json_get(calibration, "distortion");
	const cJSON *pose = u_json_get(json, "pose_in_tracking_origin_xrt");
	char model[64] = {0};
	int width = 0;
	int height = 0;
	double fx = 0.0, fy = 0.0, cx = 0.0, cy = 0.0;
	double k[4] = {0};

	bool good = u_json_get_string_into_array(u_json_get(calibration, "model"), model, sizeof(model));
	good = good && strcmp(model, "fisheye_equidistant4") == 0;
	good = good && u_json_get_int(u_json_get(resolution, "width"), &width);
	good = good && u_json_get_int(u_json_get(resolution, "height"), &height);
	good = good && width == 512 && height == 508;
	good = good && get_number(intrinsics, "fx", &fx) && get_number(intrinsics, "fy", &fy);
	good = good && get_number(intrinsics, "cx", &cx) && get_number(intrinsics, "cy", &cy);
	good = good && get_number(distortion, "k1", &k[0]) && get_number(distortion, "k2", &k[1]);
	good = good && get_number(distortion, "k3", &k[2]) && get_number(distortion, "k4", &k[3]);
	good = good && fx > 0.0 && fy > 0.0 && u_json_get_pose(pose, &out_camera->pose_in_origin) &&
	       math_pose_validate(&out_camera->pose_in_origin);
	if (!good) {
		return false;
	}

	out_camera->calibration.image_size_pixels = (struct xrt_size){(uint32_t)width, (uint32_t)height};
	out_camera->calibration.intrinsics[0][0] = fx;
	out_camera->calibration.intrinsics[0][2] = cx;
	out_camera->calibration.intrinsics[1][1] = fy;
	out_camera->calibration.intrinsics[1][2] = cy;
	out_camera->calibration.intrinsics[2][2] = 1.0;
	out_camera->calibration.kb4 = (struct t_camera_calibration_kb4_params){k[0], k[1], k[2], k[3]};
	out_camera->calibration.distortion_model = T_DISTORTION_FISHEYE_KB4;
	out_camera->has_concrete_pose = true;
	return true;
}

bool
psvr2_constellation_load_calibration(const char *path,
                                     struct t_constellation_tracker_params *out_params,
                                     struct xrt_pose *out_head_from_camera0,
                                     bool *out_have_head_from_camera0)
{
	char *contents = u_file_read_content_from_path(path, NULL);
	if (contents == NULL) {
		U_LOG_E("Could not read PS VR2 constellation calibration '%s'.", path);
		return false;
	}
	cJSON *root = cJSON_Parse(contents);
	free(contents);
	if (root == NULL) {
		U_LOG_E("Could not parse PS VR2 constellation calibration '%s'.", path);
		return false;
	}

	char format[64] = {0};
	const cJSON *cameras = u_json_get(root, "cameras");
	bool good = u_json_get_string_into_array(u_json_get(root, "format"), format, sizeof(format));
	good = good && strcmp(format, "psvr2-mode4-constellation-calibration-v1") == 0;
	good = good && cJSON_IsArray(cameras) && cJSON_GetArraySize(cameras) == 4;
	bool seen[4] = {false};
	for (int index = 0; good && index < 4; index++) {
		const cJSON *camera = cJSON_GetArrayItem(cameras, index);
		int camera_index = -1;
		good = u_json_get_int(u_json_get(camera, "camera"), &camera_index);
		good = good && camera_index >= 0 && camera_index < 4 && !seen[camera_index];
		if (good) {
			seen[camera_index] = true;
			good = load_camera(camera, &out_params->mosaics[0].cameras[camera_index]);
		}
	}
	*out_head_from_camera0 = (struct xrt_pose)XRT_POSE_IDENTITY;
	*out_have_head_from_camera0 =
	    good && u_json_get_pose(u_json_get(root, "head_from_camera0_xrt"), out_head_from_camera0);
	if (*out_have_head_from_camera0 && !math_pose_validate(out_head_from_camera0)) {
		good = false;
	}
	if (good && cJSON_IsFalse(u_json_get(root, "runtime_usable"))) {
		U_LOG_W("Using experimental PS Sense calibration marked runtime_usable=false: '%s'.", path);
	}
	cJSON_Delete(root);
	if (!good) {
		U_LOG_E("'%s' is not a valid four-camera PS VR2 mode-4 constellation calibration.", path);
		return false;
	}
	out_params->num_mosaics = 1;
	out_params->mosaics[0].num_cameras = 4;
	return true;
}


/*
 *
 * Runtime pipeline.
 *
 */

/*!
 * The settings the recorded sessions of 3-4 Oct ran with (scripts/psvr2_sense_session.sh plus the exports in
 * doc/macos-pssense-mr2940-frontend-evaluation.md), as defaults for the opt-in runtime. Each can still be overridden.
 */
static const char *const sense_tracking_defaults[][2] = {
    {"PSVR2_CAMERA_STREAMS", "1"},
    {"PSVR2_CAMERA_MODE", "4"},
    {"PSVR2_ROBUST_CLOCK", "1"},
    {"PSVR2_ROBUST_CLOCK_MAX_PPM", "200"},
    {"CONSTELLATION_TRACKER_JOINT", "1"},
    {"PSSENSE_FILTER", "1"},
    {"PSSENSE_FUTURE_LED_SCHEDULE", "1"},
    {"PSSENSE_GYRO_BIAS_AUTO", "1"},
    {"PSSENSE_CLOCK_OFFSET_SNAP_US", "250"},
    {"PSSENSE_LED_CORRECTION", "1"},
    {"PSSENSE_LEDS_OFF_ON_EXIT", "1"},
    {"PSSENSE_LED_BOOTSTRAP", "1"},
    {"PSSENSE_LED_BOOTSTRAP_FIRST", "R"},
    {"PSSENSE_LED_BOOTSTRAP_HINT_US", "16350"},
    {"PSSENSE_LED_BOOTSTRAP_KEEP_LOCK", "1"},
    {"PSSENSE_LED_BOOTSTRAP_LED_BLOBS", "1"},
    {"PSSENSE_LED_BOOTSTRAP_STRICT", "1"},
    {"PSSENSE_LED_BOOTSTRAP_TRACK", "1"},
    {"PSSENSE_LED_BOOTSTRAP_TRACK_COVERAGE", "1"},
    {"PSSENSE_LED_BOOTSTRAP_WIDE_PERIOD_ID", "32"},
    // Sony-like LED schedule: no always-lit fault or lockout in any run since 5 Oct.
    {"PSSENSE_CLOCK_STEADY", "1"},
    {"PSSENSE_LED_BOOTSTRAP_LOCK_PERIOD_ID", "32"},
    {"PSSENSE_LED_BROAD_S", "10"},
    {"PSSENSE_LED_LATCH_INTERVAL_MS", "1000"},
    {"PSSENSE_LED_NOMINAL_CYCLE", "1"},
    {"PSVR2_BLOB_PIXEL_THRESHOLD", "50"},
    {"PSVR2_BLOB_REQUIRED_THRESHOLD", "120"},
    {"CONSTELLATION_TRACKER_ORIENTED_BOOTSTRAP", "1"},
};

struct psvr2_sense_tracking
{
	struct xrt_frame_context xfctx;
	struct psvr2_head_tracking_origin origin;
	struct t_constellation_tracker *tracker;
	struct xrt_device *head;
	struct xrt_device *controllers[2];
	bool owns_camera_sinks;
	bool owns_led_detector_sinks;
};

bool
psvr2_sense_tracking_requested(void)
{
	if (!debug_get_bool_option_psvr2_sense_6dof()) {
		return false;
	}
	for (size_t i = 0; i < ARRAY_SIZE(sense_tracking_defaults); i++) {
		setenv(sense_tracking_defaults[i][0], sense_tracking_defaults[i][1], 0);
	}
	return true;
}

#ifdef XRT_BUILD_DRIVER_PSSENSE

static void
sense_tracking_stop(void *data)
{
	struct psvr2_sense_tracking *st = data;

	// No more frames into the blob pipeline; returns once no push is still running.
	if (st->owns_camera_sinks) {
		(void)psvr2_set_camera_frame_sinks(st->head, NULL);
	}
	if (st->owns_led_detector_sinks) {
		(void)psvr2_set_led_detector_blob_sinks(st->head, NULL);
	}
	for (size_t i = 0; i < 2; i++) {
		if (st->controllers[i] != NULL) {
			pssense_remove_from_constellation_tracker(st->controllers[i]);
		}
	}
	xrt_frame_context_destroy_nodes(&st->xfctx);
	free(st);
	U_LOG_I("PS Sense optical tracking stopped.");
}

bool
psvr2_sense_tracking_start(struct xrt_device *head, struct xrt_device *left, struct xrt_device *right)
{
	const char *calibration = debug_get_option_psvr2_sense_6dof_calibration();
	if (calibration == NULL) {
		U_LOG_E(
		    "PSVR2_SENSE_6DOF is set but PSVR2_SENSE_6DOF_CALIBRATION is not: controllers stay "
		    "orientation-only.");
		return false;
	}
	// The headset's own LED detections replace blob detection on the camera images, which then need not stream.
	const bool detector_blobs = psvr2_led_detector_blobs_requested();
	struct psvr2_camera_diagnostics diag = {0};
	if (head == NULL || !psvr2_get_camera_diagnostics(head, &diag) ||
	    (!detector_blobs && (!diag.enabled || diag.configured_mode != 4))) {
		U_LOG_E(
		    "PS Sense optical tracking needs the PS VR2 cameras in mode 4: controllers stay orientation-only.");
		return false;
	}
	if (left == NULL && right == NULL) {
		U_LOG_W("PS Sense optical tracking: no controllers connected.");
		return false;
	}

	struct psvr2_sense_tracking *st = U_TYPED_CALLOC(struct psvr2_sense_tracking);
	if (st == NULL) {
		U_LOG_E("Failed to allocate PS Sense tracking state.");
		return false;
	}
	st->head = head;
	st->controllers[0] = left;
	st->controllers[1] = right;

	struct t_constellation_tracker_params params = {0};
	params.flags = T_CONSTELLATION_TRACKER_FLAGS_ALLOW_JOINT;
	psvr2_head_tracking_origin_init(&st->origin, head);
	bool have_head_from_camera0 = false;
	if (!psvr2_constellation_load_calibration(calibration, &params, &st->origin.head_from_camera0,
	                                          &have_head_from_camera0)) {
		free(st);
		return false;
	}
	if (!have_head_from_camera0) {
		U_LOG_W(
		    "Calibration has no head_from_camera0_xrt: controller positions will be off by the camera's lever "
		    "arm.");
	}
	// Track in the headset's space: the cameras follow the head pose at each exposure.
	params.mosaics[0].tracking_origin = &st->origin.base;

	if (t_constellation_tracker_create(&st->xfctx, &params, &st->tracker) != 0) {
		U_LOG_E("Failed to create the constellation tracker.");
		free(st);
		return false;
	}

	struct t_rift_blobwatch_params blob_params = {
	    .pixel_threshold = (uint8_t)CLAMP(debug_get_num_option_psvr2_sense_blob_pixel_threshold(), 0, UINT8_MAX),
	    .blob_required_threshold =
	        (uint8_t)CLAMP(debug_get_num_option_psvr2_sense_blob_required_threshold(), 0, UINT8_MAX),
	    .max_match_dist = 50.0f,
	    .max_blob_width = (uint16_t)CLAMP(debug_get_num_option_psvr2_sense_blob_max_width(), 1, UINT16_MAX),
	};
	struct xrt_frame_sink *frame_sinks[SENSE_TRACKING_CAMERAS] = {0};
	struct t_blob_sink *blob_sinks[SENSE_TRACKING_CAMERAS] = {0};
	for (size_t i = 0; i < SENSE_TRACKING_CAMERAS; i++) {
		blob_sinks[i] = params.mosaics[0].cameras[i].blob_sink;
		if (detector_blobs) {
			continue;
		}
		struct t_blobwatch *blobwatch = NULL;
		if (t_rift_blobwatch_create(&blob_params, &st->xfctx, params.mosaics[0].cameras[i].blob_sink,
		                            &frame_sinks[i], &blobwatch) != 0 ||
		    !u_sink_simple_queue_create(&st->xfctx, frame_sinks[i], &frame_sinks[i])) {
			U_LOG_E("Failed to create the blob pipeline for camera %zu.", i);
			xrt_frame_context_destroy_nodes(&st->xfctx);
			free(st);
			return false;
		}
	}

	size_t attached = 0;
	for (size_t i = 0; i < 2; i++) {
		struct xrt_device *controller = st->controllers[i];
		if (controller == NULL) {
			continue;
		}
		if (pssense_add_to_constellation_tracker(controller, st->tracker) != 0) {
			U_LOG_E("Failed to attach the %s Sense controller to the tracker.", i == 0 ? "left" : "right");
			st->controllers[i] = NULL;
			continue;
		}
		/*
		 * Its poses are in the headset's tracking space, so it shares the headset's tracking origin: the space
		 * overseer must not treat the controllers as a separate tracking system.
		 */
		controller->tracking_origin = head->tracking_origin;
		controller->supported.position_tracking = true;
		attached++;
	}
	if (attached == 0) {
		xrt_frame_context_destroy_nodes(&st->xfctx);
		free(st);
		return false;
	}

	st->origin.tracker = st->tracker;
	if (!psvr2_set_teardown_hook(head, sense_tracking_stop, st)) {
		U_LOG_E("Failed to register PS Sense tracker lifetime hook.");
		sense_tracking_stop(st);
		return false;
	}
	if (detector_blobs ? !psvr2_set_led_detector_blob_sinks(head, blob_sinks)
	                   : !psvr2_set_camera_frame_sinks(head, frame_sinks)) {
		U_LOG_E("Failed to attach the PS Sense tracker to the PS VR2.");
		(void)psvr2_set_teardown_hook(head, NULL, NULL);
		sense_tracking_stop(st);
		return false;
	}

	st->owns_camera_sinks = !detector_blobs;
	st->owns_led_detector_sinks = detector_blobs;
	if (detector_blobs) {
		U_LOG_W("PS Sense optical tracking uses the headset's own LED detections (PSVR2_LED_DETECTOR_BLOBS).");
	}
	U_LOG_W("EXPERIMENTAL: PS Sense optical (6DoF) tracking started for %zu controller(s), calibration '%s'.",
	        attached, calibration);
	return true;
}

#else

bool
psvr2_sense_tracking_start(struct xrt_device *head, struct xrt_device *left, struct xrt_device *right)
{
	(void)head;
	(void)left;
	(void)right;
	return false;
}

#endif
