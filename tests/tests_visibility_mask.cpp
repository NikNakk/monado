// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Visibility masks from an outline, and the PS VR2 lens outline.
 */

#include "math/m_mathinclude.h"
#include "util/u_visibility_mask.h"

#include "catch_amalgamated.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" {
void
psvr2_compute_distortion_asymmetric(
    float *calibration, struct xrt_uv_triplet *distCoords, int eEye, float fU, float fV);
bool
psvr2_visibility_outline(const float *calibration,
                         int eEye,
                         float max_r2,
                         float margin,
                         uint32_t count,
                         struct xrt_vec2 *out_centre,
                         struct xrt_vec2 *out_outline);
}

namespace {

xrt_fov
fov_degrees(float left, float right, float down, float up)
{
	const float k = (float)M_PI / 180.f;
	return xrt_fov{left * k, right * k, up * k, down * k};
}

float
area2(xrt_vec2 a, xrt_vec2 b, xrt_vec2 c)
{
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

struct Mask
{
	xrt_visibility_mask *mask = nullptr;
	~Mask()
	{
		free(mask);
	}
	// Total triangle area; every triangle must be counter-clockwise.
	float
	area() const
	{
		const uint32_t *idx = xrt_visibility_mask_get_indices(mask);
		const xrt_vec2 *v = xrt_visibility_mask_get_vertices(mask);
		REQUIRE(mask->index_count % 3 == 0);
		float sum = 0;
		for (uint32_t i = 0; i < mask->index_count; i += 3) {
			REQUIRE(idx[i] < mask->vertex_count);
			REQUIRE(idx[i + 1] < mask->vertex_count);
			REQUIRE(idx[i + 2] < mask->vertex_count);
			const float a = area2(v[idx[i]], v[idx[i + 1]], v[idx[i + 2]]) / 2;
			CHECK(a > 0);
			sum += a;
		}
		return sum;
	}
};

float
tangent_area(const xrt_fov &f)
{
	return (tanf(f.angle_right) - tanf(f.angle_left)) * (tanf(f.angle_up) - tanf(f.angle_down));
}

// Hidden and visible meshes from one outline: areas, winding, and the rectangle covered once.
void
check_outline_masks(const xrt_fov &fov, xrt_vec2 centre, const std::vector<xrt_vec2> &outline, float *out_hidden_share)
{
	Mask hidden, visible, line;
	u_visibility_mask_from_outline(XRT_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH, &fov, &centre, outline.data(),
	                               (uint32_t)outline.size(), &hidden.mask);
	u_visibility_mask_from_outline(XRT_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH, &fov, &centre, outline.data(),
	                               (uint32_t)outline.size(), &visible.mask);
	u_visibility_mask_from_outline(XRT_VISIBILITY_MASK_TYPE_LINE_LOOP, &fov, &centre, outline.data(),
	                               (uint32_t)outline.size(), &line.mask);
	REQUIRE(hidden.mask != nullptr);
	REQUIRE(visible.mask != nullptr);
	REQUIRE(line.mask != nullptr);
	CHECK(hidden.mask->type == XRT_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH);
	CHECK(line.mask->index_count == outline.size());

	const float total = tangent_area(fov);
	const float h = hidden.area();
	const float v = visible.area();
	CHECK(h + v == Catch::Approx(total).epsilon(1e-4));

	// Every vertex stays inside the view's rectangle.
	for (Mask *m : {&hidden, &visible, &line}) {
		const xrt_vec2 *p = xrt_visibility_mask_get_vertices(m->mask);
		for (uint32_t i = 0; i < m->mask->vertex_count; i++) {
			CHECK(p[i].x >= tanf(fov.angle_left) - 1e-5f);
			CHECK(p[i].x <= tanf(fov.angle_right) + 1e-5f);
			CHECK(p[i].y >= tanf(fov.angle_down) - 1e-5f);
			CHECK(p[i].y <= tanf(fov.angle_up) + 1e-5f);
		}
	}
	*out_hidden_share = h / total;
}

bool
inside_outline(xrt_vec2 p, const std::vector<xrt_vec2> &poly)
{
	bool in = false;
	for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
		const xrt_vec2 a = poly[i], b = poly[j];
		if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) {
			in = !in;
		}
	}
	return in;
}

// Typical calibration: the driver's default offsets with no lens tilt, and a tilted one.
const float calib_straight[8] = {-0.09919293f, 0.f, 0.09919293f, 0.f, 1.f, 0.f, 1.f, 0.f};
const float calib_tilted[8] = {-0.0950f, 0.0040f, 0.1010f, -0.0030f, 0.99939f, 0.03490f, 0.99863f, -0.05234f};

xrt_fov
psvr2_fov(int eye)
{
	return eye == 0 ? fov_degrees(-61.5f, 43.5f, -53.f, 53.f) : fov_degrees(-43.5f, 61.5f, -53.f, 53.f);
}

} // namespace

TEST_CASE("Visibility mask from an ellipse outline")
{
	const xrt_fov fov = fov_degrees(-45, 45, -45, 45);
	for (float scale : {0.6f, 1.0f, 1.3f}) {
		CAPTURE(scale);
		std::vector<xrt_vec2> outline(64);
		xrt_vec2 centre;
		u_visibility_mask_ellipse_outline(scale, (uint32_t)outline.size(), &centre, outline.data());
		float hidden = 0;
		check_outline_masks(fov, centre, outline, &hidden);
		if (scale <= 1.0f) {
			// A 64-sided polygon inside the ellipse: a little more hidden than 1 - pi s^2 / 4.
			const float ellipse = 1.f - (float)M_PI * scale * scale / 4.f;
			CHECK(hidden >= ellipse);
			CHECK(hidden <= ellipse + 0.005f);
		} else {
			CHECK(hidden > 0.f);
			CHECK(hidden < 1.f - (float)M_PI / 4.f);
		}
	}
}

TEST_CASE("Full visibility mask hides nothing and shows the whole view")
{
	// The emulated quad-view inset uses this: an empty visible mesh leaves
	// Unreal's post-processing out of the inset, which then shows black.
	const xrt_fov fov = fov_degrees(-50, 30, -20, 40);
	Mask hidden, visible, line;
	u_visibility_mask_get_full(XRT_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH, &fov, &hidden.mask);
	u_visibility_mask_get_full(XRT_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH, &fov, &visible.mask);
	u_visibility_mask_get_full(XRT_VISIBILITY_MASK_TYPE_LINE_LOOP, &fov, &line.mask);
	REQUIRE(hidden.mask != nullptr);
	REQUIRE(visible.mask != nullptr);
	REQUIRE(line.mask != nullptr);
	CHECK(hidden.mask->index_count == 0);
	CHECK(hidden.mask->type == XRT_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH);
	CHECK(visible.mask->type == XRT_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH);
	CHECK(visible.area() == Catch::Approx(tangent_area(fov)).epsilon(1e-5));
	CHECK(line.mask->index_count == 4);
	CHECK(line.mask->vertex_count == 4);
}

TEST_CASE("Visibility mask vertices are in view space with y up")
{
	// Asymmetric so a flipped axis would show.
	const xrt_fov fov = fov_degrees(-50, 30, -20, 40);
	// Outline is a small diamond near the top of the image (v small).
	const xrt_vec2 centre = {0.5f, 0.2f};
	const std::vector<xrt_vec2> outline = {{0.6f, 0.2f}, {0.5f, 0.3f}, {0.4f, 0.2f}, {0.5f, 0.1f}};
	Mask visible;
	u_visibility_mask_from_outline(XRT_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH, &fov, &centre, outline.data(), 4,
	                               &visible.mask);
	REQUIRE(visible.mask != nullptr);
	const xrt_vec2 *v = xrt_visibility_mask_get_vertices(visible.mask);
	const float tl = tanf(fov.angle_left), tr = tanf(fov.angle_right);
	const float tu = tanf(fov.angle_up), td = tanf(fov.angle_down);
	// Vertex 4 is the centre.
	CHECK(v[4].x == Catch::Approx(tl + 0.5f * (tr - tl)));
	CHECK(v[4].y == Catch::Approx(tu - 0.2f * (tu - td)));
	CHECK(v[4].y > 0.f);
	float sum = visible.area();
	CHECK(sum > 0.f);
}

TEST_CASE("PS VR2 lens outline contains every image point the panel reads")
{
	for (const float *calib : {calib_straight, calib_tilted}) {
		for (int eye = 0; eye < 2; eye++) {
			CAPTURE(eye, calib == calib_tilted);
			std::vector<xrt_vec2> outline(96);
			xrt_vec2 centre;
			REQUIRE(psvr2_visibility_outline(calib, eye, 0.f, 0.f, (uint32_t)outline.size(), &centre,
			                                 outline.data()));
			// Brute force over the panel: inside the image, every channel's point is in the outline,
			// allowing for the straight edges between outline points.
			std::vector<xrt_vec2> grown(outline.size());
			for (size_t i = 0; i < outline.size(); i++) {
				grown[i] = {centre.x + (outline[i].x - centre.x) * 1.002f,
				            centre.y + (outline[i].y - centre.y) * 1.002f};
			}
			int outside = 0, read = 0;
			const int n = 400;
			for (int y = 0; y <= n; y++) {
				for (int x = 0; x <= n; x++) {
					xrt_uv_triplet uv;
					psvr2_compute_distortion_asymmetric((float *)calib, &uv, eye, (float)x / n,
					                                    (float)y / n);
					for (xrt_vec2 p : {uv.r, uv.g, uv.b}) {
						if (p.x < 0 || p.x > 1 || p.y < 0 || p.y > 1) {
							continue;
						}
						read++;
						outside += inside_outline(p, grown) ? 0 : 1;
					}
				}
			}
			CHECK(read > 0);
			CHECK(outside == 0);
		}
	}
}

TEST_CASE("PS VR2 lens outline rejects the fallback calibration")
{
	const float fallback[8] = {-0.09919293f, 0.f, 0.09919293f, 0.f, 0.f, 0.f, 0.f, 0.f};
	xrt_vec2 centre, outline[16];
	CHECK_FALSE(psvr2_visibility_outline(fallback, 0, 0.f, 0.f, 16, &centre, outline));
}

TEST_CASE("PS VR2 lens mask hidden share")
{
	// Shares of each eye's render the mask hides, for the doc (printed, loosely checked).
	float previous = -1;
	for (float r2 : {0.f, 1.0f, 0.9f, 0.8f, 0.7f}) {
		for (float margin : {0.f, 0.03f}) {
			float shares[2];
			for (int eye = 0; eye < 2; eye++) {
				std::vector<xrt_vec2> outline(96);
				xrt_vec2 centre;
				REQUIRE(psvr2_visibility_outline(calib_straight, eye, r2, margin,
				                                 (uint32_t)outline.size(), &centre, outline.data()));
				check_outline_masks(psvr2_fov(eye), centre, outline, &shares[eye]);
			}
			CHECK(shares[0] == Catch::Approx(shares[1]).margin(0.01));
			std::printf("r2 %.1f margin %.2f: hidden %.1f%% / %.1f%%\n", r2, margin, shares[0] * 100,
			            shares[1] * 100);
			if (margin == 0.f) {
				CHECK(shares[0] > previous);
				previous = shares[0];
			}
		}
	}
}
