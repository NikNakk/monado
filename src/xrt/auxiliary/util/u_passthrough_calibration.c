// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "util/u_passthrough_calibration.h"
#include "util/u_json.h"
#include "math/m_api.h"

#include <math.h>
#include <string.h>

static bool
number(const cJSON *object, const char *key, double *out)
{
	return u_json_get_double(u_json_get(object, key), out) && isfinite(*out);
}

static bool
parse(const char *json, const char *serial, bool require_serial, struct u_passthrough_calibration *out)
{
	if (json == NULL || serial == NULL || serial[0] == '\0' || out == NULL) {
		return false;
	}
	cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
	struct u_passthrough_calibration result = {0};
	char format[64] = {0}, headset[256] = {0}, projection[64] = {0};
	const cJSON *cameras = u_json_get(root, "cameras");
	bool good = u_json_get_string_into_array(u_json_get(root, "format"), format, sizeof(format)) &&
	            strcmp(format, "psvr2-passthrough-calibration-v1") == 0 &&
	            ((!require_serial && cJSON_IsNull(u_json_get(root, "headset_serial"))) ||
	             (u_json_get_string_into_array(u_json_get(root, "headset_serial"), headset, sizeof(headset)) &&
	              strcmp(headset, serial) == 0)) &&
	            u_json_get_string_into_array(u_json_get(root, "projection"), projection, sizeof(projection)) &&
	            strcmp(projection, "rotation-only") == 0 && cJSON_IsArray(cameras) &&
	            cJSON_GetArraySize(cameras) == 2;
	bool seen[2] = {false};
	for (int i = 0; good && i < 2; i++) {
		const cJSON *entry = cJSON_GetArrayItem(cameras, i);
		double view = -1, width = 0, height = 0;
		char model[64] = {0};
		good = number(entry, "view", &view) && (view == 0 || view == 1) && !seen[(int)view] &&
		       number(entry, "width", &width) && width == 1024 && number(entry, "height", &height) &&
		       height == 1016 &&
		       u_json_get_string_into_array(u_json_get(entry, "model"), model, sizeof(model)) &&
		       strcmp(model, "fisheye_equidistant4") == 0;
		if (!good) {
			break;
		}
		seen[(int)view] = true;
		struct u_passthrough_camera *cam = &result.cameras[(int)view];
		const cJSON *intrinsics = u_json_get(entry, "intrinsics");
		const cJSON *distortion = u_json_get(entry, "distortion");
		good = number(intrinsics, "fx", &cam->fx) && cam->fx > 0 && number(intrinsics, "fy", &cam->fy) &&
		       cam->fy > 0 && number(intrinsics, "cx", &cam->cx) && cam->cx >= 0 && cam->cx < width &&
		       number(intrinsics, "cy", &cam->cy) && cam->cy >= 0 && cam->cy < height &&
		       number(distortion, "k1", &cam->k[0]) && number(distortion, "k2", &cam->k[1]) &&
		       number(distortion, "k3", &cam->k[2]) && number(distortion, "k4", &cam->k[3]) &&
		       u_json_get_pose(u_json_get(entry, "head_from_camera_xrt"), &cam->head_from_camera) &&
		       math_pose_validate(&cam->head_from_camera);
	}
	cJSON_Delete(root);
	if (good) {
		*out = result;
	}
	return good;
}

bool
u_passthrough_calibration_parse(const char *json, const char *serial, struct u_passthrough_calibration *out)
{
	return parse(json, serial, false, out);
}

bool
u_passthrough_calibration_parse_default(const char *json, const char *serial, struct u_passthrough_calibration *out)
{
	return parse(json, serial, true, out);
}

bool
u_passthrough_calibration_project(const struct u_passthrough_camera *camera,
                                  const struct xrt_vec3 *head_ray,
                                  struct xrt_vec2 *out_uv)
{
	struct xrt_quat inverse;
	struct xrt_vec3 ray;
	math_quat_invert(&camera->head_from_camera.orientation, &inverse);
	math_quat_rotate_vec3(&inverse, head_ray, &ray);
	// Camera XRT -> OpenCV: x right, y down, z forward.
	double x = ray.x, y = -ray.y, z = -ray.z;
	if (!isfinite(x) || !isfinite(y) || !isfinite(z) || z <= 0) {
		return false;
	}
	double r = hypot(x, y);
	double theta = atan2(r, z);
	double t2 = theta * theta;
	double distorted =
	    theta * (1 + t2 * (camera->k[0] + t2 * (camera->k[1] + t2 * (camera->k[2] + t2 * camera->k[3]))));
	if (!isfinite(distorted) || distorted < 0) {
		return false;
	}
	double scale = r > 1e-12 ? distorted / r : 0;
	// Intrinsics use integer pixel centres; Metal normalized sampling uses half pixels.
	double u = (camera->fx * x * scale + camera->cx + 0.5) / 1024;
	double v = (camera->fy * y * scale + camera->cy + 0.5) / 1016;
	if (!isfinite(u) || !isfinite(v) || u < 0 || u > 1 || v < 0 || v > 1) {
		return false;
	}
	*out_uv = (struct xrt_vec2){(float)u, (float)v};
	return true;
}


bool
u_passthrough_calibration_rotation(const struct u_passthrough_camera *camera,
                                   const struct xrt_quat *capture_head,
                                   const struct xrt_quat *display_head,
                                   struct xrt_quat *out_camera_from_display)
{
	if (!math_quat_validate(&camera->head_from_camera.orientation) || !math_quat_validate(capture_head) ||
	    !math_quat_validate(display_head)) {
		return false;
	}
	struct xrt_quat capture_from_world, camera_from_capture, capture_from_display;
	math_quat_invert(capture_head, &capture_from_world);
	math_quat_invert(&camera->head_from_camera.orientation, &camera_from_capture);
	math_quat_rotate(&capture_from_world, display_head, &capture_from_display);
	math_quat_rotate(&camera_from_capture, &capture_from_display, out_camera_from_display);
	math_quat_normalize(out_camera_from_display);
	return true;
}


bool
u_passthrough_calibration_frame_is_fresh(int64_t timestamp_ns, int64_t source_timestamp_ns, int64_t now_ns)
{
	return source_timestamp_ns > 0 && timestamp_ns > 0 && timestamp_ns <= now_ns &&
	       now_ns - timestamp_ns < 250000000;
}
