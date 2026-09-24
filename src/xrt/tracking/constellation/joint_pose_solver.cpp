// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Joint multi-camera pose refinement for constellation-tracked devices.
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "joint_pose_solver.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <Eigen/Cholesky>

#include <algorithm>
#include <cmath>

namespace xrt::tracking::constellation {

namespace {

struct Rigid
{
	Eigen::Quaterniond q;
	Eigen::Vector3d t;
};

Rigid
from_xrt(const xrt_pose &pose)
{
	return Rigid{Eigen::Quaterniond(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z)
	                 .normalized(),
	             Eigen::Vector3d(pose.position.x, pose.position.y, pose.position.z)};
}

xrt_pose
to_xrt(const Rigid &rigid)
{
	xrt_pose pose;
	pose.orientation = xrt_quat{(float)rigid.q.x(), (float)rigid.q.y(), (float)rigid.q.z(), (float)rigid.q.w()};
	pose.position = xrt_vec3{(float)rigid.t.x(), (float)rigid.t.y(), (float)rigid.t.z()};
	return pose;
}

//! Apply a world-frame increment: rotation vector delta[0..2], translation delta[3..5].
Rigid
apply_increment(const Rigid &pose, const Eigen::Matrix<double, 6, 1> &delta)
{
	Eigen::Vector3d omega = delta.head<3>();
	double angle = omega.norm();
	Eigen::Quaterniond dq = angle > 1e-12 ? Eigen::Quaterniond(Eigen::AngleAxisd(angle, omega / angle))
	                                      : Eigen::Quaterniond::Identity();
	return Rigid{(dq * pose.q).normalized(), pose.t + delta.tail<3>()};
}

struct CameraFrame
{
	Eigen::Matrix3d R_cam_world;
	Eigen::Vector3d t_world_cam;
};

//! Project LED @p led of @p model at device pose @p device into @p camera. Returns false if not visible.
bool
project_led(const JointSolveCamera &camera,
            const CameraFrame &frame,
            const t_constellation_tracker_led &led,
            const Rigid &device,
            bool check_visibility,
            Eigen::Vector2d &out_px,
            double visibility_margin_rad = 0.0)
{
	Eigen::Vector3d p_device(led.position.x, led.position.y, led.position.z);
	Eigen::Vector3d p_cam = frame.R_cam_world * (device.q * p_device + device.t - frame.t_world_cam);
	if (p_cam.z() <= 0.02) {
		return false;
	}

	if (check_visibility) {
		Eigen::Vector3d n_device(led.normal.x, led.normal.y, led.normal.z);
		Eigen::Vector3d n_cam = frame.R_cam_world * (device.q * n_device);
		// The view ray points away from the camera and the normal towards it (see pose_metrics.c).
		double facing = p_cam.normalized().dot(n_cam);
		if (facing > std::cos(M_PI - (led.visibility_angle - visibility_margin_rad))) {
			return false;
		}
	}

	float u, v;
	if (!t_camera_models_project(camera.model, (float)p_cam.x(), (float)p_cam.y(), (float)p_cam.z(), &u, &v)) {
		return false;
	}
	if (u < 0 || v < 0 || u >= camera.width || v >= camera.height) {
		return false;
	}
	out_px = Eigen::Vector2d(u, v);
	return true;
}

struct Correspondence
{
	uint32_t camera;
	uint32_t blob;
	uint32_t led;
};

//! Greedy nearest-first, one-to-one association of visible LEDs and free blobs within @p gate_px.
std::vector<Correspondence>
associate(const std::vector<JointSolveCamera> &cameras,
          const std::vector<CameraFrame> &frames,
          const t_constellation_tracker_led_model &model,
          const Rigid &device,
          float gate_px)
{
	struct Candidate
	{
		double d2;
		Correspondence c;
	};
	std::vector<Candidate> candidates;
	const double gate2 = (double)gate_px * gate_px;

	for (uint32_t ci = 0; ci < cameras.size(); ci++) {
		const JointSolveCamera &camera = cameras[ci];
		for (uint32_t li = 0; li < model.led_count; li++) {
			Eigen::Vector2d px;
			if (!project_led(camera, frames[ci], model.leds[li], device, true, px)) {
				continue;
			}
			for (uint32_t bi = 0; bi < camera.blob_count; bi++) {
				if (camera.blob_owner != nullptr &&
				    camera.blob_owner[bi] != XRT_CONSTELLATION_INVALID_DEVICE_ID) {
					continue;
				}
				double dx = camera.blobs[bi].center.x - px.x();
				double dy = camera.blobs[bi].center.y - px.y();
				double d2 = dx * dx + dy * dy;
				if (d2 <= gate2) {
					candidates.push_back(Candidate{d2, Correspondence{ci, bi, li}});
				}
			}
		}
	}

	std::sort(candidates.begin(), candidates.end(),
	          [](const Candidate &a, const Candidate &b) { return a.d2 < b.d2; });

	std::vector<Correspondence> out;
	std::vector<std::vector<bool>> blob_used(cameras.size());
	std::vector<std::vector<bool>> led_used(cameras.size(), std::vector<bool>(model.led_count, false));
	for (uint32_t ci = 0; ci < cameras.size(); ci++) {
		blob_used[ci].assign(cameras[ci].blob_count, false);
	}
	for (const Candidate &candidate : candidates) {
		const Correspondence &c = candidate.c;
		if (blob_used[c.camera][c.blob] || led_used[c.camera][c.led]) {
			continue;
		}
		blob_used[c.camera][c.blob] = true;
		led_used[c.camera][c.led] = true;
		out.push_back(c);
	}
	return out;
}

//! Residuals (projection minus blob) for every correspondence; unprojectable ones get a large residual.
Eigen::VectorXd
residuals(const std::vector<JointSolveCamera> &cameras,
          const std::vector<CameraFrame> &frames,
          const t_constellation_tracker_led_model &model,
          const Rigid &device,
          const std::vector<Correspondence> &matches)
{
	Eigen::VectorXd r(2 * matches.size());
	for (size_t i = 0; i < matches.size(); i++) {
		const Correspondence &c = matches[i];
		Eigen::Vector2d px;
		if (project_led(cameras[c.camera], frames[c.camera], model.leds[c.led], device, false, px)) {
			r(2 * i) = px.x() - cameras[c.camera].blobs[c.blob].center.x;
			r(2 * i + 1) = px.y() - cameras[c.camera].blobs[c.blob].center.y;
		} else {
			r(2 * i) = 100.0;
			r(2 * i + 1) = 100.0;
		}
	}
	return r;
}

struct OrientationPrior
{
	bool enabled{false};
	Eigen::Quaterniond q;
	double inv_sigma_rad{0.0};
};

//! Reprojection residuals followed, if enabled, by the orientation prior's three (in sigma units).
Eigen::VectorXd
all_residuals(const std::vector<JointSolveCamera> &cameras,
              const std::vector<CameraFrame> &frames,
              const t_constellation_tracker_led_model &model,
              const Rigid &device,
              const std::vector<Correspondence> &matches,
              const OrientationPrior &orientation)
{
	Eigen::VectorXd r = residuals(cameras, frames, model, device, matches);
	if (!orientation.enabled) {
		return r;
	}
	Eigen::AngleAxisd error(device.q * orientation.q.conjugate());
	Eigen::Vector3d log = error.angle() * error.axis();
	if (error.angle() > M_PI) {
		log = (error.angle() - 2.0 * M_PI) * error.axis();
	}
	Eigen::VectorXd out(r.size() + 3);
	out << r, log * orientation.inv_sigma_rad;
	return out;
}

//! One robust Gauss-Newton step with a numeric Jacobian. Returns the updated pose.
Rigid
gauss_newton_step(const std::vector<JointSolveCamera> &cameras,
                  const std::vector<CameraFrame> &frames,
                  const t_constellation_tracker_led_model &model,
                  const Rigid &device,
                  const std::vector<Correspondence> &matches,
                  double huber_px,
                  const OrientationPrior &orientation)
{
	const Eigen::VectorXd r0 = all_residuals(cameras, frames, model, device, matches, orientation);
	const Eigen::Index n = r0.size();

	Eigen::Matrix<double, Eigen::Dynamic, 6> J(n, 6);
	// t_camera_models_project works in single precision, so steps much below 1e-4 differentiate rounding noise.
	const double eps[6] = {1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4};
	for (int k = 0; k < 6; k++) {
		Eigen::Matrix<double, 6, 1> delta = Eigen::Matrix<double, 6, 1>::Zero();
		delta(k) = eps[k];
		J.col(k) = (all_residuals(cameras, frames, model, apply_increment(device, delta), matches, orientation) -
		            r0) /
		           eps[k];
	}

	// Huber weights per correspondence (both coordinates share one weight); the prior's rows keep weight 1.
	Eigen::VectorXd w = Eigen::VectorXd::Ones(n);
	const Eigen::Index reprojection_rows = 2 * (Eigen::Index)matches.size();
	for (Eigen::Index i = 0; i < reprojection_rows; i += 2) {
		double e = std::hypot(r0(i), r0(i + 1));
		double weight = e <= huber_px ? 1.0 : huber_px / e;
		w(i) = weight;
		w(i + 1) = weight;
	}

	Eigen::Matrix<double, 6, 6> H = J.transpose() * w.asDiagonal() * J;
	Eigen::Matrix<double, 6, 1> g = J.transpose() * w.asDiagonal() * r0;
	H.diagonal().array() += 1e-9 * std::max(1.0, H.trace());
	Eigen::Matrix<double, 6, 1> delta = -H.ldlt().solve(g);
	if (!delta.allFinite()) {
		return device;
	}
	return apply_increment(device, delta);
}

} // namespace

namespace {

bool
refine_impl(const std::vector<JointSolveCamera> &cameras,
            const t_constellation_tracker_led_model &model,
            const xrt_pose &prior,
            const OrientationPrior &orientation,
            const JointSolveParams &params,
            JointSolveResult &out)
{
	out = JointSolveResult{};

	std::vector<CameraFrame> frames(cameras.size());
	for (size_t i = 0; i < cameras.size(); i++) {
		Rigid cam = from_xrt(cameras[i].Tcv_world_cam);
		frames[i].R_cam_world = cam.q.toRotationMatrix().transpose();
		frames[i].t_world_cam = cam.t;
	}

	Rigid device = from_xrt(prior);
	std::vector<Correspondence> matches;
	for (float gate : params.gates_px) {
		matches = associate(cameras, frames, model, device, gate);
		if (matches.size() < 3) {
			break;
		}
		for (int step = 0; step < params.steps_per_gate; step++) {
			device = gauss_newton_step(cameras, frames, model, device, matches, params.huber_px, orientation);
		}
	}

	// Settle at the tightest gate, then measure the correspondences that support the pose.
	float final_gate = params.gates_px.empty() ? 3.0f : params.gates_px.back();
	matches = associate(cameras, frames, model, device, final_gate);
	if (matches.size() >= 3) {
		for (int step = 0; step < params.steps_per_gate; step++) {
			device = gauss_newton_step(cameras, frames, model, device, matches, params.huber_px, orientation);
		}
		matches = associate(cameras, frames, model, device, final_gate);
	}
	Eigen::VectorXd r = residuals(cameras, frames, model, device, matches);

	std::vector<bool> camera_used(cameras.size(), false);
	double sum2 = 0.0;
	for (size_t i = 0; i < matches.size(); i++) {
		double e2 = r(2 * i) * r(2 * i) + r(2 * i + 1) * r(2 * i + 1);
		sum2 += e2;
		if (e2 > (double)params.outlier_px * params.outlier_px) {
			out.outliers++;
		}
		camera_used[matches[i].camera] = true;
		out.correspondences.push_back(
		    JointSolveMatch{matches[i].camera, matches[i].blob, matches[i].led, (float)std::sqrt(e2)});
	}

	// Coverage counts only LEDs that face a camera comfortably: edge-on LEDs, and ones the ring hides from itself
	// (which a normal test cannot know about), are often missing on a correct pose.
	const double margin = params.coverage_margin_deg * M_PI / 180.0;
	uint32_t covered = 0;
	for (uint32_t ci = 0; ci < cameras.size(); ci++) {
		for (uint32_t li = 0; li < model.led_count; li++) {
			Eigen::Vector2d px;
			if (!project_led(cameras[ci], frames[ci], model.leds[li], device, true, px, margin)) {
				continue;
			}
			out.visible_leds++;
			for (const Correspondence &m : matches) {
				if (m.camera == ci && m.led == li) {
					covered++;
					break;
				}
			}
		}
	}

	out.Tcv_world_device = to_xrt(device);
	out.matches = (uint32_t)matches.size();
	out.cameras_used = (uint32_t)std::count(camera_used.begin(), camera_used.end(), true);
	out.rms_px = matches.empty() ? INFINITY : (float)std::sqrt(sum2 / (double)matches.size());
	out.coverage = out.visible_leds > 0 ? (float)covered / (float)out.visible_leds : 0.0f;
	out.ok = out.matches >= params.min_matches && out.rms_px <= params.max_rms_px &&
	         out.coverage >= params.min_coverage &&
	         (float)out.outliers <= params.max_outlier_fraction * (float)out.matches;
	return out.ok;
}

} // namespace

bool
joint_solve_refine(const std::vector<JointSolveCamera> &cameras,
                   const t_constellation_tracker_led_model &model,
                   const xrt_pose &prior,
                   const JointSolveParams &params,
                   JointSolveResult &out)
{
	return refine_impl(cameras, model, prior, OrientationPrior{}, params, out);
}

bool
joint_solve_refine(const std::vector<JointSolveCamera> &cameras,
                   const t_constellation_tracker_led_model &model,
                   const xrt_pose &prior,
                   const xrt_quat &orientation_prior,
                   const JointSolveParams &params,
                   JointSolveResult &out)
{
	OrientationPrior orientation;
	if (params.orientation_prior_sigma_deg > 0.0f) {
		orientation.enabled = true;
		orientation.q = Eigen::Quaterniond(orientation_prior.w, orientation_prior.x, orientation_prior.y,
		                                   orientation_prior.z)
		                    .normalized();
		orientation.inv_sigma_rad = 1.0 / (params.orientation_prior_sigma_deg * M_PI / 180.0);
	}
	return refine_impl(cameras, model, prior, orientation, params, out);
}

} // namespace xrt::tracking::constellation
