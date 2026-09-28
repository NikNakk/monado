// Copyright 2026, Beyley Cardellio
// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Experimental IMU + optical sliding-window fusion for one PS Sense controller seen by moving PS VR2 cameras.
 *
 * Derived from Monado MR 3015's `optimizer/sensor_fusion.{hpp,cpp}` (head cd7174257, Beyley Cardellio): a fixed
 * window of keyframes, each a pose with velocity and an IMU bias, joined by preintegrated IMU factors and bias random
 * walks, observed through robust (Huber) LED reprojection factors, with the oldest keyframe marginalised into a
 * square-root linear prior (first-estimate Jacobians) when the window moves on.
 *
 * Adapted for the PS VR2 rather than stationary CV1 sensors:
 *  - every camera observation carries its own camera world pose, T_world_HMD(t) * T_HMD_camera at its exposure, so
 *    the cameras may move. The HMD/SLAM pose is taken as exact (its uncertainty is not modelled);
 *  - an exposure is one keyframe holding every camera's observation, seeded from this branch's M1/M2 joint solve
 *    rather than a per-camera PnP;
 *  - synchronous and single-threaded (offline replay), with no IMU ring buffer or fusion thread.
 *
 * Additions, none of which upstream has:
 *  - a pre-admission gate against the IMU prediction and a post-solve residual check; a keyframe that fails either
 *    is dropped with the window restored, so it never reaches the marginalisation prior;
 *  - camera factors whose residual is still an outlier when their keyframe is evicted are left out of the prior;
 *  - a reset (window and prior dropped, biases kept as seeds) on a change of the LED synchronisation / controller
 *    clock epoch, after too long a gap, or after repeated rejections;
 *  - bounded IMU and output buffers.
 *
 * Conventions: OpenCV throughout (see psvr2_fusion_frames.hpp). IMU samples are in the IMU frame; poses reported and
 * seeded are of the LED model frame.
 *
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @author Nick Kennedy
 * @ingroup tracking
 */

#pragma once

#include "imu_preintegration.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xrt::tracking::constellation::fusion {

//! One camera's correspondences for one device at one exposure.
struct FusionCameraObservation
{
	uint32_t camera_index{0};
	//! The camera's world pose at the exposure (CV convention): T_world_HMD(t) * T_HMD_camera.
	xrt_pose Tcv_world_cam = XRT_POSE_IDENTITY;
	t_camera_model_params model{};
	//! Blob centres, distorted pixels.
	std::vector<Eigen::Vector2d> points2d;
	//! The matched LEDs, LED model frame (CV).
	std::vector<Eigen::Vector3d> points3d;
};

//! Everything the optical frontend produced for one device at one exposure.
struct FusionExposure
{
	int64_t timestamp_ns{0};
	/*!
	 * Changes whenever the controller's LED synchronisation or clock mapping changes (a new lock, a phase move, a
	 * clock-offset snap). Time-dependent state is reset when it does.
	 */
	uint32_t sync_epoch{0};
	//! The frontend's pose of the LED model frame (CV), used to seed and to gate.
	xrt_pose Tcv_world_model_seed = XRT_POSE_IDENTITY;
	std::vector<FusionCameraObservation> observations;
};

struct SlidingWindowFusionParams
{
	//! Keyframes in the window (upstream: 3), and how many newer ones seal a keyframe (upstream: 2).
	uint32_t window_size{3};

	ImuNoiseModel noise{ImuNoiseModel::psSense()};

	/*!
	 * The calibrated IMU bias the oldest keyframe's bias is anchored to, and every reset re-seeds from. Upstream
	 * anchors to the latest estimate instead, which lets a bias that has run away (seen on 014505) hold itself there
	 * across resets. The recorded Sense samples already have the driver's factory and online gyro bias removed, so
	 * zero is the calibration.
	 */
	Eigen::Vector3d calibrated_gyro_bias{Eigen::Vector3d::Zero()};
	Eigen::Vector3d calibrated_accel_bias{Eigen::Vector3d::Zero()};

	//! Blob centroid noise, pixels (upstream: 0.16 for the CV1). M1's live floor is ~0.3-0.5 px RMS.
	double blob_sigma_px{0.5};
	//! Huber threshold per observation, in standard deviations of its chi-square (upstream: 3).
	double huber_delta_sigmas{3.0};
	//! Ceres iterations per solve (upstream: 15).
	int max_iterations{15};

	/*!
	 * Seed IMU -> LED model rotation (CV), and whether to solve for it. Upstream solves for it; here it is fixed by
	 * default, since the Sense ring constrains its own tilt too weakly for the extrinsic to stay put (see the
	 * evaluation document).
	 */
	Eigen::Quaterniond Q_imu_model{Eigen::Quaterniond::Identity()};
	bool optimize_extrinsics{false};

	/*!
	 * Optional prior tying each keyframe's LED model orientation to the seed's, 1 sigma in degrees; 0 disables it
	 * (upstream has none). M1 itself regularises the ring's weakly observed tilt with a 3 degree IMU prior, which
	 * reprojection factors alone discard.
	 */
	double seed_orientation_sigma_deg{0.0};

	/*!
	 * Pre-admission gate against the IMU prediction. The seed's position must lie within gate_position_m +
	 * 0.5 gate_accel dt^2 of it, and the observations must reproject within gate_reprojection_px (RMS) at the seed's
	 * position with the predicted orientation: a wrong lock fails that, while a disagreement about the ring's
	 * weakly observed tilt does not. A coarse angle gate catches the rest.
	 */
	bool gate{true};
	double gate_position_m{0.04};
	double gate_accel_m_s2{40.0};
	double gate_reprojection_px{4.0};
	double gate_orientation_deg{45.0};
	double gate_rate_deg_s{90.0};
	//! Post-solve check: a new keyframe whose observations still exceed this RMS (pixels) after the solve is dropped.
	double post_solve_max_rms_px{2.0};
	//! Camera factors above this RMS (pixels) at eviction are not folded into the prior.
	double marginalise_max_rms_px{2.0};

	//! Reset after this long without an accepted keyframe, or this many consecutive rejections.
	int64_t reset_gap_ns{1'000'000'000};
	uint32_t reset_after_rejections{5};

	//! Reported poses: position valid this long after the last accepted keyframe (as the EKF's position_valid_ns).
	int64_t position_valid_ns{300'000'000};

	//! Bounds on buffered IMU samples and published states.
	size_t max_imu_samples{8192};
	size_t max_published_states{512};
};

enum class FusionUpdateStatus
{
	//! First keyframe after start or a reset.
	Initialised,
	//! Added and solved.
	Accepted,
	//! Refused by the pre-admission gate.
	RejectedGate,
	//! Solved, but its residuals stayed too large; dropped with the window restored.
	RejectedPostSolve,
	//! Rejected, and that triggered a reset; this exposure then re-initialised the fusion.
	ResetAndInitialised,
	//! Not usable (no observations, older than the newest keyframe, or no IMU yet).
	Ignored,
};

const char *
fusion_update_status_name(FusionUpdateStatus status);

struct FusionUpdateResult
{
	FusionUpdateStatus status{FusionUpdateStatus::Ignored};
	//! Why it was rejected or reset, empty otherwise.
	std::string reason;
	//! Seed against the IMU prediction (when there was one).
	double gate_position_error_m{0.0};
	double gate_orientation_error_deg{0.0};
	//! RMS reprojection (pixels) at the seed position with the predicted orientation.
	double gate_reprojection_px{0.0};
	//! RMS reprojection (pixels) of this exposure's observations at the solved pose.
	double solved_rms_px{0.0};
	int iterations{0};
	double solve_us{0.0};
	//! The epoch changed and reset the fusion before this exposure.
	bool epoch_reset{false};
};

//! A pose of the LED model frame (CV) at some time, as the fusion reports it.
struct FusionPose
{
	bool orientation_valid{false};
	bool position_valid{false};
	xrt_pose Tcv_world_model = XRT_POSE_IDENTITY;
	Eigen::Vector3d velocity{Eigen::Vector3d::Zero()};
	//! World-frame angular velocity, rad/s.
	Eigen::Vector3d angular_velocity{Eigen::Vector3d::Zero()};
};

struct FusionStats
{
	uint64_t exposures{0};
	uint64_t initialisations{0};
	uint64_t accepted{0};
	uint64_t rejected_gate{0};
	uint64_t rejected_post_solve{0};
	uint64_t resets_gap{0};
	uint64_t resets_rejections{0};
	uint64_t resets_epoch{0};
	uint64_t marginalisations{0};
	//! Camera factors left out of a prior for being outliers at eviction.
	uint64_t factors_excluded_from_prior{0};
	//! Priors dropped because a block they named had left the problem.
	uint64_t priors_dropped{0};
	uint64_t imu_samples{0};
	uint64_t imu_samples_trimmed{0};
	//! Peak buffered IMU samples, for checking memory stays bounded.
	size_t peak_imu_buffer{0};
	Eigen::Vector3d gyro_bias{Eigen::Vector3d::Zero()};
	Eigen::Vector3d accel_bias{Eigen::Vector3d::Zero()};
	Eigen::Quaterniond Q_imu_model{Eigen::Quaterniond::Identity()};
};

class SlidingWindowFusion
{
public:
	explicit SlidingWindowFusion(const SlidingWindowFusionParams &params);
	~SlidingWindowFusion();

	SlidingWindowFusion(const SlidingWindowFusion &) = delete;
	SlidingWindowFusion &
	operator=(const SlidingWindowFusion &) = delete;

	//! One IMU sample, IMU frame, CV convention. Must arrive in time order.
	void
	pushImu(const xrt_imu_sample &sample);

	/*!
	 * One exposure's optical observations. IMU samples up to (and ideally just past) its timestamp should have been
	 * pushed already. Exposures must arrive in time order.
	 */
	FusionUpdateResult
	pushExposure(const FusionExposure &exposure);

	/*!
	 * The pose at @p timestamp_ns, propagated with the IMU from the newest published keyframe at or before it. Causal:
	 * it only uses keyframes already pushed.
	 */
	FusionPose
	getPose(int64_t timestamp_ns) const;

	const FusionStats &
	stats() const;

	//! Drop everything but the bias and extrinsic estimates.
	void
	reset();

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace xrt::tracking::constellation::fusion
