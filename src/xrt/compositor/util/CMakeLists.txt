# Copyright 2019-2024, Collabora, Ltd.
# Copyright 2025, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0

##
# Util library
#

add_library(
	comp_util STATIC
	comp_base.h
	comp_base.c
	comp_high_level_render.c
	comp_high_level_render.h
	comp_high_level_scratch.c
	comp_high_level_scratch.h
	comp_layer_accum.h
	comp_layer_accum.c
	comp_render.h
	comp_render_cs.c
	comp_render_gfx.c
	comp_render_helpers.h
	comp_scratch.c
	comp_scratch.h
	comp_semaphore.h
	comp_semaphore.c
	comp_swapchain.h
	comp_swapchain.c
	comp_sync.h
	comp_sync.c
	comp_vulkan.h
	comp_vulkan.c
	)
target_link_libraries(
	comp_util
	PUBLIC xrt-interfaces comp_includes
	PRIVATE aux_util aux_os aux_vk comp_render
	)

if(APPLE)
	target_sources(
		comp_util
		PRIVATE comp_metal_semaphore_probe.h
			comp_metal_semaphore_probe.m
			comp_metal_swapchain_handoff.h
			comp_metal_swapchain_import.h
			comp_metal_swapchain_import.c
			comp_metal_swapchain_import_override.h
		)

	# Redirect the two vk_image_allocator calls in comp_swapchain.c only. The
	# override falls straight through to the ordinary allocator unless the
	# current thread carries a matching in-process Metal-first request.
	set_property(
		SOURCE comp_swapchain.c
		APPEND
		PROPERTY COMPILE_OPTIONS "-include" "${CMAKE_CURRENT_SOURCE_DIR}/comp_metal_swapchain_import_override.h"
		)

	target_link_libraries(comp_util PRIVATE "-framework Metal")
endif()
