# Copyright 2019-2023, Collabora, Ltd.
# Copyright 2026, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0

# High-frequency macOS/PSVR2 CSV timing diagnostics are useful during cadence
# investigations, but they add work to IMU, pose, and compositor hot paths.
option(
	XRT_FEATURE_MACOS_TIMING_DIAGNOSTICS
	"Compile high-frequency macOS/PSVR2 timing diagnostics"
	OFF
	)
if(XRT_FEATURE_MACOS_TIMING_DIAGNOSTICS)
	add_compile_definitions(XRT_FEATURE_MACOS_TIMING_DIAGNOSTICS)
endif()

# Order matters.
add_subdirectory(include)
add_subdirectory(auxiliary)
add_subdirectory(tracking)
add_subdirectory(base)
add_subdirectory(drivers)
if(XRT_MODULE_COMPOSITOR)
	add_subdirectory(compositor)
endif()
add_subdirectory(state_trackers)
if(XRT_MODULE_IPC)
	add_subdirectory(ipc)
endif()
add_subdirectory(targets)
