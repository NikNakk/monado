// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "xrt/xrt_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

void
psvr2_compute_distortion_asymmetric(
    float *calibration, struct xrt_uv_triplet *distCoords, int eEye, float fU, float fV);

#ifdef __cplusplus
}
#endif
