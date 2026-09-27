# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0

add_library(aux_foveation STATIC u_foveation.c)
target_link_libraries(aux_foveation PUBLIC aux-includes)
