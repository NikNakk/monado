// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define U_FOVEATION_ANGULAR_COLUMNS 64
#define U_FOVEATION_ANGULAR_ROWS 32
enum u_foveation_angular_shape
{
	U_FOVEATION_ANGULAR_SMOOTHSTEP = 0,
	U_FOVEATION_ANGULAR_GAUSSIAN_CLAMPED = 1,
	U_FOVEATION_ANGULAR_GAUSSIAN_FLOOR = 2,
	U_FOVEATION_ANGULAR_GAUSSIAN_CORE = 3,
};
struct u_foveation_angular_config
{
	float outer, core, edge, step;
	bool eye_tracked;
	enum u_foveation_angular_shape shape;
	float sigma_degrees;
};
struct u_foveation_angular_view
{
	int32_t x, y, width, height;
	float left, right, up, down;
};
struct u_foveation_angular_target
{
	uint32_t width, height;
	struct u_foveation_angular_view views[2];
};
struct u_foveation_angular_history
{
	float yaw, pitch;
	uint32_t invalid_calls;
	bool have_gaze;
};
struct u_foveation_angular_recipe
{
	float horizontal[64], vertical[32];
};
bool
u_foveation_angular_config_valid(const struct u_foveation_angular_config *c);
bool
u_foveation_angular_target_valid(const struct u_foveation_angular_target *t);
void
u_foveation_angular_update(const struct u_foveation_angular_config *c,
                           bool valid,
                           float yaw,
                           float pitch,
                           struct u_foveation_angular_history *h);
void
u_foveation_angular_angles(const struct u_foveation_angular_view *v, float x, float y, float *yaw, float *pitch);
void
u_foveation_angular_build_recipe(const struct u_foveation_angular_config *c,
                                 const struct u_foveation_angular_target *t,
                                 const struct u_foveation_angular_history *h,
                                 struct u_foveation_angular_recipe *out);
#ifdef __cplusplus
}
#endif

/* Per-axis linear sampling rate. Area density is the product of two axes.
 * Exposed for analysis/tests; recipe generation remains the rendering path.
 */
#ifdef __cplusplus
extern "C" {
#endif
float
u_foveation_angular_axis_rate(const struct u_foveation_angular_config *c,
                              float delta,
                              float active_core,
                              float active_edge);
#ifdef __cplusplus
}
#endif
