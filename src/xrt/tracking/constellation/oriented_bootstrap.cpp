// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Re-acquisition of a constellation-tracked device from one camera, given its orientation.
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "oriented_bootstrap.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <Eigen/LU>

#include <algorithm>
#include <cmath>

namespace xrt::tracking::constellation {

namespace {

	struct Hypothesis
	{
		Eigen::Vector3d t;
		uint32_t inliers{0};
		double error{0.0};
	};

	Eigen::Matrix3d
	skew(const Eigen::Vector3d &v)
	{
		Eigen::Matrix3d m;
		m << 0, -v.z(), v.y(), v.z(), 0, -v.x(), -v.y(), v.x(), 0;
		return m;
	}

	//! One camera's view of the device at a known orientation.
	struct View
	{
		const JointSolveCamera *camera;
		Eigen::Matrix3d R_cw;                 //!< World to camera rotation.
		Eigen::Vector3d t_wc;                 //!< Camera position in the world.
		Eigen::Matrix3d R_wd;                 //!< Device to world rotation (the known orientation).
		std::vector<uint32_t> blobs;          //!< Free blob indices.
		std::vector<Eigen::Vector3d> rays;    //!< Their unit rays, camera frame.
		std::vector<Eigen::Vector3d> led_cam; //!< R_cw * R_wd * led position (translation-free part).
		std::vector<Eigen::Vector3d> normal_cam;

		Eigen::Vector3d
		led_in_camera(uint32_t led, const Eigen::Vector3d &t) const
		{
			return led_cam[led] + R_cw * (t - t_wc);
		}
	};

	//! Blobs this hypothesis explains in its camera (each blob at most once), and their summed squared error.
	void
	score(const View &view,
	      const t_constellation_tracker_led_model &model,
	      const OrientedBootstrapParams &params,
	      Hypothesis &h)
	{
		const double gate2 = (double)params.inlier_px * params.inlier_px;
		std::vector<bool> used(view.blobs.size(), false);
		h.inliers = 0;
		h.error = 0.0;
		for (uint32_t i = 0; i < model.led_count; i++) {
			Eigen::Vector3d p = view.led_in_camera(i, h.t);
			if (p.z() <= 0.02) {
				continue;
			}
			// The view ray points away from the camera and the normal towards it (see pose_metrics.c).
			if (p.normalized().dot(view.normal_cam[i]) > std::cos(M_PI - model.leds[i].visibility_angle)) {
				continue;
			}
			float u, v;
			if (!t_camera_models_project(view.camera->model, (float)p.x(), (float)p.y(), (float)p.z(), &u,
			                             &v)) {
				continue;
			}
			double best = gate2;
			int best_k = -1;
			for (size_t k = 0; k < view.blobs.size(); k++) {
				if (used[k]) {
					continue;
				}
				const t_blob &b = view.camera->blobs[view.blobs[k]];
				double dx = b.center.x - u, dy = b.center.y - v;
				double d2 = dx * dx + dy * dy;
				if (d2 < best) {
					best = d2;
					best_k = (int)k;
				}
			}
			if (best_k >= 0) {
				used[best_k] = true;
				h.inliers++;
				h.error += best;
			}
		}
	}

} // namespace

bool
oriented_bootstrap(const std::vector<JointSolveCamera> &cameras,
                   const t_constellation_tracker_led_model &model,
                   const xrt_quat &Tcv_world_orientation,
                   const OrientedBootstrapParams &params,
                   OrientedBootstrapResult &out)
{
	out = OrientedBootstrapResult{};
	if (model.led_count < 2) {
		return false;
	}
	Eigen::Quaterniond q_wd(Tcv_world_orientation.w, Tcv_world_orientation.x, Tcv_world_orientation.y,
	                        Tcv_world_orientation.z);
	q_wd.normalize();
	const Eigen::Matrix3d R_wd = q_wd.toRotationMatrix();

	struct Candidate
	{
		int camera;
		Hypothesis h;
	};
	std::vector<Candidate> best;

	for (size_t ci = 0; ci < cameras.size() && out.hypotheses < params.max_hypotheses; ci++) {
		const JointSolveCamera &camera = cameras[ci];
		View view{};
		view.camera = &camera;
		Eigen::Quaterniond q_wc(camera.Tcv_world_cam.orientation.w, camera.Tcv_world_cam.orientation.x,
		                        camera.Tcv_world_cam.orientation.y, camera.Tcv_world_cam.orientation.z);
		q_wc.normalize();
		view.R_cw = q_wc.toRotationMatrix().transpose();
		view.t_wc = Eigen::Vector3d(camera.Tcv_world_cam.position.x, camera.Tcv_world_cam.position.y,
		                            camera.Tcv_world_cam.position.z);
		view.R_wd = R_wd;

		for (uint32_t bi = 0; bi < camera.blob_count; bi++) {
			if (camera.blob_owner != nullptr &&
			    camera.blob_owner[bi] != XRT_CONSTELLATION_INVALID_DEVICE_ID) {
				continue;
			}
			float x, y, z;
			if (!t_camera_models_unproject(camera.model, camera.blobs[bi].center.x,
			                               camera.blobs[bi].center.y, &x, &y, &z)) {
				continue;
			}
			Eigen::Vector3d d(x, y, z);
			if (d.norm() < 1e-9 || d.z() <= 0.0) {
				continue;
			}
			view.blobs.push_back(bi);
			view.rays.push_back(d.normalized());
		}
		if (view.blobs.size() < params.min_blobs) {
			continue;
		}

		for (uint32_t i = 0; i < model.led_count; i++) {
			const t_constellation_tracker_led &led = model.leds[i];
			view.led_cam.push_back(
			    view.R_cw * (R_wd * Eigen::Vector3d(led.position.x, led.position.y, led.position.z)));
			view.normal_cam.push_back(view.R_cw *
			                          (R_wd * Eigen::Vector3d(led.normal.x, led.normal.y, led.normal.z)));
		}

		// LEDs that could produce each blob: facing back along its ray, with a margin for orientation error.
		const double margin = params.facing_margin_deg * M_PI / 180.0;
		std::vector<std::vector<uint32_t>> candidates(view.blobs.size());
		for (size_t k = 0; k < view.blobs.size(); k++) {
			for (uint32_t i = 0; i < model.led_count; i++) {
				// Widen each LED's visibility cone by the margin: the orientation is only approximately
				// known.
				double limit = std::cos(M_PI - std::min(M_PI, model.leds[i].visibility_angle + margin));
				if (view.rays[k].dot(view.normal_cam[i]) <= limit) {
					candidates[k].push_back(i);
				}
			}
		}

		for (size_t k1 = 0; k1 < view.blobs.size() && out.hypotheses < params.max_hypotheses; k1++) {
			for (size_t k2 = k1 + 1; k2 < view.blobs.size() && out.hypotheses < params.max_hypotheses;
			     k2++) {
				const Eigen::Matrix3d S1 = skew(view.rays[k1]);
				const Eigen::Matrix3d S2 = skew(view.rays[k2]);
				// Collinearity: rays[k] x (led_cam[i] + R_cw (t - t_wc)) = 0, linear in t.
				const Eigen::Matrix3d A1 = S1 * view.R_cw;
				const Eigen::Matrix3d A2 = S2 * view.R_cw;
				const Eigen::Matrix3d N = A1.transpose() * A1 + A2.transpose() * A2;
				if (std::fabs(N.determinant()) < 1e-12) {
					continue;
				}
				const Eigen::Matrix3d N_inv = N.inverse();
				for (uint32_t i1 : candidates[k1]) {
					for (uint32_t i2 : candidates[k2]) {
						if (i1 == i2 || out.hypotheses >= params.max_hypotheses) {
							continue;
						}
						out.hypotheses++;
						Eigen::Vector3d b1 = -S1 * (view.led_cam[i1] - view.R_cw * view.t_wc);
						Eigen::Vector3d b2 = -S2 * (view.led_cam[i2] - view.R_cw * view.t_wc);
						Hypothesis h;
						h.t = N_inv * (A1.transpose() * b1 + A2.transpose() * b2);
						Eigen::Vector3d p1 = view.led_in_camera(i1, h.t);
						Eigen::Vector3d p2 = view.led_in_camera(i2, h.t);
						if (p1.z() < params.min_depth_m || p2.z() < params.min_depth_m ||
						    p1.z() > params.max_depth_m || p2.z() > params.max_depth_m ||
						    p1.dot(view.rays[k1]) <= 0.0 || p2.dot(view.rays[k2]) <= 0.0) {
							continue;
						}
						score(view, model, params, h);
						if (h.inliers < params.min_inliers) {
							continue;
						}
						// Keep the best few distinct hypotheses (most blobs explained, then
						// least error).
						auto better = [](const Hypothesis &a, const Hypothesis &b) {
							return a.inliers > b.inliers ||
							       (a.inliers == b.inliers && a.error < b.error);
						};
						bool duplicate = false;
						for (Candidate &c : best) {
							if ((c.h.t - h.t).norm() < 0.005) {
								duplicate = true;
								if (better(h, c.h)) {
									c = Candidate{(int)ci, h};
								}
							}
						}
						if (!duplicate) {
							best.push_back(Candidate{(int)ci, h});
						}
						std::sort(best.begin(), best.end(),
						          [&](const Candidate &a, const Candidate &b) {
							          return better(a.h, b.h);
						          });
						if (best.size() > params.refine_candidates) {
							best.resize(params.refine_candidates);
						}
					}
				}
			}
		}
	}

	for (const Candidate &c : best) {
		out.inliers = std::max(out.inliers, c.h.inliers);
		xrt_pose prior;
		prior.orientation = Tcv_world_orientation;
		prior.position = xrt_vec3{(float)c.h.t.x(), (float)c.h.t.y(), (float)c.h.t.z()};
		if (joint_solve_refine(cameras, model, prior, Tcv_world_orientation, params.refine, out.refined)) {
			out.ok = true;
			out.camera = c.camera;
			return true;
		}
	}
	return false;
}

} // namespace xrt::tracking::constellation
