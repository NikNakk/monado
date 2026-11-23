// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: LGPL-2.1-or-later
#import <Metal/Metal.h>
#include <openxr/openxr.h>
#include "metal/m_metal_foveation_angular.h"
#include "catch_amalgamated.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "legacy_angular_map.inc"
TEST_CASE("Angular runtime maps match legacy descriptors and coordinates", "[foveation][angular][metal]")
{
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (!device || ![device supportsRasterizationRateMapWithLayerCount:1]) {
		[device release];
		SKIP("Metal rate-map device required");
	}
	@autoreleasepool {
		u_foveation_angular_config c = {.3f, 12, 27, 2, true, U_FOVEATION_ANGULAR_SMOOTHSTEP, 0};
		gaze.outer = c.outer;
		auto *cache = m_metal_foveation_angular_create((void *)device, &c);
		if (!u_foveation_angular_available()) {
			// Without threshold-foveation the provider is unsupported, not approximated.
			REQUIRE_FALSE(cache);
			[device release];
			return;
		}
		REQUIRE(cache);
		u_foveation_angular_target t = {
		    1024, 512, {{0, 0, 512, 512, -.9f, .8f, .85f, -.8f}, {512, 0, 512, 512, -.8f, .9f, .85f, -.8f}}};
		m_metal_foveation_angular_result out = {};
		const auto initial = t;
		for (unsigned k = 0; k < 80; ++k) {
			if (k == 20) {
				t.width = 768;
				t.height = 384;
				for (unsigned i = 0; i < 2; ++i) {
					t.views[i].x = i * 384;
					t.views[i].width = 384;
					t.views[i].height = 384;
				}
			}
			if (k == 40) {
				t = initial;
				for (unsigned i = 0; i < 2; ++i) {
					t.views[i].x = 0;
					t.views[i].width = 1024;
				}
				t.views[0].up = .7f;
			}
			if (k == 60)
				t = initial;

			bool valid = k < 40 || k > 70;
			float yaw = std::round(20 * std::sin(k * .17f) / 2) * 2,
			      pitch = std::round(12 * std::cos(k * .13f) / 2) * 2;
			REQUIRE(m_metal_foveation_angular_lookup(cache, &t, valid, yaw, pitch, true, &out));
			mw_gaze_foveation_params p = {};
			p.width = t.width;
			p.height = t.height;
			for (unsigned i = 0; i < 2; ++i) {
				auto &v = t.views[i];
				p.rects[i] = {{v.x, v.y}, {v.width, v.height}};
				p.fovs[i] = {v.left, v.right, v.up, v.down};
			}
			auto h = out.history;
			id<MTLRasterizationRateMap> legacy =
			    legacy_build(device, &p, h.have_gaze ? h.yaw : 0, h.have_gaze ? h.pitch : 0,
			                 h.have_gaze ? 12 : 20, h.have_gaze ? 27 : 35);
			REQUIRE(legacy);
			REQUIRE(std::memcmp(&legacy_recipe, &out.recipe, sizeof(legacy_recipe)) == 0);
			id<MTLRasterizationRateMap> actual = (id<MTLRasterizationRateMap>)out.map;
			auto a = [actual physicalSizeForLayer:0], b = [legacy physicalSizeForLayer:0];
			REQUIRE(a.width == b.width);
			REQUIRE(a.height == b.height);
			for (unsigned y = 0; y < 17; ++y)
				for (unsigned x = 0; x < 33; ++x) {
					MTLCoordinate2D q = {(float)x / 32 * t.width, (float)y / 16 * t.height};
					auto u = [actual mapScreenToPhysicalCoordinates:q forLayer:0],
					     v = [legacy mapScreenToPhysicalCoordinates:q forLayer:0];
					REQUIRE(u.x == v.x);
					REQUIRE(u.y == v.y);
				}
			[legacy release];
		}
		// A-B-A reuses the exact native object; full vertical FOV changes get a fresh map.
		t.width = 1100;
		REQUIRE(m_metal_foveation_angular_lookup(cache, &t, true, 0, 0, true, &out));
		void *first = out.map;
		uint32_t built = out.builds;
		REQUIRE(m_metal_foveation_angular_lookup(cache, &t, true, 20, 10, true, &out));
		REQUIRE(m_metal_foveation_angular_lookup(cache, &t, true, 0, 0, true, &out));
		REQUIRE(out.map == first);
		REQUIRE(out.builds == built + 1);
		t.views[0].up -= .05f;
		REQUIRE(m_metal_foveation_angular_lookup(cache, &t, true, 0, 0, true, &out));
		REQUIRE(out.map != first);
		// Failure preserves borrow and history; caller retains snapshots across eviction.
		id<MTLRasterizationRateMap> held = [(id<MTLRasterizationRateMap>)out.map retain];
		auto before = out;
		t.width = 0;
		REQUIRE_FALSE(m_metal_foveation_angular_lookup(cache, &t, true, 40, 40, true, &out));
		REQUIRE(out.map == before.map);
		t.width = 1024;
		m_metal_foveation_angular_destroy(cache);
		REQUIRE(held.screenSize.width == 1100);
		cache = m_metal_foveation_angular_create((void *)device, &c);
		REQUIRE(cache);
		for (unsigned i = 0; i < 256; ++i) {
			t.width = 1024 + i;
			REQUIRE(m_metal_foveation_angular_lookup(cache, &t, false, 0, 0, true, &out));
			REQUIRE(out.cache_count == i + 1);
			REQUIRE(out.builds == i + 1);
		}
		t.width = 1280;
		REQUIRE(m_metal_foveation_angular_lookup(cache, &t, false, 0, 0, true, &out));
		REQUIRE(out.cache_count == 1);
		REQUIRE(out.builds == 257);
		m_metal_foveation_angular_destroy(cache);
		REQUIRE(held.screenSize.width == 1100);
		[held release];
	}
	[device release];
}

#ifdef XRT_TEST_HAVE_THRESHOLD_FOVEATION
#include <threshold_foveation.h>
#endif

TEST_CASE("Image rate maps are published on the root texture for drivers", "[foveation][angular][metal]")
{
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	if (!device || ![device supportsRasterizationRateMapWithLayerCount:1] || !u_foveation_angular_available()) {
		[device release];
		SKIP("Metal rate-map device and threshold-foveation required");
	}
	@autoreleasepool {
		u_foveation_angular_config c = {.25f, 10, 22, 2, false, U_FOVEATION_ANGULAR_SMOOTHSTEP, 0};
		auto *cache = m_metal_foveation_angular_create((void *)device, &c);
		REQUIRE(cache);
		u_foveation_angular_target t = {
		    1024, 512, {{0, 0, 512, 512, -.9f, .8f, .85f, -.8f}, {512, 0, 512, 512, -.8f, .9f, .85f, -.8f}}};
		m_metal_foveation_angular_result out = {};
		REQUIRE(m_metal_foveation_angular_lookup(cache, &t, false, 0, 0, false, &out));
		REQUIRE(out.map);

		MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		                                                                                width:1024
		                                                                               height:512
		                                                                            mipmapped:NO];
		desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
		id<MTLTexture> image = [device newTextureWithDescriptor:desc];
		id<MTLTexture> view = [image newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
		REQUIRE(u_foveation_angular_bind_metal_texture((void *)image, out.map));
#ifdef XRT_TEST_HAVE_THRESHOLD_FOVEATION
		// A driver that imported the image, or holds a view of it, finds the same map.
		void *found = tf_metal_copy_texture_map((void *)view);
		REQUIRE(found == out.map);
		tf_metal_release(found);
#endif
		// A map for another size is refused and leaves the binding alone.
		u_foveation_angular_target small = {
		    512, 256, {{0, 0, 256, 256, -.9f, .8f, .85f, -.8f}, {256, 0, 256, 256, -.8f, .9f, .85f, -.8f}}};
		m_metal_foveation_angular_result other = {};
		REQUIRE(m_metal_foveation_angular_lookup(cache, &small, false, 0, 0, false, &other));
		REQUIRE_FALSE(u_foveation_angular_bind_metal_texture((void *)image, other.map));
		REQUIRE(u_foveation_angular_bind_metal_texture((void *)image, nullptr));
#ifdef XRT_TEST_HAVE_THRESHOLD_FOVEATION
		REQUIRE(tf_metal_copy_texture_map((void *)image) == nullptr);
#endif
#ifdef XRT_TEST_HAVE_THRESHOLD_FOVEATION
		// Level 3: the per-view profile behind the map, for drivers that foveate their
		// own targets. Two eyes side by side, gaze held straight ahead.
		REQUIRE(u_foveation_angular_bind_metal_texture((void *)image, out.map));
		u_foveation_angular_history held = {0, 0, 0, true};
		REQUIRE(u_foveation_angular_publish_profile(&c, &t, 2, &held, 7, (void *)image));
		tf_image_profile profile = {};
		profile.struct_size = sizeof(profile);
		REQUIRE(tf_metal_copy_texture_profile((void *)view, &profile) == 0);
		REQUIRE(profile.revision == 7);
		REQUIRE(profile.view_count == 2);
		REQUIRE(profile.sample_count == TF_IMAGE_PROFILE_SAMPLES);
		REQUIRE(profile.views[1].x == 512);
		REQUIRE(profile.views[1].width == 512);
		for (unsigned v = 0; v < 2; v++) {
			const auto &pv = profile.views[v];
			float peak = 0, low = 1;
			for (unsigned i = 0; i < profile.sample_count; i++) {
				peak = std::max(peak, pv.horizontal[i]);
				low = std::min(low, pv.horizontal[i]);
			}
			// Full rate around the centre of each eye, the peripheral rate at its edges.
			REQUIRE(peak == 1.f);
			REQUIRE(low == Catch::Approx(c.outer));
			REQUIRE(pv.vertical[0] == Catch::Approx(c.outer));
		}
		// The same profile is the process's current one (level 4), and a NULL texture
		// publishes only that.
		tf_image_profile process = {};
		process.struct_size = sizeof(process);
		REQUIRE(tf_copy_process_profile(&process) == 0);
		REQUIRE(process.revision == 7);
		REQUIRE(u_foveation_angular_publish_profile(&c, &t, 2, &held, 8, nullptr));
		REQUIRE(tf_copy_process_profile(&process) == 0);
		REQUIRE(process.revision == 8);
		REQUIRE(process.views[1].x == 512);
		// Clearing the map clears its profile.
		REQUIRE(u_foveation_angular_bind_metal_texture((void *)image, nullptr));
		REQUIRE(tf_metal_copy_texture_profile((void *)image, &profile) == 1);
#endif
		REQUIRE_FALSE(u_foveation_angular_bind_metal_texture(nullptr, nullptr));
		[view release];
		[image release];
		m_metal_foveation_angular_destroy(cache);
	}
	[device release];
}
