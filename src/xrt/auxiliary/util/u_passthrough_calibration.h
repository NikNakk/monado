// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "xrt/xrt_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

//! Experimental PS VR2 mode-16 calibration. Poses map camera XRT axes into head axes.
struct u_passthrough_camera
{
	double fx, fy, cx, cy;
	double k[4];
	struct xrt_pose head_from_camera;
};

struct u_passthrough_calibration
{
	struct u_passthrough_camera cameras[2];
};

//! Strictly parse v1; match the headset serial when the file supplies one. Does not modify output on failure.
bool
u_passthrough_calibration_parse(const char *json, const char *serial, struct u_passthrough_calibration *out);

//! Automatically selected files must bind to the current headset serial.
bool
u_passthrough_calibration_parse_default(const char *json, const char *serial, struct u_passthrough_calibration *out);

//! Project a head-space XRT direction at infinity. False means outside the image or invalid.
bool
u_passthrough_calibration_project(const struct u_passthrough_camera *camera,
                                  const struct xrt_vec3 *head_ray,
                                  struct xrt_vec2 *out_uv);

/*!
 * Fixed-depth mapping: the head-space ray from @p eye_position meets a sphere of radius @p depth_m around the eye;
 * return the direction to that point from the camera centre, scaled to z = -1 (the map's tangent form). A depth of
 * zero or less keeps @p head_ray (infinity). False when the point is not in front of the camera's head plane.
 */
bool
u_passthrough_calibration_depth_ray(const struct u_passthrough_camera *camera,
                                    const struct xrt_vec3 *eye_position,
                                    const struct xrt_vec3 *head_ray,
                                    double depth_m,
                                    struct xrt_vec3 *out_ray);

//! Require mapped hardware timing and a frame less than 250 ms old (never future-dated).
bool
u_passthrough_calibration_frame_is_fresh(int64_t timestamp_ns, int64_t source_timestamp_ns, int64_t now_ns);

//! Map a display-head ray into the camera at capture: inverse(camera) * inverse(capture) * display.
//! Orientations must be valid unit quaternions in the same tracking origin.
bool
u_passthrough_calibration_rotation(const struct u_passthrough_camera *camera,
                                   const struct xrt_quat *capture_head,
                                   const struct xrt_quat *display_head,
                                   struct xrt_quat *out_camera_from_display);

#ifdef __cplusplus
}
#endif
