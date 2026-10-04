// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Coordinate conventions between the PS VR2 / PS Sense data and the experimental fusion.
 *
 * Frames (see doc/macos-pssense-upstream-fusion-evaluation.md):
 *  - OpenXR (XR) convention: x right, y up, z backwards. Drivers, datasets' camera poses (`Txr_world_cam`) and the
 *    Sense IMU samples use it.
 *  - OpenCV (CV) convention: x right, y down, z forward. The constellation tracker's internals, the recorded LED
 *    models and M1 use it. A pose converts by conjugating both frames with C = diag(1, -1, -1), i.e. a 180 degree
 *    rotation about x; a vector converts by C.
 *  - IMU frame: the Sense IMU's axes. The LED model frame is the IMU frame rotated by the mounting angle about x
 *    (50.27 degrees, `pssense_imu_angle`): v_model = R_x(-angle) v_imu. Rotations about x commute with C, so the
 *    same quaternion holds in both conventions.
 *  - Camera: `T_world_camera(t) = T_world_HMD(t) * T_HMD_camera`, where T_HMD_camera is the calibration's
 *    `head_from_camera0_xrt` composed with the camera's pose in the camera-0 origin. World-frame recordings
 *    (`PSVR2_CONSTELLATION_WORLD=1`) store the composed T_world_camera(t) for every sample.
 *
 * @author Nick Kennedy
 * @ingroup tracking
 */

#pragma once

#include "xrt/xrt_defines.h"
#include "xrt/xrt_tracking.h"

#include "math/m_api.h"

#include <Eigen/Geometry>

#include <cmath>

namespace xrt::tracking::constellation::fusion {

//! The PS Sense IMU's mounting rotation about x, degrees (pssense_imu_angle).
constexpr double kSenseImuAngleDeg = 50.27;

//! XR <-> CV for a pose (the conversion is its own inverse).
inline xrt_pose
pose_xr_cv(const xrt_pose &in)
{
	xrt_pose out;
	math_pose_convert_from_opencv(&in, &out);
	return out;
}

//! XR <-> CV for a vector.
inline xrt_vec3_f64
vec_xr_cv(const xrt_vec3_f64 &in)
{
	return xrt_vec3_f64{in.x, -in.y, -in.z};
}

//! A Sense IMU sample (IMU frame, XR convention) in the IMU frame, CV convention, as the fusion takes it.
inline xrt_imu_sample
imu_sample_xr_to_cv(const xrt_imu_sample &in)
{
	xrt_imu_sample out = in;
	out.accel_m_s2 = vec_xr_cv(in.accel_m_s2);
	out.gyro_rad_secs = vec_xr_cv(in.gyro_rad_secs);
	return out;
}

/*!
 * The IMU -> LED model rotation the fusion's extrinsic block holds: a point in the LED model frame is
 * `Q_imu_model * p` in the IMU frame. R_x(+angle), identical in XR and CV.
 */
inline Eigen::Quaterniond
sense_q_imu_model(double imu_angle_deg = kSenseImuAngleDeg)
{
	return Eigen::Quaterniond(Eigen::AngleAxisd(imu_angle_deg * M_PI / 180.0, Eigen::Vector3d::UnitX()));
}

//! T_world_camera = T_world_HMD * T_HMD_camera.
inline xrt_pose
world_from_camera(const xrt_pose &T_world_hmd, const xrt_pose &T_hmd_camera)
{
	xrt_pose out;
	math_pose_transform(&T_world_hmd, &T_hmd_camera, &out);
	return out;
}

/*!
 * The HMD pose a world-frame recording implies: camera 0's world pose is head * head_from_camera0 (camera 0 is the
 * tracking origin), so head = T_world_camera0 * head_from_camera0^-1.
 */
inline xrt_pose
hmd_from_world_camera0(const xrt_pose &T_world_camera0, const xrt_pose &head_from_camera0)
{
	xrt_pose inverse, out;
	math_pose_invert(&head_from_camera0, &inverse);
	math_pose_transform(&T_world_camera0, &inverse, &out);
	return out;
}

} // namespace xrt::tracking::constellation::fusion
