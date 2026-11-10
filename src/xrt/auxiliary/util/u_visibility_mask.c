// Copyright 2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Visibility mask utilitary
 * @author Simon Zeni <simon.zeni@collabora.com>
 * @ingroup aux_util
 */

#include "math/m_mathinclude.h"

#include "u_misc.h"
#include "u_visibility_mask.h"
#include "u_logging.h"


#include <string.h>
#include <stdlib.h>

static const struct xrt_vec2 vertices_hidden[] = {
    {1.0, 0.75},   {1.0, 1.0},   {0.75, 1.0},   {-1.0, 1.0},  {-1.0, 0.75}, {-0.75, 1.0},
    {-1.0, -0.75}, {-1.0, -1.0}, {-0.75, -1.0}, {0.75, -1.0}, {1.0, -1.0},  {1.0, -0.75},
};

static const uint32_t indices_hidden[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

static const struct xrt_vec2 vertices_visible[] = {
    {1.f, -.75f}, {.75f, -1.f}, {-.75f, -1.f}, {-1.f, -.75f}, {-1.f, .75f},
    {-.75f, 1.f}, {.75f, 1.f},  {1.f, .75f},   {0.f, 0.f},
};

static const uint32_t indices_visible[] = {
    8, 2, 1, 3, 2, 8, 8, 1, 0, 6, 8, 7, 4, 8, 5, 8, 0, 7, 5, 8, 6, 4, 3, 8,
};

static const struct xrt_vec2 vertices_line[] = {
    {-.75f, -1.f}, {.75f, -1.f}, {1.f, -.75f}, {1.f, .75f}, {.75f, 1.f}, {-.75f, 1.f}, {-1.f, .75f}, {-1.f, -.75f},
};

static const uint32_t indices_line[] = {0, 1, 2, 3, 4, 5, 6, 7};

void
u_visibility_mask_get_default(enum xrt_visibility_mask_type type,
                              const struct xrt_fov *fov,
                              struct xrt_visibility_mask **out_mask)
{
	struct xrt_visibility_mask *mask = NULL;
	uint32_t nvertices = 0, nindices = 0;

	switch (type) {
	case XRT_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH:
		nvertices = ARRAY_SIZE(vertices_hidden);
		nindices = ARRAY_SIZE(indices_hidden);
		break;
	case XRT_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH:
		nvertices = ARRAY_SIZE(vertices_visible);
		nindices = ARRAY_SIZE(indices_visible);
		break;
	case XRT_VISIBILITY_MASK_TYPE_LINE_LOOP:
		nvertices = ARRAY_SIZE(vertices_line);
		nindices = ARRAY_SIZE(indices_line);
		break;
	}

	const size_t size =
	    sizeof(struct xrt_visibility_mask) + sizeof(uint32_t) * nindices + sizeof(struct xrt_vec2) * nvertices;
	mask = U_CALLOC_WITH_CAST(struct xrt_visibility_mask, size);
	if (mask == NULL) {
		U_LOG_E("failed to allocate out xrt_visibility_mask");
		goto out;
	}


	mask->index_count = nindices;
	mask->vertex_count = nvertices;

	const struct xrt_vec2 *vertices = NULL;
	const uint32_t *indices = NULL;

	switch (type) {
	case XRT_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH:
		vertices = vertices_hidden;
		indices = indices_hidden;
		break;
	case XRT_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH:
		vertices = vertices_visible;
		indices = indices_visible;
		break;
	case XRT_VISIBILITY_MASK_TYPE_LINE_LOOP:
		vertices = vertices_line;
		indices = indices_line;
		break;
	}

	memcpy(xrt_visibility_mask_get_indices(mask), indices, sizeof(uint32_t) * nindices);


	const struct xrt_fov copy = *fov;

	const float tan_left = tanf(copy.angle_left);
	const float tan_right = tanf(copy.angle_right);

	const float tan_down = tanf(copy.angle_down);
	const float tan_up = tanf(copy.angle_up);

	const float tan_half_width = (tan_right - tan_left);
	const float tan_half_height = (tan_up - tan_down);

	const float tan_offset_x = ((tan_right + tan_left) - tan_half_width) / 2;
	const float tan_offset_y = (-(tan_up + tan_down) - tan_half_height) / 2;

	struct xrt_vec2 *dst = xrt_visibility_mask_get_vertices(mask);
	for (uint32_t i = 0; i < nvertices; i++) {
		struct xrt_vec2 v = vertices[i];

		// Yes this is really the simplest form, WolframAlpha agrees.
		v.x = (v.x * 0.5 + 0.5) * tan_half_width + tan_offset_x;
		v.y = (v.y * 0.5 + 0.5) * tan_half_height + tan_offset_y;

		dst[i] = v;
	}

out:
	*out_mask = mask; // Always NULL or allocated data.
}

void
u_visibility_mask_get_noop(enum xrt_visibility_mask_type type, struct xrt_visibility_mask **out_mask)
{
	struct xrt_visibility_mask *mask = U_TYPED_CALLOC(struct xrt_visibility_mask);
	if (mask == NULL) {
		U_LOG_E("failed to allocate out xrt_visibility_mask");
		*out_mask = NULL;
		return;
	}

	mask->type = type;
	mask->index_count = 0;
	mask->vertex_count = 0;

	*out_mask = mask;
}


/*
 *
 * Masks from an outline.
 *
 */

static float
triangle_area2(struct xrt_vec2 a, struct xrt_vec2 b, struct xrt_vec2 c)
{
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

/*!
 * Where the ray from @p c through @p p leaves the unit square. Edges are
 * numbered in the order the outline visits them with v down: 0 right,
 * 1 bottom, 2 left, 3 top.
 */
static struct xrt_vec2
exit_unit_square(struct xrt_vec2 c, struct xrt_vec2 p, int *out_edge)
{
	const float dx = p.x - c.x;
	const float dy = p.y - c.y;
	float t = INFINITY;
	int edge = 0;

	if (dx > 0 && (1.f - c.x) / dx < t) {
		t = (1.f - c.x) / dx;
		edge = 0;
	}
	if (dy > 0 && (1.f - c.y) / dy < t) {
		t = (1.f - c.y) / dy;
		edge = 1;
	}
	if (dx < 0 && -c.x / dx < t) {
		t = -c.x / dx;
		edge = 2;
	}
	if (dy < 0 && -c.y / dy < t) {
		t = -c.y / dy;
		edge = 3;
	}

	*out_edge = edge;
	return (struct xrt_vec2){c.x + dx * t, c.y + dy * t};
}

static void
push_triangle(const struct xrt_vec2 *uv, uint32_t *indices, uint32_t *count, uint32_t a, uint32_t b, uint32_t c)
{
	// Drop slivers, such as where the outline has been clamped to the edge.
	if (fabsf(triangle_area2(uv[a], uv[b], uv[c])) < 1e-9f) {
		return;
	}
	indices[(*count)++] = a;
	indices[(*count)++] = b;
	indices[(*count)++] = c;
}

void
u_visibility_mask_from_outline(enum xrt_visibility_mask_type type,
                               const struct xrt_fov *fov,
                               const struct xrt_vec2 *centre,
                               const struct xrt_vec2 *outline,
                               uint32_t count,
                               struct xrt_visibility_mask **out_mask)
{
	static const struct xrt_vec2 corners[4] = {{1.f, 1.f}, {0.f, 1.f}, {0.f, 0.f}, {1.f, 0.f}};
	const uint32_t n = count;
	const struct xrt_vec2 c = *centre;

	// Vertices: outline [0, n), its edge points [n, 2n), image corners [2n, 2n + 4), centre 2n + 4.
	const uint32_t max_vertices = 2 * n + 5;
	const uint32_t max_indices = 18 * n;
	struct xrt_vec2 *uv = U_TYPED_ARRAY_CALLOC(struct xrt_vec2, max_vertices);
	int *edges = U_TYPED_ARRAY_CALLOC(int, n);
	uint32_t *indices = U_TYPED_ARRAY_CALLOC(uint32_t, max_indices);
	struct xrt_visibility_mask *mask = NULL;
	uint32_t vertex_count = 0;
	uint32_t index_count = 0;

	if (uv == NULL || edges == NULL || indices == NULL || n < 3) {
		U_LOG_E("failed to build visibility mask from %u outline points", n);
		goto out;
	}

	for (uint32_t i = 0; i < n; i++) {
		struct xrt_vec2 o = exit_unit_square(c, outline[i], &edges[i]);
		struct xrt_vec2 p = outline[i];
		const float dp = (p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y);
		const float de = (o.x - c.x) * (o.x - c.x) + (o.y - c.y) * (o.y - c.y);
		uv[i] = dp < de ? p : o;
		uv[n + i] = o;
	}
	for (uint32_t i = 0; i < 4; i++) {
		uv[2 * n + i] = corners[i];
	}
	uv[2 * n + 4] = c;

	switch (type) {
	case XRT_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH:
		vertex_count = 2 * n + 4;
		for (uint32_t i = 0; i < n; i++) {
			const uint32_t j = (i + 1) % n;
			// Fan from this outline point along the image edge to the next edge point,
			// through any image corners between them.
			uint32_t prev = n + i;
			for (int e = edges[i], k = 0; e != edges[j] && k < 4; e = (e + 1) % 4, k++) {
				push_triangle(uv, indices, &index_count, i, prev, 2 * n + (uint32_t)e);
				prev = 2 * n + (uint32_t)e;
			}
			push_triangle(uv, indices, &index_count, i, prev, n + j);
			push_triangle(uv, indices, &index_count, i, n + j, j);
		}
		break;
	case XRT_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH:
		// Put the centre first and the outline after it.
		uv[n] = c;
		vertex_count = n + 1;
		for (uint32_t i = 0; i < n; i++) {
			push_triangle(uv, indices, &index_count, n, i, (i + 1) % n);
		}
		break;
	case XRT_VISIBILITY_MASK_TYPE_LINE_LOOP:
		vertex_count = n;
		for (uint32_t i = 0; i < n; i++) {
			indices[index_count++] = i;
		}
		break;
	}

	const size_t size = sizeof(struct xrt_visibility_mask) + sizeof(uint32_t) * index_count +
	                    sizeof(struct xrt_vec2) * vertex_count;
	mask = U_CALLOC_WITH_CAST(struct xrt_visibility_mask, size);
	if (mask == NULL) {
		U_LOG_E("failed to allocate out xrt_visibility_mask");
		goto out;
	}
	mask->type = type;
	mask->index_count = index_count;
	mask->vertex_count = vertex_count;

	// Image coordinates to tangents in view space (x right, y up, at z = -1).
	const float tan_left = tanf(fov->angle_left);
	const float tan_right = tanf(fov->angle_right);
	const float tan_down = tanf(fov->angle_down);
	const float tan_up = tanf(fov->angle_up);

	struct xrt_vec2 *dst = xrt_visibility_mask_get_vertices(mask);
	for (uint32_t i = 0; i < vertex_count; i++) {
		dst[i].x = tan_left + uv[i].x * (tan_right - tan_left);
		dst[i].y = tan_up - uv[i].y * (tan_up - tan_down);
	}

	// Counter-clockwise triangles in view space.
	uint32_t *dst_indices = xrt_visibility_mask_get_indices(mask);
	memcpy(dst_indices, indices, sizeof(uint32_t) * index_count);
	if (type != XRT_VISIBILITY_MASK_TYPE_LINE_LOOP) {
		for (uint32_t i = 0; i + 2 < index_count; i += 3) {
			uint32_t *t = &dst_indices[i];
			if (triangle_area2(dst[t[0]], dst[t[1]], dst[t[2]]) < 0) {
				const uint32_t tmp = t[1];
				t[1] = t[2];
				t[2] = tmp;
			}
		}
	}

out:
	free(uv);
	free(edges);
	free(indices);
	*out_mask = mask; // Always NULL or allocated data.
}

void
u_visibility_mask_ellipse_outline(float scale,
                                  uint32_t count,
                                  struct xrt_vec2 *out_centre,
                                  struct xrt_vec2 *out_outline)
{
	*out_centre = (struct xrt_vec2){0.5f, 0.5f};
	for (uint32_t i = 0; i < count; i++) {
		const float a = 2.f * (float)M_PI * (float)i / (float)count;
		out_outline[i] = (struct xrt_vec2){0.5f + 0.5f * scale * cosf(a), 0.5f + 0.5f * scale * sinf(a)};
	}
}
