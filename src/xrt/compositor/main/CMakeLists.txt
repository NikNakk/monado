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
		target_include_directories(comp_main SYSTEM PRIVATE ${XCB_INCLUDE_DIRS})
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
			"-include;${CMAKE_CURRENT_SOURCE_DIR}/comp_window_macos_trace_buffer.h;-include;${CMAKE_CURRENT_SOURCE_DIR}/comp_window_macos_cametal_probe.h;-include;${CMAKE_CURRENT_SOURCE_DIR}/comp_window_macos_cametal_idle_black.h;-include;${CMAKE_CURRENT_SOURCE_DIR}/comp_window_macos_cametal_drive_cv.h"
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
	file(MAKE_DIRECTORY "${WL_PROTOS_DIR}")

	set(WL_PROTOS_XML ${WL_PROTOS_PKG_DIR}/stable/xdg-shell/xdg-shell.xml)

	target_include_directories(
		comp_main SYSTEM PRIVATE ${WAYLAND_PROTO_INCLUDE_DIRS}
		)
	target_link_libraries(comp_main PRIVATE ${WAYLAND_LIBRARIES})

	if(XRT_HAVE_WAYLAND_DIRECT)
		list(APPEND WL_PROTOS_XML ${WL_PROTOS_PKG_DIR}/staging/drm-lease/drm-lease-v1.xml)

		target_sources(comp_main PRIVATE comp_window_direct_wayland.c)

		target_link_libraries(comp_main PRIVATE PkgConfig::LIBDRM)
	endif()

	foreach(wl_proto_xml ${WL_PROTOS_XML})
		get_filename_component(wl_proto_name ${wl_proto_xml} NAME_WE)
		set(wl_proto_c "${CMAKE_CURRENT_BINARY_DIR}/${wl_proto_name}-protocol.c")
		set(wl_proto_h "${CMAKE_CURRENT_BINARY_DIR}/${wl_proto_name}-client-protocol.h")
		add_custom_command(
			OUTPUT ${wl_proto_c} ${wl_proto_h}
			COMMAND ${WAYLAND_SCANNER} private-code ${wl_proto_xml} ${wl_proto_c}
			COMMAND ${WAYLAND_SCANNER} client-header ${wl_proto_xml} ${wl_proto_h}
			DEPENDS ${wl_proto_xml}
			VERBATIM
			)
		target_sources(comp_main PRIVATE ${wl_proto_c} ${wl_proto_h})
	endforeach()
endif()
