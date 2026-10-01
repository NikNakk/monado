// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Per-image Metal foveation map association tests.
 *
 * The cache calls mirror the swapchain paths: get/get_packed are the
 * xrGetFoveationMetalStateMNDX query, bind_released_image is a successful
 * xrReleaseSwapchainImage and get_image is what xrEndFrame submits.
 */

#import <Metal/Metal.h>

#include "comp_metal_foveation_cache.h"
#include "foveation/u_foveation.h"

#include "catch_amalgamated.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr uint32_t kWidth = 2048;
constexpr uint32_t kHeight = 1024;
constexpr uint32_t kImages = 3;
constexpr uint32_t kZones = XRT_METAL_FOVEATION_ZONE_COUNT;

struct MetalDevice
{
	id<MTLDevice> device = nil;

	MetalDevice()
	{
		device = MTLCreateSystemDefaultDevice();
		if (device != nil && ![device supportsRasterizationRateMapWithLayerCount:1]) {
			[device release];
			device = nil;
		}
	}
	~MetalDevice()
	{
		[device release];
	}
};

xrt_foveation_state
make_state(float left_x, float right_x, float y = 0.0f)
{
	xrt_foveation_state state = {};
	state.enabled = true;
	state.dynamic = true;
	state.eye_tracked = true;
	state.center_rate = 1.0f;
	state.middle_rate = 0.5f;
	state.peripheral_rate = 0.25f;
	state.center_half_extent = 0.15f;
	state.middle_half_extent = 0.35f;
	state.view_count = 2;
	state.views[0] = {{left_x, y}, true};
	state.views[1] = {{right_x, y}, true};
	return state;
}

xrt_foveation_state
disabled_state()
{
	xrt_foveation_state state = {};
	state.enabled = false;
	return state;
}

const xrt_metal_foveation_view_layout kSideBySide[2] = {
    {0, 0, 0, kWidth / 2, kHeight, 0},
    {1, kWidth / 2, 0, kWidth / 2, kHeight, 0},
};

bool
same_map(const xrt_metal_foveation_state &a, const xrt_metal_foveation_state &b)
{
	return a.enabled == b.enabled && a.rasterization_rate_map == b.rasterization_rate_map &&
	       a.revision == b.revision && a.physical_width == b.physical_width &&
	       a.physical_height == b.physical_height &&
	       std::memcmp(a.horizontal_rates, b.horizontal_rates, sizeof(a.horizontal_rates)) == 0 &&
	       std::memcmp(a.vertical_rates, b.vertical_rates, sizeof(a.vertical_rates)) == 0 &&
	       std::memcmp(&a.compositor_map, &b.compositor_map, sizeof(a.compositor_map)) == 0;
}

xrt_metal_foveation_state
select_packed(comp_metal_foveation_cache &cache, uint32_t layer = 0)
{
	xrt_metal_foveation_state out = {};
	REQUIRE(comp_metal_foveation_cache_get_packed(&cache, kSideBySide, 2, layer, &out) == XRT_SUCCESS);
	return out;
}

xrt_metal_foveation_state
submitted(comp_metal_foveation_cache &cache, uint32_t image, uint32_t layer = 0)
{
	xrt_metal_foveation_state out = {};
	REQUIRE(comp_metal_foveation_cache_get_image(&cache, image, layer, &out) == XRT_SUCCESS);
	return out;
}

void
release(comp_metal_foveation_cache &cache, uint32_t image)
{
	REQUIRE(comp_metal_foveation_cache_bind_released_image(&cache, image) == XRT_SUCCESS);
}

} // namespace

TEST_CASE("metal foveation cache pairs submitted images with their render maps")
{
	MetalDevice metal;
	if (metal.device == nil) {
		SKIP("Metal rasterization-rate maps are unavailable on this device");
	}

	comp_metal_foveation_cache cache = {};
	REQUIRE(comp_metal_foveation_cache_init(&cache, (__bridge void *)metal.device, kWidth, kHeight, 1, kImages));

	SECTION("sparse reuse keeps the original map and re-rendering replaces it")
	{
		// Frame N: image 0 rendered with A.
		const xrt_foveation_state state_a = make_state(-0.6f, -0.6f);
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state_a) == XRT_SUCCESS);
		const xrt_metal_foveation_state map_a = select_packed(cache);
		REQUIRE(map_a.enabled);
		release(cache, 0);

		// Gaze moves: runtime resolves B, but no new image is rendered.
		const xrt_foveation_state state_b = make_state(0.6f, 0.6f);
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state_b) == XRT_SUCCESS);
		CHECK(same_map(submitted(cache, 0), map_a));

		// Image 1 rendered with B.
		const xrt_metal_foveation_state map_b = select_packed(cache);
		REQUIRE(map_b.enabled);
		REQUIRE_FALSE(same_map(map_a, map_b));
		CHECK(std::memcmp(map_a.horizontal_rates, map_b.horizontal_rates, sizeof(map_a.horizontal_rates)) != 0);
		release(cache, 1);
		CHECK(same_map(submitted(cache, 1), map_b));
		CHECK(same_map(submitted(cache, 0), map_a));

		// Image 0 re-rendered with C replaces its association.
		const xrt_foveation_state state_c = make_state(0.0f, 0.0f, 0.7f);
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state_c) == XRT_SUCCESS);
		const xrt_metal_foveation_state map_c = select_packed(cache);
		REQUIRE_FALSE(same_map(map_c, map_a));
		REQUIRE_FALSE(same_map(map_c, map_b));
		release(cache, 0);
		CHECK(same_map(submitted(cache, 0), map_c));
		CHECK(same_map(submitted(cache, 1), map_b));

		// A is no longer referenced by anything; B (image 1) and C remain.
		CHECK(cache.live_entry_count == 2);
	}

	SECTION("an unchanged state re-selects the same immutable entry")
	{
		const xrt_foveation_state state = make_state(0.1f, -0.1f);
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state) == XRT_SUCCESS);
		const xrt_metal_foveation_state first = select_packed(cache);
		release(cache, 0);
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state) == XRT_SUCCESS);
		const xrt_metal_foveation_state second = select_packed(cache);
		release(cache, 1);
		CHECK(same_map(first, second));
		CHECK(cache.live_entry_count == 1);
	}

	SECTION("released image without a selected map is unfoveated")
	{
		CHECK_FALSE(submitted(cache, 2).enabled);
		release(cache, 2);
		CHECK_FALSE(submitted(cache, 2).enabled);

		xrt_metal_foveation_state out = {};
		CHECK(comp_metal_foveation_cache_get_image(&cache, kImages, 0, &out) == XRT_ERROR_INVALID_ARGUMENT);
		CHECK(comp_metal_foveation_cache_get_image(&cache, 0, 1, &out) == XRT_ERROR_INVALID_ARGUMENT);
		CHECK(comp_metal_foveation_cache_bind_released_image(&cache, kImages) == XRT_ERROR_INVALID_ARGUMENT);
	}

	SECTION("disabling keeps already-rendered images and stops binding new ones")
	{
		const xrt_foveation_state state_a = make_state(-0.3f, 0.3f);
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state_a) == XRT_SUCCESS);
		const xrt_metal_foveation_state map_a = select_packed(cache);
		release(cache, 0);

		const xrt_foveation_state off = disabled_state();
		REQUIRE(comp_metal_foveation_cache_set(&cache, &off) == XRT_SUCCESS);

		// Image 0 still holds compacted pixels laid out by A.
		CHECK(same_map(submitted(cache, 0), map_a));

		// An image released after disabling, without a new query, has no map.
		release(cache, 1);
		CHECK_FALSE(submitted(cache, 1).enabled);

		// Querying while disabled reports disabled and binds nothing.
		const xrt_metal_foveation_state queried = select_packed(cache);
		CHECK_FALSE(queried.enabled);
		CHECK(queried.physical_width == kWidth);
		CHECK(queried.physical_height == kHeight);
		release(cache, 0);
		CHECK_FALSE(submitted(cache, 0).enabled);
		CHECK(cache.live_entry_count == 0);

		// Re-enabling works again.
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state_a) == XRT_SUCCESS);
		const xrt_metal_foveation_state again = select_packed(cache);
		release(cache, 2);
		CHECK(submitted(cache, 2).enabled);
		CHECK(submitted(cache, 2).rasterization_rate_map == again.rasterization_rate_map);
	}

	SECTION("a failed query clears the selection")
	{
		const xrt_foveation_state state_a = make_state(-0.3f, 0.3f);
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state_a) == XRT_SUCCESS);
		(void)select_packed(cache);

		xrt_foveation_state invalid = make_state(0.5f, 0.5f);
		invalid.views[1].center_valid = false;
		REQUIRE(comp_metal_foveation_cache_set(&cache, &invalid) == XRT_SUCCESS);
		xrt_metal_foveation_state out = {};
		CHECK(comp_metal_foveation_cache_get_packed(&cache, kSideBySide, 2, 0, &out) != XRT_SUCCESS);
		release(cache, 0);
		CHECK_FALSE(submitted(cache, 0).enabled);

		// Out-of-bounds packed rectangles also fail and clear.
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state_a) == XRT_SUCCESS);
		(void)select_packed(cache);
		const xrt_metal_foveation_view_layout too_wide[2] = {
		    {0, 0, 0, kWidth / 2, kHeight, 0},
		    {1, kWidth / 2 + 1, 0, kWidth / 2, kHeight, 0},
		};
		CHECK(comp_metal_foveation_cache_get_packed(&cache, too_wide, 2, 0, &out) ==
		      XRT_ERROR_INVALID_ARGUMENT);
		release(cache, 1);
		CHECK_FALSE(submitted(cache, 1).enabled);
	}

	SECTION("a selection survives state changes until the application queries again")
	{
		const xrt_foveation_state state_a = make_state(-0.5f, 0.5f);
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state_a) == XRT_SUCCESS);
		const xrt_metal_foveation_state map_a = select_packed(cache);

		// The app renders the next image with A because it has not seen B.
		const xrt_foveation_state state_b = make_state(0.5f, -0.5f);
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state_b) == XRT_SUCCESS);
		release(cache, 0);
		CHECK(same_map(submitted(cache, 0), map_a));
	}

	SECTION("lookup entries are bounded within one revision")
	{
		const xrt_foveation_state state = make_state(0.0f, 0.0f);
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state) == XRT_SUCCESS);
		for (uint32_t i = 0; i < 3 * COMP_METAL_FOVEATION_CACHE_MAX_LOOKUP_ENTRIES; ++i) {
			const uint32_t shift = 16 * (i + 1);
			const xrt_metal_foveation_view_layout layout[2] = {
			    {0, 0, 0, kWidth / 2 - shift, kHeight, 0},
			    {1, (int32_t)(kWidth / 2), 0, kWidth / 2, kHeight, 0},
			};
			xrt_metal_foveation_state out = {};
			REQUIRE(comp_metal_foveation_cache_get_packed(&cache, layout, 2, 0, &out) == XRT_SUCCESS);
			release(cache, i % kImages);
			CHECK(cache.entry_count <= COMP_METAL_FOVEATION_CACHE_MAX_LOOKUP_ENTRIES);
			// Lookup list plus at most one entry per image.
			CHECK(cache.live_entry_count <= COMP_METAL_FOVEATION_CACHE_MAX_LOOKUP_ENTRIES + kImages);
		}
	}

	SECTION("destruction releases bound and selected entries")
	{
		const xrt_foveation_state state_a = make_state(-0.5f, 0.5f);
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state_a) == XRT_SUCCESS);
		(void)select_packed(cache);
		release(cache, 0);
		const xrt_foveation_state state_b = make_state(0.5f, -0.5f);
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state_b) == XRT_SUCCESS);
		(void)select_packed(cache);
		CHECK(cache.live_entry_count == 2);
		// destroy() asserts live_entry_count reaches zero.
	}

	comp_metal_foveation_cache_destroy(&cache);
	CHECK(cache.metal_device == nullptr);
}

TEST_CASE("metal foveation cache binds every array layer independently")
{
	MetalDevice metal;
	if (metal.device == nil) {
		SKIP("Metal rasterization-rate maps are unavailable on this device");
	}

	comp_metal_foveation_cache cache = {};
	REQUIRE(comp_metal_foveation_cache_init(&cache, (__bridge void *)metal.device, 1024, 1024, 2, kImages));

	const xrt_foveation_state state = make_state(-0.7f, 0.7f);
	REQUIRE(comp_metal_foveation_cache_set(&cache, &state) == XRT_SUCCESS);

	// Array-layer stereo: view 0 on layer 0, view 1 on layer 1.
	xrt_metal_foveation_state left = {};
	xrt_metal_foveation_state right = {};
	REQUIRE(comp_metal_foveation_cache_get(&cache, 0, 0, &left) == XRT_SUCCESS);
	REQUIRE(comp_metal_foveation_cache_get(&cache, 1, 1, &right) == XRT_SUCCESS);
	REQUIRE_FALSE(same_map(left, right));
	release(cache, 1);
	CHECK(same_map(submitted(cache, 1, 0), left));
	CHECK(same_map(submitted(cache, 1, 1), right));

	// Only layer 1 selection changes for the next image.
	const xrt_foveation_state moved = make_state(-0.7f, -0.7f);
	REQUIRE(comp_metal_foveation_cache_set(&cache, &moved) == XRT_SUCCESS);
	xrt_metal_foveation_state right_moved = {};
	REQUIRE(comp_metal_foveation_cache_get(&cache, 1, 1, &right_moved) == XRT_SUCCESS);
	release(cache, 2);
	CHECK(same_map(submitted(cache, 2, 0), left));
	CHECK(same_map(submitted(cache, 2, 1), right_moved));
	CHECK(same_map(submitted(cache, 1, 1), right));

	comp_metal_foveation_cache_destroy(&cache);
}

namespace {

float
expected_rate(const xrt_foveation_state &state,
              const xrt_metal_foveation_view_layout *views,
              uint32_t view_count,
              uint32_t sample,
              bool horizontal)
{
	const u_foveation_profile profile = {
	    "test",
	    state.center_rate,
	    state.middle_rate,
	    state.peripheral_rate,
	    state.center_half_extent,
	    state.middle_half_extent,
	};
	float rate = 0.0f;
	for (uint32_t i = 0; i < view_count; ++i) {
		const xrt_metal_foveation_view_layout &v = views[i];
		const xrt_foveation_view_state &view = state.views[v.view_index];
		float local = horizontal ? 0.5f * (view.center.x + 1.0f) : 0.5f * (1.0f - view.center.y);
		if (!horizontal && (v.flags & XRT_METAL_FOVEATION_VIEW_VERTICAL_FLIP) != 0) {
			local = 1.0f - local; // The view's top is stored at the bottom of its rect.
		}
		const float offset = horizontal ? (float)v.offset_x : (float)v.offset_y;
		const float extent = horizontal ? (float)v.width : (float)v.height;
		const float full = horizontal ? (float)kWidth : (float)kHeight;
		const float target = (offset + local * extent) / full;
		const uint32_t zone = std::min<uint32_t>(kZones - 1, (uint32_t)std::floor(target * kZones));
		const uint32_t d = sample > zone ? sample - zone : zone - sample;
		// Distances are converted back to the view's own normalized extent.
		const float view_local = ((float)d / (float)kZones) / (extent / full);
		rate = std::max(rate, u_foveation_profile_rate_for_offset(&profile, view_local));
	}
	return rate;
}

void
check_packed_layout(id<MTLDevice> device,
                    const xrt_metal_foveation_view_layout *views,
                    const xrt_foveation_state &state)
{
	comp_metal_foveation_cache cache = {};
	REQUIRE(comp_metal_foveation_cache_init(&cache, (__bridge void *)device, kWidth, kHeight, 1, 1));
	REQUIRE(comp_metal_foveation_cache_set(&cache, &state) == XRT_SUCCESS);

	xrt_metal_foveation_state out = {};
	REQUIRE(comp_metal_foveation_cache_get_packed(&cache, views, 2, 0, &out) == XRT_SUCCESS);
	REQUIRE(out.enabled);
	REQUIRE(out.sample_count == kZones);
	for (uint32_t s = 0; s < kZones; ++s) {
		CHECK(out.horizontal_rates[s] == Catch::Approx(expected_rate(state, views, 2, s, true)));
		CHECK(out.vertical_rates[s] == Catch::Approx(expected_rate(state, views, 2, s, false)));
	}

	// The compositor mapping is the rate map's own transform.
	id<MTLRasterizationRateMap> map = (id<MTLRasterizationRateMap>)out.rasterization_rate_map;
	const MTLSize physical = [map physicalSizeForLayer:0];
	CHECK(physical.width == out.physical_width);
	CHECK(physical.height == out.physical_height);
	CHECK(out.physical_width < kWidth);
	CHECK(out.compositor_map.boundary_count == XRT_FOVEATION_MAP_BOUNDARY_COUNT);
	CHECK(out.compositor_map.x[0] == Catch::Approx(0.0f).margin(1e-5));
	CHECK(out.compositor_map.x[XRT_FOVEATION_MAP_BOUNDARY_COUNT - 1] * kWidth ==
	      Catch::Approx((float)out.physical_width).margin(1.0));
	CHECK(out.compositor_map.y[XRT_FOVEATION_MAP_BOUNDARY_COUNT - 1] * kHeight ==
	      Catch::Approx((float)out.physical_height).margin(1.0));
	for (uint32_t i = 1; i < XRT_FOVEATION_MAP_BOUNDARY_COUNT; ++i) {
		CHECK(out.compositor_map.x[i] >= out.compositor_map.x[i - 1]);
		CHECK(out.compositor_map.y[i] >= out.compositor_map.y[i - 1]);
	}

	// Reconstructing from the transported samples gives the same physical size.
	MTLRasterizationRateLayerDescriptor *layer = [[MTLRasterizationRateLayerDescriptor alloc]
	    initWithSampleCount:MTLSizeMake(out.sample_count, out.sample_count, 1)
	             horizontal:out.horizontal_rates
	               vertical:out.vertical_rates];
	MTLRasterizationRateMapDescriptor *descriptor = [[MTLRasterizationRateMapDescriptor alloc] init];
	descriptor.screenSize = MTLSizeMake(kWidth, kHeight, 1);
	[descriptor setLayer:layer atIndex:0];
	id<MTLRasterizationRateMap> rebuilt = [device newRasterizationRateMapWithDescriptor:descriptor];
	REQUIRE(rebuilt != nil);
	const MTLSize rebuilt_physical = [rebuilt physicalSizeForLayer:0];
	CHECK(rebuilt_physical.width == out.physical_width);
	CHECK(rebuilt_physical.height == out.physical_height);
	[rebuilt release];
	[descriptor release];
	[layer release];

	comp_metal_foveation_cache_destroy(&cache);
}

} // namespace

TEST_CASE("packed stereo maps use view-local profile extents")
{
	MetalDevice metal;
	if (metal.device == nil) {
		SKIP("Metal rasterization-rate maps are unavailable on this device");
	}

	SECTION("equal side-by-side views")
	{
		check_packed_layout(metal.device, kSideBySide, make_state(0.0f, 0.0f));
	}

	SECTION("different sizes, arbitrary offsets and non-zero Y")
	{
		const xrt_metal_foveation_view_layout views[2] = {
		    {0, 16, 8, 1200, 900, 0},
		    {1, 1240, 100, 800, 920, 0},
		};
		check_packed_layout(metal.device, views, make_state(0.4f, -0.4f, 0.3f));
	}

	SECTION("vertically flipped views mirror the centre within their rects")
	{
		const xrt_metal_foveation_view_layout views[2] = {
		    {0, 0, 24, 1024, 1000, XRT_METAL_FOVEATION_VIEW_VERTICAL_FLIP},
		    {1, 1024, 0, 1024, 1000, XRT_METAL_FOVEATION_VIEW_VERTICAL_FLIP},
		};
		check_packed_layout(metal.device, views, make_state(0.2f, -0.2f, 0.6f));
	}

	SECTION("the flip flag is part of the map identity")
	{
		const xrt_foveation_state state = make_state(0.0f, 0.0f, 0.6f);
		comp_metal_foveation_cache cache = {};
		REQUIRE(comp_metal_foveation_cache_init(&cache, (__bridge void *)metal.device, kWidth, kHeight, 1, 1));
		REQUIRE(comp_metal_foveation_cache_set(&cache, &state) == XRT_SUCCESS);
		xrt_metal_foveation_view_layout views[2] = {kSideBySide[0], kSideBySide[1]};
		xrt_metal_foveation_state upright = {};
		REQUIRE(comp_metal_foveation_cache_get_packed(&cache, views, 2, 0, &upright) == XRT_SUCCESS);
		views[0].flags = views[1].flags = XRT_METAL_FOVEATION_VIEW_VERTICAL_FLIP;
		xrt_metal_foveation_state flipped = {};
		REQUIRE(comp_metal_foveation_cache_get_packed(&cache, views, 2, 0, &flipped) == XRT_SUCCESS);
		CHECK(upright.rasterization_rate_map != flipped.rasterization_rate_map);
		CHECK(std::memcmp(upright.vertical_rates, flipped.vertical_rates, sizeof(upright.vertical_rates)) != 0);
		CHECK(std::memcmp(upright.horizontal_rates, flipped.horizontal_rates,
		                  sizeof(upright.horizontal_rates)) == 0);

		views[0].flags = 0x80u; // Unknown bits are rejected.
		xrt_metal_foveation_state out = {};
		CHECK(comp_metal_foveation_cache_get_packed(&cache, views, 2, 0, &out) == XRT_ERROR_INVALID_ARGUMENT);
		comp_metal_foveation_cache_destroy(&cache);
	}

	SECTION("views listed out of order")
	{
		const xrt_metal_foveation_view_layout views[2] = {
		    {1, kWidth / 2, 0, kWidth / 2, kHeight, 0},
		    {0, 0, 0, kWidth / 2, kHeight, 0},
		};
		check_packed_layout(metal.device, views, make_state(-0.2f, 0.6f));
	}
}
