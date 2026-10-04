// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Writes a synthetic constellation dataset with exact ground truth (constellation_synth).
 *
 * Two PS Sense controllers move in front of a head carrying four fisheye cameras placed like the PS VR2's. Every
 * exposure renders the LEDs that face each camera through the same camera model the solvers use, with pixel noise,
 * dropped and merged blobs and static background lights. The file has the same packets a recording has (camera
 * samples, device info, tracking-source orientation and IMU samples) plus head poses and ground-truth poses, so any
 * frontend or backend can be replayed on it and scored against the truth.
 *
 * Nothing here models the real Sense LED blinking, motion blur or the headset's exposure timing: a synthetic result
 * shows that a pipeline works and how it degrades with noise, not how it will perform on recordings.
 *
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "t_constellation_tracker_dataset.hpp"
#include "t_constellation_tracker.h"

#include "math/m_api.h"
#include "math/m_vec3.h"
#include "tracking/t_camera_models.h"

#include "pssense/pssense_led_model.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace xrt::tracking::constellation;

namespace {

struct Options
{
	const char *out{nullptr};
	double duration_s{30.0};
	double exposure_hz{60.0};
	double imu_hz{66.0};
	//! Motion speed multiplier.
	double speed{1.0};
	uint32_t seed{1};
	double pixel_sigma{0.2};
	double drop_probability{0.05};
	//! Static background lights, seen by every camera that faces them.
	uint32_t background_lights{3};
	//! Hold each controller still for this long every period (0: never), with static_begin/end annotations.
	double still_s{3.0};
	double still_period_s{10.0};
	double gyro_noise{0.004};
	double accel_noise{0.04};
};

constexpr int kCameraCount = 4;
constexpr int kImageSize = 508;

xrt_pose
pose_from_euler(double yaw_deg, double pitch_deg, double roll_deg, xrt_vec3 position)
{
	// XR convention: yaw about +y, pitch about +x, roll about -z.
	xrt_quat yaw, pitch, roll, tmp, out;
	xrt_vec3 y{0, 1, 0}, x{1, 0, 0}, z{0, 0, -1};
	math_quat_from_angle_vector((float)(yaw_deg * M_PI / 180.0), &y, &yaw);
	math_quat_from_angle_vector((float)(pitch_deg * M_PI / 180.0), &x, &pitch);
	math_quat_from_angle_vector((float)(roll_deg * M_PI / 180.0), &z, &roll);
	math_quat_rotate(&yaw, &pitch, &tmp);
	math_quat_rotate(&tmp, &roll, &out);
	math_quat_normalize(&out);
	return xrt_pose{out, position};
}

t_camera_calibration
make_calibration()
{
	// A fisheye with roughly the PS VR2 tracking cameras' field of view on their 508 x 508 native image.
	t_camera_calibration cal{};
	cal.image_size_pixels.w = kImageSize;
	cal.image_size_pixels.h = kImageSize;
	cal.intrinsics[0][0] = 205.0;
	cal.intrinsics[1][1] = 205.0;
	cal.intrinsics[0][2] = 253.5;
	cal.intrinsics[1][2] = 253.5;
	cal.intrinsics[2][2] = 1.0;
	cal.distortion_model = T_DISTORTION_FISHEYE_KB4;
	cal.kb4 = t_camera_calibration_kb4_params{0.02, -0.006, 0.0015, -0.0002};
	return cal;
}

//! Camera c in the head frame (OpenXR convention): a lower and an upper pair at the front corners, splayed outwards.
xrt_pose
head_from_camera(int c)
{
	switch (c) {
	case 0: return pose_from_euler(25.0, -25.0, 0.0, xrt_vec3{-0.040f, -0.035f, -0.075f});
	case 1: return pose_from_euler(-25.0, -25.0, 0.0, xrt_vec3{0.040f, -0.035f, -0.075f});
	case 2: return pose_from_euler(40.0, 10.0, 0.0, xrt_vec3{-0.075f, 0.020f, -0.065f});
	default: return pose_from_euler(-40.0, 10.0, 0.0, xrt_vec3{0.075f, 0.020f, -0.065f});
	}
}

xrt_pose
head_pose(double t, const Options &o)
{
	double s = o.speed;
	return pose_from_euler(20.0 * std::sin(2 * M_PI * 0.11 * s * t),
	                       -12.0 + 8.0 * std::sin(2 * M_PI * 0.07 * s * t), 3.0 * std::sin(2 * M_PI * 0.05 * s * t),
	                       xrt_vec3{(float)(0.03 * std::sin(2 * M_PI * 0.09 * s * t)), 1.60f,
	                                (float)(0.02 * std::sin(2 * M_PI * 0.06 * s * t))});
}

//! Whether controller @p hand is being held still at @p t, and the time its stillness began.
bool
is_still(double t, int hand, const Options &o, double &began)
{
	if (o.still_s <= 0.0 || o.still_period_s <= o.still_s) {
		return false;
	}
	// Offset the two controllers so their still intervals differ.
	double phase = std::fmod(t + hand * 0.5 * o.still_period_s, o.still_period_s);
	double start = o.still_period_s - o.still_s;
	if (phase < start) {
		return false;
	}
	began = t - (phase - start);
	return true;
}

xrt_pose
moving_device_pose(double t, int hand, const Options &o)
{
	double side = hand == 0 ? -1.0 : 1.0;
	double s = o.speed;
	double w = 2 * M_PI * (0.35 + 0.1 * hand) * s;
	// Hands sweep arcs below and in front of the head; wrists pitch, yaw and roll.
	xrt_vec3 position{(float)(side * 0.20 + 0.12 * std::sin(w * t)),
	                  (float)(1.25 + 0.10 * std::sin(0.7 * w * t + 1.0)),
	                  (float)(-0.38 + 0.08 * std::cos(0.9 * w * t))};
	return pose_from_euler(side * 10.0 + 35.0 * std::sin(0.8 * w * t + hand), 25.0 * std::sin(1.1 * w * t),
	                       side * 15.0 + 30.0 * std::sin(0.6 * w * t + 2.0), position);
}

xrt_pose
device_pose(double t, int hand, const Options &o)
{
	double began;
	return is_still(t, hand, o, began) ? moving_device_pose(began, hand, o) : moving_device_pose(t, hand, o);
}

//! Angular velocity in the body frame from a central difference of the true pose, rad/s.
xrt_vec3
body_angular_velocity(double t, int hand, const Options &o)
{
	const double h = 1e-4;
	xrt_pose a = device_pose(t - h, hand, o), b = device_pose(t + h, hand, o);
	xrt_quat a_inv, delta;
	math_quat_invert(&a.orientation, &a_inv);
	math_quat_rotate(&a_inv, &b.orientation, &delta);
	math_quat_normalize(&delta);
	// SO(3) log returns the full rotation vector over the 2*h interval.
	xrt_vec3 axis_angle;
	math_quat_ln_so3(&delta, &axis_angle);
	return m_vec3_mul_scalar(axis_angle, (float)(1.0 / (2 * h)));
}

xrt_vec3
world_acceleration(double t, int hand, const Options &o)
{
	const double h = 1e-3;
	xrt_vec3 a = device_pose(t - h, hand, o).position, b = device_pose(t, hand, o).position,
	         c = device_pose(t + h, hand, o).position;
	return xrt_vec3{(float)((a.x - 2 * b.x + c.x) / (h * h)), (float)((a.y - 2 * b.y + c.y) / (h * h)),
	                (float)((a.z - 2 * b.z + c.z) / (h * h))};
}

struct Blob
{
	xrt_vec2 center;
	float radius_px;
	float brightness;
	//! Stable identity while the source stays visible: device * 1000 + led, or 100000 + light.
	uint32_t source;
};

int
usage(const char *name)
{
	std::fprintf(stderr,
	             "usage: %s OUT.ctd [--duration S] [--speed X] [--seed N] [--pixel-sigma PX] [--drop P] "
	             "[--background N] [--still S] [--still-period S] [--exposure-hz HZ] [--imu-hz HZ]\n",
	             name);
	return 1;
}

} // namespace

int
main(int argc, char **argv)
{
	// The shared LED header also defines the IMU offsets, which the synthetic IMU does not use.
	(void)T_led_imu_left;
	(void)T_led_imu_right;

	Options o;
	if (argc < 2) {
		return usage(argv[0]);
	}
	o.out = argv[1];
	for (int i = 2; i < argc; i++) {
		std::string arg = argv[i];
		auto next = [&]() { return i + 1 < argc ? std::atof(argv[++i]) : NAN; };
		if (arg == "--duration") {
			o.duration_s = next();
		} else if (arg == "--speed") {
			o.speed = next();
		} else if (arg == "--seed") {
			o.seed = (uint32_t)next();
		} else if (arg == "--pixel-sigma") {
			o.pixel_sigma = next();
		} else if (arg == "--drop") {
			o.drop_probability = next();
		} else if (arg == "--background") {
			o.background_lights = (uint32_t)next();
		} else if (arg == "--still") {
			o.still_s = next();
		} else if (arg == "--still-period") {
			o.still_period_s = next();
		} else if (arg == "--exposure-hz") {
			o.exposure_hz = next();
		} else if (arg == "--imu-hz") {
			o.imu_hz = next();
		} else {
			return usage(argv[0]);
		}
	}

	std::mt19937 rng(o.seed);
	std::normal_distribution<double> unit(0.0, 1.0);
	std::uniform_real_distribution<double> uniform(0.0, 1.0);

	t_camera_calibration calibration = make_calibration();
	t_camera_model_params model;
	t_camera_model_params_from_t_camera_calibration(&calibration, &model);

	// LED models: the driver's tables (OpenXR), and the tracker's OpenCV convention that datasets store.
	std::vector<t_constellation_tracker_led> xr_leds[2] = {
	    std::vector<t_constellation_tracker_led>(pssense_left_leds,
	                                             pssense_left_leds + ARRAY_SIZE(pssense_left_leds)),
	    std::vector<t_constellation_tracker_led>(pssense_right_leds,
	                                             pssense_right_leds + ARRAY_SIZE(pssense_right_leds))};
	std::vector<t_constellation_tracker_led> cv_leds[2];
	for (int d = 0; d < 2; d++) {
		cv_leds[d] = xr_leds[d];
		for (t_constellation_tracker_led &led : cv_leds[d]) {
			led.position.y = -led.position.y;
			led.position.z = -led.position.z;
			led.normal.y = -led.normal.y;
			led.normal.z = -led.normal.z;
		}
	}

	std::vector<xrt_vec3> lights;
	for (uint32_t i = 0; i < o.background_lights; i++) {
		// Ceiling lights and a window edge, a few metres away.
		lights.push_back(xrt_vec3{(float)(-1.5 + 1.5 * i), 2.6f, (float)(-2.0 - 0.7 * i)});
	}

	DataRecorder recorder(o.out, {std::vector<t_camera_calibration>(kCameraCount, calibration)});

	char info[512];
	std::snprintf(info, sizeof(info),
	              "{\"tool\":\"constellation_synth\",\"seed\":%u,\"duration_s\":%.1f,\"speed\":%.2f,"
	              "\"pixel_sigma\":%.3f,\"drop_probability\":%.3f,\"background_lights\":%u,\"still_s\":%.1f,"
	              "\"still_period_s\":%.1f,\"exposure_hz\":%.1f,\"imu_hz\":%.1f}",
	              o.seed, o.duration_s, o.speed, o.pixel_sigma, o.drop_probability, o.background_lights, o.still_s,
	              o.still_period_s, o.exposure_hz, o.imu_hz);
	recorder.recordSessionInfo(info);

	for (int d = 0; d < 2; d++) {
		t_constellation_tracker_led_model led_model{};
		led_model.leds = cv_leds[d].data();
		led_model.led_count = cv_leds[d].size();
		recorder.recordDeviceInfo((t_constellation_device_id_t)d, led_model);
	}

	// The IMU world differs from the optical world by a yaw, as a 3-DoF fusion's does.
	xrt_quat imu_world_from_world;
	xrt_vec3 up{0, 1, 0};
	math_quat_from_angle_vector((float)(37.0 * M_PI / 180.0), &up, &imu_world_from_world);

	// The IMU body is the LED model rotated about x by the Sense mounting angle.
	xrt_quat imu_from_model;
	xrt_vec3 x_axis{1, 0, 0};
	math_quat_from_angle_vector(pssense_imu_angle, &x_axis, &imu_from_model);

	const int64_t start_ns = 1'000'000'000;
	const double exposure_dt = 1.0 / o.exposure_hz;
	const double imu_dt = 1.0 / o.imu_hz;
	double next_imu_t[2] = {0.0, 0.37 * imu_dt};
	bool was_still[2] = {false, false};
	uint64_t sample_id = 1;
	uint32_t next_blob_id = 1;
	// Blob ids persist per camera while their source stays visible, as a blob tracker keeps them.
	std::map<std::pair<int, uint32_t>, uint32_t> live_ids, next_live_ids;
	size_t blob_total = 0, frame_total = 0;

	for (double t = 0.0; t < o.duration_s; t += exposure_dt) {
		const int64_t exposure_ns = start_ns + (int64_t)std::llround(t * 1e9);

		// IMU samples up to this exposure, per controller.
		for (int d = 0; d < 2; d++) {
			for (; next_imu_t[d] <= t; next_imu_t[d] += imu_dt) {
				double ti = next_imu_t[d];
				xrt_pose p = device_pose(ti, d, o);
				xrt_vec3 omega_model = body_angular_velocity(ti, d, o);
				// Specific force in the world: acceleration minus gravity (OpenXR, y up).
				xrt_vec3 a_world = world_acceleration(ti, d, o);
				a_world.y += 9.81f;
				xrt_quat world_from_model_inv;
				math_quat_invert(&p.orientation, &world_from_model_inv);
				xrt_vec3 f_model, gyro, accel;
				math_quat_rotate_vec3(&world_from_model_inv, &a_world, &f_model);
				math_quat_rotate_vec3(&imu_from_model, &omega_model, &gyro);
				math_quat_rotate_vec3(&imu_from_model, &f_model, &accel);
				xrt_imu_sample s{};
				s.timestamp_ns = start_ns + (int64_t)std::llround(ti * 1e9);
				s.gyro_rad_secs =
				    xrt_vec3_f64{gyro.x + o.gyro_noise * unit(rng), gyro.y + o.gyro_noise * unit(rng),
				                 gyro.z + o.gyro_noise * unit(rng)};
				s.accel_m_s2 = xrt_vec3_f64{accel.x + o.accel_noise * unit(rng),
				                            accel.y + o.accel_noise * unit(rng),
				                            accel.z + o.accel_noise * unit(rng)};
				recorder.recordImuSample((t_constellation_device_id_t)d, s);
				const double no_bias[3] = {0.0, 0.0, 0.0};
				recorder.recordImuTiming(DatasetImuTiming{(t_constellation_device_id_t)d,
				                                          s.timestamp_ns,
				                                          s.timestamp_ns,
				                                          0.0,
				                                          {no_bias[0], no_bias[1], no_bias[2]}});
			}
		}

		const xrt_pose head = head_pose(t, o);
		xrt_pose truth[2] = {device_pose(t, 0, o), device_pose(t, 1, o)};

		for (int d = 0; d < 2; d++) {
			double began;
			bool still = is_still(t, d, o, began);
			if (still != was_still[d]) {
				recorder.recordAnnotation(DatasetAnnotation{(t_constellation_device_id_t)d, exposure_ns,
				                                            still ? "static_begin" : "static_end"});
				was_still[d] = still;
			}
			recorder.recordGroundTruth(DatasetGroundTruth{(t_constellation_device_id_t)d, exposure_ns,
			                                              truth[d], 0.0f, 0.0f,
			                                              T_CONSTELLATION_GROUND_TRUTH_SYNTHETIC});
		}

		next_live_ids.clear();
		for (int c = 0; c < kCameraCount; c++) {
			xrt_pose h_c = head_from_camera(c);
			xrt_pose Txr_world_cam;
			math_pose_transform(&head, &h_c, &Txr_world_cam);
			xrt_pose cam_from_world;
			math_pose_invert(&Txr_world_cam, &cam_from_world);

			std::vector<Blob> blobs;
			auto project = [&](const xrt_vec3 &world, float radius_m, float brightness, uint32_t source) {
				xrt_vec3 p;
				math_pose_transform_point(&cam_from_world, &world, &p);
				if (-p.z < 0.05f) {
					return;
				}
				float u, v;
				if (!t_camera_models_flip_and_project(&model, p.x, p.y, p.z, &u, &v) || u < 2.0f ||
				    v < 2.0f || u > kImageSize - 3 || v > kImageSize - 3) {
					return;
				}
				float radius_px = std::max(1.0f, (float)(205.0 * radius_m / -p.z));
				blobs.push_back(Blob{xrt_vec2{u, v}, radius_px, brightness, source});
			};

			for (int d = 0; d < 2; d++) {
				for (const t_constellation_tracker_led &led : xr_leds[d]) {
					xrt_vec3 led_world, normal_world, to_camera;
					math_pose_transform_point(&truth[d], &led.position, &led_world);
					math_quat_rotate_vec3(&truth[d].orientation, &led.normal, &normal_world);
					to_camera = m_vec3_sub(Txr_world_cam.position, led_world);
					float cos_angle = m_vec3_dot(m_vec3_normalize(to_camera), normal_world);
					// Visible within the LED's cone, minus a margin for the controller body's
					// shadow.
					if (cos_angle < std::cos(led.visibility_angle) + 0.15f) {
						continue;
					}
					if (uniform(rng) < o.drop_probability) {
						continue;
					}
					project(led_world, led.radius_m * 0.5f, (float)(0.6 + 0.4 * cos_angle),
					        (uint32_t)(d * 1000 + led.id));
				}
			}
			for (size_t l = 0; l < lights.size(); l++) {
				project(lights[l], 0.05f, 0.9f, (uint32_t)(100000 + l));
			}

			// Blobs closer than their radii merge into one, as a blob detector would see them.
			for (size_t i = 0; i < blobs.size(); i++) {
				for (size_t j = i + 1; j < blobs.size();) {
					float dx = blobs[i].center.x - blobs[j].center.x,
					      dy = blobs[i].center.y - blobs[j].center.y;
					if (std::sqrt(dx * dx + dy * dy) < blobs[i].radius_px + blobs[j].radius_px) {
						blobs[i].center.x = 0.5f * (blobs[i].center.x + blobs[j].center.x);
						blobs[i].center.y = 0.5f * (blobs[i].center.y + blobs[j].center.y);
						blobs[i].radius_px =
						    std::max(blobs[i].radius_px, blobs[j].radius_px) * 1.3f;
						blobs.erase(blobs.begin() + (ptrdiff_t)j);
					} else {
						j++;
					}
				}
			}

			CameraSample sample{};
			sample.id = sample_id++;
			sample.timestamp_ns = exposure_ns + c * 50'000;
			sample.mosaic_index = 0;
			sample.camera_index = (uint32_t)c;
			sample.Txr_world_cam = Txr_world_cam;
			for (const Blob &b : blobs) {
				if (sample.blob_count >= XRT_CONSTELLATION_MAX_BLOBS_PER_FRAME) {
					break;
				}
				auto key = std::make_pair(c, b.source);
				auto it = live_ids.find(key);
				uint32_t id = it != live_ids.end() ? it->second : next_blob_id++;
				next_live_ids[key] = id;

				t_blob &out = sample.blobs[sample.blob_count++];
				out.blob_id = id;
				out.matched_device_id = XRT_CONSTELLATION_INVALID_DEVICE_ID;
				out.matched_device_led_id = XRT_CONSTELLATION_INVALID_LED_ID;
				out.center = xrt_vec2{(float)(b.center.x + o.pixel_sigma * unit(rng)),
				                      (float)(b.center.y + o.pixel_sigma * unit(rng))};
				out.size = xrt_vec2{2.0f * b.radius_px, 2.0f * b.radius_px};
				out.brightness = b.brightness;
				out.bounding_box.offset.w = (int)(out.center.x - b.radius_px);
				out.bounding_box.offset.h = (int)(out.center.y - b.radius_px);
				out.bounding_box.extent.w = (int)std::ceil(2.0f * b.radius_px);
				out.bounding_box.extent.h = (int)std::ceil(2.0f * b.radius_px);
			}
			blob_total += sample.blob_count;
			frame_total++;
			recorder.recordSample(sample);

			// The camera's world pose comes from the head, exactly.
			xrt_space_relation head_relation = XRT_SPACE_RELATION_ZERO;
			head_relation.pose = head;
			head_relation.relation_flags =
			    (xrt_space_relation_flags)(XRT_SPACE_RELATION_POSITION_VALID_BIT |
			                               XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
			                               XRT_SPACE_RELATION_POSITION_TRACKED_BIT |
			                               XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
			recorder.recordHeadPose(DatasetHeadPose{
			    sample.timestamp_ns, head_relation.relation_flags, head, sample.timestamp_ns,
			    T_CONSTELLATION_HEAD_POSE_EXACT | T_CONSTELLATION_HEAD_POSE_INTERPOLATED});

			// Each controller's tracking-source orientation: the truth in the yawed IMU world, slowly
			// drifting.
			for (int d = 0; d < 2; d++) {
				xrt_quat drift, imu_orientation, tmp;
				math_quat_from_angle_vector((float)(0.5 * M_PI / 180.0 / 60.0 * t), &up, &drift);
				math_quat_rotate(&imu_world_from_world, &truth[d].orientation, &tmp);
				math_quat_rotate(&drift, &tmp, &imu_orientation);
				xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
				relation.pose.orientation = imu_orientation;
				relation.relation_flags =
				    (xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
				                               XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT);
				recorder.recordDeviceTracking(sample, (t_constellation_device_id_t)d, relation);
			}
		}
		live_ids.swap(next_live_ids);
	}

	std::printf("wrote %s: %zu camera frames, %.1f blobs per frame, %.0f s\n", o.out, frame_total,
	            frame_total ? (double)blob_total / (double)frame_total : 0.0, o.duration_s);
	return 0;
}
