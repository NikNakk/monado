// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pose bootstrap for constellation-tracked devices from multi-camera triangulation.
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "stereo_bootstrap.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <Eigen/SVD>

#include <algorithm>
#include <cmath>
#include <map>

namespace xrt::tracking::constellation {

namespace {

struct Ray
{
	uint32_t camera;
	uint32_t blob;
	Eigen::Vector3d origin;
	Eigen::Vector3d direction;
};

struct Point
{
	Eigen::Vector3d position;
	uint32_t observations{0};
	//! Blob index per camera, or -1.
	std::vector<int> blob_of_camera;
	//! Cameras that saw it (for LED normal checks).
	std::vector<Eigen::Vector3d> camera_positions;
};

struct Hypothesis
{
	Eigen::Matrix3d R;
	Eigen::Vector3d t;
	uint32_t inliers{0};
	double error{0.0};
};

std::vector<Ray>
make_rays(const std::vector<JointSolveCamera> &cameras)
{
	std::vector<Ray> rays;
	for (uint32_t ci = 0; ci < cameras.size(); ci++) {
		const JointSolveCamera &camera = cameras[ci];
		Eigen::Quaterniond q(camera.Tcv_world_cam.orientation.w, camera.Tcv_world_cam.orientation.x,
		                     camera.Tcv_world_cam.orientation.y, camera.Tcv_world_cam.orientation.z);
		q.normalize();
		Eigen::Vector3d origin(camera.Tcv_world_cam.position.x, camera.Tcv_world_cam.position.y,
		                       camera.Tcv_world_cam.position.z);
		for (uint32_t bi = 0; bi < camera.blob_count; bi++) {
			if (camera.blob_owner != nullptr && camera.blob_owner[bi] != XRT_CONSTELLATION_INVALID_DEVICE_ID) {
				continue;
			}
			float x, y, z;
			if (!t_camera_models_unproject(camera.model, camera.blobs[bi].center.x, camera.blobs[bi].center.y, &x,
			                               &y, &z)) {
				continue;
			}
			Eigen::Vector3d d(x, y, z);
			if (d.norm() < 1e-9 || d.z() <= 0.0) {
				continue;
			}
			rays.push_back(Ray{ci, bi, origin, (q * d).normalized()});
		}
	}
	return rays;
}

//! Pair rays from different cameras that nearly intersect, and merge the midpoints that belong to the same LED.
std::vector<Point>
triangulate(const std::vector<Ray> &rays, size_t camera_count, const StereoBootstrapParams &params)
{
	struct Candidate
	{
		double gap;
		uint32_t a, b;
		Eigen::Vector3d midpoint;
	};
	std::vector<Candidate> candidates;
	for (uint32_t i = 0; i < rays.size(); i++) {
		for (uint32_t j = i + 1; j < rays.size(); j++) {
			const Ray &r1 = rays[i];
			const Ray &r2 = rays[j];
			if (r1.camera == r2.camera) {
				continue;
			}
			Eigen::Vector3d w0 = r1.origin - r2.origin;
			double b = r1.direction.dot(r2.direction);
			double d = r1.direction.dot(w0);
			double e = r2.direction.dot(w0);
			double denom = 1.0 - b * b;
			if (denom < 1e-9) {
				continue;
			}
			double s = (b * e - d) / denom;
			double t = (e - b * d) / denom;
			if (s < params.min_depth_m || t < params.min_depth_m || s > params.max_depth_m ||
			    t > params.max_depth_m) {
				continue;
			}
			Eigen::Vector3d p1 = r1.origin + s * r1.direction;
			Eigen::Vector3d p2 = r2.origin + t * r2.direction;
			double gap = (p1 - p2).norm();
			if (gap <= params.max_ray_gap_m) {
				candidates.push_back(Candidate{gap, i, j, 0.5 * (p1 + p2)});
			}
		}
	}
	std::sort(candidates.begin(), candidates.end(),
	          [](const Candidate &x, const Candidate &y) { return x.gap < y.gap; });

	std::vector<Point> points;
	std::vector<int> point_of_ray(rays.size(), -1);
	auto add_ray = [&](Point &point, uint32_t ray) {
		point.blob_of_camera[rays[ray].camera] = (int)rays[ray].blob;
		point.camera_positions.push_back(rays[ray].origin);
	};
	for (const Candidate &c : candidates) {
		int pa = point_of_ray[c.a];
		int pb = point_of_ray[c.b];
		if (pa >= 0 && pb >= 0) {
			continue;
		}
		int target = pa >= 0 ? pa : pb;
		if (target < 0) {
			// Join a nearby point that has no blob yet from either camera, or start a new one.
			for (size_t k = 0; k < points.size(); k++) {
				if ((points[k].position - c.midpoint).norm() <= params.merge_radius_m &&
				    points[k].blob_of_camera[rays[c.a].camera] < 0 &&
				    points[k].blob_of_camera[rays[c.b].camera] < 0) {
					target = (int)k;
					break;
				}
			}
		}
		if (target < 0) {
			Point point;
			point.position = c.midpoint;
			point.observations = 1;
			point.blob_of_camera.assign(camera_count, -1);
			add_ray(point, c.a);
			add_ray(point, c.b);
			points.push_back(point);
			point_of_ray[c.a] = point_of_ray[c.b] = (int)points.size() - 1;
			continue;
		}
		Point &point = points[target];
		if ((point.position - c.midpoint).norm() > params.merge_radius_m) {
			continue;
		}
		for (uint32_t ray : {c.a, c.b}) {
			if (point_of_ray[ray] < 0 && point.blob_of_camera[rays[ray].camera] < 0) {
				add_ray(point, ray);
				point_of_ray[ray] = target;
			}
		}
		point.position = (point.position * point.observations + c.midpoint) / (point.observations + 1);
		point.observations++;
	}
	return points;
}

bool
kabsch(const std::vector<Eigen::Vector3d> &from, const std::vector<Eigen::Vector3d> &to, Eigen::Matrix3d &R, Eigen::Vector3d &t)
{
	if (from.size() < 3 || from.size() != to.size()) {
		return false;
	}
	Eigen::Vector3d cf = Eigen::Vector3d::Zero(), ct = Eigen::Vector3d::Zero();
	for (size_t i = 0; i < from.size(); i++) {
		cf += from[i];
		ct += to[i];
	}
	cf /= (double)from.size();
	ct /= (double)to.size();
	Eigen::Matrix3d H = Eigen::Matrix3d::Zero();
	for (size_t i = 0; i < from.size(); i++) {
		H += (from[i] - cf) * (to[i] - ct).transpose();
	}
	Eigen::JacobiSVD<Eigen::Matrix3d> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);
	Eigen::Matrix3d D = Eigen::Matrix3d::Identity();
	D(2, 2) = (svd.matrixV() * svd.matrixU().transpose()).determinant() < 0 ? -1.0 : 1.0;
	R = svd.matrixV() * D * svd.matrixU().transpose();
	t = ct - R * cf;
	return R.allFinite() && t.allFinite();
}

//! Count points explained by the model at (R, t); an explaining LED must face a camera that saw the point.
void
score(Hypothesis &h,
      const std::vector<Point> &points,
      const std::vector<Eigen::Vector3d> &leds,
      const std::vector<Eigen::Vector3d> &normals,
      const StereoBootstrapParams &params,
      std::vector<std::pair<uint32_t, uint32_t>> *out_pairs = nullptr)
{
	h.inliers = 0;
	h.error = 0.0;
	const double r2 = (double)params.inlier_radius_m * params.inlier_radius_m;
	std::vector<Eigen::Vector3d> world(leds.size()), world_normals(leds.size());
	for (size_t l = 0; l < leds.size(); l++) {
		world[l] = h.R * leds[l] + h.t;
		world_normals[l] = h.R * normals[l];
	}
	for (uint32_t p = 0; p < points.size(); p++) {
		double best = r2;
		int best_led = -1;
		for (uint32_t l = 0; l < leds.size(); l++) {
			double d2 = (world[l] - points[p].position).squaredNorm();
			if (d2 >= best) {
				continue;
			}
			bool faces = false;
			for (const Eigen::Vector3d &camera : points[p].camera_positions) {
				faces |= world_normals[l].dot(camera - world[l]) > 0.0;
			}
			if (faces) {
				best = d2;
				best_led = (int)l;
			}
		}
		if (best_led >= 0) {
			h.inliers++;
			h.error += best;
			if (out_pairs) {
				out_pairs->push_back({(uint32_t)best_led, p});
			}
		}
	}
}

} // namespace

bool
stereo_bootstrap(const std::vector<JointSolveCamera> &cameras,
                 const t_constellation_tracker_led_model &model,
                 const StereoBootstrapParams &params,
                 StereoBootstrapResult &out)
{
	out = StereoBootstrapResult{};

	std::vector<Point> points = triangulate(make_rays(cameras), cameras.size(), params);
	out.points = (uint32_t)points.size();
	if (points.size() < 3 || model.led_count < 3) {
		return false;
	}

	std::vector<Eigen::Vector3d> leds(model.led_count), normals(model.led_count);
	for (size_t l = 0; l < model.led_count; l++) {
		leds[l] = Eigen::Vector3d(model.leds[l].position.x, model.leds[l].position.y, model.leds[l].position.z);
		normals[l] = Eigen::Vector3d(model.leds[l].normal.x, model.leds[l].normal.y, model.leds[l].normal.z);
	}
	const double tol = params.distance_tolerance_m;

	// Keep a few of the best hypotheses: the best by point count is occasionally a near-symmetric wrong fit.
	std::vector<Hypothesis> best;
	auto consider = [&](const Hypothesis &h) {
		best.push_back(h);
		std::sort(best.begin(), best.end(), [](const Hypothesis &a, const Hypothesis &b) {
			return a.inliers != b.inliers ? a.inliers > b.inliers : a.error < b.error;
		});
		if (best.size() > 4) {
			best.pop_back();
		}
	};

	const size_t n = points.size();
	for (size_t i = 0; i < n && out.hypotheses < params.max_hypotheses; i++) {
		for (size_t j = i + 1; j < n && out.hypotheses < params.max_hypotheses; j++) {
			double dij = (points[i].position - points[j].position).norm();
			for (size_t k = j + 1; k < n && out.hypotheses < params.max_hypotheses; k++) {
				double dik = (points[i].position - points[k].position).norm();
				double djk = (points[j].position - points[k].position).norm();
				Eigen::Vector3d cross =
				    (points[j].position - points[i].position).cross(points[k].position - points[i].position);
				if (cross.norm() < 4e-5) { // Near-collinear triples give an unstable rotation.
					continue;
				}
				for (uint32_t a = 0; a < model.led_count; a++) {
					for (uint32_t b = 0; b < model.led_count; b++) {
						if (a == b || std::fabs((leds[a] - leds[b]).norm() - dij) > tol) {
							continue;
						}
						for (uint32_t c = 0; c < model.led_count; c++) {
							if (c == a || c == b || std::fabs((leds[a] - leds[c]).norm() - dik) > tol ||
							    std::fabs((leds[b] - leds[c]).norm() - djk) > tol) {
								continue;
							}
							Hypothesis h;
							if (!kabsch({leds[a], leds[b], leds[c]},
							            {points[i].position, points[j].position, points[k].position}, h.R,
							            h.t)) {
								continue;
							}
							out.hypotheses++;
							score(h, points, leds, normals, params);
							if (h.inliers >= params.min_inliers) {
								consider(h);
							}
						}
					}
				}
			}
		}
	}

	for (Hypothesis &h : best) {
		out.inliers = std::max(out.inliers, h.inliers);

		// Re-fit to every explained point before handing over to the joint refinement.
		std::vector<std::pair<uint32_t, uint32_t>> pairs;
		score(h, points, leds, normals, params, &pairs);
		std::vector<Eigen::Vector3d> from, to;
		for (const auto &[led, point] : pairs) {
			from.push_back(leds[led]);
			to.push_back(points[point].position);
		}
		Eigen::Matrix3d R = h.R;
		Eigen::Vector3d t = h.t;
		kabsch(from, to, R, t);

		Eigen::Quaterniond q(R);
		q.normalize();
		xrt_pose prior;
		prior.orientation = xrt_quat{(float)q.x(), (float)q.y(), (float)q.z(), (float)q.w()};
		prior.position = xrt_vec3{(float)t.x(), (float)t.y(), (float)t.z()};
		if (joint_solve_refine(cameras, model, prior, params.refine, out.refined)) {
			out.ok = true;
			return true;
		}
	}
	return false;
}

} // namespace xrt::tracking::constellation
