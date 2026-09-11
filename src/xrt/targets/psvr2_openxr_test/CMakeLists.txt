# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

# Build the controlled comparison wrapper. It includes the base diagnostic app
# and adds a small distance/elevation/projected-size panel while preserving the
# same OpenXR and Metal rendering path.
add_executable(psvr2-openxr-test psvr2_openxr_test_matrix.mm)

target_compile_features(psvr2-openxr-test PRIVATE cxx_std_17)
target_link_libraries(
	psvr2-openxr-test
	PRIVATE
		xrt-external-openxr
		"-framework Foundation"
		"-framework Metal"
	)

# The test deliberately has no build-time dependency on the Khronos loader. It
# dlopens the loader so the source can be built using only Monado's bundled
# OpenXR headers. If an installed loader is visible to CMake, bake its path in as
# one of the runtime fallbacks; PSVR2_OPENXR_LOADER can always override it.
find_library(PSVR2_OPENXR_LOADER_LIBRARY NAMES openxr_loader)
mark_as_advanced(PSVR2_OPENXR_LOADER_LIBRARY)
if(PSVR2_OPENXR_LOADER_LIBRARY)
	target_compile_definitions(
		psvr2-openxr-test
		PRIVATE PSVR2_OPENXR_LOADER_DEFAULT="${PSVR2_OPENXR_LOADER_LIBRARY}"
		)
endif()
