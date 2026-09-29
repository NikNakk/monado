// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Controlled distance/elevation/size comparison scene layered onto the PS VR2 OpenXR diagnostic app.
 *
 * The base diagnostic owns the OpenXR lifecycle and render loop. This wrapper
 * only installs a scene augmentation callback, so new action, foveation,
 * swapchain, depth, and rendering plumbing automatically stays in sync.
 */

#define main psvr2_openxr_base_main
#include "psvr2_openxr_test.mm"
#undef main

static float
comparison_target_size(float distance, float angular_size_degrees)
{
	const float half_angle = angular_size_degrees * (float)M_PI / 360.0f;
	return 2.0f * distance * tanf(half_angle);
}

static void
add_comparison_target(diagnostic_scene &scene,
                      float azimuth_degrees,
                      float elevation_degrees,
                      float distance,
                      float angular_size_degrees,
                      simd_float4 color)
{
	const float azimuth = azimuth_degrees * (float)M_PI / 180.0f;
	const float elevation = elevation_degrees * (float)M_PI / 180.0f;
	const float horizontal_distance = cosf(elevation) * distance;
	const float x = sinf(azimuth) * horizontal_distance;
	const float y = sinf(elevation) * distance;
	const float z = cosf(azimuth) * horizontal_distance;
	const float size = comparison_target_size(distance, angular_size_degrees);
	add_world_box(scene, x, y, z, make_float3(size, size, size), color);
}

static void
add_shimmer_comparison_panel(diagnostic_scene &scene)
{
	const simd_float4 depth_color = make_float4(0.20f, 0.95f, 0.36f, 1.0f);
	const simd_float4 elevation_color = make_float4(0.15f, 0.82f, 1.0f, 1.0f);
	const simd_float4 size_color = make_float4(1.0f, 0.38f, 0.18f, 1.0f);

	/*
	 * DEPTH TEST — bright green, elevation 0 degrees, constant 2-degree
	 * apparent size. Each distance appears once on each side, with the order
	 * reversed, so distance is not systematically coupled to azimuth.
	 */
	const std::array<float, 4> distances = {0.75f, 1.5f, 3.0f, 6.0f};
	const std::array<float, 4> left_azimuths = {-32.0f, -22.0f, -12.0f, -2.0f};
	const std::array<float, 4> right_azimuths = {2.0f, 12.0f, 22.0f, 32.0f};
	for (size_t i = 0; i < distances.size(); ++i) {
		add_comparison_target(scene, left_azimuths[i], 0.0f, distances[i], 2.0f, depth_color);
		add_comparison_target(scene, right_azimuths[i], 0.0f, distances[distances.size() - 1 - i], 2.0f,
		                      depth_color);
	}

	/*
	 * ELEVATION TEST — cyan, fixed 2.4 m distance and constant 2-degree size.
	 * Identical vertical columns at +/-42 degrees azimuth isolate elevation
	 * while also giving a left/right control.
	 */
	const std::array<float, 4> elevations = {0.0f, 15.0f, 30.0f, 45.0f};
	for (float elevation : elevations) {
		add_comparison_target(scene, -42.0f, elevation, 2.4f, 2.0f, elevation_color);
		add_comparison_target(scene, 42.0f, elevation, 2.4f, 2.0f, elevation_color);
	}

	/*
	 * PROJECTED-SIZE TEST — orange, fixed 2.4 m distance and -18-degree
	 * elevation. Sizes are mirrored left/right so angular size is not coupled
	 * to one particular azimuth. If only the smallest targets shimmer, that
	 * strongly favours raster/distortion resampling rather than pose error.
	 */
	const std::array<float, 4> angular_sizes = {0.75f, 1.5f, 3.0f, 6.0f};
	const std::array<float, 4> size_left_azimuths = {-34.0f, -24.0f, -14.0f, -4.0f};
	const std::array<float, 4> size_right_azimuths = {4.0f, 14.0f, 24.0f, 34.0f};
	for (size_t i = 0; i < angular_sizes.size(); ++i) {
		add_comparison_target(scene, size_left_azimuths[i], -18.0f, 2.4f, angular_sizes[i], size_color);
		add_comparison_target(scene, size_right_azimuths[i], -18.0f, 2.4f,
		                      angular_sizes[angular_sizes.size() - 1 - i], size_color);
	}

	fprintf(stderr,
	        "psvr2-openxr-test: shimmer panel added — green=depth (0.75/1.5/3/6m at 0deg), "
	        "cyan=elevation (0/15/30/45deg at 2.4m), orange=size (0.75/1.5/3/6deg at 2.4m)\n");
}

static void
add_controlled_comparison_scene(diagnostic_scene &scene)
{
	add_shimmer_comparison_panel(scene);
	fprintf(stderr, "psvr2-openxr-test: controlled scene now contains %zu world-locked boxes\n",
	        scene.world_instances.size());
}

int
main(int argc, char **argv)
{
	g_diagnostic_scene_augment = add_controlled_comparison_scene;
	return psvr2_openxr_base_main(argc, argv);
}
