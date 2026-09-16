# Copyright 2019-2024, Collabora, Ltd.
# Copyright 2025, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0

###
# Multi client compositor library
#

add_library(
	comp_multi STATIC comp_multi_compositor.c comp_multi_interface.h comp_multi_private.h
			  comp_multi_system.c comp_multi_system_macos_trace.h
	)
target_link_libraries(
	comp_multi
	PUBLIC xrt-interfaces comp_includes
	PRIVATE aux_util aux_os comp_util
	)

if(APPLE)
	target_sources(
		comp_multi PRIVATE
		comp_multi_macos_displaylink.c
		comp_multi_macos_displaylink.h
		comp_multi_system_macos_displaylink_drive.h
		)

	# Diagnostic-only mapping from each 120 Hz system-compositor frame to the
	# delivered client frame that supplied it. The displaylink experiment header
	# is included after it so the existing trace/QoS wrappers remain available.
	set_property(
		SOURCE comp_multi_system.c
		APPEND
		PROPERTY COMPILE_OPTIONS
			"-include"
			"${CMAKE_CURRENT_SOURCE_DIR}/comp_multi_system_macos_trace.h"
			"-include"
			"${CMAKE_CURRENT_SOURCE_DIR}/comp_multi_system_macos_displaylink_drive.h"
		)
endif()
