# Copyright 2024, Coreforge
# SPDX-License-Identifier: BSL-1.0

add_library(drv_pimax_p2 STATIC pimax_p2.c)
target_link_libraries(
	drv_pimax_p2
	PRIVATE
		xrt-interfaces
		aux_util
		aux_math
		aux_tracking
		xrt-external-cjson
		HIDAPI::hidapi
	)
target_include_directories(drv_pimax_p2 PRIVATE ${HIDAPI_INCLUDE_DIRS})
