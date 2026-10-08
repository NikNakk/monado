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

//! Project a head-space XRT direction at infinity. False means outside the image or invalid.
bool
u_passthrough_calibration_project(const struct u_passthrough_camera *camera,
                                  const struct xrt_vec3 *head_ray,
                                  struct xrt_vec2 *out_uv);

#ifdef __cplusplus
}
#endif
