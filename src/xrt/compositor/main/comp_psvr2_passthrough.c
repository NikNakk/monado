// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "main/comp_psvr2_passthrough.h"
#include "main/comp_compositor.h"
#include "util/u_debug.h"
#include "util/u_file.h"
#include "util/u_passthrough_calibration.h"
#include "util/u_frame_share.h"
#include "util/u_time.h"
#include "math/m_api.h"
#include "os/os_time.h"
#include <pthread.h>
#include <stdatomic.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PSVR2_PASSTHROUGH_WIDTH 1024
#define PSVR2_PASSTHROUGH_HEIGHT 1016
#define PSVR2_PASSTHROUGH_MAP_SIZE 512
DEBUG_GET_ONCE_BOOL_OPTION(macos_passthrough_rotation, "XRT_MACOS_PASSTHROUGH_ROTATION", true)
DEBUG_GET_ONCE_NUM_OPTION(macos_passthrough_fov_deg, "XRT_MACOS_PASSTHROUGH_FOV_DEG", 150)
DEBUG_GET_ONCE_NUM_OPTION(macos_passthrough_convergence_milli, "XRT_MACOS_PASSTHROUGH_CONVERGENCE_MILLI", 100)
DEBUG_GET_ONCE_NUM_OPTION(macos_passthrough_brightness_percent, "XRT_MACOS_PASSTHROUGH_BRIGHTNESS_PERCENT", 160)

struct psvr2_passthrough_sink
{
	struct xrt_frame_sink base;
	struct comp_psvr2_passthrough *pt;
	uint32_t eye;
};
struct comp_psvr2_passthrough
{
	struct comp_compositor *c;
	bool is_service;
	float *maps[2];
	struct psvr2_passthrough_sink passthrough_sinks[2];
	struct xrt_frame *passthrough_frames[2];
	int64_t passthrough_uploaded_timestamp[2];
	struct u_passthrough_calibration passthrough_calibration;
	bool passthrough_rotation_enabled;
	struct xrt_quat passthrough_capture_orientation[2];
	bool passthrough_capture_valid[2];
	bool passthrough_rotation_active;
	bool passthrough_rotation_status_logged;
	pthread_mutex_t passthrough_mutex;
	atomic_bool passthrough_shutdown;
	bool passthrough_sinks_attached;
};

static void
psvr2_passthrough_sink_push_frame(struct xrt_frame_sink *sink, struct xrt_frame *frame)
{
	struct psvr2_passthrough_sink *pts = container_of(sink, struct psvr2_passthrough_sink, base);
	struct comp_psvr2_passthrough *pt = pts->pt;

	if (atomic_load_explicit(&pt->passthrough_shutdown, memory_order_acquire)) {
		return;
	}
	if (frame == NULL || frame->format != XRT_FORMAT_BC4 || frame->width != PSVR2_PASSTHROUGH_WIDTH ||
	    frame->height != PSVR2_PASSTHROUGH_HEIGHT) {
		return;
	}

	pthread_mutex_lock(&pt->passthrough_mutex);
	if (!atomic_load_explicit(&pt->passthrough_shutdown, memory_order_acquire)) {
		xrt_frame_reference(&pt->passthrough_frames[pts->eye], frame);
	}
	pthread_mutex_unlock(&pt->passthrough_mutex);

	// In the service: also pass the frame to clients compositing in-process.
	u_passthrough_share_push(pts->eye, frame);
}

static bool
psvr2_passthrough_create_uv_maps(struct comp_psvr2_passthrough *pt)
{
	struct xrt_device *xdev = pt->c->xdev;
	if (xdev == NULL || xdev->compute_distortion == NULL) {
		return false;
	}

	struct u_passthrough_calibration calibration = {0};
	bool calibrated = false;
	const char *calibration_path = getenv("XRT_MACOS_PASSTHROUGH_CALIBRATION");
	bool automatic = calibration_path == NULL;
	char default_path[4096] = {0};
	if (automatic && xdev->name == XRT_DEVICE_PSVR2) {
		int size = u_file_get_path_in_config_dir("psvr2/passthrough.json", default_path, sizeof(default_path));
		if (size > 0 && (size_t)size < sizeof(default_path)) {
			calibration_path = default_path;
		}
	}
	if (calibration_path != NULL && calibration_path[0] != '\0') {
		char *json = u_file_read_content_from_path(calibration_path, NULL);
		calibrated = xdev->name == XRT_DEVICE_PSVR2 &&
		             (automatic ? u_passthrough_calibration_parse_default(json, xdev->serial, &calibration)
		                        : u_passthrough_calibration_parse(json, xdev->serial, &calibration));
		bool report_failure = !automatic || json != NULL;
		free(json);
		if (calibrated) {
			COMP_INFO(pt->c, "Experimental calibrated passthrough: %s (rotation only)", calibration_path);
		} else if (report_failure) {
			COMP_WARN(pt->c,
			          "Invalid/unreadable passthrough calibration or headset mismatch: %s; using "
			          "approximate mapping",
			          calibration_path);
		}
	}

	pt->passthrough_calibration = calibration;
	pt->passthrough_rotation_enabled = calibrated && debug_get_bool_option_macos_passthrough_rotation();
	if (debug_get_bool_option_macos_passthrough_rotation() && !calibrated && !automatic) {
		COMP_WARN(pt->c, "Passthrough rotation requires a valid calibrated mapping; using static view");
	}

	const float fx = 0.3585564f;
	const float fy = 0.3762281f;
	const float camera_width_ratio = 1016.0f / 1024.0f;
	float fov_deg = (float)debug_get_num_option_macos_passthrough_fov_deg();
	float convergence = (float)debug_get_num_option_macos_passthrough_convergence_milli() / 1000.0f;
	if (fov_deg < 90.0f)
		fov_deg = 90.0f;
	if (fov_deg > 190.0f)
		fov_deg = 190.0f;
	const float fov_rad = fov_deg * 3.14159265358979323846f / 180.0f;

	size_t count = (size_t)PSVR2_PASSTHROUGH_MAP_SIZE * PSVR2_PASSTHROUGH_MAP_SIZE;
	float *map = calloc(count * 4, sizeof(float));
	if (map == NULL) {
		return false;
	}

	for (uint32_t eye = 0; eye < 2; eye++) {
		const float cx = eye == 0 ? 0.6603788f : 0.3396213f;
		const float shift = eye == 0 ? convergence : -convergence;

		for (uint32_t y = 0; y < PSVR2_PASSTHROUGH_MAP_SIZE; y++) {
			float v = ((float)y + 0.5f) / (float)PSVR2_PASSTHROUGH_MAP_SIZE;
			for (uint32_t x = 0; x < PSVR2_PASSTHROUGH_MAP_SIZE; x++) {
				float u = ((float)x + 0.5f) / (float)PSVR2_PASSTHROUGH_MAP_SIZE;
				struct xrt_uv_triplet distortion = {0};
				size_t index = ((size_t)y * PSVR2_PASSTHROUGH_MAP_SIZE + x) * 4;

				if (xrt_device_compute_distortion(xdev, eye, u, v, &distortion) != XRT_SUCCESS) {
					map[index + 0] = -1.0f;
					map[index + 1] = -1.0f;
					map[index + 2] = NAN;
					map[index + 3] = NAN;
					continue;
				}

				/* Recover the green-channel tangent ray from the existing
				 * PS VR2 optical distortion mapping, then apply GAV's proven
				 * equidistant camera model. */
				float tan_x = (distortion.g.x - cx) / fx;
				float tan_y_down = (distortion.g.y - 0.5f) / fy;
				// Keep optical rays alongside the static UVs for GPU rotation.
				map[index + 2] = tan_x;
				map[index + 3] = tan_y_down;
				if (calibrated) {
					/* PS VR2 view orientations are identity relative to the head.
					 * At infinity, eye/camera translations do not affect the ray. */
					struct xrt_vec3 head_ray = {tan_x, -tan_y_down, -1.0f};
					struct xrt_vec2 uv;
					if (u_passthrough_calibration_project(&calibration.cameras[eye], &head_ray,
					                                      &uv)) {
						map[index + 0] = uv.x;
						map[index + 1] = uv.y;
					} else {
						map[index + 0] = -1.0f;
						map[index + 1] = -1.0f;
					}
					continue;
				}
				float len = sqrtf(tan_x * tan_x + tan_y_down * tan_y_down + 1.0f);
				float dir_x = tan_x / len;
				float dir_y = -tan_y_down / len;
				float neg_dir_z = 1.0f / len;
				float theta = acosf(fmaxf(-1.0f, fminf(1.0f, neg_dir_z)));
				float radius = theta / fov_rad;
				float xy_len = sqrtf(dir_x * dir_x + dir_y * dir_y);

				if (radius > 0.5f || xy_len < 1e-7f) {
					if (radius <= 0.5f && xy_len < 1e-7f) {
						map[index + 0] = (0.5f + shift) * camera_width_ratio;
						map[index + 1] = 0.5f;
					} else {
						map[index + 0] = -1.0f;
						map[index + 1] = -1.0f;
					}
					continue;
				}

				float cam_u = (0.5f + radius * (dir_x / xy_len) + shift) * camera_width_ratio;
				float cam_v = 0.5f - radius * (dir_y / xy_len);
				if (cam_u < 0.0f || cam_u > 1.0f || cam_v < 0.0f || cam_v > 1.0f) {
					map[index + 0] = -1.0f;
					map[index + 1] = -1.0f;
				} else {
					map[index + 0] = cam_u;
					map[index + 1] = cam_v;
				}
			}
		}

		pt->maps[eye] = malloc(count * 4 * sizeof(float));
		if (pt->maps[eye] == NULL) {
			free(map);
			return false;
		}
		memcpy(pt->maps[eye], map, count * 4 * sizeof(float));
	}

	free(map);
	return true;
}

struct comp_psvr2_passthrough *
comp_psvr2_passthrough_create(struct comp_compositor *c, bool is_service)
{
	struct xrt_device *head = c->xdev;
	if (head == NULL || head->set_passthrough_sinks == NULL || head->compute_distortion == NULL) {
		return NULL;
	}
	struct comp_psvr2_passthrough *pt = calloc(1, sizeof(*pt));
	if (pt == NULL)
		return NULL;
	pt->c = c;
	pt->is_service = is_service;
	if (pthread_mutex_init(&pt->passthrough_mutex, NULL) != 0) {
		free(pt);
		return NULL;
	}
	atomic_init(&pt->passthrough_shutdown, false);
	if (!psvr2_passthrough_create_uv_maps(pt))
		goto fail;
	for (uint32_t eye = 0; eye < 2; eye++) {
		pt->passthrough_sinks[eye].base.push_frame = psvr2_passthrough_sink_push_frame;
		pt->passthrough_sinks[eye].pt = pt;
		pt->passthrough_sinks[eye].eye = eye;
	}
	if (head->set_passthrough_sinks(head, &pt->passthrough_sinks[0].base, &pt->passthrough_sinks[1].base) !=
	    XRT_SUCCESS)
		goto fail;
	pt->passthrough_sinks_attached = true;
	if (is_service)
		u_passthrough_share_set_source_available(true);
	return pt;
fail:
	comp_psvr2_passthrough_destroy(&pt);
	return NULL;
}

const float *
comp_psvr2_passthrough_map(struct comp_psvr2_passthrough *pt, uint32_t eye)
{
	return eye < 2 ? pt->maps[eye] : NULL;
}

void
comp_psvr2_passthrough_sample_release(struct comp_psvr2_passthrough_sample *sample)
{
	for (uint32_t eye = 0; eye < 2; eye++)
		xrt_frame_reference(&sample->frames[eye], NULL);
}

bool
comp_psvr2_passthrough_sample(struct comp_psvr2_passthrough *pt,
                              const struct xrt_quat *display_orientation,
                              bool has_app,
                              struct comp_psvr2_passthrough_sample *sample)
{
	memset(sample, 0, sizeof(*sample));
	pthread_mutex_lock(&pt->passthrough_mutex);
	for (uint32_t eye = 0; eye < 2; eye++)
		xrt_frame_reference(&sample->frames[eye], pt->passthrough_frames[eye]);
	pthread_mutex_unlock(&pt->passthrough_mutex);
	if (sample->frames[0] == NULL || sample->frames[1] == NULL) {
		comp_psvr2_passthrough_sample_release(sample);
		return false;
	}
	for (uint32_t eye = 0; eye < 2; eye++) {
		struct xrt_frame *frame = sample->frames[eye];
		if (frame->timestamp != pt->passthrough_uploaded_timestamp[eye]) {
			pt->passthrough_uploaded_timestamp[eye] = frame->timestamp;
			pt->passthrough_capture_valid[eye] = false;
			if (pt->passthrough_rotation_enabled &&
			    u_passthrough_calibration_frame_is_fresh(frame->timestamp, frame->source_timestamp,
			                                             os_monotonic_get_ns())) {
				struct xrt_space_relation relation = {0};
				struct xrt_device *head = pt->c->xdev;
				const enum xrt_space_relation_flags flags = XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
				                                            XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT;
				if (xrt_device_get_tracked_pose(head, XRT_INPUT_GENERIC_HEAD_POSE, frame->timestamp,
				                                &relation) == XRT_SUCCESS &&
				    (relation.relation_flags & flags) == flags &&
				    math_quat_validate(&relation.pose.orientation)) {
					pt->passthrough_capture_orientation[eye] = relation.pose.orientation;
					pt->passthrough_capture_valid[eye] = true;
				}
			}
		}
	}
	struct render_camera_params *params = &sample->params;
	params->brightness = (float)debug_get_num_option_macos_passthrough_brightness_percent() / 100.0f;
	params->has_app = has_app;
	params->map_size = PSVR2_PASSTHROUGH_MAP_SIZE;
	bool rotate = pt->passthrough_rotation_enabled && display_orientation != NULL;
	int64_t now_ns = os_monotonic_get_ns();
	for (uint32_t eye = 0; rotate && eye < 2; eye++) {
		const struct u_passthrough_camera *camera = &pt->passthrough_calibration.cameras[eye];
		struct xrt_quat camera_from_display;
		rotate = pt->passthrough_capture_valid[eye] && now_ns >= pt->passthrough_uploaded_timestamp[eye] &&
		         now_ns - pt->passthrough_uploaded_timestamp[eye] < 250 * U_TIME_1MS_IN_NS &&
		         u_passthrough_calibration_rotation(camera, &pt->passthrough_capture_orientation[eye],
		                                            display_orientation, &camera_from_display);
		if (!rotate) {
			break;
		}
		params->cameras[eye].intrinsics[0] = (float)camera->fx;
		params->cameras[eye].intrinsics[1] = (float)camera->fy;
		params->cameras[eye].intrinsics[2] = (float)camera->cx;
		params->cameras[eye].intrinsics[3] = (float)camera->cy;
		for (uint32_t k = 0; k < 4; k++) {
			params->cameras[eye].k[k] = (float)camera->k[k];
		}
		for (uint32_t axis = 0; axis < 3; axis++) {
			struct xrt_vec3 basis = {0}, column;
			if (axis == 0)
				basis.x = 1;
			if (axis == 1)
				basis.y = 1;
			if (axis == 2)
				basis.z = 1;
			math_quat_rotate_vec3(&camera_from_display, &basis, &column);
			params->cameras[eye].rows[0][axis] = column.x;
			params->cameras[eye].rows[1][axis] = column.y;
			params->cameras[eye].rows[2][axis] = column.z;
		}
	}
	params->rotate = rotate ? 1 : 0;
	if (pt->passthrough_rotation_enabled &&
	    (!pt->passthrough_rotation_status_logged || rotate != pt->passthrough_rotation_active)) {
		COMP_INFO(pt->c, "Passthrough rotation %s (requires fresh camera/header clock and tracked poses)",
		          rotate ? "active" : "unavailable; using static mapping");
		pt->passthrough_rotation_active = rotate;
		pt->passthrough_rotation_status_logged = true;
	}

	return true;
}
static void
comp_psvr2_passthrough_stop(struct comp_psvr2_passthrough *pt)
{
	if (pt == NULL) {
		return;
	}
	atomic_store_explicit(&pt->passthrough_shutdown, true, memory_order_release);
	struct xrt_device *xdev = pt->c->xdev;
	if (pt->passthrough_sinks_attached && xdev != NULL && xdev->set_passthrough_sinks != NULL) {
		(void)xdev->set_passthrough_sinks(xdev, NULL, NULL);
		pt->passthrough_sinks_attached = false;
		if (pt->is_service) {
			u_passthrough_share_set_source_available(false);
		}
	}
}

void
comp_psvr2_passthrough_destroy(struct comp_psvr2_passthrough **ptr)
{
	struct comp_psvr2_passthrough *pt = *ptr;
	if (pt == NULL)
		return;
	comp_psvr2_passthrough_stop(pt);
	for (uint32_t eye = 0; eye < 2; eye++) {
		xrt_frame_reference(&pt->passthrough_frames[eye], NULL);
		free(pt->maps[eye]);
	}
	pthread_mutex_destroy(&pt->passthrough_mutex);
	free(pt);
	*ptr = NULL;
}
