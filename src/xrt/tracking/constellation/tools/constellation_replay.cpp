// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Offline replay of constellation tracker datasets (constellation.ctd).
 *
 * Loads a dataset written by the tracker's data recorder, regroups the camera samples into exposures and reports
 * what was recorded. Solvers are run against the same input so their results and cost can be compared.
 *
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "t_constellation_tracker_dataset.hpp"
#include "joint_pose_solver.hpp"
#include "stereo_bootstrap.hpp"
#include "t_imu_optical_filter.h"
#include "t_constellation_tracker.h"
#include "replay_records.hpp"
#include "frontend_compare.hpp"
#ifdef XRT_HAVE_CONSTELLATION_FUSION_EVAL
#include "fusion_compare.hpp"
#endif

#include "xrt/xrt_frame.h"

#include "math/m_api.h"
#include "util/u_file.h"
#include "util/u_json.h"

#include <Eigen/Geometry>
#include <Eigen/SVD>
#include "tracking/t_camera_models.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xrt::tracking::constellation;

namespace {

//! Samples from different cameras of one exposure are this close in time.
constexpr int64_t kExposureToleranceNs = 2'000'000;

struct Exposure
{
	int64_t timestamp_ns;
	std::vector<const CameraSample *> samples;
};

std::vector<Exposure>
group_exposures(const std::vector<CameraSample> &samples)
{
	std::vector<const CameraSample *> sorted;
	sorted.reserve(samples.size());
	for (const CameraSample &sample : samples) {
		sorted.push_back(&sample);
	}
	std::sort(sorted.begin(), sorted.end(),
	          [](const CameraSample *a, const CameraSample *b) { return a->timestamp_ns < b->timestamp_ns; });

	std::vector<Exposure> exposures;
	for (const CameraSample *sample : sorted) {
		if (exposures.empty() || sample->timestamp_ns - exposures.back().timestamp_ns > kExposureToleranceNs) {
			exposures.push_back(Exposure{sample->timestamp_ns, {}});
		}
		exposures.back().samples.push_back(sample);
	}
	return exposures;
}

struct Stats
{
	std::vector<double> values;

	void
	add(double v)
	{
		values.push_back(v);
	}

	double
	pct(double p)
	{
		if (values.empty()) {
			return NAN;
		}
		std::sort(values.begin(), values.end());
		return values[std::min(values.size() - 1, (size_t)(p * (double)(values.size() - 1) + 0.5))];
	}
};

const char *
flags_name(xrt_space_relation_flags flags)
{
	bool orientation = (flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0;
	bool position = (flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) != 0;
	if (orientation && position) {
		return "pose";
	}
	if (orientation) {
		return "orientation-only";
	}
	if (position) {
		return "position-only";
	}
	return "none";
}

int
summarise(const DatasetReader &dataset)
{
	std::printf("read: %s\n", dataset.stop_reason.empty() ? "clean end of file" : dataset.stop_reason.c_str());

	for (size_t m = 0; m < dataset.mosaics.size(); m++) {
		const DatasetMosaic &mosaic = dataset.mosaics[m];
		std::printf("mosaic %zu: %zu cameras\n", m, mosaic.camera_calibrations.size());
		for (size_t c = 0; c < mosaic.camera_calibrations.size(); c++) {
			const t_camera_calibration &cal = mosaic.camera_calibrations[c];
			std::printf("  camera %zu: %ux%u fx %.1f fy %.1f cx %.1f cy %.1f model %d\n", c,
			            cal.image_size_pixels.w, cal.image_size_pixels.h, cal.intrinsics[0][0],
			            cal.intrinsics[1][1], cal.intrinsics[0][2], cal.intrinsics[1][2],
			            (int)cal.distortion_model);
		}
	}

	for (const DatasetDevice &device : dataset.devices) {
		std::printf("device %d: %zu LEDs\n", (int)device.id, device.leds.size());
	}

	for (const std::string &info : dataset.session_info) {
		std::printf("session: %s\n", info.c_str());
	}
	if (!dataset.sync_events.empty() || !dataset.imu_timing.empty() || !dataset.head_poses.empty() ||
	    !dataset.ground_truth.empty() || !dataset.annotations.empty() || dataset.unknown_extensions > 0) {
		std::printf(
		    "extension records: %zu sync events, %zu IMU timings, %zu head poses, %zu ground-truth poses, "
		    "%zu annotations, %zu of unknown kinds\n",
		    dataset.sync_events.size(), dataset.imu_timing.size(), dataset.head_poses.size(),
		    dataset.ground_truth.size(), dataset.annotations.size(), dataset.unknown_extensions);
	}
	if (!dataset.head_poses.empty()) {
		Stats age_ms;
		size_t predicted = 0;
		for (const DatasetHeadPose &h : dataset.head_poses) {
			if (h.source_ns == 0) {
				continue;
			}
			age_ms.add((double)(h.timestamp_ns - h.source_ns) / 1e6);
			predicted += (h.source_flags & T_CONSTELLATION_HEAD_POSE_INTERPOLATED) == 0 ? 1 : 0;
		}
		std::printf(
		    "head pose at exposure minus newest SLAM pose, ms: p05 %.1f p50 %.1f p95 %.1f; %zu of %zu "
		    "predicted past it\n",
		    age_ms.pct(0.05), age_ms.pct(0.5), age_ms.pct(0.95), predicted, age_ms.values.size());
	}
	for (const DatasetAnnotation &a : dataset.annotations) {
		std::printf("annotation at %" PRIi64 " device %d: %s\n", a.host_ns, (int)a.device_id, a.text.c_str());
	}

	std::map<uint32_t, size_t> samples_per_camera;
	std::map<uint32_t, size_t> posed_per_camera;
	std::map<uint32_t, size_t> blobs_per_camera;
	std::map<std::pair<int, uint32_t>, size_t> found_per_device_camera;
	for (const CameraSample &sample : dataset.samples) {
		samples_per_camera[sample.camera_index]++;
		posed_per_camera[sample.camera_index] += sample.Txr_world_cam.has_value() ? 1 : 0;
		blobs_per_camera[sample.camera_index] += sample.blob_count;
		for (uint32_t d = 0; d < sample.device_count; d++) {
			const DeviceState &state = sample.device_states[d];
			if (state.found_pose.has_value()) {
				found_per_device_camera[{(int)state.device_id, sample.camera_index}]++;
			}
		}
	}
	for (const auto &[camera, count] : samples_per_camera) {
		std::printf("camera %u: %zu samples, %zu with a camera pose, mean %.1f blobs\n", camera, count,
		            posed_per_camera[camera], (double)blobs_per_camera[camera] / (double)count);
	}
	for (const auto &[key, count] : found_per_device_camera) {
		std::printf("device %d camera %u: %zu recorded poses\n", key.first, key.second, count);
	}

	std::vector<Exposure> exposures = group_exposures(dataset.samples);
	std::map<size_t, size_t> exposure_sizes;
	for (const Exposure &exposure : exposures) {
		exposure_sizes[exposure.samples.size()]++;
	}
	if (!exposures.empty()) {
		double span_s = (double)(exposures.back().timestamp_ns - exposures.front().timestamp_ns) / 1e9;
		std::printf("exposures: %zu over %.1f s (%.1f Hz); cameras per exposure:", exposures.size(), span_s,
		            span_s > 0 ? (double)exposures.size() / span_s : 0.0);
		for (const auto &[size, count] : exposure_sizes) {
			std::printf(" %zu:%zu", size, count);
		}
		std::printf("\n");
	}

	std::map<std::pair<int, std::string>, size_t> tracking_kinds;
	for (const DatasetDeviceTracking &tracking : dataset.device_tracking) {
		tracking_kinds[{(int)tracking.device_id, flags_name(tracking.relation_flags)}]++;
	}
	std::printf("device tracking packets: %zu\n", dataset.device_tracking.size());
	for (const auto &[key, count] : tracking_kinds) {
		std::printf("  device %d %s: %zu\n", key.first, key.second.c_str(), count);
	}

	return dataset.stop_reason.empty() ? 0 : 1;
}

/*
 *
 * M1 replay: joint multi-camera tracking from recorded blobs.
 *
 */


double
quat_angle_deg(const xrt_quat &a, const xrt_quat &b)
{
	double dot = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
	return 2.0 * std::acos(std::min(1.0, dot)) * 180.0 / M_PI;
}

double
distance_m(const xrt_vec3 &a, const xrt_vec3 &b)
{
	return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

struct DeviceTrack
{
	const DatasetDevice *device;
	bool tracking{false};
	xrt_pose Tcv_world_device{};
	int64_t last_solved_ns{0};
	uint32_t consecutive_failures{0};

	//! optical = align * imu, refreshed from every solve (as the driver's optical_from_imu_orientation).
	bool have_align{false};
	xrt_quat align{0, 0, 0, 1};

	// Results.
	uint32_t exposures_with_blobs{0};
	uint32_t solved{0};
	uint32_t seeds{0};
	uint32_t bootstrap_attempts{0};
	uint32_t bootstraps{0};
	Stats bootstrap_us;
	std::map<uint32_t, uint32_t> cameras_used;
	Stats rms_px, coverage, matches, solve_us, recorded_delta_mm, recorded_delta_deg;
	//! Tilt of the optical-from-IMU alignment: how far it moves the vertical. ~0 if both worlds agree on gravity.
	Stats align_tilt_deg;
	//! (optical, IMU) orientation pairs from solved exposures, for estimating the IMU-to-model body offset.
	std::vector<std::pair<Eigen::Quaterniond, Eigen::Quaterniond>> orientation_pairs;
	std::vector<std::pair<int64_t, xrt_vec3>> positions;
};

//! The device's recorded tracking-source orientation at @p timestamp_ns (OpenCV convention), if any.
bool
imu_orientation_at(const DatasetReader &dataset,
                   t_constellation_device_id_t device_id,
                   int64_t timestamp_ns,
                   xrt_quat &out)
{
	const DatasetDeviceTracking *best = nullptr;
	for (const DatasetDeviceTracking &t : dataset.device_tracking) {
		if (t.device_id != device_id || (t.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) == 0) {
			continue;
		}
		if (best == nullptr ||
		    std::llabs(t.timestamp_ns - timestamp_ns) < std::llabs(best->timestamp_ns - timestamp_ns)) {
			best = &t;
		}
	}
	if (best == nullptr || std::llabs(best->timestamp_ns - timestamp_ns) > kExposureToleranceNs) {
		return false;
	}
	xrt_pose cv;
	math_pose_convert_from_opencv(&best->pose, &cv);
	out = cv.orientation;
	return true;
}

//! A recorded per-camera pose for the device in this exposure (the live tracker's candidate), in the CV world.
bool
recorded_pose(const Exposure &exposure, t_constellation_device_id_t device_id, xrt_pose &out)
{
	for (const CameraSample *sample : exposure.samples) {
		if (!sample->Txr_world_cam.has_value()) {
			continue;
		}
		for (uint32_t d = 0; d < sample->device_count; d++) {
			const DeviceState &state = sample->device_states[d];
			if (state.device_id != device_id || !state.found_pose.has_value()) {
				continue;
			}
			xrt_pose Tcv_world_cam;
			math_pose_convert_from_opencv(&sample->Txr_world_cam.value(), &Tcv_world_cam);
			math_pose_transform(&Tcv_world_cam, &state.found_pose->Tcv_cam_device, &out);
			return true;
		}
	}
	return false;
}

/*!
 * Estimate the fixed rotation B between the IMU body frame and the LED model frame, from q_opt = A q_imu B with A a
 * constant world alignment. Body-frame relative rotations satisfy q_opt,rel = B^-1 q_imu,rel B, so B^-1 maps each IMU
 * relative-rotation axis onto the optical one (Kabsch). With B known, A = q_opt B^-1 q_imu^-1 should be constant and,
 * if both worlds are gravity-aligned, a pure rotation about the vertical.
 */
void
report_imu_offset(const DeviceTrack &track)
{
	const auto &pairs = track.orientation_pairs;
	if (pairs.size() < 50) {
		return;
	}

	Eigen::Matrix3d H = Eigen::Matrix3d::Zero();
	uint32_t used = 0;
	const size_t stride = std::max<size_t>(1, pairs.size() / 400);
	for (size_t i = 0; i < pairs.size(); i += stride) {
		for (size_t j = i + stride; j < pairs.size(); j += stride) {
			Eigen::AngleAxisd opt_rel(pairs[j].first.conjugate() * pairs[i].first);
			Eigen::AngleAxisd imu_rel(pairs[j].second.conjugate() * pairs[i].second);
			if (opt_rel.angle() < 10.0 * M_PI / 180.0 || imu_rel.angle() < 10.0 * M_PI / 180.0) {
				continue;
			}
			double weight = std::min(opt_rel.angle(), imu_rel.angle());
			H += weight * imu_rel.axis() * opt_rel.axis().transpose();
			used++;
		}
	}
	if (used < 20) {
		std::printf("  IMU body offset: not enough rotation to estimate (%u pairs over 10 degrees)\n", used);
		return;
	}
	Eigen::JacobiSVD<Eigen::Matrix3d> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);
	Eigen::Matrix3d D = Eigen::Matrix3d::Identity();
	D(2, 2) = (svd.matrixV() * svd.matrixU().transpose()).determinant() < 0 ? -1.0 : 1.0;
	// R maps IMU axes to optical axes: axis_opt = R axis_imu, and R = B^-1.
	Eigen::Matrix3d R = svd.matrixV() * D * svd.matrixU().transpose();
	Eigen::Quaterniond B_inv(R);
	Eigen::Quaterniond B = B_inv.conjugate();

	Stats axis_residual_deg, tilt_deg, align_spread_deg;
	std::vector<Eigen::Quaterniond> aligns;
	for (size_t i = 0; i < pairs.size(); i += stride) {
		Eigen::Quaterniond A = pairs[i].first * B_inv * pairs[i].second.conjugate();
		aligns.push_back(A);
		Eigen::Vector3d up(0, -1, 0);
		double c = std::max(-1.0, std::min(1.0, up.dot(A * up)));
		tilt_deg.add(std::acos(c) * 180.0 / M_PI);
	}
	for (const auto &A : aligns) {
		align_spread_deg.add(A.angularDistance(aligns[aligns.size() / 2]) * 180.0 / M_PI);
	}
	Eigen::AngleAxisd b(B);
	std::printf(
	    "  IMU body offset B: %.1f deg about (%.3f, %.3f, %.3f) from %u relative rotations; quat "
	    "(x %.4f, y %.4f, z %.4f, w %.4f)\n",
	    b.angle() * 180.0 / M_PI, b.axis().x(), b.axis().y(), b.axis().z(), used, B.x(), B.y(), B.z(), B.w());
	std::printf("  with B: world alignment tilt deg p50 %.2f p95 %.2f; alignment spread deg p50 %.2f p95 %.2f\n",
	            tilt_deg.pct(0.5), tilt_deg.pct(0.95), align_spread_deg.pct(0.5), align_spread_deg.pct(0.95));
}

/*!
 * Replace the dataset's camera calibration with a psvr2-constellation calibration JSON, so one recording can be
 * replayed against different calibrations. Intrinsics are swapped; each sample's world pose keeps the recorded
 * tracking origin (recorded camera c pose times the inverse of its recorded pose relative to camera 0) and applies
 * the file's camera pose in that origin. Both calibrations must use camera 0 as their tracking origin.
 */
//! head_from_camera0_xrt from a calibration file, identity if the file or the field is absent.
xrt_pose
read_head_from_camera0(const char *path)
{
	xrt_pose pose = XRT_POSE_IDENTITY;
	if (path == nullptr) {
		return pose;
	}
	char *contents = u_file_read_content_from_path(path, nullptr);
	cJSON *root = contents ? cJSON_Parse(contents) : nullptr;
	std::free(contents);
	if (!u_json_get_pose(u_json_get(root, "head_from_camera0_xrt"), &pose)) {
		pose = XRT_POSE_IDENTITY;
	}
	cJSON_Delete(root);
	return pose;
}

void
override_calibration(DatasetReader &dataset, const char *path, const char *recorded_calibration)
{
	if (dataset.mosaics.empty()) {
		throw std::runtime_error("no cameras to override");
	}
	DatasetMosaic &mosaic = dataset.mosaics[0];
	const size_t camera_count = mosaic.camera_calibrations.size();

	char *contents = u_file_read_content_from_path(path, nullptr);
	cJSON *root = contents ? cJSON_Parse(contents) : nullptr;
	std::free(contents);
	const cJSON *cameras = u_json_get(root, "cameras");
	if (!cJSON_IsArray(cameras) || (size_t)cJSON_GetArraySize(cameras) != camera_count) {
		cJSON_Delete(root);
		throw std::runtime_error("calibration has no matching cameras array");
	}
	std::vector<xrt_pose> new_in_origin(camera_count);
	for (size_t c = 0; c < camera_count; c++) {
		const cJSON *camera = cJSON_GetArrayItem(cameras, (int)c);
		const cJSON *cal = u_json_get(camera, "calibration");
		const cJSON *in = u_json_get(cal, "intrinsics");
		const cJSON *d = u_json_get(cal, "distortion");
		double v[8];
		const char *names[8] = {"fx", "fy", "cx", "cy", "k1", "k2", "k3", "k4"};
		bool good = u_json_get_pose(u_json_get(camera, "pose_in_tracking_origin_xrt"), &new_in_origin[c]);
		for (int i = 0; i < 8; i++) {
			good = good && u_json_get_double(u_json_get(i < 4 ? in : d, names[i]), &v[i]);
		}
		if (!good) {
			cJSON_Delete(root);
			throw std::runtime_error("bad camera entry in calibration");
		}
		t_camera_calibration &out = mosaic.camera_calibrations[c];
		out.intrinsics[0][0] = v[0];
		out.intrinsics[1][1] = v[1];
		out.intrinsics[0][2] = v[2];
		out.intrinsics[1][2] = v[3];
		out.kb4 = t_camera_calibration_kb4_params{v[4], v[5], v[6], v[7]};
		out.distortion_model = T_DISTORTION_FISHEYE_KB4;
	}
	cJSON_Delete(root);

	// Recorded rig relative to camera 0, from the first exposure with every camera's pose.
	std::vector<std::optional<xrt_pose>> world(camera_count);
	for (const Exposure &exposure : group_exposures(dataset.samples)) {
		std::vector<std::optional<xrt_pose>> w(camera_count);
		size_t have = 0;
		for (const CameraSample *sample : exposure.samples) {
			if (sample->camera_index < camera_count && sample->Txr_world_cam.has_value()) {
				w[sample->camera_index] = sample->Txr_world_cam;
				have++;
			}
		}
		if (have == camera_count) {
			world = w;
			break;
		}
	}
	if (!world[0].has_value()) {
		throw std::runtime_error("no exposure with every camera's pose");
	}
	/*
	 * World-frame recordings place camera 0 at head * head_from_camera0 (X). Swapping X: head = recorded camera 0 *
	 * X_recorded^-1, so the new camera 0 is recorded camera 0 * X_recorded^-1 * X_new. Head-relative recordings
	 * have no head pose, so X cannot change them; both default to identity.
	 */
	xrt_pose x_recorded = read_head_from_camera0(recorded_calibration);
	xrt_pose x_new = read_head_from_camera0(path);
	xrt_pose x_recorded_inverse, x_change;
	math_pose_invert(&x_recorded, &x_recorded_inverse);
	math_pose_transform(&x_recorded_inverse, &x_new, &x_change);

	xrt_pose inverse_cam0;
	math_pose_invert(&world[0].value(), &inverse_cam0);
	std::vector<xrt_pose> origin_from_recorded(camera_count);
	for (size_t c = 0; c < camera_count; c++) {
		xrt_pose recorded_in_origin, inverse_recorded, changed_in_origin;
		math_pose_transform(&inverse_cam0, &world[c].value(), &recorded_in_origin);
		math_pose_invert(&recorded_in_origin, &inverse_recorded);
		// World pose of camera c becomes: recorded world pose * inverse(recorded in origin) * X change * new in
		// origin.
		math_pose_transform(&x_change, &new_in_origin[c], &changed_in_origin);
		math_pose_transform(&inverse_recorded, &changed_in_origin, &origin_from_recorded[c]);
		const xrt_pose &d = origin_from_recorded[c];
		std::printf("  camera %zu: calibration change %.2f mm, %.3f deg\n", c,
		            1000.0 * std::sqrt(d.position.x * d.position.x + d.position.y * d.position.y +
		                               d.position.z * d.position.z),
		            2.0 * std::acos(std::min(1.0f, std::fabs(d.orientation.w))) * 180.0 / M_PI);
	}
	for (CameraSample &sample : dataset.samples) {
		if (sample.camera_index < camera_count && sample.Txr_world_cam.has_value()) {
			xrt_pose updated;
			math_pose_transform(&sample.Txr_world_cam.value(), &origin_from_recorded[sample.camera_index],
			                    &updated);
			sample.Txr_world_cam = updated;
		}
	}
	std::printf("calibration overridden from %s\n", path);
}

/*!
 * Move LEDs of the recorded models (--led-offsets): rows of "device,led,dx_mm,dy_mm,dz_mm" in the model's own frame,
 * as --residuals-csv reports its err_*_mm. For trying a corrected model on a recording.
 */
void
apply_led_offsets(DatasetReader &dataset, const char *path)
{
	std::ifstream in(path);
	if (!in) {
		throw std::runtime_error(std::string("cannot open ") + path);
	}
	std::string line;
	size_t applied = 0;
	while (std::getline(in, line)) {
		int device, led;
		double dx, dy, dz;
		if (std::sscanf(line.c_str(), "%d,%d,%lf,%lf,%lf", &device, &led, &dx, &dy, &dz) != 5) {
			continue; // header or comment
		}
		for (DatasetDevice &d : dataset.devices) {
			if ((int)d.id != device || led < 0 || (size_t)led >= d.led_model.led_count) {
				continue;
			}
			xrt_vec3 &p = d.led_model.leds[led].position;
			p.x += (float)(dx / 1000.0);
			p.y += (float)(dy / 1000.0);
			p.z += (float)(dz / 1000.0);
			applied++;
		}
	}
	std::printf("LED offsets from %s applied to %zu LEDs\n", path, applied);
}

/*!
 * One row per correspondence of a solve, for looking at where the reprojection error sits (--residuals-csv): which
 * LED, which camera, how far from the camera (range_m) and how obliquely (facing_deg, 0 is head on) the LED was seen.
 *
 * The pixel residual is blob minus projection. err_*_mm is the same residual as a displacement of the LED in the
 * device frame, perpendicular to the view ray (ray_*, LED towards camera, device frame): the smallest move of the LED
 * that would put its projection on the blob. Averaged over many views, per LED, it estimates the LED's position error
 * in the model.
 */
void
write_residual_rows(FILE *out,
                    int64_t timestamp_ns,
                    t_constellation_device_id_t id,
                    bool solved,
                    bool seeded,
                    const JointSolveResult &result,
                    const std::vector<JointSolveCamera> &cameras,
                    const std::vector<uint32_t> &camera_index_of,
                    const t_constellation_tracker_led_model &model)
{
	const xrt_pose &pose = result.Tcv_world_device;
	Eigen::Quaterniond q_wd =
	    Eigen::Quaterniond(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z)
	        .normalized();
	Eigen::Vector3d t_wd(pose.position.x, pose.position.y, pose.position.z);

	for (const JointSolveMatch &m : result.correspondences) {
		const JointSolveCamera &camera = cameras[m.camera];
		const t_constellation_tracker_led &led = model.leds[m.led];
		const t_blob &blob = camera.blobs[m.blob];

		const xrt_pose &cam = camera.Tcv_world_cam;
		Eigen::Quaterniond q_wc =
		    Eigen::Quaterniond(cam.orientation.w, cam.orientation.x, cam.orientation.y, cam.orientation.z)
		        .normalized();
		Eigen::Vector3d t_wc(cam.position.x, cam.position.y, cam.position.z);

		Eigen::Vector3d p_device(led.position.x, led.position.y, led.position.z);
		Eigen::Vector3d p_cam = q_wc.conjugate() * (q_wd * p_device + t_wd - t_wc);
		auto project = [&](const Eigen::Vector3d &p, Eigen::Vector2d &px) {
			float u, v;
			if (!t_camera_models_project(camera.model, (float)p.x(), (float)p.y(), (float)p.z(), &u, &v)) {
				return false;
			}
			px = Eigen::Vector2d(u, v);
			return true;
		};
		Eigen::Vector2d px;
		if (!project(p_cam, px)) {
			continue;
		}
		Eigen::Vector2d d(blob.center.x - px.x(), blob.center.y - px.y());

		// LED towards camera, and the LED's normal against it.
		Eigen::Vector3d ray_cam = -p_cam.normalized();
		Eigen::Vector3d n_cam =
		    q_wc.conjugate() * (q_wd * Eigen::Vector3d(led.normal.x, led.normal.y, led.normal.z));
		double facing_deg =
		    std::acos(std::max(-1.0, std::min(1.0, ray_cam.dot(n_cam.normalized())))) * 180.0 / M_PI;

		// Two directions across the view ray, and how the projection moves along each (finite difference).
		Eigen::Vector3d e1 = ray_cam.unitOrthogonal();
		Eigen::Vector3d e2 = ray_cam.cross(e1);
		const double eps = 1e-4;
		Eigen::Vector2d a1, a2;
		Eigen::Vector3d err_device(NAN, NAN, NAN);
		if (project(p_cam + eps * e1, a1) && project(p_cam + eps * e2, a2)) {
			Eigen::Matrix2d A;
			A.col(0) = (a1 - px) / eps;
			A.col(1) = (a2 - px) / eps;
			if (std::fabs(A.determinant()) > 1e-9) {
				Eigen::Vector2d c = A.inverse() * d;
				err_device = q_wd.conjugate() * (q_wc * (c.x() * e1 + c.y() * e2));
			}
		}
		Eigen::Vector3d ray_device = q_wd.conjugate() * (q_wc * ray_cam);

		std::fprintf(out,
		             "%" PRIi64
		             ",%d,%d,%d,%.4f,%.3f,%u,%u,%u,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,"
		             "%.2f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f\n",
		             timestamp_ns, (int)id, solved ? 1 : 0, seeded ? 1 : 0, result.rms_px, result.coverage,
		             camera_index_of[m.camera], m.led, blob.blob_id, blob.center.x, blob.center.y, blob.size.x,
		             blob.size.y, blob.brightness, px.x(), px.y(), d.x(), d.y(), p_cam.norm(), facing_deg,
		             ray_device.x(), ray_device.y(), ray_device.z(), 1000.0 * err_device.x(),
		             1000.0 * err_device.y(), 1000.0 * err_device.z());
	}
}

/*!
 * @param records     Optional: every accepted solve with its correspondences, so other backends can be run on
 *                    exactly this optical input (--fusion-compare). Collecting them does not change the replay.
 * @param exposure_us Optional: solve and bootstrap time of each exposure, all devices (--compare-frontend).
 * @param residuals_path Optional: one row per correspondence of every solve, accepted or not (--residuals-csv).
 */
int
replay_m1(const DatasetReader &dataset,
          const char *csv_path,
          bool seed_recorded,
          const char *blobs_path,
          std::vector<FrontendRecord> *records = nullptr,
          std::vector<double> *exposure_us = nullptr,
          const char *residuals_path = nullptr)
{
	if (dataset.mosaics.empty()) {
		std::fprintf(stderr, "no cameras in dataset\n");
		return 1;
	}
	const DatasetMosaic &mosaic = dataset.mosaics[0];
	std::vector<t_camera_model_params> models(mosaic.camera_calibrations.size());
	for (size_t c = 0; c < models.size(); c++) {
		t_camera_model_params_from_t_camera_calibration(&mosaic.camera_calibrations[c], &models[c]);
	}

	std::vector<DeviceTrack> tracks;
	for (const DatasetDevice &device : dataset.devices) {
		DeviceTrack track;
		track.device = &device;
		tracks.push_back(track);
	}

	FILE *csv = csv_path ? std::fopen(csv_path, "w") : nullptr;
	if (csv) {
		std::fprintf(csv,
		             "timestamp_ns,device,solved,seeded,cameras,matches,rms_px,coverage,outliers,solve_us,"
		             "px,py,pz,qx,qy,qz,qw,rms_cam0,rms_cam1,rms_cam2,rms_cam3,n_cam0,n_cam1,n_cam2,n_cam3\n");
	}

	// Every blob with its owner after the exposure's solves (-1 for none), for studying background light.
	FILE *blobs_csv = blobs_path ? std::fopen(blobs_path, "w") : nullptr;
	if (blobs_csv) {
		std::fprintf(blobs_csv, "timestamp_ns,camera,blob_id,cx,cy,w,h,brightness,owner\n");
	}

	FILE *residuals_csv = residuals_path ? std::fopen(residuals_path, "w") : nullptr;
	if (residuals_csv) {
		// The models the residuals refer to, in the frame of err_*_mm.
		for (const DatasetDevice &device : dataset.devices) {
			for (size_t l = 0; l < device.led_model.led_count; l++) {
				const xrt_vec3 &p = device.led_model.leds[l].position;
				std::printf("LED model: device %d led %zu at mm (%.3f, %.3f, %.3f)\n", (int)device.id,
				            l, 1000.0 * p.x, 1000.0 * p.y, 1000.0 * p.z);
			}
		}
		std::fprintf(
		    residuals_csv,
		    "timestamp_ns,device,solved,seeded,rms_px,coverage,camera,led,blob_id,blob_x,blob_y,blob_w,"
		    "blob_h,brightness,proj_x,proj_y,dx,dy,range_m,facing_deg,ray_x,ray_y,ray_z,err_x_mm,err_y_mm,"
		    "err_z_mm\n");
	}

	JointSolveParams params;
	params.orientation_prior_sigma_deg = 3.0f;

	std::vector<Exposure> exposures = group_exposures(dataset.samples);
	for (const Exposure &exposure : exposures) {
		// Cameras of this exposure, blob ownership shared between devices.
		std::vector<JointSolveCamera> cameras;
		std::vector<uint32_t> camera_index_of;
		std::vector<std::vector<t_constellation_device_id_t>> owners;
		owners.reserve(exposure.samples.size());
		for (const CameraSample *sample : exposure.samples) {
			if (!sample->Txr_world_cam.has_value() || sample->camera_index >= models.size()) {
				continue;
			}
			xrt_pose Tcv_world_cam;
			math_pose_convert_from_opencv(&sample->Txr_world_cam.value(), &Tcv_world_cam);
			const t_camera_calibration &cal = mosaic.camera_calibrations[sample->camera_index];
			owners.emplace_back(sample->blob_count, XRT_CONSTELLATION_INVALID_DEVICE_ID);
			camera_index_of.push_back(sample->camera_index);
			cameras.push_back(JointSolveCamera{Tcv_world_cam, &models[sample->camera_index],
			                                   (int)cal.image_size_pixels.w, (int)cal.image_size_pixels.h,
			                                   sample->blobs, sample->blob_count, nullptr});
		}
		for (size_t i = 0; i < cameras.size(); i++) {
			cameras[i].blob_owner = owners[i].data();
		}
		bool any_blobs = false;
		for (const JointSolveCamera &cam : cameras) {
			any_blobs |= cam.blob_count > 0;
		}

		// Tracked devices first, so a lost device cannot claim a tracked ring's blobs.
		std::vector<DeviceTrack *> order;
		for (DeviceTrack &t : tracks) {
			order.push_back(&t);
		}
		std::stable_sort(order.begin(), order.end(), [](const DeviceTrack *a, const DeviceTrack *b) {
			return a->tracking && !b->tracking;
		});

		double exposure_cost_us = 0.0;
		for (DeviceTrack *track : order) {
			t_constellation_device_id_t id = track->device->id;
			if (any_blobs) {
				track->exposures_with_blobs++;
			}

			xrt_quat imu;
			bool have_imu = imu_orientation_at(dataset, id, exposure.timestamp_ns, imu);

			xrt_pose prior;
			bool seeded = false;
			bool bootstrapped = false;
			JointSolveResult result;
			bool ok = false;
			double us = 0.0;
			if (track->tracking) {
				prior = track->Tcv_world_device;
				if (have_imu && track->have_align) {
					math_quat_rotate(&track->align, &imu, &prior.orientation);
				}
				auto start = std::chrono::steady_clock::now();
				ok = have_imu && track->have_align
				         ? joint_solve_refine(cameras, track->device->led_model, prior,
				                              prior.orientation, params, result)
				         : joint_solve_refine(cameras, track->device->led_model, prior,
				                              JointSolveParams{}, result);
				us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
				         .count();
				track->solve_us.add(us);
			} else if (seed_recorded) {
				if (!recorded_pose(exposure, id, prior)) {
					continue;
				}
				seeded = true;
				auto start = std::chrono::steady_clock::now();
				ok = joint_solve_refine(cameras, track->device->led_model, prior, JointSolveParams{},
				                        result);
				us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
				         .count();
				track->solve_us.add(us);
			} else {
				StereoBootstrapResult bootstrap;
				auto start = std::chrono::steady_clock::now();
				ok = stereo_bootstrap(cameras, track->device->led_model, StereoBootstrapParams{},
				                      bootstrap);
				us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
				         .count();
				track->bootstrap_attempts++;
				track->bootstrap_us.add(us);
				result = bootstrap.refined;
				bootstrapped = ok;
				seeded = true;
			}
			exposure_cost_us += us;

			if (csv) {
				const xrt_pose &p = result.Tcv_world_device;
				// Per recorded camera index: RMS and count of this solve's correspondences.
				double sum2[4] = {0, 0, 0, 0};
				int count[4] = {0, 0, 0, 0};
				for (const JointSolveMatch &m : result.correspondences) {
					uint32_t cam = camera_index_of[m.camera];
					if (cam < 4) {
						sum2[cam] += (double)m.residual_px * m.residual_px;
						count[cam]++;
					}
				}
				std::fprintf(csv,
				             "%" PRIi64
				             ",%d,%d,%d,%u,%u,%.4f,%.3f,%u,%.1f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
				             exposure.timestamp_ns, (int)id, ok ? 1 : 0, seeded ? 1 : 0,
				             result.cameras_used, result.matches, result.rms_px, result.coverage,
				             result.outliers, us, p.position.x, p.position.y, p.position.z,
				             p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
				for (int c = 0; c < 4; c++) {
					std::fprintf(csv, ",%.4f", count[c] ? std::sqrt(sum2[c] / count[c]) : NAN);
				}
				for (int c = 0; c < 4; c++) {
					std::fprintf(csv, ",%d", count[c]);
				}
				std::fprintf(csv, "\n");
			}

			if (residuals_csv) {
				write_residual_rows(residuals_csv, exposure.timestamp_ns, id, ok, seeded, result,
				                    cameras, camera_index_of, track->device->led_model);
			}

			if (!ok) {
				if (track->tracking && ++track->consecutive_failures > 3) {
					track->tracking = false;
				}
				continue;
			}

			track->solved++;
			track->seeds += seeded && !bootstrapped ? 1 : 0;
			track->bootstraps += bootstrapped ? 1 : 0;
			track->tracking = true;
			track->consecutive_failures = 0;
			track->Tcv_world_device = result.Tcv_world_device;
			track->last_solved_ns = exposure.timestamp_ns;
			track->cameras_used[result.cameras_used]++;
			track->rms_px.add(result.rms_px);
			track->coverage.add(result.coverage);
			track->matches.add(result.matches);
			track->positions.push_back({exposure.timestamp_ns, result.Tcv_world_device.position});
			if (have_imu) {
				const xrt_quat &o = result.Tcv_world_device.orientation;
				track->orientation_pairs.push_back(
				    {Eigen::Quaterniond(o.w, o.x, o.y, o.z).normalized(),
				     Eigen::Quaterniond(imu.w, imu.x, imu.y, imu.z).normalized()});
				xrt_quat inverse_imu;
				math_quat_invert(&imu, &inverse_imu);
				math_quat_rotate(&result.Tcv_world_device.orientation, &inverse_imu, &track->align);
				math_quat_normalize(&track->align);
				track->have_align = true;
				// Vertical in the OpenCV-convention world is -y.
				xrt_vec3 up{0.0f, -1.0f, 0.0f}, rotated;
				math_quat_rotate_vec3(&track->align, &up, &rotated);
				double c = std::max(-1.0, std::min(1.0, (double)(-rotated.y)));
				track->align_tilt_deg.add(std::acos(c) * 180.0 / M_PI);
			}
			for (const JointSolveMatch &m : result.correspondences) {
				owners[m.camera][m.blob] = id;
			}
			if (records != nullptr) {
				FrontendRecord record{exposure.timestamp_ns,
				                      id,
				                      result.Tcv_world_device,
				                      result.rms_px,
				                      result.cameras_used,
				                      result.matches,
				                      bootstrapped,
				                      us,
				                      {}};
				for (const JointSolveMatch &m : result.correspondences) {
					record.correspondences.push_back(
					    FrontendMatch{camera_index_of[m.camera], cameras[m.camera].Tcv_world_cam,
					                  cameras[m.camera].blobs[m.blob].center, m.led});
				}
				records->push_back(std::move(record));
			}

			xrt_pose recorded;
			if (!seeded && recorded_pose(exposure, id, recorded)) {
				track->recorded_delta_mm.add(
				    1000.0 * distance_m(recorded.position, result.Tcv_world_device.position));
				track->recorded_delta_deg.add(
				    quat_angle_deg(recorded.orientation, result.Tcv_world_device.orientation));
			}
		}
		if (exposure_us != nullptr) {
			exposure_us->push_back(exposure_cost_us);
		}
		if (blobs_csv) {
			for (size_t i = 0; i < cameras.size(); i++) {
				for (uint32_t b = 0; b < cameras[i].blob_count; b++) {
					const t_blob &blob = cameras[i].blobs[b];
					std::fprintf(
					    blobs_csv, "%" PRIi64 ",%u,%u,%.2f,%.2f,%.2f,%.2f,%.3f,%d\n",
					    exposure.timestamp_ns, camera_index_of[i], blob.blob_id, blob.center.x,
					    blob.center.y, blob.size.x, blob.size.y, blob.brightness,
					    owners[i][b] == XRT_CONSTELLATION_INVALID_DEVICE_ID ? -1
					                                                        : (int)owners[i][b]);
				}
			}
		}
	}
	if (csv) {
		std::fclose(csv);
	}
	if (blobs_csv) {
		std::fclose(blobs_csv);
	}
	if (residuals_csv) {
		std::fclose(residuals_csv);
	}

	for (DeviceTrack &track : tracks) {
		// Static jitter: position spread within 1 s windows whose motion stays under 20 mm.
		Stats jitter_mm;
		size_t begin = 0;
		for (size_t i = 0; i < track.positions.size(); i++) {
			if (track.positions[i].first - track.positions[begin].first < 1'000'000'000 &&
			    i + 1 < track.positions.size()) {
				continue;
			}
			size_t n = i - begin;
			if (n >= 20) {
				double mx = 0, my = 0, mz = 0;
				for (size_t k = begin; k < i; k++) {
					mx += track.positions[k].second.x;
					my += track.positions[k].second.y;
					mz += track.positions[k].second.z;
				}
				mx /= n, my /= n, mz /= n;
				double worst = 0, sum2 = 0;
				for (size_t k = begin; k < i; k++) {
					double d = std::sqrt(std::pow(track.positions[k].second.x - mx, 2) +
					                     std::pow(track.positions[k].second.y - my, 2) +
					                     std::pow(track.positions[k].second.z - mz, 2));
					worst = std::max(worst, d);
					sum2 += d * d;
				}
				if (worst < 0.02) {
					jitter_mm.add(1000.0 * std::sqrt(sum2 / n));
				}
			}
			begin = i;
		}

		std::printf(
		    "M1 device %d: solved %u of %u exposures with blobs (%.1f%%), %u from recorded seeds, %u "
		    "bootstraps from %u attempts\n",
		    (int)track.device->id, track.solved, track.exposures_with_blobs,
		    track.exposures_with_blobs ? 100.0 * track.solved / track.exposures_with_blobs : 0.0, track.seeds,
		    track.bootstraps, track.bootstrap_attempts);
		std::printf("  bootstrap us p50 %.0f p95 %.0f max %.0f\n", track.bootstrap_us.pct(0.5),
		            track.bootstrap_us.pct(0.95), track.bootstrap_us.pct(1.0));
		std::printf("  cameras used:");
		for (const auto &[cams, count] : track.cameras_used) {
			std::printf(" %u:%u", cams, count);
		}
		std::printf("\n  matches p50 %.0f; rms px p50 %.3f p95 %.3f; coverage p50 %.2f p05 %.2f\n",
		            track.matches.pct(0.5), track.rms_px.pct(0.5), track.rms_px.pct(0.95),
		            track.coverage.pct(0.5), track.coverage.pct(0.05));
		std::printf("  solve us p50 %.0f p95 %.0f max %.0f\n", track.solve_us.pct(0.5),
		            track.solve_us.pct(0.95), track.solve_us.pct(1.0));
		std::printf("  static jitter mm (1 s windows) p50 %.2f p95 %.2f over %zu windows\n", jitter_mm.pct(0.5),
		            jitter_mm.pct(0.95), jitter_mm.values.size());
		std::printf("  optical-from-IMU alignment tilt deg p50 %.2f p95 %.2f\n", track.align_tilt_deg.pct(0.5),
		            track.align_tilt_deg.pct(0.95));
		std::printf("  vs recorded per-camera poses: mm p50 %.1f p95 %.1f, deg p50 %.2f p95 %.2f (n=%zu)\n",
		            track.recorded_delta_mm.pct(0.5), track.recorded_delta_mm.pct(0.95),
		            track.recorded_delta_deg.pct(0.5), track.recorded_delta_deg.pct(0.95),
		            track.recorded_delta_mm.values.size());
	}
	for (DeviceTrack &track : tracks) {
		report_imu_offset(track);
	}
	return 0;
}

/*
 *
 * Tracker replay: recorded blobs through the real ConstellationTracker (joint path, deterministic).
 *
 */

struct FakeOrigin
{
	t_constellation_tracker_tracking_source base;
	//! Camera 0's recorded world pose by timestamp; the origin is placed at camera 0.
	std::vector<std::pair<int64_t, xrt_pose>> poses;
};

void
fake_origin_get(t_constellation_tracker_tracking_source *source, int64_t when_ns, xrt_space_relation *out)
{
	FakeOrigin *origin = (FakeOrigin *)source;
	*out = XRT_SPACE_RELATION_ZERO;
	if (origin->poses.empty()) {
		return;
	}
	auto it = std::lower_bound(origin->poses.begin(), origin->poses.end(), when_ns,
	                           [](const std::pair<int64_t, xrt_pose> &p, int64_t t) { return p.first < t; });
	if (it == origin->poses.end() ||
	    (it != origin->poses.begin() && when_ns - (it - 1)->first < it->first - when_ns)) {
		it = it == origin->poses.begin() ? it : it - 1;
	}
	out->pose = it->second;
	out->relation_flags = (xrt_space_relation_flags)(XRT_SPACE_RELATION_POSITION_VALID_BIT |
	                                                 XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                                                 XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
	                                                 XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
}

struct FakeDevice;

struct FakeTrackingSource : t_constellation_tracker_tracking_source
{
	FakeDevice *device{nullptr};
};

//! Stands in for the device driver: accepts every pushed sample and predicts the last one while it is recent.
struct FakeDevice
{
	t_constellation_tracker_device base;
	FakeTrackingSource source;
	std::vector<t_constellation_tracker_led> leds; // XR convention, as a driver provides them
	t_constellation_device_id_t id{XRT_CONSTELLATION_INVALID_DEVICE_ID};

	bool have_last{false};
	t_constellation_tracker_sample last{};
	//! The recorded tracking-source relations for this device, returned when there is no recent push, as a driver
	//! returns its (unaligned) IMU orientation before it has optical history.
	std::vector<std::pair<int64_t, xrt_space_relation>> recorded;
	uint32_t pushes{0};
	std::map<uint32_t, uint32_t> joint_cameras;
	Stats rms_px;
	std::vector<std::pair<int64_t, xrt_vec3>> positions;
	FILE *csv{nullptr};
	//! --tracker-filter: the driver's IMU + optical EKF provides the prior instead.
	t_imu_optical_filter *filter{nullptr};
	std::vector<const xrt_imu_sample *> imu;
	size_t next_imu{0};
};

FakeDevice *
fake_device_of_source(t_constellation_tracker_tracking_source *source)
{
	return static_cast<FakeTrackingSource *>(source)->device;
}

bool
fake_device_push(t_constellation_tracker_device *device, t_constellation_tracker_sample *sample)
{
	FakeDevice *fake = (FakeDevice *)device;
	fake->pushes++;
	fake->joint_cameras[sample->joint_camera_count]++;
	fake->rms_px.add(sample->metrics.reprojection_error);
	fake->positions.push_back({sample->timestamp_ns, sample->pose.position});
	if (fake->csv) {
		const xrt_pose &p = sample->pose;
		std::fprintf(fake->csv, "%" PRIi64 ",%d,%u,%u,%.4f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
		             sample->timestamp_ns, (int)fake->id, sample->joint_camera_count,
		             sample->metrics.matched_blob_count, sample->metrics.reprojection_error, p.position.x,
		             p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z,
		             p.orientation.w);
	}
	fake->last = *sample;
	fake->have_last = true;
	if (fake->filter != nullptr) {
		float scale = std::max(1.0f, (float)sample->metrics.reprojection_error / 0.5f);
		t_imu_optical_filter_push_pose(fake->filter, sample->timestamp_ns, &sample->pose, 0.002f * scale,
		                               0.008f * scale);
	}
	return true;
}

void
fake_device_get(t_constellation_tracker_tracking_source *source, int64_t when_ns, xrt_space_relation *out)
{
	/*
	 * Like the Sense driver: orientation is the IMU's (the recorded relation's orientation, in the IMU's own
	 * world), position is the last optical pose while it is fresh. Before any push there is only the orientation.
	 */
	FakeDevice *fake = fake_device_of_source(source);
	*out = XRT_SPACE_RELATION_ZERO;
	if (fake->filter != nullptr && t_imu_optical_filter_get_relation(fake->filter, when_ns, out)) {
		return;
	}
	auto it = std::lower_bound(fake->recorded.begin(), fake->recorded.end(), when_ns,
	                           [](const auto &p, int64_t t) { return p.first < t; });
	if (it != fake->recorded.end() && std::llabs(it->first - when_ns) < 5'000'000 &&
	    (it->second.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) != 0) {
		out->pose.orientation = it->second.pose.orientation;
		out->relation_flags = (xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
		                                                 XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
	}
	if (fake->have_last && std::llabs(when_ns - fake->last.timestamp_ns) < 100'000'000) {
		out->pose.position = fake->last.pose.position;
		out->relation_flags =
		    (xrt_space_relation_flags)(out->relation_flags | XRT_SPACE_RELATION_POSITION_VALID_BIT);
	}
}

int
replay_tracker(const DatasetReader &dataset, const char *csv_path, bool use_filter, double imu_angle_deg)
{
	FILE *csv = csv_path ? std::fopen(csv_path, "w") : nullptr;
	if (csv) {
		std::fprintf(csv, "timestamp_ns,device,cameras,matches,rms_px,px,py,pz,qx,qy,qz,qw\n");
	}
	if (dataset.mosaics.empty() || dataset.samples.empty()) {
		std::fprintf(stderr, "nothing to replay\n");
		return 1;
	}
	const DatasetMosaic &mosaic = dataset.mosaics[0];
	const size_t camera_count = mosaic.camera_calibrations.size();

	// Rig geometry from the first exposure that has every camera's pose.
	std::vector<Exposure> exposures = group_exposures(dataset.samples);
	std::vector<std::optional<xrt_pose>> first_world(camera_count);
	for (const Exposure &exposure : exposures) {
		size_t have = 0;
		std::vector<std::optional<xrt_pose>> world(camera_count);
		for (const CameraSample *sample : exposure.samples) {
			if (sample->camera_index < camera_count && sample->Txr_world_cam.has_value()) {
				world[sample->camera_index] = sample->Txr_world_cam;
				have++;
			}
		}
		if (have == camera_count && world[0].has_value()) {
			first_world = world;
			break;
		}
	}
	if (!first_world[0].has_value()) {
		std::fprintf(stderr, "no exposure with every camera's pose\n");
		return 1;
	}

	FakeOrigin origin{};
	origin.base.get_tracked_pose = fake_origin_get;
	for (const CameraSample &sample : dataset.samples) {
		if (sample.camera_index == 0 && sample.Txr_world_cam.has_value()) {
			origin.poses.push_back({sample.timestamp_ns, sample.Txr_world_cam.value()});
		}
	}
	std::sort(origin.poses.begin(), origin.poses.end(),
	          [](const auto &a, const auto &b) { return a.first < b.first; });

	t_constellation_tracker_params params{};
	params.flags = (t_constellation_tracker_flags)(T_CONSTELLATION_TRACKER_FLAGS_DETERMINISTIC |
	                                               T_CONSTELLATION_TRACKER_FLAGS_ALLOW_JOINT);
	params.num_mosaics = 1;
	params.mosaics[0].tracking_origin = &origin.base;
	params.mosaics[0].num_cameras = camera_count;
	xrt_pose inverse_cam0;
	math_pose_invert(&first_world[0].value(), &inverse_cam0);
	for (size_t c = 0; c < camera_count; c++) {
		params.mosaics[0].cameras[c].calibration = mosaic.camera_calibrations[c];
		math_pose_transform(&inverse_cam0, &first_world[c].value(),
		                    &params.mosaics[0].cameras[c].pose_in_origin);
		params.mosaics[0].cameras[c].has_concrete_pose = true;
	}

	setenv("CONSTELLATION_TRACKER_JOINT", "1", 1);
	xrt_frame_context xfctx{};
	t_constellation_tracker *tracker = nullptr;
	if (t_constellation_tracker_create(&xfctx, &params, &tracker) != 0) {
		std::fprintf(stderr, "failed to create tracker\n");
		return 1;
	}

	std::vector<std::unique_ptr<FakeDevice>> fakes;
	for (const DatasetDevice &device : dataset.devices) {
		auto fake = std::make_unique<FakeDevice>();
		fake->base.push_optical_sample = fake_device_push;
		fake->base.push_camera_blob_count = nullptr;
		fake->base.push_camera_led_blob_count = nullptr;
		fake->source.get_tracked_pose = fake_device_get;
		fake->source.device = fake.get();
		fake->csv = csv;
		for (const DatasetDeviceTracking &t : dataset.device_tracking) {
			if (t.device_id == device.id) {
				xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
				relation.pose = t.pose;
				relation.relation_flags = t.relation_flags;
				fake->recorded.push_back({t.timestamp_ns, relation});
			}
		}
		std::sort(fake->recorded.begin(), fake->recorded.end(),
		          [](const auto &a, const auto &b) { return a.first < b.first; });
		// The recorded model is in the tracker's OpenCV convention; drivers hand over OpenXR.
		fake->leds = device.leds;
		for (t_constellation_tracker_led &led : fake->leds) {
			led.position.y = -led.position.y;
			led.position.z = -led.position.z;
			led.normal.y = -led.normal.y;
			led.normal.z = -led.normal.z;
		}
		t_constellation_tracker_device_params dparams{};
		dparams.led_model = device.led_model;
		dparams.led_model.leds = fake->leds.data();
		dparams.led_model.led_count = fake->leds.size();
		dparams.led_model.compute_led_visibility = nullptr;
		dparams.tracking_source = &fake->source;
		if (use_filter) {
			t_imu_optical_filter_params fparams;
			t_imu_optical_filter_default_params(&fparams);
			fake->filter = t_imu_optical_filter_create(&fparams);
			for (const DatasetImuSample &s : dataset.imu_samples) {
				if (s.device_id == device.id) {
					fake->imu.push_back(&s.sample);
				}
			}
		}
		t_constellation_tracker_add_device(tracker, &dparams, &fake->base, &fake->id);
		fakes.push_back(std::move(fake));
	}

	// Feed every camera's frames in time order through the normal blob sinks.
	std::vector<const CameraSample *> order;
	for (const CameraSample &sample : dataset.samples) {
		order.push_back(&sample);
	}
	std::stable_sort(order.begin(), order.end(), [](const CameraSample *a, const CameraSample *b) {
		return a->timestamp_ns != b->timestamp_ns ? a->timestamp_ns < b->timestamp_ns
		                                          : a->camera_index < b->camera_index;
	});
	auto start = std::chrono::steady_clock::now();
	std::vector<t_blob> blobs;
	const double half = imu_angle_deg * M_PI / 180.0 * 0.5;
	const xrt_quat imu_to_led{(float)-std::sin(half), 0.0f, 0.0f, (float)std::cos(half)};
	for (const CameraSample *sample : order) {
		if (sample->camera_index >= camera_count) {
			continue;
		}
		// Live, the joint worker solves an exposure ~30 ms after it, with the IMU up to then already fused.
		for (auto &fake : fakes) {
			while (fake->filter != nullptr && fake->next_imu < fake->imu.size() &&
			       fake->imu[fake->next_imu]->timestamp_ns <= sample->timestamp_ns + 30'000'000) {
				const xrt_imu_sample *s = fake->imu[fake->next_imu++];
				xrt_vec3 a{(float)s->accel_m_s2.x, (float)s->accel_m_s2.y, (float)s->accel_m_s2.z};
				xrt_vec3 g{(float)s->gyro_rad_secs.x, (float)s->gyro_rad_secs.y,
				           (float)s->gyro_rad_secs.z};
				xrt_vec3 a_led, g_led;
				math_quat_rotate_vec3(&imu_to_led, &a, &a_led);
				math_quat_rotate_vec3(&imu_to_led, &g, &g_led);
				t_imu_optical_filter_push_imu(fake->filter, s->timestamp_ns, &a_led, &g_led);
			}
		}
		blobs.assign(sample->blobs, sample->blobs + sample->blob_count);
		for (t_blob &b : blobs) {
			b.matched_device_id = XRT_CONSTELLATION_INVALID_DEVICE_ID;
			b.matched_device_led_id = XRT_CONSTELLATION_INVALID_LED_ID;
		}
		t_blob_observation observation{};
		observation.source = nullptr;
		observation.id = sample->id;
		observation.timestamp_ns = sample->timestamp_ns;
		observation.blobs = blobs.data();
		observation.num_blobs = (uint32_t)blobs.size();
		t_blob_sink *sink = params.mosaics[0].cameras[sample->camera_index].blob_sink;
		sink->push_blobs(sink, &observation);
	}
	double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

	xrt_frame_context_destroy_nodes(&xfctx);
	for (auto &fake : fakes) {
		t_imu_optical_filter_destroy(&fake->filter);
	}
	if (csv) {
		std::fclose(csv);
	}

	std::printf("tracker replay (joint path): %zu frames in %.2f s (%.0f us per exposure)\n", order.size(), seconds,
	            1e6 * seconds / (double)std::max<size_t>(1, exposures.size()));
	for (auto &fake : fakes) {
		std::printf("  device %d: %u pushed of %zu exposures; cameras:", (int)fake->id, fake->pushes,
		            exposures.size());
		for (const auto &[cams, count] : fake->joint_cameras) {
			std::printf(" %u:%u", cams, count);
		}
		std::printf("; rms px p50 %.3f p95 %.3f\n", fake->rms_px.pct(0.5), fake->rms_px.pct(0.95));
	}
	return 0;
}

/*
 * Offline evaluation of the IMU + optical EKF (--filter-eval TRACKER.csv): the dataset's IMU samples (rotated into the
 * LED model frame by the Sense mounting angle) and the tracker's poses (constellation_replay --tracker-csv, world
 * frame) are replayed through t_imu_optical_filter as the driver would see them, each optical pose arriving
 * kOpticalDelayNs after its exposure. Every kGapPeriodNs the optical poses are hidden for kGapLengthNs; the filter's
 * pose at each hidden exposure is compared with the hidden optical pose, against holding the last optical pose.
 */
constexpr int64_t kOpticalDelayNs = 35'000'000;
constexpr int64_t kGapPeriodNs = 2'000'000'000;
constexpr int64_t kGapLengthNs = 300'000'000;

struct TrackerPose
{
	int64_t t;
	xrt_pose pose;
	float rms_px;
};

double
quat_angle_rad(const xrt_quat &a, const xrt_quat &b)
{
	double d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
	return 2.0 * std::acos(std::min(1.0, d));
}

int
replay_filter(const DatasetReader &dataset, const char *tracker_csv, double imu_angle_deg, const char *out_csv)
{
	std::map<int, std::vector<TrackerPose>> poses;
	std::ifstream in(tracker_csv);
	std::string line;
	std::getline(in, line); // header: timestamp_ns,device,cameras,matches,rms_px,px,py,pz,qx,qy,qz,qw
	while (std::getline(in, line)) {
		std::stringstream ss(line);
		std::string field;
		std::vector<std::string> f;
		while (std::getline(ss, field, ',')) {
			f.push_back(field);
		}
		if (f.size() < 12) {
			continue;
		}
		TrackerPose tp{};
		tp.t = std::stoll(f[0]);
		tp.rms_px = std::stof(f[4]);
		tp.pose.position = {std::stof(f[5]), std::stof(f[6]), std::stof(f[7])};
		tp.pose.orientation = {std::stof(f[8]), std::stof(f[9]), std::stof(f[10]), std::stof(f[11])};
		poses[std::stoi(f[1])].push_back(tp);
	}

	const double half = imu_angle_deg * M_PI / 180.0 * 0.5;
	xrt_quat imu_to_led{(float)-std::sin(half), 0.0f, 0.0f,
	                    (float)std::cos(half)}; // inverse of the mounting rotation

	FILE *out = out_csv ? std::fopen(out_csv, "w") : nullptr;
	if (out) {
		std::fprintf(out,
		             "timestamp_ns,device,hidden,raw_px,raw_py,raw_pz,raw_qx,raw_qy,raw_qz,raw_qw,"
		             "filt_px,filt_py,filt_pz,filt_qx,filt_qy,filt_qz,filt_qw\n");
	}

	for (auto &[device, list] : poses) {
		std::sort(list.begin(), list.end(),
		          [](const TrackerPose &a, const TrackerPose &b) { return a.t < b.t; });
		std::vector<const xrt_imu_sample *> imu;
		for (const DatasetImuSample &s : dataset.imu_samples) {
			if ((int)s.device_id == device) {
				imu.push_back(&s.sample);
			}
		}
		if (imu.empty() || list.empty()) {
			std::printf("filter device %d: no IMU samples or poses\n", device);
			continue;
		}
		t_imu_optical_filter_params params;
		t_imu_optical_filter_default_params(&params);
		// Tuning overrides for offline sweeps (tool only).
		auto env = [](const char *name, float &value) {
			if (const char *v = std::getenv(name)) {
				value = std::strtof(v, nullptr);
			}
		};
		env("FILTER_GYRO_NOISE", params.gyro_noise_rad_s);
		env("FILTER_ACCEL_NOISE", params.accel_noise_m_s2);
		env("FILTER_GYRO_BIAS_WALK", params.gyro_bias_walk_rad_s2);
		env("FILTER_ACCEL_BIAS_WALK", params.accel_bias_walk_m_s3);
		env("FILTER_GATE", params.gate_chi2);
		float pose_sigma_scale = 1.0f;
		env("FILTER_POSE_SIGMA_SCALE", pose_sigma_scale);
		t_imu_optical_filter *filter = t_imu_optical_filter_create(&params);

		Stats hidden_filter_mm, hidden_filter_deg, hidden_hold_mm, hidden_hold_deg, visible_diff_mm;
		Stats gravity_x, gravity_y, gravity_z;
		size_t next_pose = 0; // next optical pose to deliver
		size_t next_eval = 0; // next exposure to evaluate (at its exposure time, as live)
		const int64_t t0 = list.front().t;
		xrt_pose last_delivered = list.front().pose;
		int64_t last_delivered_t = 0;
		bool have_delivered = false;
		auto hidden = [&](int64_t t) { return ((t - t0) % kGapPeriodNs) >= kGapPeriodNs - kGapLengthNs; };

		for (const xrt_imu_sample *s : imu) {
			xrt_vec3 a{(float)s->accel_m_s2.x, (float)s->accel_m_s2.y, (float)s->accel_m_s2.z};
			xrt_vec3 g{(float)s->gyro_rad_secs.x, (float)s->gyro_rad_secs.y, (float)s->gyro_rad_secs.z};
			xrt_vec3 a_led, g_led;
			math_quat_rotate_vec3(&imu_to_led, &a, &a_led);
			math_quat_rotate_vec3(&imu_to_led, &g, &g_led);
			t_imu_optical_filter_push_imu(filter, s->timestamp_ns, &a_led, &g_led);

			// Gravity check while nearly still: the accelerometer rotated into the world by the optical
			// orientation.
			double gyro_len = std::sqrt(g.x * g.x + g.y * g.y + g.z * g.z);
			// Only with a fresh optical orientation (exposed within the delivery delay plus 20 ms; a stale
			// one from before the controller was put down out of view says nothing), slow rotation and ~1 g
			// (little linear acceleration).
			double accel_len = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
			if (have_delivered && gyro_len < 0.5 && std::fabs(accel_len - 9.80665) < 0.4 &&
			    s->timestamp_ns - last_delivered_t < kOpticalDelayNs + 20'000'000) {
				xrt_vec3 a_world;
				math_quat_rotate_vec3(&last_delivered.orientation, &a_led, &a_world);
				gravity_x.add(a_world.x);
				gravity_y.add(a_world.y);
				gravity_z.add(a_world.z);
			}

			// Deliver optical poses whose delay has passed, unless they fall in a hidden gap.
			while (next_pose < list.size() && list[next_pose].t + kOpticalDelayNs <= s->timestamp_ns) {
				const TrackerPose &tp = list[next_pose++];
				if (hidden(tp.t)) {
					continue;
				}
				float scale = std::max(1.0f, tp.rms_px / 0.5f) * pose_sigma_scale;
				t_imu_optical_filter_push_pose(filter, tp.t, &tp.pose, 0.002f * scale, 0.008f * scale);
				last_delivered = tp.pose;
				last_delivered_t = tp.t;
				have_delivered = true;
			}
			// Evaluate exposures as the live driver would when asked for them: at the exposure time itself,
			// once the IMU has reached it (the joint tracker's prior), using everything delivered by then.
			while (next_eval < list.size() && list[next_eval].t <= s->timestamp_ns) {
				const TrackerPose &tp = list[next_eval++];
				xrt_space_relation rel;
				if (!have_delivered || !t_imu_optical_filter_get_relation(filter, tp.t, &rel)) {
					continue;
				}
				double mm = 1000.0 * distance_m(rel.pose.position, tp.pose.position);
				double deg = quat_angle_rad(rel.pose.orientation, tp.pose.orientation) * 180.0 / M_PI;
				bool is_hidden = hidden(tp.t);
				if (is_hidden) {
					hidden_filter_mm.add(mm);
					hidden_filter_deg.add(deg);
					hidden_hold_mm.add(1000.0 *
					                   distance_m(last_delivered.position, tp.pose.position));
					hidden_hold_deg.add(
					    quat_angle_rad(last_delivered.orientation, tp.pose.orientation) * 180.0 /
					    M_PI);
				} else {
					visible_diff_mm.add(mm);
				}
				if (out) {
					std::fprintf(
					    out,
					    "%" PRIi64
					    ",%d,%d,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
					    "%.6f,%.6f\n",
					    tp.t, device, is_hidden ? 1 : 0, tp.pose.position.x, tp.pose.position.y,
					    tp.pose.position.z, tp.pose.orientation.x, tp.pose.orientation.y,
					    tp.pose.orientation.z, tp.pose.orientation.w, rel.pose.position.x,
					    rel.pose.position.y, rel.pose.position.z, rel.pose.orientation.x,
					    rel.pose.orientation.y, rel.pose.orientation.z, rel.pose.orientation.w);
				}
			}
		}

		t_imu_optical_filter_stats st;
		t_imu_optical_filter_get_stats(filter, &st);
		std::printf("filter device %d: %zu IMU samples, %zu poses; updates %" PRIu64 " rejections %" PRIu64
		            " reinitialisations %" PRIu64 "\n",
		            device, imu.size(), list.size(), st.updates, st.rejections, st.reinitialisations);
		std::printf(
		    "  hidden %zu exposures (300 ms every 2 s): filter mm p50 %.1f p95 %.1f, deg p50 %.2f p95 %.2f;"
		    " hold-last mm p50 %.1f p95 %.1f, deg p50 %.2f p95 %.2f\n",
		    hidden_filter_mm.values.size(), hidden_filter_mm.pct(0.5), hidden_filter_mm.pct(0.95),
		    hidden_filter_deg.pct(0.5), hidden_filter_deg.pct(0.95), hidden_hold_mm.pct(0.5),
		    hidden_hold_mm.pct(0.95), hidden_hold_deg.pct(0.5), hidden_hold_deg.pct(0.95));
		std::printf("  visible exposures: filter vs optical mm p50 %.1f p95 %.1f\n", visible_diff_mm.pct(0.5),
		            visible_diff_mm.pct(0.95));
		std::printf("  ~1 g accelerometer in the world (expect ~0, +9.8, 0): %.2f %.2f %.2f (%zu samples)\n",
		            gravity_x.pct(0.5), gravity_y.pct(0.5), gravity_z.pct(0.5), gravity_x.values.size());
		std::printf("  learnt gyro bias deg/s %.2f %.2f %.2f, accel bias m/s^2 %.3f %.3f %.3f\n",
		            st.gyro_bias_rad_s.x * 180 / M_PI, st.gyro_bias_rad_s.y * 180 / M_PI,
		            st.gyro_bias_rad_s.z * 180 / M_PI, st.accel_bias_m_s2.x, st.accel_bias_m_s2.y,
		            st.accel_bias_m_s2.z);
		t_imu_optical_filter_destroy(&filter);
	}
	if (out) {
		std::fclose(out);
	}
	return 0;
}

} // namespace

int
main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(
		    stderr,
		    "usage: %s DATASET.ctd [--m1] [--seed-recorded] [--csv OUT.csv] [--tracker] [--tracker-csv "
		    "OUT.csv] [--upstream-distortion-codes] [--calibration CAL.json [--recorded-calibration "
		    "SESSION/calibration.json]] [--blobs-csv "
		    "OUT.csv] [--residuals-csv OUT.csv] [--led-offsets OFFSETS.csv] [--geometry PREFIX] [--filter-eval "
		    "TRACKER.csv [--filter-out OUT.csv]] [--fusion-compare "
		    "[--run-log SESSION/run.log] [--fusion-out PREFIX] [--fusion-scenarios nominal,dropout,corrupt] "
		    "[--fusion-frontend NAME]] [--compare-frontend NAME=RECORDS.csv ...] [--compare-out OUT.csv]\n",
		    argv[0]);
		return 2;
	}
	bool m1 = false;
	bool tracker = false;
	bool tracker_filter = false;
	const char *tracker_csv = nullptr;
	const char *tracking_csv = nullptr;
	const char *geometry_prefix = nullptr;
	const char *imu_csv = nullptr;
	const char *filter_eval = nullptr;
	const char *filter_out = nullptr;
	double imu_angle_deg = 50.27; // pssense_imu_angle: the Sense IMU's mounting rotation about x
	bool seed_recorded = false;
	bool upstream_distortion_codes = false;
	const char *csv = nullptr;
	const char *calibration = nullptr;
	const char *recorded_calibration = nullptr;
	const char *blobs_csv = nullptr;
	const char *residuals_csv = nullptr;
	const char *led_offsets = nullptr;
	bool fusion = false;
	const char *run_log = nullptr;
	const char *fusion_out = nullptr;
	const char *fusion_scenarios = nullptr;
	const char *fusion_frontend = nullptr;
	std::vector<std::pair<std::string, std::string>> compare_frontends;
	bool compare = false;
	const char *compare_out = nullptr;
	for (int i = 2; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "--upstream-distortion-codes") {
			upstream_distortion_codes = true;
		} else if (arg == "--m1") {
			m1 = true;
		} else if (arg == "--tracker-filter") {
			tracker = true;
			tracker_filter = true;
		} else if (arg == "--tracker") {
			tracker = true;
		} else if (arg == "--filter-eval" && i + 1 < argc) {
			filter_eval = argv[++i];
		} else if (arg == "--filter-out" && i + 1 < argc) {
			filter_out = argv[++i];
		} else if (arg == "--imu-angle-deg" && i + 1 < argc) {
			imu_angle_deg = std::atof(argv[++i]);
		} else if (arg == "--imu-csv" && i + 1 < argc) {
			imu_csv = argv[++i];
		} else if (arg == "--geometry" && i + 1 < argc) {
			geometry_prefix = argv[++i];
		} else if (arg == "--tracking-csv" && i + 1 < argc) {
			tracking_csv = argv[++i];
		} else if (arg == "--tracker-csv" && i + 1 < argc) {
			tracker = true;
			tracker_csv = argv[++i];
		} else if (arg == "--seed-recorded") {
			seed_recorded = true;
		} else if (arg == "--csv" && i + 1 < argc) {
			csv = argv[++i];
		} else if (arg == "--blobs-csv" && i + 1 < argc) {
			m1 = true;
			blobs_csv = argv[++i];
		} else if (arg == "--residuals-csv" && i + 1 < argc) {
			m1 = true;
			residuals_csv = argv[++i];
		} else if (arg == "--led-offsets" && i + 1 < argc) {
			led_offsets = argv[++i];
		} else if (arg == "--recorded-calibration" && i + 1 < argc) {
			recorded_calibration = argv[++i];
		} else if (arg == "--calibration" && i + 1 < argc) {
			calibration = argv[++i];
		} else if (arg == "--fusion-compare") {
			fusion = true;
		} else if (arg == "--run-log" && i + 1 < argc) {
			run_log = argv[++i];
		} else if (arg == "--fusion-out" && i + 1 < argc) {
			fusion_out = argv[++i];
		} else if (arg == "--fusion-scenarios" && i + 1 < argc) {
			fusion_scenarios = argv[++i];
		} else if (arg == "--fusion-frontend" && i + 1 < argc) {
			fusion_frontend = argv[++i];
		} else if (arg == "--compare-frontend" && i + 1 < argc) {
			std::string spec = argv[++i];
			size_t eq = spec.find('=');
			if (eq == std::string::npos || eq == 0) {
				std::fprintf(stderr, "--compare-frontend wants NAME=RECORDS.csv, got %s\n",
				             spec.c_str());
				return 2;
			}
			compare_frontends.push_back({spec.substr(0, eq), spec.substr(eq + 1)});
			compare = true;
		} else if (arg == "--compare-out" && i + 1 < argc) {
			compare_out = argv[++i];
			compare = true;
		}
	}

	try {
		DatasetReader dataset(argv[1], upstream_distortion_codes);
		if (calibration) {
			override_calibration(dataset, calibration, recorded_calibration);
		}
		if (led_offsets) {
			apply_led_offsets(dataset, led_offsets);
		}
		int status = summarise(dataset);
		if (imu_csv) {
			// IMU samples as the devices pushed them (host time; accel m/s^2 and gyro rad/s in the IMU
			// frame).
			FILE *f = std::fopen(imu_csv, "w");
			std::fprintf(f, "timestamp_ns,device,ax,ay,az,gx,gy,gz\n");
			for (const DatasetImuSample &imu : dataset.imu_samples) {
				const xrt_imu_sample &s = imu.sample;
				std::fprintf(f, "%" PRIi64 ",%d,%.6f,%.6f,%.6f,%.7f,%.7f,%.7f\n", s.timestamp_ns,
				             (int)imu.device_id, s.accel_m_s2.x, s.accel_m_s2.y, s.accel_m_s2.z,
				             s.gyro_rad_secs.x, s.gyro_rad_secs.y, s.gyro_rad_secs.z);
			}
			std::fclose(f);
			std::printf("imu samples: %zu\n", dataset.imu_samples.size());
		}
		if (geometry_prefix) {
			// Camera world poses per sample (XR convention) and each device's LED model (device frame, as
			// stored).
			std::string prefix = geometry_prefix;
			FILE *f = std::fopen((prefix + "-cameras.csv").c_str(), "w");
			std::fprintf(f, "timestamp_ns,camera,px,py,pz,qx,qy,qz,qw\n");
			for (const CameraSample &sample : dataset.samples) {
				if (!sample.Txr_world_cam.has_value()) {
					continue;
				}
				const xrt_pose &c = sample.Txr_world_cam.value();
				std::fprintf(f, "%" PRIi64 ",%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
				             sample.timestamp_ns, sample.camera_index, c.position.x, c.position.y,
				             c.position.z, c.orientation.x, c.orientation.y, c.orientation.z,
				             c.orientation.w);
			}
			std::fclose(f);
			f = std::fopen((prefix + "-leds.csv").c_str(), "w");
			std::fprintf(f, "device,led,px,py,pz,nx,ny,nz,visibility_angle\n");
			for (const DatasetDevice &device : dataset.devices) {
				for (size_t l = 0; l < device.leds.size(); l++) {
					const t_constellation_tracker_led &led = device.leds[l];
					std::fprintf(f, "%d,%zu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.4f\n", (int)device.id,
					             l, led.position.x, led.position.y, led.position.z, led.normal.x,
					             led.normal.y, led.normal.z, led.visibility_angle);
				}
			}
			std::fclose(f);
		}
		if (tracking_csv) {
			// Every recorded tracking-source relation (what the device predicted at each exposure), XR
			// convention.
			FILE *f = std::fopen(tracking_csv, "w");
			std::fprintf(f, "timestamp_ns,device,camera,flags,px,py,pz,qx,qy,qz,qw\n");
			for (const DatasetDeviceTracking &t : dataset.device_tracking) {
				std::fprintf(f, "%" PRIi64 ",%d,%u,%u,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
				             t.timestamp_ns, (int)t.device_id, t.camera_index,
				             (unsigned)t.relation_flags, t.pose.position.x, t.pose.position.y,
				             t.pose.position.z, t.pose.orientation.x, t.pose.orientation.y,
				             t.pose.orientation.z, t.pose.orientation.w);
			}
			std::fclose(f);
		}
		if (filter_eval) {
			status = replay_filter(dataset, filter_eval, imu_angle_deg, filter_out) != 0 ? 1 : status;
		}
		std::vector<int64_t> exposure_times;
		for (const Exposure &exposure : group_exposures(dataset.samples)) {
			exposure_times.push_back(exposure.timestamp_ns);
		}
		// Front ends: M1/M2 runs once; imported ones come from records files. Every consumer sees the same
		// solves.
		std::vector<FrontendRun> frontends;
		if (fusion || compare) {
			FrontendRun ours;
			ours.name = "M1";
			status = replay_m1(dataset, csv, seed_recorded, blobs_csv, &ours.records, &ours.exposure_us,
			                   residuals_csv) != 0
			             ? 1
			             : status;
			frontends.push_back(std::move(ours));
			for (const auto &[name, path] : compare_frontends) {
				FrontendRun run;
				run.name = name;
				if (!load_frontend_records(dataset, exposure_times, path.c_str(), run)) {
					return 1;
				}
				frontends.push_back(std::move(run));
			}
		}
		if (compare) {
			status = frontend_compare(dataset, exposure_times, frontends, compare_out) != 0 ? 1 : status;
		}
		if (fusion) {
#ifdef XRT_HAVE_CONSTELLATION_FUSION_EVAL
			const FrontendRun *input = &frontends[0];
			if (fusion_frontend != nullptr) {
				input = nullptr;
				for (const FrontendRun &run : frontends) {
					input = run.name == fusion_frontend ? &run : input;
				}
				if (input == nullptr) {
					std::fprintf(stderr, "--fusion-frontend %s: no such front end\n",
					             fusion_frontend);
					return 1;
				}
			}
			std::printf("fusion input: %s front end\n", input->name.c_str());
			const std::vector<FrontendRecord> &records = input->records;
			FusionCompareOptions options;
			options.run_log = run_log;
			options.out_prefix = fusion_out;
			options.imu_angle_deg = imu_angle_deg;
			if (fusion_scenarios != nullptr) {
				options.scenarios = fusion_scenarios;
			}
			status = fusion_compare(dataset, exposure_times, records, options) != 0 ? 1 : status;
#else
			std::fprintf(stderr, "--fusion-compare needs a build with Ceres >= 2.1\n");
			status = 1;
#endif
		} else if (m1 && !compare) {
			status = replay_m1(dataset, csv, seed_recorded, blobs_csv, nullptr, nullptr, residuals_csv) != 0
			             ? 1
			             : status;
		}
		if (tracker) {
			status = replay_tracker(dataset, tracker_csv, tracker_filter, imu_angle_deg) != 0 ? 1 : status;
		}
		return status;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "failed to load %s: %s\n", argv[1], e.what());
		return 1;
	}
}
