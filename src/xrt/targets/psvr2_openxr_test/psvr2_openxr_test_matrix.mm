// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Controlled distance/elevation/size comparison scene layered onto the PS VR2 OpenXR diagnostic app.
 *
 * Keep the original diagnostic implementation in one place, but rename the
 * scene/render entry points while including it so this translation unit can add
 * a small controlled comparison panel without duplicating the OpenXR/Metal
 * setup code.
 */

#define initialize_scene initialize_scene_base
#define render_views render_views_base
#define render_frame render_frame_base
#define run run_base
#define main psvr2_openxr_base_main
#include "psvr2_openxr_test.mm"
#undef main
#undef run
#undef render_frame
#undef render_views
#undef initialize_scene

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
initialize_scene(diagnostic_scene &scene, const XrPosef &head_pose)
{
	initialize_scene_base(scene, head_pose);
	add_shimmer_comparison_panel(scene);
	fprintf(stderr, "psvr2-openxr-test: controlled scene now contains %zu world-locked boxes\n",
	        scene.world_instances.size());
}

static void
render_views(application &app, XrTime predicted_display_time)
{
	const XrPosef head_pose = head_pose_for_frame(app, predicted_display_time);
	if (!app.scene.initialized) {
		initialize_scene(app.scene, head_pose);
	}
	app.frame_instances = app.scene.world_instances;
	append_head_locked_cross(app.frame_instances, head_pose);
	if (app.frame_instances.size() > app.renderer.max_instances) {
		fatal("diagnostic scene exceeded Metal instance buffer capacity");
	}
	memcpy([app.renderer.instance_buffer contents], app.frame_instances.data(),
	       app.frame_instances.size() * sizeof(instance_data));

	std::vector<uint32_t> image_indices(app.swapchains.size(), 0);
	for (size_t i = 0; i < app.swapchains.size(); ++i) {
		XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		check_xr(app.xr.acquire_swapchain_image(app.swapchains[i].handle, &acquire_info, &image_indices[i]),
		         "xrAcquireSwapchainImage");
		XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wait_info.timeout = XR_INFINITE_DURATION;
		check_xr(app.xr.wait_swapchain_image(app.swapchains[i].handle, &wait_info), "xrWaitSwapchainImage");
	}

	id<MTLCommandBuffer> command_buffer = [app.command_queue commandBuffer];
	if (command_buffer == nil) {
		fatal("could not allocate Metal command buffer");
	}

	for (size_t i = 0; i < app.swapchains.size(); ++i) {
		view_swapchain &swapchain = app.swapchains[i];
		if (image_indices[i] >= swapchain.images.size()) {
			fatal("OpenXR returned an out-of-range swapchain image index");
		}
		id<MTLTexture> color_texture = (__bridge id<MTLTexture>)swapchain.images[image_indices[i]].texture;
		if (color_texture == nil) {
			fatal("OpenXR returned a nil Metal swapchain texture");
		}

		MTLRenderPassDescriptor *render_pass = [MTLRenderPassDescriptor renderPassDescriptor];
		render_pass.colorAttachments[0].texture = color_texture;
		render_pass.colorAttachments[0].loadAction = MTLLoadActionClear;
		render_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
		render_pass.colorAttachments[0].clearColor = MTLClearColorMake(0.012, 0.018, 0.024, 1.0);
		render_pass.depthAttachment.texture = swapchain.depth_texture;
		render_pass.depthAttachment.loadAction = MTLLoadActionClear;
		render_pass.depthAttachment.storeAction = MTLStoreActionDontCare;
		render_pass.depthAttachment.clearDepth = 1.0;

		id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:render_pass];
		if (encoder == nil) {
			fatal("could not create Metal render command encoder");
		}
		[encoder setRenderPipelineState:app.renderer.pipeline];
		[encoder setDepthStencilState:app.renderer.depth_state];
		[encoder setCullMode:MTLCullModeNone];
		[encoder setViewport:(MTLViewport){0.0, 0.0, (double)swapchain.width, (double)swapchain.height, 0.0, 1.0}];
		[encoder setVertexBuffer:app.renderer.cube_vertex_buffer offset:0 atIndex:0];
		[encoder setVertexBuffer:app.renderer.instance_buffer offset:0 atIndex:1];
		const matrix_float4x4 projection = projection_matrix(app.views[i].fov, 0.05f, 100.0f);
		const matrix_float4x4 view = view_matrix(app.views[i].pose);
		const matrix_float4x4 view_projection = simd_mul(projection, view);
		[encoder setVertexBytes:&view_projection length:sizeof(view_projection) atIndex:2];
		[encoder drawPrimitives:MTLPrimitiveTypeTriangle
		            vertexStart:0
		            vertexCount:sizeof(k_cube_vertices) / sizeof(k_cube_vertices[0])
		          instanceCount:app.frame_instances.size()];
		[encoder endEncoding];
	}
	[command_buffer commit];

	for (view_swapchain &swapchain : app.swapchains) {
		XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
		check_xr(app.xr.release_swapchain_image(swapchain.handle, &release_info), "xrReleaseSwapchainImage");
	}
}

static void
render_frame(application &app)
{
	XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
	XrFrameState frame_state{XR_TYPE_FRAME_STATE};
	check_xr(app.xr.wait_frame(app.session, &wait_info, &frame_state), "xrWaitFrame");
	XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
	check_xr(app.xr.begin_frame(app.session, &begin_info), "xrBeginFrame");

	bool submit_projection = false;
	XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	if (frame_state.shouldRender == XR_TRUE) {
		XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
		locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate_info.displayTime = frame_state.predictedDisplayTime;
		locate_info.space = app.app_space;
		XrViewState view_state{XR_TYPE_VIEW_STATE};
		uint32_t view_count = 0;
		check_xr(app.xr.locate_views(app.session, &locate_info, &view_state, (uint32_t)app.views.size(), &view_count,
		                             app.views.data()),
		         "xrLocateViews");
		const XrViewStateFlags required = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
		if (view_count == app.views.size() && (view_state.viewStateFlags & required) == required) {
			@autoreleasepool {
				render_views(app, frame_state.predictedDisplayTime);
			}
			for (size_t i = 0; i < app.projection_views.size(); ++i) {
				XrCompositionLayerProjectionView &projection_view = app.projection_views[i];
				projection_view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
				projection_view.pose = app.views[i].pose;
				projection_view.fov = app.views[i].fov;
				projection_view.subImage.swapchain = app.swapchains[i].handle;
				projection_view.subImage.imageRect.offset = {0, 0};
				projection_view.subImage.imageRect.extent = {(int32_t)app.swapchains[i].width,
				                                            (int32_t)app.swapchains[i].height};
				projection_view.subImage.imageArrayIndex = 0;
			}
			layer.space = app.app_space;
			layer.viewCount = (uint32_t)app.projection_views.size();
			layer.views = app.projection_views.data();
			submit_projection = true;
		}
	}

	const XrCompositionLayerBaseHeader *layers[] = {
	    reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer),
	};
	XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
	end_info.displayTime = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = submit_projection ? 1u : 0u;
	end_info.layers = submit_projection ? layers : nullptr;
	check_xr(app.xr.end_frame(app.session, &end_info), "xrEndFrame");
}

static int
run(int argc, char **argv)
{
	const char *loader_path = nullptr;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--loader") == 0 && i + 1 < argc) {
			loader_path = argv[++i];
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			fprintf(stderr,
			        "Usage: %s [--loader /path/to/libopenxr_loader.1.dylib]\n"
			        "Environment: XR_RUNTIME_JSON selects the runtime; PSVR2_OPENXR_LOADER selects the loader.\n",
			        argv[0]);
			return EXIT_SUCCESS;
		} else {
			fprintf(stderr, "Unknown argument: %s\n", argv[i]);
			return EXIT_FAILURE;
		}
	}

	application app;
	app.loader = open_openxr_loader(loader_path);
	fprintf(stderr, "psvr2-openxr-test: OpenXR loader %s\n", app.loader.path.c_str());
	load_global_xr_functions(app.loader, app.xr);
	create_instance(app);
	create_system_and_session(app);
	create_swapchains(app);

	while (!g_stop_requested && !app.exit_requested) {
		if (!poll_events(app)) {
			break;
		}
		if (!app.session_running) {
			usleep(10000);
			continue;
		}
		render_frame(app);
	}

	cleanup(app);
	return EXIT_SUCCESS;
}

int
main(int argc, char **argv)
{
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	@autoreleasepool {
		return run(argc, argv);
	}
}
