# Copyright 2018-2024, Collabora, Ltd.
# Copyright 2024-2026, NVIDIA CORPORATION.
#
# SPDX-License-Identifier: BSL-1.0

set(tests
    tests_cxx_wrappers
    tests_deque
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
	if(APPLE)
		target_sources(tests_render_pipeline_cache PRIVATE tests_macos_metal_device.mm)
		target_link_libraries(tests_render_pipeline_cache PRIVATE "-framework Metal")
	endif()
endif()

if(XRT_FEATURE_OPENXR)
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
	add_executable(tests_macos_runtime_probe tests_macos_runtime_probe.c)
	target_link_libraries(
		tests_macos_runtime_probe PRIVATE target_instance xrt-interfaces aux_util
		)
	set_target_properties(tests_macos_runtime_probe PROPERTIES FOLDER monado_tests)
endif()

if(APPLE AND TARGET comp_macos_present_copy)
	add_executable(tests_macos_present_copy tests_macos_present_copy.mm)
	target_compile_features(tests_macos_present_copy PRIVATE cxx_std_17)
	target_link_libraries(
		tests_macos_present_copy PRIVATE comp_macos_present_copy xrt-external-catch2
		)
	add_test(NAME tests_macos_present_copy COMMAND tests_macos_present_copy --success)
	set_target_properties(tests_macos_present_copy PROPERTIES FOLDER monado_tests)
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

	add_executable(tests_macos_openxr_loaderless_probe tests_macos_openxr_loaderless_probe.c)
	target_link_libraries(
		tests_macos_openxr_loaderless_probe PRIVATE xrt-interfaces xrt-external-openxr
		)
	set_target_properties(tests_macos_openxr_loaderless_probe PROPERTIES FOLDER monado_tests)

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
