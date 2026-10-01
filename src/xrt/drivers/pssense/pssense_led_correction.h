// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Measured corrections to the PlayStation Sense controller LED models.
 *
 * Per-LED position offsets, in metres, in the frame of pssense_led_model.h and in its LED order. They were fitted to
 * the joint solver's reprojection residuals on recordings of one pair of controllers, with the combined mode-4 rig
 * calibration of 25 September 2026. Both rings come out about 1% larger than the model. The offsets carry no rigid
 * motion, so the model's origin and axes are unchanged.
 *
 * Whether they correct the model or compensate the rig calibration is not settled, so they are opt-in
 * (PSSENSE_LED_CORRECTION) and belong with that calibration. See doc/macos-pssense-mr2940-frontend-evaluation.md,
 * "The LED model's fit".
 *
 * @author Nick Kennedy
 * @ingroup drv_pssense
 */

#pragma once

#include "xrt/xrt_defines.h"


static const struct xrt_vec3 pssense_left_led_corrections[] = {
    {-0.0005135f, -0.0008520f, +0.0004913f}, // 0
    {-0.0005562f, -0.0006066f, -0.0005200f}, // 1
    {-0.0009781f, -0.0006820f, +0.0004288f}, // 2
    {-0.0009704f, -0.0006691f, +0.0000830f}, // 3
    {-0.0012706f, +0.0002069f, +0.0001211f}, // 4
    {-0.0009133f, +0.0005012f, +0.0001267f}, // 5
    {+0.0008472f, +0.0000930f, +0.0004638f}, // 6
    {+0.0005396f, -0.0002501f, +0.0001860f}, // 7
    {+0.0008031f, -0.0000383f, +0.0001988f}, // 8
    {+0.0009651f, +0.0004767f, +0.0003110f}, // 9
    {+0.0006835f, +0.0000353f, -0.0001038f}, // 10
    {+0.0005967f, +0.0005415f, +0.0000351f}, // 11
    {+0.0002022f, +0.0001548f, -0.0002888f}, // 12
    {+0.0001179f, +0.0005143f, -0.0000526f}, // 13
    {-0.0001251f, -0.0000663f, -0.0006095f}, // 14
    {+0.0001474f, +0.0001740f, -0.0004964f}, // 15
    {+0.0004244f, +0.0004669f, -0.0003745f}, // 16
};

static const struct xrt_vec3 pssense_right_led_corrections[] = {
    {+0.0000422f, -0.0006545f, +0.0006031f}, // 0
    {+0.0004753f, -0.0008164f, -0.0005075f}, // 1
    {+0.0011329f, -0.0007961f, +0.0004085f}, // 2
    {+0.0012525f, -0.0002085f, -0.0002256f}, // 3
    {+0.0011783f, +0.0003667f, +0.0000554f}, // 4
    {+0.0007570f, +0.0004465f, +0.0003482f}, // 5
    {-0.0008993f, -0.0000265f, +0.0006837f}, // 6
    {-0.0007439f, -0.0005651f, +0.0001707f}, // 7
    {-0.0009179f, -0.0001870f, +0.0001109f}, // 8
    {-0.0008889f, +0.0006847f, +0.0002786f}, // 9
    {-0.0007311f, +0.0001315f, -0.0000783f}, // 10
    {-0.0003340f, +0.0006282f, +0.0002528f}, // 11
    {-0.0001500f, +0.0003061f, -0.0001395f}, // 12
    {+0.0001223f, +0.0005417f, +0.0001956f}, // 13
    {+0.0002501f, -0.0000355f, -0.0007597f}, // 14
    {-0.0001148f, +0.0000164f, -0.0007398f}, // 15
    {-0.0004309f, +0.0001676f, -0.0006572f}, // 16
};
