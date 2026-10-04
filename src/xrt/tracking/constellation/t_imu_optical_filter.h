// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Error-state EKF fusing a device's IMU with delayed optical (constellation) poses.
 *
 * The state is the body's world position, velocity and orientation plus gyro and accelerometer biases. IMU samples
 * propagate it at their own rate; each optical pose updates it at its exposure time, which is in the past by the time
 * it arrives, so the filter keeps a short history of states and IMU samples, rewinds to the exposure, applies the
 * update and re-propagates. Optical poses far from the prediction are rejected (Mahalanobis gate); after a long optical
 * gap, or repeated rejections, the next pose re-initialises position and velocity.
 *
 * Frames: "body" is the frame optical poses are given in (the LED model frame); IMU samples must already be rotated
 * into it. "World" is the optical poses' frame; gravity in it is a parameter (default -9.80665 m/s^2 along y, OpenXR's
 * gravity-aligned worlds). All times are one monotonic clock, in nanoseconds.
 *
 * @author Nick Kennedy
 * @ingroup tracking
 */

#pragma once

#include "xrt/xrt_defines.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct t_imu_optical_filter;

struct t_imu_optical_filter_params
{
	//! Gravity in the world frame, m/s^2.
	struct xrt_vec3 gravity_m_s2;

	//! IMU white noise densities (per sqrt(Hz)) and bias random walks (per sqrt(s)).
	float gyro_noise_rad_s;
	float accel_noise_m_s2;
	float gyro_bias_walk_rad_s2;
	float accel_bias_walk_m_s3;

	//! Initial 1-sigma uncertainty of velocity and the biases when (re)initialised from an optical pose.
	float initial_velocity_sigma_m_s;
	float initial_gyro_bias_sigma_rad_s;
	float initial_accel_bias_sigma_m_s2;

	//! Reject an optical pose whose squared Mahalanobis distance exceeds this (6 degrees of freedom).
	float gate_chi2;
	//! Re-initialise from the next optical pose after this long without an accepted one...
	int64_t reinit_gap_ns;
	//! ...or after this many consecutive rejections.
	uint32_t reinit_after_rejections;

	//! How far back optical poses may arrive (history kept), and the longest forward prediction.
	int64_t history_ns;
	int64_t max_prediction_ns;
	//! Position is reported as tracked until this long after the last accepted optical pose...
	int64_t position_tracked_ns;
	/*!
	 * ...and as valid at all only until this long after it; orientation stays valid. An IMU-only position drifts
	 * fast, and on 26 Sep (014505) a stale one used as the joint tracker's prior stopped every re-acquired track
	 * from confirming (unconfirmed tracks do not reach the filter): 1 left pose in 90 s.
	 */
	int64_t position_valid_ns;
};

enum t_imu_optical_filter_update_result
{
	T_IMU_OPTICAL_FILTER_INITIALISED = 0,
	T_IMU_OPTICAL_FILTER_UPDATED = 1,
	T_IMU_OPTICAL_FILTER_REJECTED = 2,
	//! Older than the history, or no IMU yet: ignored.
	T_IMU_OPTICAL_FILTER_IGNORED = 3,
	//! Re-initialised after a long gap or repeated rejections.
	T_IMU_OPTICAL_FILTER_REINITIALISED = 4,
};

struct t_imu_optical_filter_stats
{
	uint64_t imu_samples;
	uint64_t updates;
	uint64_t rejections;
	uint64_t reinitialisations;
	//! Squared Mahalanobis distance of the last optical pose.
	float last_mahalanobis2;
	struct xrt_vec3 gyro_bias_rad_s;
	struct xrt_vec3 accel_bias_m_s2;
};

void
t_imu_optical_filter_default_params(struct t_imu_optical_filter_params *params);

struct t_imu_optical_filter *
t_imu_optical_filter_create(const struct t_imu_optical_filter_params *params);

void
t_imu_optical_filter_destroy(struct t_imu_optical_filter **filter_ptr);

//! Push one IMU sample (body frame). Samples must arrive in time order.
void
t_imu_optical_filter_push_imu(struct t_imu_optical_filter *filter,
                              int64_t timestamp_ns,
                              const struct xrt_vec3 *accel_m_s2,
                              const struct xrt_vec3 *gyro_rad_s);

/*!
 * Push an optical pose of the body in the world, exposed at @p timestamp_ns, with 1-sigma position (m) and orientation
 * (rad) noise.
 */
enum t_imu_optical_filter_update_result
t_imu_optical_filter_push_pose(struct t_imu_optical_filter *filter,
                               int64_t timestamp_ns,
                               const struct xrt_pose *pose,
                               float position_sigma_m,
                               float orientation_sigma_rad);

/*!
 * The filtered pose at @p timestamp_ns: from the state history if it lies within it, otherwise predicted forward from
 * the latest state (at most max_prediction_ns). Returns false before the first optical pose.
 */
bool
t_imu_optical_filter_get_relation(struct t_imu_optical_filter *filter,
                                  int64_t timestamp_ns,
                                  struct xrt_space_relation *out_relation);

void
t_imu_optical_filter_get_stats(const struct t_imu_optical_filter *filter, struct t_imu_optical_filter_stats *out_stats);

#ifdef __cplusplus
}
#endif
