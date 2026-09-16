# Copyright 2019-2020, Collabora, Ltd.
# Copyright 2026, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0

######
# This is where we collect all of the pieces from the different parts of
# the source tree and build a complete driver or integration part.

add_subdirectory(helpers)
add_subdirectory(common)

# Monado management library. The implementation uses the portable IPC client
# layer and is useful on macOS as well as Linux for runtime/client management.
if(XRT_FEATURE_SERVICE AND (XRT_HAVE_LINUX OR APPLE))
	add_subdirectory(libmonado)
endif()

if(XRT_FEATURE_OPENXR)
	add_subdirectory(openxr)
endif()

# Standalone OpenXR/Metal diagnostic scene for the native macOS PS VR2 path.
if(APPLE AND XRT_FEATURE_OPENXR)
	add_subdirectory(psvr2_openxr_test)
endif()

if(XRT_MODULE_MONADO_CLI)
	add_subdirectory(cli)
endif()

if(XRT_MODULE_MONADO_GUI)
	add_subdirectory(gui)
endif()

if(XRT_FEATURE_SERVICE AND NOT WIN32)
	add_subdirectory(ctl)
endif()

if(XRT_FEATURE_SERVICE AND XRT_FEATURE_OPENXR)
	if(ANDROID)
		add_subdirectory(service-lib)
	else()
		add_subdirectory(service)
	endif()
endif()

if(XRT_FEATURE_STEAMVR_PLUGIN)
	add_subdirectory(steamvr_drv)
endif()

# sdl-test has external-memory/synchronisation implementations for FD,
# Android hardware buffers, and Win32 handles, but not macOS IOSurface/Metal.
if(XRT_FEATURE_SERVICE
   AND XRT_HAVE_SDL2
   AND XRT_HAVE_VULKAN
   AND NOT APPLE
	)
	add_subdirectory(sdl_test)
endif()
