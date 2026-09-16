# Copyright 2019-2024, Collabora, Ltd.
# Copyright 2025, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0

##
# Main library
#

add_library(
	comp_main STATIC
	comp_compositor.c
	comp_compositor.h
	comp_documentation.h
	comp_main_interface.h
	comp_renderer.c
	comp_renderer.h
	comp_settings.c
	comp_settings.h
	comp_target.h
	comp_target_swapchain.c
	comp_target_swapchain.h
	comp_window.h
	comp_window_debug_image.c
	comp_mirror_to_debug_gui.c
	comp_mirror_to_debug_gui.h
	)
target_link_libraries(
	comp_main
	PUBLIC xrt-interfaces comp_includes
	PRIVATE
		aux_util
		aux_os
		aux_vk
		comp_util
		comp_render
	)

if(XRT_HAVE_XCB)
	target_sources(comp_main PRIVATE comp_window_xcb.c)
	target_include_directories(comp_main SYSTEM PRIVATE ${XCB_INCLUDE_DIRS})
	target_link_libraries(comp_main PRIVATE ${XCB_LIBRARIES})
	if(XRT_HAVE_EGL)
		target_include_directories(comp_main SYSTEM PRIVATE ${EGL_INCLUDE_DIRS})
		target_link_libraries(comp_main PRIVATE ${XCB_LIBRARIES})
	endif()
endif()
if(XRT_HAVE_XCB AND XRT_HAVE_XLIB)
	target_sources(comp_main PRIVATE comp_window_direct_randr.c comp_window_direct_nvidia.c)
	target_link_libraries(comp_main PRIVATE ${X11_X11_LIB})
endif()
if(XRT_FEATURE_WINDOW_PEEK)
	target_sources(comp_main PRIVATE comp_window_peek.c)
	target_link_libraries(comp_main PRIVATE SDL2::SDL2)
endif()
if(WIN32)
	target_sources(comp_main PRIVATE comp_window_mswin.c)
endif()
if(APPLE)
	target_sources(comp_main PRIVATE comp_window_macos_latest.m)
	set_source_files_properties(
		comp_window_macos_latest.m
		PROPERTIES COMPILE_OPTIONS
			"-include;${CMAKE_CURRENT_SOURCE_DIR}/comp_window_macos_trace_buffer.h;-include;${CMAKE_CURRENT_SOURCE_DIR}/comp_window_macos_cametal_probe.h"
		)

	# Add the service-Metal reuse timeline to the exact renderer queue submit
	# without changing the ordinary renderer/presentation code path.
	set_property(
		SOURCE comp_renderer.c
		APPEND
		PROPERTY COMPILE_OPTIONS
			"-include"
			"${CMAKE_CURRENT_SOURCE_DIR}/../util/comp_swapchain_gpu_reuse_renderer_override.h"
		)

	# Ensure native layer-accumulator claims are also released on renderer
	# early-return/error paths that never reach vkQueueSubmit.
	set_property(
		SOURCE comp_compositor.c
		APPEND
		PROPERTY COMPILE_OPTIONS
			"-include"
			"${CMAKE_CURRENT_SOURCE_DIR}/../util/comp_swapchain_gpu_reuse_compositor_override.h"
		)

	target_link_libraries(
		comp_main
		PRIVATE "-framework AppKit" "-framework CoreGraphics" "-framework CoreVideo" "-framework Metal" "-framework MetalKit"
			"-framework QuartzCore"
		)
endif()
if(VK_USE_PLATFORM_DISPLAY_KHR)
	target_sources(comp_main PRIVATE comp_window_vk_display.c)
endif()
if(VK_USE_PLATFORM_DISPLAY_KHR OR XRT_HAVE_XCB)
	target_sources(comp_main PRIVATE comp_window_direct.c)
endif()

# generate wayland protocols
if(XRT_HAVE_WAYLAND)
	target_sources(comp_main PRIVATE comp_window_wayland.c)
	pkg_get_variable(WL_PROTOS_PKG_DIR wayland-protocols pkgdatadir)
	pkg_get_variable(WL_SCANNER_PKG_DATA wayland-scanner pkgdatadir)
	find_program(WAYLAND_SCANNER NAMES wayland-scanner PATHS ${WL_SCANNER_PKG_DATA})

	file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols)

	set(wl_protos_dir ${WL_PROTOS_PKG_DIR})
	list(
		APPEND
		wl_proto_xml
		stable/xdg-shell/xdg-shell.xml
		unstable/tablet/tablet-unstable-v2.xml
		)

	if(XRT_HAVE_WAYLAND_DIRECT)
		list(APPEND wl_proto_xml stable/linux-dmabuf/linux-dmabuf-v1.xml)
	endif()

	foreach(wl_proto ${wl_proto_xml})
		get_filename_component(proto_name ${wl_proto} NAME_WE)
		set(generated_file ${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols/${proto_name})

		add_custom_command(
			OUTPUT ${generated_file}-client-protocol.h
			COMMAND ${WAYLAND_SCANNER} client-header ${wl_protos_dir}/${wl_proto}
				${generated_file}-client-protocol.h
			DEPENDS ${wl_protos_dir}/${wl_proto}
			)

		add_custom_command(
			OUTPUT ${generated_file}-protocol.c
			COMMAND ${WAYLAND_SCANNER} private-code ${wl_protos_dir}/${wl_proto}
				${generated_file}-protocol.c
			DEPENDS ${wl_protos_dir}/${wl_proto}
			)

		target_sources(
			comp_main
			PRIVATE ${generated_file}-client-protocol.h ${generated_file}-protocol.c
			)
	endforeach()

	target_include_directories(comp_main PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols)
endif()
if(ANDROID)
	target_sources(comp_main PRIVATE comp_window_android.c)
	target_link_libraries(comp_main PRIVATE aux_ogl aux_android)
endif()

target_link_libraries(comp_main PRIVATE comp_multi)
