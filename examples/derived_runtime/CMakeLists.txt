# Copyright 2026, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.22)
project(
	derived_runtime_example
	VERSION 1.0.0
	LANGUAGES C CXX
	)

include(GNUInstallDirs)

set(MONADO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include(${MONADO_ROOT}/cmake/MonadoSetOptions.cmake)
include(${MONADO_ROOT}/cmake/MonadoOpenXRRuntime.cmake)

monado_set_options(
	ENABLE
		XRT_MODULE_AUX_RENDER
		XRT_MODULE_AUX_SHADERS
		XRT_MODULE_COMPOSITOR
		XRT_MODULE_COMPOSITOR_MAIN
		XRT_MODULE_COMPOSITOR_CLIENT
		XRT_MODULE_COMPOSITOR_MULTI
		XRT_MODULE_COMPOSITOR_UTIL
		XRT_MODULE_IPC
		XRT_MODULE_OPENXR_STATE_TRACKER
		XRT_BUILD_DRIVER_SIMULATED
	KEEP XRT_FEATURE_SSE2 XRT_FEATURE_COLOR_LOG
	KEEP_SET OPENXR_FEATURES DEPENDENCIES
	)

add_subdirectory(${MONADO_ROOT} monado)

add_executable(derived-service src/service_main.c)
target_link_libraries(
	derived-service
	PRIVATE
		aux_util
		aux_util_debug_gui
		st_prober
		ipc_server
		target_lists
		target_instance
	)
add_sanitizers(derived-service)
install(TARGETS derived-service RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# To also build Monado's default monado-service and libopenxr_monado, add
# XRT_FEATURE_OPENXR and XRT_FEATURE_SERVICE to the ENABLE list above.

monado_add_openxr_runtime(
	openxr_derived
	SOURCES src/target.c
	LINK_LIBRARIES ipc_client
	INSTALL
	)
