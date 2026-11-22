// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * Angular foveation policy. Monado owns the accepted parameter ranges, gaze
 * quantization, hysteresis, the loss hold and the fallback for lost tracked
 * gaze. The profile mathematics itself is threshold-foveation's
 * (tf_angular_build); without that library the angular provider is
 * unsupported rather than carrying a second copy.
 */
#include "u_foveation_angular.h"
#include <math.h>
#ifdef XRT_HAVE_THRESHOLD_FOVEATION
#include <threshold_foveation.h>
#endif

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
u_foveation_angular_available(void)
{
#ifdef XRT_HAVE_THRESHOLD_FOVEATION
	return tf_api_level() >= 2;
#else
	return false;
#endif
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

#ifdef XRT_HAVE_THRESHOLD_FOVEATION
static struct tf_view
to_tf_view(const struct u_foveation_angular_view *v)
{
	return (struct tf_view){v->x, v->y, v->width, v->height, v->left, v->right, v->up, v->down};
}

static struct tf_angular_profile
to_tf_profile(const struct u_foveation_angular_config *c, float active_core, float active_edge)
{
	return (struct tf_angular_profile){sizeof(struct tf_angular_profile),
	                                   (uint32_t)c->shape,
	                                   c->outer,
	                                   active_core,
	                                   active_edge,
	                                   c->sigma_degrees};
}

bool
u_foveation_angular_target_valid(const struct u_foveation_angular_target *t)
{
	if (!t || !t->width || !t->height || t->width > 16384 || t->height > 16384)
		return false;
	for (unsigned i = 0; i < 2; ++i) {
		struct tf_view v = to_tf_view(&t->views[i]);
		if (!tf_view_valid(&v, t->width, t->height))
			return false;
	}
	return true;
}

void
u_foveation_angular_angles(const struct u_foveation_angular_view *v, float x, float y, float *yaw, float *pitch)
{
	struct tf_view view = to_tf_view(v);
	tf_view_angles(&view, x, y, yaw, pitch);
}

float
u_foveation_angular_axis_rate(const struct u_foveation_angular_config *c,
                              float delta,
                              float active_core,
                              float active_edge)
{
	struct tf_angular_profile p = to_tf_profile(c, active_core, active_edge);
	return tf_angular_axis_rate(&p, delta);
}

bool
u_foveation_angular_build_recipe(const struct u_foveation_angular_config *c,
                                 const struct u_foveation_angular_target *t,
                                 const struct u_foveation_angular_history *h,
                                 struct u_foveation_angular_recipe *out)
{
	/* Policy: preserve the legacy broad safety profile, straight ahead, for
	 * missing tracked gaze. Explicit fixed-mode profiles keep their shape. */
	struct u_foveation_angular_config fallback;
	if (c->eye_tracked && !h->have_gaze && c->shape != U_FOVEATION_ANGULAR_SMOOTHSTEP) {
		fallback = *c;
		fallback.shape = U_FOVEATION_ANGULAR_SMOOTHSTEP;
		c = &fallback;
	}
	const struct tf_angular_profile p =
	    to_tf_profile(c, h->have_gaze ? c->core : 20.f, h->have_gaze ? c->edge : 35.f);
	const struct tf_view views[2] = {to_tf_view(&t->views[0]), to_tf_view(&t->views[1])};
	const struct tf_angular_request q = {sizeof(struct tf_angular_request),
	                                     0,
	                                     t->width,
	                                     t->height,
	                                     2,
	                                     views,
	                                     h->have_gaze ? h->yaw : 0.f,
	                                     h->have_gaze ? h->pitch : 0.f,
	                                     U_FOVEATION_ANGULAR_COLUMNS,
	                                     U_FOVEATION_ANGULAR_ROWS};
	return tf_angular_build(&p, &q, out->horizontal, out->vertical) == 0;
}
#else
bool
u_foveation_angular_target_valid(const struct u_foveation_angular_target *t)
{
	(void)t;
	return false;
}

void
u_foveation_angular_angles(const struct u_foveation_angular_view *v, float x, float y, float *yaw, float *pitch)
{
	(void)v, (void)x, (void)y;
	*yaw = *pitch = 0.f;
}

float
u_foveation_angular_axis_rate(const struct u_foveation_angular_config *c,
                              float delta,
                              float active_core,
                              float active_edge)
{
	(void)c, (void)delta, (void)active_core, (void)active_edge;
	return 1.f;
}

bool
u_foveation_angular_build_recipe(const struct u_foveation_angular_config *c,
                                 const struct u_foveation_angular_target *t,
                                 const struct u_foveation_angular_history *h,
                                 struct u_foveation_angular_recipe *out)
{
	(void)c, (void)t, (void)h, (void)out;
	return false;
}
#endif
