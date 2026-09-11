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
	pkg_get_variable(WL_SCANNER wayland-protocols wayland_scanner)

	set(WL_PROTOS_DIR "${CMAKE_CURRENT_BINARY_DIR}/wayland-protocols")
	file(MAKE_DIRECTORY "${WL_PROTOS_DIR}")

	set(WL_PROTOS_XML ${WL_PROTOS_PKG_DIR}/stable/xdg-shell/xdg-shell.xml)

	target_include_directories(
		comp_main SYSTEM PRIVATE ${WL_PROTOS_DIR} ${WAYLAND_INCLUDE_DIRS}
		)
	target_link_libraries(comp_main PRIVATE ${WAYLAND_LIBRARIES})

	if(XRT_HAVE_WAYLAND_DIRECT)
		list(APPEND WL_PROTOS_XML ${WL_PROTOS_PKG_DIR}/staging/drm-lease/drm-lease-v1.xml)

		target_sources(comp_main PRIVATE comp_window_direct_wayland.c)

		target_link_libraries(comp_main PRIVATE PkgConfig::LIBDRM)
	endif()

	foreach(wl_proto_xml ${WL_PROTOS_XML})
		get_filename_component(wl_proto_name ${wl_proto_xml} NAME_WE)
		set(wl_proto_client_header ${WL_PROTOS_DIR}/${wl_proto_name}-client-protocol.h)
		set(wl_proto_private_code ${WL_PROTOS_DIR}/${wl_proto_name}-protocol.c)

		add_custom_command(
			OUTPUT ${wl_proto_client_header}
			COMMAND ${WL_SCANNER} client-header ${wl_proto_xml} ${wl_proto_client_header}
			DEPENDS ${wl_proto_xml}
			VERBATIM
			)
		add_custom_command(
			OUTPUT ${wl_proto_private_code}
			COMMAND ${WL_SCANNER} private-code ${wl_proto_xml} ${wl_proto_private_code}
			DEPENDS ${wl_proto_xml}
			VERBATIM
			)
		target_sources(comp_main PRIVATE ${wl_proto_client_header} ${wl_proto_private_code})
	endforeach()
endif()

if(ANDROID)
	target_sources(comp_main PRIVATE comp_window_android.c)
endif()
