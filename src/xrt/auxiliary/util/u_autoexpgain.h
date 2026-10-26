// Copyright 2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Automatically compute exposure and gain values from an image stream
 * @author Mateo de Mayo <mateo.demayo@collabora.com>
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_frame.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Most steps a custom table passed to @ref u_autoexpgain_create_with_steps can have.
#define U_AEG_MAX_STEPS 16

//! An auto exposure/gain strategy tunes the algorithm for specific objectives.
enum u_aeg_strategy
{
	U_AEG_STRATEGY_TRACKING = 0,  //!< Lower exposure and gain at the cost of darker images.
	U_AEG_STRATEGY_DYNAMIC_RANGE, //!< Tries to maximize the image information
	U_AEG_STRATEGY_COUNT
};

struct u_autoexpgain;

//! These are steps for constructing a piecewise linear function that maps
//! brightness into (exposure, gain) pairs.
struct u_aeg_step
{
	float brightness; //!< Must be between 0 and 1
	float exposure;   //!< Must be set such that pixel brightness increases with brightness, ideally linearly
	float gain;       //!< @copydoc exposure
};

/*!
 * Create auto exposure and gain (AEG) algorithm object.
 *
 * @param strategy What objective is preferred for the algorithm.
 * @param enabled_from_start Update exposure/gain from the start.
 * @param frame_delay About how many frames does it take for exp and gain to settle in.
 * @return struct u_autoexpgain* Created object
 */
struct u_autoexpgain *
u_autoexpgain_create(enum u_aeg_strategy strategy, bool enabled_from_start, int frame_delay);

/*!
 * @copydoc u_autoexpgain_create
 * @param steps Brightness ascending, from 0 to 1; copied.
 * @param step_count Between 2 and @ref U_AEG_MAX_STEPS.
 */
struct u_autoexpgain *
u_autoexpgain_create_with_steps(enum u_aeg_strategy strategy,
                                bool enabled_from_start,
                                int frame_delay,
                                const struct u_aeg_step *steps,
                                int step_count);

//! Setup UI for the AEG algorithm
void
u_autoexpgain_add_vars(struct u_autoexpgain *aeg, void *root, char *prefix);

//! Update the AEG with a frame
void
u_autoexpgain_update(struct u_autoexpgain *aeg, struct xrt_frame *xf);

//! Get currently computed exposure value in usecs.
float
u_autoexpgain_get_exposure(struct u_autoexpgain *aeg);

//! Get currently computed gain value
float
u_autoexpgain_get_gain(struct u_autoexpgain *aeg);

//! Destroy AEG object
void
u_autoexpgain_destroy(struct u_autoexpgain **aeg);

#ifdef __cplusplus
}
#endif
