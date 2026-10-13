// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "xrt/xrt_defines.h"
#include <math.h>

/* Conservative bounds in the layer shader's pre-timewarp view UV space.
 * Positive-depth perspective projection maps a planar rectangle to a convex
 * polygon, whose extrema are at its corners. Eye-plane crossings and invalid
 * inputs fall back to the full view. Keep the exact ray/plane test inside this
 * rectangle; the rectangle alone never establishes a hit.
 */
static inline void
render_quad_view_bounds(const struct xrt_matrix_4x4 *plane_to_view,
                        const struct xrt_vec2 *extent,
                        const struct xrt_normalized_rect *pre_transform,
                        uint32_t width,
                        uint32_t height,
                        float bounds[4])
{
	bounds[0] = bounds[1] = 0.0f;
	bounds[2] = bounds[3] = 1.0f;
	if (width == 0 || height == 0 || !isfinite(extent->x) || !isfinite(extent->y) || extent->x <= 0.0f ||
	    extent->y <= 0.0f || !isfinite(pre_transform->w) || !isfinite(pre_transform->h) ||
	    fabs(pre_transform->w) < 1e-8 || fabs(pre_transform->h) < 1e-8) {
		return;
	}

	double min_u = INFINITY, min_v = INFINITY, max_u = -INFINITY, max_v = -INFINITY;
	const float *m = plane_to_view->v;
	for (int sx = -1; sx <= 1; sx += 2) {
		for (int sy = -1; sy <= 1; sy += 2) {
			double local_x = sx * (double)extent->x * 0.5;
			double local_y = sy * (double)extent->y * 0.5;
			double x = m[0] * local_x + m[4] * local_y + m[12];
			double y = m[1] * local_x + m[5] * local_y + m[13];
			double z = m[2] * local_x + m[6] * local_y + m[14];
			if (!isfinite(z) || z >= -1e-4) {
				return;
			}
			double u = (x / -z - pre_transform->x) / pre_transform->w;
			double v = (-y / -z - pre_transform->y) / pre_transform->h;
			if (!isfinite(u) || !isfinite(v)) {
				return;
			}
			min_u = fmin(min_u, u);
			max_u = fmax(max_u, u);
			min_v = fmin(min_v, v);
			max_v = fmax(max_v, v);
		}
	}

	// A pixel plus a relative margin covers float matrix and projection error.
	double margin_u = 1.0 / width + 1e-5 * fmax(1.0, fmax(fabs(min_u), fabs(max_u)));
	double margin_v = 1.0 / height + 1e-5 * fmax(1.0, fmax(fabs(min_v), fabs(max_v)));
	float projected[] = {(float)(min_u - margin_u), (float)(min_v - margin_v), (float)(max_u + margin_u),
	                     (float)(max_v + margin_v)};
	for (int i = 0; i < 4; ++i) {
		if (!isfinite(projected[i])) {
			return;
		}
	}
	for (int i = 0; i < 4; ++i) {
		bounds[i] = projected[i];
	}
}
