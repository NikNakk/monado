# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

add_library(aux_foveation STATIC u_foveation.c u_foveation_angular.c)
target_link_libraries(aux_foveation PUBLIC aux-includes)

# Angular profile mathematics live in threshold-foveation (API level 2). Without
# it the angular MNDX provider reports unsupported; FB/META policy is unaffected.
find_package(ThresholdFoveation 0.2 CONFIG QUIET)
if(ThresholdFoveation_FOUND)
	target_link_libraries(aux_foveation PUBLIC ThresholdFoveation::threshold_foveation)
	target_compile_definitions(aux_foveation PRIVATE XRT_HAVE_THRESHOLD_FOVEATION)
	message(STATUS "Angular foveation profiles: threshold-foveation ${ThresholdFoveation_VERSION}")
else()
	message(STATUS "Angular foveation profiles: unavailable (threshold-foveation >= 0.2 not found)")
endif()
