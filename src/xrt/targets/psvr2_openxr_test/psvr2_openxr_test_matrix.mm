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

static bool g_freeze_frame = false;
static bool g_freeze_frame_ready = false;

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
initialize_reprojection_scene(diagnostic_scene &scene, const XrPosef &head_pose)
{
	// Keep this scene intentionally sparse: the purpose is to isolate
	// positional reprojection and disocclusion behaviour, not exercise the
	// general 360-degree diagnostic world.
	scene.origin = xr_position(head_pose.position);
	scene.up = make_float3(0.0f, 1.0f, 0.0f);
	scene.forward = rotate_vector(head_pose.orientation, make_float3(0.0f, 0.0f, -1.0f));
	scene.forward.y = 0.0f;
	if (simd_length(scene.forward) < 0.001f) {
		scene.forward = make_float3(0.0f, 0.0f, -1.0f);
	} else {
		scene.forward = simd_normalize(scene.forward);
	}
	scene.right = simd_normalize(simd_cross(scene.forward, scene.up));
	scene.world_instances.clear();

	const simd_float4 wall_color = make_float4(0.08f, 0.11f, 0.14f, 1.0f);
	const simd_float4 marker_color = make_float4(0.70f, 0.74f, 0.78f, 1.0f);
	const simd_float4 near_color = make_float4(0.20f, 0.95f, 0.36f, 1.0f);
	const simd_float4 mid_color = make_float4(1.0f, 0.38f, 0.18f, 1.0f);
	const simd_float4 far_color = make_float4(0.15f, 0.82f, 1.0f, 1.0f);
	const simd_float4 thin_color = make_float4(1.0f, 0.16f, 0.62f, 1.0f);
	const simd_float4 volume_color = make_float4(0.72f, 0.42f, 0.96f, 1.0f);

	// Continuous background at 6 m. The wall ensures a translated view reveals
	// known background content instead of the swapchain clear colour.
	add_world_box(scene, 0.0f, 0.0f, 6.0f, make_float3(4.5f, 2.6f, 0.05f), wall_color);

	// Sparse markers just in front of the wall make background correspondence
	// obvious without creating another dense field of foreground silhouettes.
	for (int y = -2; y <= 2; ++y) {
		for (int x = -4; x <= 4; ++x) {
			if ((x + y) % 2 == 0) {
				add_world_box(scene, 0.65f * (float)x, 0.45f * (float)y, 5.88f,
				              make_float3(0.055f, 0.055f, 0.025f), marker_color);
			}
		}
	}

	// Three constant-angular-size foreground cards. Keep them almost planar so
	// every surface needed for lateral reprojection is already present in the
	// frozen source frame. The previous full cubes were a poor correctness test:
	// lateral motion legitimately reveals side faces that a single-layer
	// colour+depth frame never captured.
	const std::array<float, 3> distances = {0.85f, 1.6f, 3.0f};
	const std::array<float, 3> xs = {-0.42f, 0.0f, 0.58f};
	const std::array<simd_float4, 3> colors = {near_color, mid_color, far_color};
	for (size_t i = 0; i < distances.size(); ++i) {
		const float size = comparison_target_size(distances[i], 7.0f);
		add_world_box(scene, xs[i], -0.05f, distances[i],
		              make_float3(size, size, 0.008f), colors[i]);
	}

	// One deliberately volumetric control. This one is expected to become
	// incomplete under sufficiently large frozen-frame translation because
	// hidden side faces are absent from a single submitted depth layer.
	add_world_box(scene, 0.95f, -0.62f, 1.8f,
	              make_float3(0.18f, 0.18f, 0.18f), volume_color);

	// One thin world-locked target is useful for revealing sub-pixel/edge
	// disagreement, but unlike the old magenta cross it does not move with the
	// head and therefore does not confound frozen-frame reprojection.
	add_world_box(scene, 0.05f, 0.52f, 2.2f, make_float3(0.32f, 0.012f, 0.018f), thin_color);
	add_world_box(scene, 0.05f, 0.52f, 2.2f, make_float3(0.012f, 0.32f, 0.018f), thin_color);

	scene.initialized = true;
	fprintf(stderr,
	        "psvr2-openxr-test: minimal frozen-frame reprojection scene contains %zu world-locked boxes "
	        "(green/orange/cyan=planar cards, purple=volumetric control)\n",
	        scene.world_instances.size());
}

static void
initialize_scene(diagnostic_scene &scene, const XrPosef &head_pose)
{
	if (g_freeze_frame) {
		initialize_reprojection_scene(scene, head_pose);
		return;
	}

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
	if (!g_freeze_frame) {
		append_head_locked_cross(app.frame_instances, head_pose);
	}
	if (app.frame_instances.size() > app.renderer.max_instances) {
		fatal("diagnostic scene exceeded Metal instance buffer capacity");
	}
	memcpy([app.renderer.instance_buffer contents], app.frame_instances.data(),
	       app.frame_instances.size() * sizeof(instance_data));

	std::vector<uint32_t> image_indices(app.swapchains.size(), 0);
	std::vector<uint32_t> depth_image_indices(app.swapchains.size(), 0);
	for (size_t i = 0; i < app.swapchains.size(); ++i) {
		XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		check_xr(app.xr.acquire_swapchain_image(app.swapchains[i].handle, &acquire_info, &image_indices[i]),
		         "xrAcquireSwapchainImage");
		XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wait_info.timeout = XR_INFINITE_DURATION;
		check_xr(app.xr.wait_swapchain_image(app.swapchains[i].handle, &wait_info), "xrWaitSwapchainImage");

		if (app.submit_depth_layer) {
			check_xr(app.xr.acquire_swapchain_image(app.swapchains[i].depth_handle, &acquire_info,
			                                         &depth_image_indices[i]),
			         "xrAcquireSwapchainImage(depth)");
			check_xr(app.xr.wait_swapchain_image(app.swapchains[i].depth_handle, &wait_info),
			         "xrWaitSwapchainImage(depth)");
		}
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
		id<MTLTexture> depth_texture = swapchain.depth_texture;
		if (app.submit_depth_layer) {
			if (depth_image_indices[i] >= swapchain.depth_images.size()) {
				fatal("OpenXR returned an out-of-range depth swapchain image index");
			}
			depth_texture = (__bridge id<MTLTexture>)swapchain.depth_images[depth_image_indices[i]].texture;
			if (depth_texture == nil) {
				fatal("OpenXR returned a nil Metal depth swapchain texture");
			}
		}
		render_pass.depthAttachment.texture = depth_texture;
		render_pass.depthAttachment.loadAction = MTLLoadActionClear;
		render_pass.depthAttachment.storeAction =
		    app.submit_depth_layer ? MTLStoreActionStore : MTLStoreActionDontCare;
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
		if (app.submit_depth_layer) {
			check_xr(app.xr.release_swapchain_image(swapchain.depth_handle, &release_info),
			         "xrReleaseSwapchainImage(depth)");
		}
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
			const bool render_new_source = !g_freeze_frame || !g_freeze_frame_ready;
			if (render_new_source) {
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

					if (app.submit_depth_layer) {
						XrCompositionLayerDepthInfoKHR &depth_info = app.depth_infos[i];
						depth_info = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
						depth_info.subImage.swapchain = app.swapchains[i].depth_handle;
						depth_info.subImage.imageRect.offset = {0, 0};
						depth_info.subImage.imageRect.extent = {(int32_t)app.swapchains[i].width,
						                                          (int32_t)app.swapchains[i].height};
						depth_info.subImage.imageArrayIndex = 0;
						depth_info.minDepth = 0.0f;
						depth_info.maxDepth = 1.0f;
						depth_info.nearZ = 0.05f;
						depth_info.farZ = 100.0f;
						projection_view.next = &depth_info;
					}
				}
				if (g_freeze_frame) {
					g_freeze_frame_ready = true;
					fprintf(stderr,
					        "psvr2-openxr-test: source frame frozen; keep translating/rotating the HMD while the "
					        "same submitted colour/depth frame is reused\n");
				}
			}

			if (!g_freeze_frame || g_freeze_frame_ready) {
				layer.space = app.app_space;
				layer.viewCount = (uint32_t)app.projection_views.size();
				layer.views = app.projection_views.data();
				submit_projection = true;
			}
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
	bool submit_depth_layer = false;
	bool freeze_frame = false;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--loader") == 0 && i + 1 < argc) {
			loader_path = argv[++i];
		} else if (strcmp(argv[i], "--depth-layer") == 0) {
			submit_depth_layer = true;
		} else if (strcmp(argv[i], "--freeze-frame") == 0) {
			freeze_frame = true;
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			fprintf(stderr,
			        "Usage: %s [--loader /path/to/libopenxr_loader.1.dylib] [--depth-layer] [--freeze-frame]\n"
			        "  --depth-layer submits the rendered Depth32Float attachment through "
			        "XR_KHR_composition_layer_depth.\n"
			        "  --freeze-frame renders/releases one source frame, then keeps submitting its fixed "
			        "poses and swapchain images for reprojection testing.\n"
			        "Environment: XR_RUNTIME_JSON selects the runtime; PSVR2_OPENXR_LOADER selects the loader.\n",
			        argv[0]);
			return EXIT_SUCCESS;
		} else {
			fprintf(stderr, "Unknown argument: %s\n", argv[i]);
			return EXIT_FAILURE;
		}
	}

	g_freeze_frame = freeze_frame;
	g_freeze_frame_ready = false;

	application app;
	app.submit_depth_layer = submit_depth_layer;
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
