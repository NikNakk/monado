# Copyright 2018-2024, Collabora, Ltd.
# Copyright 2024-2026, NVIDIA CORPORATION.
#
# SPDX-License-Identifier: BSL-1.0

set(tests
    tests_comp_refresh_request
    tests_present_feedback_correction
    tests_cxx_wrappers
    tests_macos_display_timing
    tests_macos_presenter
    tests_deque
    tests_desktop_pose
    tests_floor_calibration
    tests_generic_callbacks
    tests_foveation
    tests_glue_device
    tests_glue_traits
    tests_history_buf
    tests_id_ringbuffer
    tests_json
    tests_lowpass_float
    tests_lowpass_integer
    tests_pacing
    tests_passthrough_calibration
    tests_quatexpmap
    tests_quat_change_of_basis
    tests_quat_swing_twist
    tests_rational
    tests_relation_chain
    tests_space_overseer
    tests_vector
    tests_worker
    tests_pose
    tests_vec3_angle
	)
if(NOT MSVC)
	list(APPEND tests tests_frame_share tests_psvr2_tracking_share)
endif()
if(XRT_HAVE_D3D11)
	list(APPEND tests tests_aux_d3d_d3d11 tests_comp_client_d3d11)
endif()
if(XRT_HAVE_D3D12)
	list(APPEND tests tests_aux_d3d_d3d12 tests_comp_client_d3d12)
endif()
if(XRT_HAVE_VULKAN)
	# The generic Vulkan client test only implements FD, Android hardware
	# buffer, and Win32 external handles. macOS uses the IOSurface/Metal path
	# and has dedicated macOS Vulkan/OpenXR probes below instead.
	if(NOT APPLE)
		list(APPEND tests tests_comp_client_vulkan)
	endif()
	list(APPEND tests tests_uv_to_tangent tests_render_pipeline_cache)
endif()
if(XRT_FEATURE_OPENXR)
	list(
		APPEND
		tests
		tests_input_transform
		tests_generic_controller
		tests_foveation_openxr
		tests_oxr_layer_color_scale_bias
		)
endif()
if(XRT_HAVE_OPENGL
   AND XRT_HAVE_OPENGL_GLX
   AND XRT_HAVE_SDL2
   AND (WIN32 OR VK_USE_PLATFORM_XLIB_XRANDR_EXT)
	)
	set(_have_opengl_test ON)
	list(APPEND tests tests_comp_client_opengl)
endif()
if(XRT_BUILD_DRIVER_HANDTRACKING)
	list(APPEND tests tests_levenbergmarquardt)
endif()

if(TARGET constellation)
	list(
		APPEND
		tests
		tests_pose_metrics
		tests_led_phase_bootstrap
		tests_imu_optical_filter
		tests_joint_pose_solver
		tests_constellation_dataset
		)
endif()
if(TARGET constellation_fusion)
	list(APPEND tests tests_pssense_fusion)
endif()

foreach(testname ${tests})
	add_executable(${testname} ${testname}.cpp)
	target_link_libraries(${testname} PRIVATE xrt-external-catch2)
	target_link_libraries(${testname} PRIVATE aux_util)
	add_test(NAME ${testname} COMMAND ${testname} --success --allow-running-no-tests)
	set_target_properties(${testname} PROPERTIES FOLDER monado_tests)
endforeach()

if(APPLE)
	target_sources(tests_macos_presenter PRIVATE ../src/xrt/compositor/main/comp_macos_presenter.c)
	foreach(mode native wsi wsi-adaptive invalid)
		add_test(NAME tests_macos_presenter_${mode} COMMAND tests_macos_presenter "[presenter-runtime]")
		set_tests_properties(tests_macos_presenter_${mode} PROPERTIES
			ENVIRONMENT "XRT_MACOS_PRESENTER=${mode};XRT_MACOS_WSI_GENERIC=1;XRT_MACOS_WSI_GENERIC_CV=1")
	endforeach()
endif()

if(UNIX
   AND NOT ANDROID
   AND TARGET ipc_shared
	)
	add_executable(tests_ipc_shmem tests_ipc_shmem.cpp)
	target_link_libraries(tests_ipc_shmem PRIVATE ipc_shared aux_util xrt-external-catch2)
	add_test(NAME tests_ipc_shmem COMMAND tests_ipc_shmem)
	set_target_properties(tests_ipc_shmem PROPERTIES FOLDER monado_tests)
endif()
if(UNIX AND NOT MSVC)
	add_executable(tests_macos_hosted_client tests_macos_hosted_client.cpp)
	target_link_libraries(tests_macos_hosted_client PRIVATE aux_util xrt-external-catch2)
	add_test(NAME tests_macos_hosted_client COMMAND tests_macos_hosted_client)
	set_target_properties(tests_macos_hosted_client PROPERTIES FOLDER monado_tests)
	add_executable(tests_time_clock tests_time_clock.cpp)
	target_link_libraries(tests_time_clock PRIVATE aux_util xrt-external-catch2)
	add_test(NAME tests_time_clock COMMAND tests_time_clock)
	set_target_properties(tests_time_clock PROPERTIES FOLDER monado_tests)
endif()

# For tests that require more than just aux_util, link those other libs down here.

target_link_libraries(tests_cxx_wrappers PRIVATE xrt-interfaces)
target_link_libraries(tests_glue_device PRIVATE xrt-interfaces aux_glue)
target_link_libraries(tests_foveation PRIVATE aux_foveation)
target_link_libraries(tests_glue_traits PRIVATE xrt-interfaces aux_glue)
target_link_libraries(tests_history_buf PRIVATE aux_math)
target_link_libraries(tests_lowpass_float PRIVATE aux_math)
target_link_libraries(tests_lowpass_integer PRIVATE aux_math)
target_link_libraries(tests_quatexpmap PRIVATE xrt-external-tinyceres aux_math)
target_link_libraries(tests_rational PRIVATE aux_math)
target_link_libraries(tests_relation_chain PRIVATE aux_math)
target_link_libraries(tests_space_overseer PRIVATE xrt-interfaces base aux_math aux_os)
target_link_libraries(tests_floor_calibration PRIVATE xrt-interfaces base aux_math aux_os)
target_link_libraries(tests_pose PRIVATE aux_math)
if(NOT MSVC)
	target_link_libraries(tests_psvr2_tracking_share PRIVATE aux_tracking)
endif()
target_link_libraries(tests_quat_change_of_basis PRIVATE aux_math)
target_link_libraries(tests_quat_swing_twist PRIVATE aux_math)
target_link_libraries(tests_vec3_angle PRIVATE aux_math)

if(XRT_BUILD_DRIVER_HANDTRACKING)
	target_link_libraries(
		tests_levenbergmarquardt
		PRIVATE
			aux_math
			t_ht_mercury_includes
			t_ht_mercury_kine_lm_includes
			t_ht_mercury
			t_ht_mercury_kine_lm
		)
endif()

if(XRT_HAVE_D3D11)
	target_link_libraries(tests_aux_d3d_d3d11 PRIVATE aux_d3d)
	target_link_libraries(tests_comp_client_d3d11 PRIVATE comp_client comp_mock)
endif()

if(XRT_HAVE_D3D12)
	target_link_libraries(tests_aux_d3d_d3d12 PRIVATE aux_d3d)
	target_link_libraries(tests_comp_client_d3d12 PRIVATE comp_client comp_mock)
endif()

if(XRT_HAVE_VULKAN)
	if(NOT APPLE)
		target_link_libraries(
			tests_comp_client_vulkan PRIVATE comp_client comp_mock comp_util aux_vk
			)
	endif()
	target_link_libraries(tests_uv_to_tangent PRIVATE aux_render)
	target_link_libraries(
		tests_render_pipeline_cache
		PRIVATE
			aux_render_cache
			comp_util
			aux_vk
			aux_shaders
			xrt-interfaces
		)
	get_target_property(_aux_shaders_binary_dir aux_shaders BINARY_DIR)
	target_include_directories(
		tests_render_pipeline_cache SYSTEM PRIVATE ${_aux_shaders_binary_dir}
		)
	add_dependencies(tests_render_pipeline_cache aux_shaders render_shader_specialization)
	# Compile workgroup variants from the production shader; runtime defaults
	# and specialization keys stay unchanged until hardware measurements justify it.
	set(_distortion_source "${CMAKE_SOURCE_DIR}/src/xrt/auxiliary/render/shaders/distortion.comp")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_distortion_source}")
	file(READ "${_distortion_source}" _distortion_glsl)
	string(FIND "${_distortion_glsl}" "local_size_x = 8, local_size_y = 8" _group_declaration)
	if(_group_declaration EQUAL -1)
		message(FATAL_ERROR "Update distortion benchmark variants for the shader workgroup declaration")
	endif()
	set(_distortion_bench_headers)
	foreach(_group 8x8 16x8 16x16)
		string(REPLACE "x" ";" _dims "${_group}")
		list(GET _dims 0 _x)
		list(GET _dims 1 _y)
		string(REPLACE "local_size_x = 8, local_size_y = 8"
		       "local_size_x = ${_x}, local_size_y = ${_y}" _variant "${_distortion_glsl}")
		set(_shader "${CMAKE_CURRENT_BINARY_DIR}/distortion_${_group}.comp")
		file(CONFIGURE OUTPUT "${_shader}" CONTENT "${_variant}" @ONLY)
		set(_header "${_shader}.h")
		add_custom_command(OUTPUT "${_header}"
			COMMAND ${GLSLANGVALIDATOR_COMMAND} -V --target-env spirv1.0
				-I${CMAKE_SOURCE_DIR}/src/xrt/auxiliary/render/shaders
				"${_shader}" --vn distortion_${_group} -o "${_header}"
			DEPENDS "${_shader}" "${CMAKE_SOURCE_DIR}/src/xrt/auxiliary/render/shaders/srgb.inc.glsl")
		list(APPEND _distortion_bench_headers "${_header}")
	endforeach()
	add_executable(
		tests_render_compute
		tests_render_compute.cpp
		tests_render_camera.cpp
		tests_render_output.cpp
		tests_projection_quad.cpp
		tests_layer_specialization.cpp
		${_distortion_bench_headers}
		)
	target_link_libraries(tests_render_compute PRIVATE aux_render aux_render_cache comp_util aux_vk aux_shaders xrt-external-catch2)
	target_include_directories(tests_render_compute SYSTEM PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${_aux_shaders_binary_dir})
	add_dependencies(tests_render_compute aux_shaders render_shader_specialization)
	add_test(NAME tests_render_compute COMMAND tests_render_compute)
	set_tests_properties(tests_render_compute PROPERTIES TIMEOUT 60 LABELS "gpu")
	if(APPLE)
		target_sources(tests_render_compute PRIVATE tests_macos_metal_device.mm)
		target_link_libraries(tests_render_compute PRIVATE "-framework Metal")
	endif()

	if(APPLE)
		add_executable(tests_macos_passthrough_rotation tests_macos_passthrough_rotation.mm)
		target_compile_features(tests_macos_passthrough_rotation PRIVATE cxx_std_17)
		target_link_libraries(
			tests_macos_passthrough_rotation
			PRIVATE
				aux_util
				aux_math
				xrt-external-catch2
				"-framework Metal"
				"-framework Foundation"
			)
		add_test(
			NAME tests_macos_passthrough_rotation
			COMMAND tests_macos_passthrough_rotation
			)
		set_target_properties(
			tests_macos_passthrough_rotation PROPERTIES FOLDER monado_tests
			)
		set_tests_properties(tests_macos_passthrough_rotation PROPERTIES SKIP_RETURN_CODE 4)
	endif()

	if(APPLE)
		target_sources(tests_render_pipeline_cache PRIVATE tests_macos_metal_device.mm)
		target_link_libraries(tests_render_pipeline_cache PRIVATE "-framework Metal")
	endif()
endif()

if(XRT_FEATURE_OPENXR)
	target_link_libraries(tests_oxr_layer_color_scale_bias PRIVATE st_oxr xrt-interfaces xrt-external-openxr)
	target_link_libraries(
		tests_input_transform PRIVATE st_oxr xrt-interfaces xrt-external-openxr
		)
	target_link_libraries(
		tests_generic_controller PRIVATE st_oxr xrt-interfaces xrt-external-openxr
		)
	add_dependencies(tests_generic_controller generate_oxr_bindings)
	target_link_libraries(
		tests_foveation_openxr PRIVATE st_oxr aux_foveation xrt-interfaces
					       xrt-external-openxr
		)
endif()
if(_have_opengl_test)
	target_link_libraries(
		tests_comp_client_opengl PRIVATE comp_client comp_mock aux_ogl SDL2::SDL2
		)
	target_include_directories(tests_comp_client_opengl PRIVATE SDL2::SDL2)
endif()

if(XRT_HAVE_VULKAN AND XRT_HAVE_D3D11)
	target_link_libraries(tests_aux_d3d_d3d11 PRIVATE comp_util aux_vk)
endif()

if(XRT_HAVE_VULKAN AND XRT_HAVE_D3D12)
	target_link_libraries(tests_aux_d3d_d3d12 PRIVATE comp_util aux_vk)
endif()

if(APPLE)
	add_executable(tests_macos_present_trace tests_macos_present_trace.cpp)
	target_link_libraries(tests_macos_present_trace PRIVATE aux_util xrt-external-catch2)
	add_test(NAME tests_macos_present_trace COMMAND tests_macos_present_trace)
	set_tests_properties(tests_macos_present_trace PROPERTIES ENVIRONMENT "XRT_MACOS_SIGNPOST=0")
	set_target_properties(tests_macos_present_trace PROPERTIES FOLDER monado_tests)
	add_executable(tests_macos_runtime_probe tests_macos_runtime_probe.c)
	target_link_libraries(
		tests_macos_runtime_probe PRIVATE target_instance xrt-interfaces aux_util
		)
	set_target_properties(tests_macos_runtime_probe PROPERTIES FOLDER monado_tests)
endif()

if(APPLE AND TARGET comp_metal_foveation_cache)
	# Per-image Metal foveation map association. Skips itself when the Metal
	# device cannot create rasterization-rate maps.
	add_executable(tests_metal_foveation_cache tests_metal_foveation_cache.mm)
	target_compile_features(tests_metal_foveation_cache PRIVATE cxx_std_17)
	target_link_libraries(
		tests_metal_foveation_cache
		PRIVATE
			comp_metal_foveation_cache
			aux_foveation
			aux_util
			xrt-external-catch2
			"-framework Metal"
		)
	add_test(
		NAME tests_metal_foveation_cache
		COMMAND tests_metal_foveation_cache --success --allow-running-no-tests
		)
	set_target_properties(tests_metal_foveation_cache PROPERTIES FOLDER monado_tests)
endif()

if(APPLE)
	add_executable(tests_macos_signpost tests_macos_signpost.c)
	target_link_libraries(tests_macos_signpost PRIVATE aux_util)
	add_test(NAME tests_macos_signpost COMMAND tests_macos_signpost)
	set_tests_properties(tests_macos_signpost PROPERTIES ENVIRONMENT "XRT_MACOS_SIGNPOST=0")
	set_target_properties(tests_macos_signpost PROPERTIES FOLDER monado_tests)
	add_executable(tests_macos_latency_guard tests_macos_latency_guard.cpp)
	target_link_libraries(tests_macos_latency_guard PRIVATE comp_util xrt-external-catch2)
	add_test(NAME tests_macos_latency_guard COMMAND tests_macos_latency_guard)
	set_target_properties(tests_macos_latency_guard PROPERTIES FOLDER monado_tests)
	add_executable(tests_macos_gpu_policy tests_macos_gpu_policy.cpp)
	target_link_libraries(tests_macos_gpu_policy PRIVATE comp_multi comp_util aux_util aux_vk xrt-external-catch2)
	add_test(NAME tests_macos_gpu_policy COMMAND tests_macos_gpu_policy)
	set_target_properties(tests_macos_gpu_policy PROPERTIES FOLDER monado_tests)
	add_executable(tests_macos_gpu_reuse tests_macos_gpu_reuse.cpp)
	target_compile_features(tests_macos_gpu_reuse PRIVATE cxx_std_17)
	target_link_libraries(tests_macos_gpu_reuse PRIVATE comp_util aux_util aux_vk xrt-interfaces xrt-external-catch2)
	add_test(NAME tests_macos_gpu_reuse COMMAND tests_macos_gpu_reuse)
	set_target_properties(tests_macos_gpu_reuse PROPERTIES FOLDER monado_tests)
	add_executable(tests_macos_gpu_sync tests_macos_gpu_sync.mm)
	target_compile_features(tests_macos_gpu_sync PRIVATE cxx_std_17)
	target_link_libraries(tests_macos_gpu_sync PRIVATE Vulkan::Vulkan "-framework Foundation" "-framework Metal" "-framework IOSurface")
	foreach(scenario normal delayed missing future nonmonotonic exit-before exit-after consumer-delay multi-queue broken-queue reuse idle cancel-callback exit-inflight consumer-gpu-delay concurrent-producers)
		add_test(NAME tests_macos_gpu_sync_${scenario} COMMAND tests_macos_gpu_sync ${scenario})
		set_tests_properties(tests_macos_gpu_sync_${scenario} PROPERTIES TIMEOUT 30 LABELS "macos;gpu")
	endforeach()
	set_target_properties(tests_macos_gpu_sync PROPERTIES FOLDER monado_tests)
	add_executable(tests_macos_iosurface_producer tests_macos_iosurface_producer.c)
	target_link_libraries(
		tests_macos_iosurface_producer PRIVATE "-framework CoreFoundation"
						       "-framework IOSurface"
		)
	set_target_properties(tests_macos_iosurface_producer PROPERTIES FOLDER monado_tests)

	add_executable(tests_macos_iosurface_import_probe tests_macos_iosurface_import_probe.c)
	target_link_libraries(
		tests_macos_iosurface_import_probe PRIVATE ipc_client xrt-interfaces aux_util
		)
	set_target_properties(tests_macos_iosurface_import_probe PROPERTIES FOLDER monado_tests)

	add_executable(tests_macos_ipc_swapchain_probe tests_macos_ipc_swapchain_probe.c)
	target_link_libraries(
		tests_macos_ipc_swapchain_probe PRIVATE ipc_client xrt-interfaces aux_util
		)
	set_target_properties(tests_macos_ipc_swapchain_probe PROPERTIES FOLDER monado_tests)

	add_executable(tests_macos_service_gpu_sync tests_macos_service_gpu_sync.mm)
	target_compile_features(tests_macos_service_gpu_sync PRIVATE cxx_std_17)
	target_link_libraries(tests_macos_service_gpu_sync PRIVATE xrt-interfaces xrt-external-openxr "-framework Foundation" "-framework Metal")
	# These start an isolated monado-service through launchd in the user's GUI
	# domain and render through Metal/MoltenVK, so they need a logged-in host
	# with a GPU. Their verification reads the timing-diagnostics CSV traces.
	option(XRT_MACOS_SERVICE_GPU_TESTS
	       "Register the launchd-hosted macOS GPU service tests (needs a GUI session and GPU)" OFF
		)
	if(XRT_MACOS_SERVICE_GPU_TESTS AND NOT XRT_FEATURE_MACOS_TIMING_DIAGNOSTICS)
		message(
			WARNING
				"XRT_MACOS_SERVICE_GPU_TESTS needs XRT_FEATURE_MACOS_TIMING_DIAGNOSTICS; not registering them"
			)
	endif()
	if(XRT_MACOS_SERVICE_GPU_TESTS
	   AND XRT_FEATURE_MACOS_TIMING_DIAGNOSTICS
	   AND TARGET monado-service
		)
		foreach(fault missing disconnect budget)
			if(fault STREQUAL "budget")
				set(service_suite --budget-suite)
			else()
				set(service_suite --suite --fault ${fault})
			endif()
			add_test(
				NAME tests_macos_service_gpu_sync_${fault}
				COMMAND
					${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/macos/run-gpu-sync-service.py
					--build ${CMAKE_BINARY_DIR} --output
					${CMAKE_CURRENT_BINARY_DIR}/gpu-service-results --unique-output
					${service_suite}
				)
			set_tests_properties(
				tests_macos_service_gpu_sync_${fault} PROPERTIES TIMEOUT 60 LABELS
										 "macos;gpu;service" RUN_SERIAL TRUE
				)
		endforeach()
	endif()
	add_executable(tests_macos_openxr_loaderless_probe tests_macos_openxr_loaderless_probe.c)
	target_link_libraries(
		tests_macos_openxr_loaderless_probe PRIVATE xrt-interfaces xrt-external-openxr
		)
	set_target_properties(tests_macos_openxr_loaderless_probe PROPERTIES FOLDER monado_tests)

	if(XRT_HAVE_VULKAN AND XRT_MODULE_COMPOSITOR_UTIL AND TARGET ipc_client)
		add_executable(tests_macos_vk_metal_texture_peer tests_macos_vk_metal_texture_peer.m)
		target_link_libraries(tests_macos_vk_metal_texture_peer PRIVATE "-framework Metal" "-framework Foundation")
		set_target_properties(tests_macos_vk_metal_texture_peer PROPERTIES FOLDER monado_tests)
		add_executable(tests_macos_vk_metal_texture_import tests_macos_vk_metal_texture_import.m)
		target_link_libraries(tests_macos_vk_metal_texture_import PRIVATE comp_client ipc_client aux_vk
			"-framework Metal" "-framework Foundation")
		add_test(NAME tests_macos_vk_metal_texture_import COMMAND tests_macos_vk_metal_texture_import)
		set_tests_properties(tests_macos_vk_metal_texture_import PROPERTIES TIMEOUT 30
			LABELS "macos;gpu" RUN_SERIAL TRUE)
		set_target_properties(tests_macos_vk_metal_texture_import PROPERTIES FOLDER monado_tests)
	endif()
	add_executable(tests_macos_openxr_vulkan_probe tests_macos_openxr_vulkan_probe.c)
	target_link_libraries(
		tests_macos_openxr_vulkan_probe PRIVATE aux_vk xrt-interfaces xrt-external-openxr
		)
	set_target_properties(tests_macos_openxr_vulkan_probe PROPERTIES FOLDER monado_tests)

	if(XRT_BUILD_DRIVER_REMOTE)
		add_executable(tests_macos_remote_pose_bridge tests_macos_remote_pose_bridge.c)
		target_link_libraries(
			tests_macos_remote_pose_bridge PRIVATE drv_remote xrt-interfaces aux_util
			)
		target_include_directories(
			tests_macos_remote_pose_bridge
			PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/xrt/drivers
			)
		set_target_properties(tests_macos_remote_pose_bridge PROPERTIES FOLDER monado_tests)

		add_executable(
			tests_macos_remote_pose_packet_sender
			tests_macos_remote_pose_packet_sender.c
			)
		target_link_libraries(
			tests_macos_remote_pose_packet_sender PRIVATE xrt-interfaces aux_util
			)
		set_target_properties(
			tests_macos_remote_pose_packet_sender PROPERTIES FOLDER monado_tests
			)

		add_executable(
			tests_macos_remote_driver_pose_probe tests_macos_remote_driver_pose_probe.c
			)
		target_link_libraries(
			tests_macos_remote_driver_pose_probe PRIVATE drv_remote xrt-interfaces
								     aux_util
			)
		target_include_directories(
			tests_macos_remote_driver_pose_probe
			PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/xrt/drivers
			)
		set_target_properties(
			tests_macos_remote_driver_pose_probe PROPERTIES FOLDER monado_tests
			)
	endif()
endif()

if(APPLE AND TARGET st_oxr AND TARGET base AND XRT_HAVE_VULKAN AND XRT_MODULE_COMPOSITOR_UTIL)
	add_executable(tests_oxr_vk_acquire_loss tests_oxr_vk_acquire_loss.cpp)
	target_link_libraries(tests_oxr_vk_acquire_loss PRIVATE st_oxr base aux_util xrt-external-catch2 xrt-external-openxr)
	add_test(NAME tests_oxr_vk_acquire_loss COMMAND tests_oxr_vk_acquire_loss)
	set_target_properties(tests_oxr_vk_acquire_loss PROPERTIES FOLDER monado_tests)
endif()

if(TARGET base)
	add_executable(tests_session_loss_status tests_session_loss_status.cpp)
	target_link_libraries(tests_session_loss_status PRIVATE base aux_util xrt-external-catch2)
	add_test(NAME tests_session_loss_status COMMAND tests_session_loss_status)
	set_target_properties(tests_session_loss_status PROPERTIES FOLDER monado_tests)
endif()

if(UNIX
   AND TARGET comp_multi
   AND TARGET ipc_shared
	)
	add_executable(
		tests_ipc_local_session tests_ipc_local_session.cpp
					../src/xrt/ipc/client/ipc_client_system.c
		)
	target_compile_definitions(tests_ipc_local_session PRIVATE XRT_IPC_MACOS_HOSTED_COMPOSITOR)
	target_link_libraries(
		tests_ipc_local_session
		PRIVATE
			comp_multi
			ipc_shared
			aux_util
			base
			xrt-external-catch2
		)
	add_test(NAME tests_ipc_local_session COMMAND tests_ipc_local_session)
	set_target_properties(tests_ipc_local_session PROPERTIES FOLDER monado_tests)
endif()

if(TARGET ipc_server)
	add_executable(
		tests_ipc_thread_shutdown tests_ipc_thread_shutdown.cpp
					  ../src/xrt/ipc/server/ipc_server_thread_shutdown.c
		)
	target_link_libraries(
		tests_ipc_thread_shutdown
		PRIVATE
			ipc_shared
			aux_util
			aux_os
			base
			xrt-external-catch2
		)
	add_test(NAME tests_ipc_thread_shutdown COMMAND tests_ipc_thread_shutdown)
	set_tests_properties(tests_ipc_thread_shutdown PROPERTIES TIMEOUT 10)
	set_target_properties(tests_ipc_thread_shutdown PROPERTIES FOLDER monado_tests)
endif()

if(UNIX
   AND NOT ANDROID
   AND TARGET ipc_shared
	)
	add_executable(tests_ipc_socket_security tests_ipc_socket_security.cpp)
	target_link_libraries(
		tests_ipc_socket_security PRIVATE ipc_shared aux_util xrt-external-catch2
		)
	add_test(NAME tests_ipc_socket_security COMMAND tests_ipc_socket_security)
	set_tests_properties(tests_ipc_socket_security PROPERTIES TIMEOUT 10)
	set_target_properties(tests_ipc_socket_security PROPERTIES FOLDER monado_tests)
endif()

if(APPLE AND TARGET ipc_server)
	add_executable(
		tests_macos_ipc_swapchain_wait tests_macos_ipc_swapchain_wait.cpp
					       ../src/xrt/ipc/server/ipc_server_swapchain_wait.c
		)
	target_include_directories(tests_macos_ipc_swapchain_wait PRIVATE ../src/xrt/ipc)
	target_link_libraries(
		tests_macos_ipc_swapchain_wait PRIVATE aux_os aux_util xrt-interfaces
						       xrt-external-catch2
		)
	add_test(NAME tests_macos_ipc_swapchain_wait COMMAND tests_macos_ipc_swapchain_wait)
	set_tests_properties(tests_macos_ipc_swapchain_wait PROPERTIES TIMEOUT 20)
	set_target_properties(tests_macos_ipc_swapchain_wait PROPERTIES FOLDER monado_tests)
endif()

if(APPLE
   AND XRT_FEATURE_SERVICE
   AND TARGET ipc_server
	)
	add_executable(
		tests_metal_xpc_registry tests_metal_xpc_registry.mm
					 ../src/xrt/ipc/shared/ipc_metal_xpc_service.m
		)
	target_compile_options(tests_metal_xpc_registry PRIVATE -fblocks)
	target_link_libraries(
		tests_metal_xpc_registry
		PRIVATE
			ipc_shared
			aux_util
			xrt-external-catch2
			"-framework Foundation"
			"-framework Metal"
		)
	add_test(NAME tests_metal_xpc_registry COMMAND tests_metal_xpc_registry)
	set_target_properties(tests_metal_xpc_registry PROPERTIES FOLDER monado_tests)
endif()

if(APPLE AND TARGET comp_main)
	add_executable(tests_macos_remote_layer tests_macos_remote_layer.mm)
	target_include_directories(tests_macos_remote_layer PRIVATE ../src/xrt/compositor)
	target_link_libraries(
		tests_macos_remote_layer PRIVATE xrt-external-catch2 "-framework AppKit"
						 "-framework QuartzCore"
		)
	add_test(NAME tests_macos_remote_layer COMMAND tests_macos_remote_layer)
	set_target_properties(tests_macos_remote_layer PROPERTIES FOLDER monado_tests)
endif()

if(TARGET constellation)
	foreach(
		testname
		tests_pose_metrics
		tests_led_phase_bootstrap
		tests_imu_optical_filter
		tests_joint_pose_solver
		tests_constellation_dataset
		)
		target_link_libraries(${testname} PRIVATE constellation aux_math aux_tracking)
		target_include_directories(
			${testname}
			PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/xrt/tracking/constellation
				${CMAKE_CURRENT_SOURCE_DIR}/../src/xrt/tracking
			)
		target_link_libraries(${testname} PRIVATE Eigen3::Eigen xrt-external-bigceres)
	endforeach()
endif()
if(TARGET constellation_fusion)
	target_link_libraries(
		tests_pssense_fusion PRIVATE constellation_fusion aux_math aux_tracking
		)
endif()

if(XRT_BUILD_DRIVER_PSVR2 AND XRT_BUILD_DRIVER_PSSENSE)
	add_executable(tests_psvr2_sense_tracking tests_psvr2_sense_tracking.cpp)
	target_link_libraries(
		tests_psvr2_sense_tracking
		PRIVATE
			target_lists
			xrt-external-catch2
			xrt-interfaces
			tracking-includes
			aux_util
			aux_math
			aux_tracking
			drv_includes
		)
	target_include_directories(
		tests_psvr2_sense_tracking SYSTEM PRIVATE ${LIBUSB1_INCLUDE_DIRS}
		)
	add_test(NAME tests_psvr2_sense_tracking COMMAND tests_psvr2_sense_tracking)
endif()

if(XRT_BUILD_DRIVER_PSSENSE)
	add_executable(
		tests_pssense_clock
		tests_pssense_clock.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/../src/xrt/drivers/pssense/pssense_clock.c
		)
	target_link_libraries(tests_pssense_clock PRIVATE xrt-external-catch2)
	target_include_directories(
		tests_pssense_clock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src/xrt/drivers/pssense
		)
	add_test(NAME tests_pssense_clock COMMAND tests_pssense_clock)
endif()

if(APPLE AND TARGET psvr2-openxr-test)
	add_executable(tests_psvr2_controller_visuals tests_psvr2_controller_visuals.mm)
	target_compile_features(tests_psvr2_controller_visuals PRIVATE cxx_std_17)
	target_link_libraries(
		tests_psvr2_controller_visuals
		PRIVATE
			xrt-external-catch2
			xrt-external-openxr
			aux_metal_foveation
			"-framework Foundation"
			"-framework Metal"
		)
	add_test(NAME tests_psvr2_controller_visuals COMMAND tests_psvr2_controller_visuals)
endif()

if(XRT_FEATURE_MACOS_PERFETTO)
	add_executable(tests_macos_perfetto tests_macos_perfetto.cpp)
	target_link_libraries(tests_macos_perfetto PRIVATE aux_util aux_os)
	target_compile_features(tests_macos_perfetto PRIVATE cxx_std_17)
	set(XRT_PERFETTO_TRACE_PROCESSOR "" CACHE FILEPATH "External Perfetto trace processor for SQL regression checks")
	if(EXISTS "${XRT_PERFETTO_TRACE_PROCESSOR}")
		add_test(NAME tests_macos_perfetto COMMAND ${PYTHON_EXECUTABLE}
			${CMAKE_SOURCE_DIR}/scripts/macos/check-perfetto.py
			--fixture $<TARGET_FILE:tests_macos_perfetto> --processor ${XRT_PERFETTO_TRACE_PROCESSOR})
		set_tests_properties(tests_macos_perfetto PROPERTIES TIMEOUT 30)
	endif()
endif()
