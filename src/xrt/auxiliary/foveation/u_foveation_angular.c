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
#include <dlfcn.h>
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

/* Policy: preserve the legacy broad safety profile, straight ahead, for
 * missing tracked gaze. Explicit fixed-mode profiles keep their shape. */
static struct tf_angular_profile
active_profile(const struct u_foveation_angular_config *c, const struct u_foveation_angular_history *h)
{
	struct u_foveation_angular_config fallback;
	if (c->eye_tracked && !h->have_gaze && c->shape != U_FOVEATION_ANGULAR_SMOOTHSTEP) {
		fallback = *c;
		fallback.shape = U_FOVEATION_ANGULAR_SMOOTHSTEP;
		c = &fallback;
	}
	return to_tf_profile(c, h->have_gaze ? c->core : 20.f, h->have_gaze ? c->edge : 35.f);
}

bool
u_foveation_angular_build_recipe(const struct u_foveation_angular_config *c,
                                 const struct u_foveation_angular_target *t,
                                 const struct u_foveation_angular_history *h,
                                 struct u_foveation_angular_recipe *out)
{
	const struct tf_angular_profile p = active_profile(c, h);
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
static bool
build_image_profile(const struct u_foveation_angular_config *c,
                    const struct u_foveation_angular_target *t,
                    uint32_t view_count,
                    const struct u_foveation_angular_history *h,
                    uint64_t revision,
                    struct tf_image_profile *result)
{
	if (view_count == 0 || view_count > 2) {
		return false;
	}
	const struct tf_angular_profile p = active_profile(c, h);
	struct tf_image_profile out = {
	    .struct_size = sizeof(out),
	    .sample_count = TF_IMAGE_PROFILE_SAMPLES,
	    .view_count = view_count,
	    .revision = revision,
	};
	for (uint32_t i = 0; i < view_count; i++) {
		const struct u_foveation_angular_view *v = &t->views[i];
		// The view alone fills the request, so the samples span exactly that view.
		struct tf_view view = to_tf_view(v);
		view.x = view.y = 0;
		const struct tf_angular_request q = {sizeof(q),
		                                     0,
		                                     (uint32_t)v->width,
		                                     (uint32_t)v->height,
		                                     1,
		                                     &view,
		                                     h->have_gaze ? h->yaw : 0.f,
		                                     h->have_gaze ? h->pitch : 0.f,
		                                     TF_IMAGE_PROFILE_SAMPLES,
		                                     TF_IMAGE_PROFILE_SAMPLES};
		if (tf_angular_build(&p, &q, out.views[i].horizontal, out.views[i].vertical) != 0) {
			return false;
		}
		out.views[i].x = v->x;
		out.views[i].y = v->y;
		out.views[i].width = (uint32_t)v->width;
		out.views[i].height = (uint32_t)v->height;
	}
	*result = out;
	return true;
}

bool
u_foveation_angular_publish_profile(const struct u_foveation_angular_config *c,
                                    const struct u_foveation_angular_target *t,
                                    uint32_t view_count,
                                    const struct u_foveation_angular_history *h,
                                    uint64_t revision,
                                    void *texture)
{
	/* Levels 3 and 4 are resolved at run time, so a 0.2 library keeps the maps working. */
	typedef int (*set_texture_fn)(void *, const struct tf_image_profile *);
	typedef int (*set_process_fn)(const struct tf_image_profile *);
	static set_texture_fn set_texture;
	static set_process_fn set_process;
	static bool looked;
	if (!looked) {
		set_texture = (set_texture_fn)dlsym(RTLD_DEFAULT, "tf_metal_set_texture_profile");
		set_process = (set_process_fn)dlsym(RTLD_DEFAULT, "tf_set_process_profile");
		looked = true;
	}
	struct tf_image_profile out;
	if ((!set_texture && !set_process) || !build_image_profile(c, t, view_count, h, revision, &out)) {
		return false;
	}
	/* Without a texture, the process's profile: for a renderer whose output reaches the
	 * runtime's images only through another API (an OpenVR layer copying OpenGL frames). */
	if (texture == NULL) {
		return set_process != NULL && set_process(&out) == 0;
	}
	return set_texture != NULL && set_texture(texture, &out) == 0;
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

bool
u_foveation_angular_bind_metal_texture(void *texture, void *map)
{
#ifdef XRT_HAVE_THRESHOLD_FOVEATION
	if (texture == NULL || tf_metal_set_texture_map(texture, map) != 0) {
		return false;
	}
	// A cleared map takes its profile with it (level 3, when the library has it).
	if (map == NULL) {
		typedef int (*set_profile_fn)(void *, const struct tf_image_profile *);
		set_profile_fn set_profile = (set_profile_fn)dlsym(RTLD_DEFAULT, "tf_metal_set_texture_profile");
		if (set_profile != NULL) {
			(void)set_profile(texture, NULL);
		}
	}
	return true;
#else
	(void)texture;
	(void)map;
	return false;
#endif
}

#ifndef XRT_HAVE_THRESHOLD_FOVEATION
bool
u_foveation_angular_publish_profile(const struct u_foveation_angular_config *c,
                                    const struct u_foveation_angular_target *t,
                                    uint32_t view_count,
                                    const struct u_foveation_angular_history *h,
                                    uint64_t revision,
                                    void *texture)
{
	(void)c, (void)t, (void)view_count, (void)h, (void)revision, (void)texture;
	return false;
}
#endif
