# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

add_library(aux_metal_foveation STATIC m_metal_foveation.m)
target_link_libraries(aux_metal_foveation PUBLIC aux-includes PRIVATE "-framework Metal")
