// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
/* Angular policy ported from macOS-wine-xr graphics_native.m. */
#include "u_foveation_angular.h"
#include <math.h>
static const float deg = 57.29577951308232f;
bool
u_foveation_angular_config_valid(const struct u_foveation_angular_config *c)
{
	return c && c->shape >= U_FOVEATION_ANGULAR_SMOOTHSTEP && c->shape <= U_FOVEATION_ANGULAR_GAUSSIAN_CORE &&
	       (c->shape == U_FOVEATION_ANGULAR_SMOOTHSTEP ||
	        (isfinite(c->sigma_degrees) && c->sigma_degrees >= .25f && c->sigma_degrees <= 180.f)) &&
	       isfinite(c->outer) && c->outer >= .1f && c->outer <= 1.f && isfinite(c->core) && c->core >= 1.f &&
	       isfinite(c->edge) && c->edge >= c->core + 1.f && isfinite(c->step) && c->step >= .25f && c->step <= 10.f;
}
bool
u_foveation_angular_target_valid(const struct u_foveation_angular_target *t)
{
	if (!t || !t->width || !t->height || t->width > 16384 || t->height > 16384)
		return false;
	for (unsigned i = 0; i < 2; ++i) {
		const struct u_foveation_angular_view *v = &t->views[i];
		if (v->x < 0 || v->y < 0 || v->width <= 0 || v->height <= 0 || (int64_t)v->x + v->width > t->width ||
		    (int64_t)v->y + v->height > t->height || !isfinite(v->left) || !isfinite(v->right) ||
		    !isfinite(v->up) || !isfinite(v->down) || v->left <= -1.5707963f || v->right >= 1.5707963f ||
		    v->down <= -1.5707963f || v->up >= 1.5707963f || v->left >= v->right || v->down >= v->up)
			return false;
	}
	return true;
}
void
u_foveation_angular_update(const struct u_foveation_angular_config *c,
                           bool valid,
                           float yaw,
                           float pitch,
                           struct u_foveation_angular_history *h)
{
	valid = valid && c->eye_tracked && isfinite(yaw) && isfinite(pitch);
	if (valid) {
		h->invalid_calls = 0;
		if (!h->have_gaze || fabsf(yaw - h->yaw) > .6f * c->step || fabsf(pitch - h->pitch) > .6f * c->step) {
			h->yaw = fmaxf(-45.f, fminf(45.f, roundf(yaw / c->step) * c->step));
			h->pitch = fmaxf(-45.f, fminf(45.f, roundf(pitch / c->step) * c->step));
			h->have_gaze = true;
		}
	} else if (h->have_gaze && ++h->invalid_calls > 30)
		h->have_gaze = false;
}
void
u_foveation_angular_angles(const struct u_foveation_angular_view *v, float x, float y, float *yaw, float *pitch)
{
	float l = tanf(v->left), r = tanf(v->right), d = tanf(v->down), u = tanf(v->up);
	*yaw = atanf(l + (x + 1.f) * .5f * (r - l)) * deg;
	*pitch = atanf(d + (y + 1.f) * .5f * (u - d)) * deg;
}
static float
rate(float delta, float core, float edge, float outer)
{
	float t = (fabsf(delta) - core) / fmaxf(edge - core, .01f);
	t = fminf(1.f, fmaxf(0.f, t));
	t = t * t * (3.f - 2.f * t);
	return 1.f + (outer - 1.f) * t;
}
float
u_foveation_angular_axis_rate(const struct u_foveation_angular_config *c,
                              float delta,
                              float active_core,
                              float active_edge)
{
	if (c->shape == U_FOVEATION_ANGULAR_SMOOTHSTEP)
		return rate(delta, active_core, active_edge, c->outer);
	float d = fabsf(delta);
	if (c->shape == U_FOVEATION_ANGULAR_GAUSSIAN_CORE)
		d = fmaxf(0.f, d - active_core);
	float z = d / c->sigma_degrees;
	float g = expf(-.5f * z * z);
	if (c->shape == U_FOVEATION_ANGULAR_GAUSSIAN_FLOOR)
		return c->outer + (1.f - c->outer) * g;
	return fmaxf(c->outer, g);
}
void
u_foveation_angular_build_recipe(const struct u_foveation_angular_config *c,
                                 const struct u_foveation_angular_target *t,
                                 const struct u_foveation_angular_history *h,
                                 struct u_foveation_angular_recipe *out)
{
	float yaw = h->have_gaze ? h->yaw : 0.f, pitch = h->have_gaze ? h->pitch : 0.f;
	float core = h->have_gaze ? c->core : 20.f, edge = h->have_gaze ? c->edge : 35.f;
	for (unsigned i = 0; i < 64; ++i) {
		float x = (i + .5f) / 64 * t->width, rr = c->outer;
		for (unsigned k = 0; k < 2; ++k) {
			const struct u_foveation_angular_view *v = &t->views[k];
			if (x < v->x || x >= v->x + v->width)
				continue;
			float l = tanf(v->left), r = tanf(v->right);
			float angle = atanf(l + (x - v->x) / v->width * (r - l)) * deg;
			rr = fmaxf(rr, u_foveation_angular_axis_rate(c, angle - yaw, core, edge));
		}
		out->horizontal[i] = rr;
	}
	for (unsigned j = 0; j < 32; ++j) {
		float y = (j + .5f) / 32 * t->height, rr = c->outer;
		for (unsigned k = 0; k < 2; ++k) {
			const struct u_foveation_angular_view *v = &t->views[k];
			if (y < v->y || y >= v->y + v->height)
				continue;
			float u = tanf(v->up), d = tanf(v->down);
			float angle = atanf(u - (y - v->y) / v->height * (u - d)) * deg;
			rr = fmaxf(rr, u_foveation_angular_axis_rate(c, angle - pitch, core, edge));
		}
		out->vertical[j] = rr;
	}
}
