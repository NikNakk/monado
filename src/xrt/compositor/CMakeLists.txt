# Copyright 2019-2024, Collabora, Ltd.
# Copyright 2025, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0

###
# Compositor includes target
#

add_library(comp_includes INTERFACE)
target_include_directories(comp_includes INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(comp_includes INTERFACE xrt-interfaces)

###
# Sub directories (order matters for some of them)
#

# Shaders library
if(XRT_MODULE_COMPOSITOR_SHADERS)
	add_subdirectory(shaders)
endif()

# Client library
if(XRT_MODULE_COMPOSITOR_CLIENT)
	add_subdirectory(client)
endif()

# Render library
if(XRT_MODULE_COMPOSITOR_RENDER)
	add_subdirectory(render)
endif()

# Util library
if(XRT_MODULE_COMPOSITOR_UTIL)
	add_subdirectory(util)
endif()

# The macOS Metal client Stage-2 release-sync diagnostic calls compositor-util
# helpers. Add this dependency only after comp_util has been defined: client/
# is intentionally configured earlier in this file.
if(APPLE AND XRT_MODULE_COMPOSITOR_CLIENT AND XRT_MODULE_COMPOSITOR_UTIL)
	target_link_libraries(comp_client PRIVATE comp_util)
endif()

# Main compositor
if(XRT_MODULE_COMPOSITOR_MAIN)
	add_subdirectory(main)
endif()

# Null compositor
if(XRT_MODULE_COMPOSITOR_NULL)
	add_subdirectory(null)
endif()

# Multi client compositor library
if(XRT_MODULE_COMPOSITOR_MULTI)
	add_subdirectory(multi)
endif()

# Mock native compositor
if(XRT_MODULE_COMPOSITOR_MOCK)
	add_subdirectory(mock)
endif()
