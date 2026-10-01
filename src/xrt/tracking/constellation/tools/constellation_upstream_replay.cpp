// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Replays a recorded constellation dataset through this tree's constellation tracker, writing what it found
 *         as frontend records for comparison with other frontends (constellation_upstream_replay).
 *
 * The tracker runs deterministically and single-threaded, so every camera frame gets its fast path and, when that
 * fails, its slow correspondence search. Live, the tracker runs those on separate threads and drops frames that
 * arrive while a thread is busy, so its coverage here is an upper bound; the per-frame processing times written out
 * show how often that would happen.
 *
 * Stand-ins replace the live pieces around the tracker:
 *
 * - The tracking origin is camera 0's recorded world pose, the other cameras fixed relative to it as recorded.
 * - Each blob detector keeps the LED labels the tracker marks and hands them back on the next frame for blobs with the
 *   same id, as t_rift_blobwatch does. Recorded blob ids come from t_rift_blobwatch, so they persist the same way.
 * - Each controller's tracking source is the history of the poses the tracker pushed for it, as the Sense driver's
 *   constellation pose is. The tracker in this tree does not use IMU samples, so none are fed to it.
 *
 * Output CSV, one row per line, the first field naming the row:
 *
 *     R,timestamp_ns,device,camera,matched_blobs,rms_px,px,py,pz,qx,qy,qz,qw
 *         a pose the tracker pushed: the LED model in the world, OpenCV convention
 *     M,timestamp_ns,device,camera,led,u,v
 *         a blob-to-LED match behind the pose with the same timestamp, device and camera (distorted pixels)
 *     F,timestamp_ns,camera,us
 *         the time the tracker spent on one camera frame, all devices
 *
 * @author Nick Kennedy
 * @ingroup tracking
 */

#include "t_constellation_tracker_dataset.hpp"
#include "t_constellation_tracker.h"

#include "math/m_api.h"
#include "math/m_relation_history.h"
#include "xrt/xrt_frame.h"

#include "pssense/pssense_led_model.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace xrt::tracking::constellation;

namespace {

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

struct FakeOrigin
{
	t_constellation_tracker_tracking_source base;
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

//! A blob detector's label memory for one camera, see the file comment.
struct FakeBlobwatch
{
	t_blobwatch base;
	uint32_t camera;
	//! blob id -> (device, LED) labels, carried to the next frame.
	std::map<uint32_t, std::pair<t_constellation_device_id_t, t_constellation_led_id_it>> labels;
	FILE *csv;
};

void
fake_blobwatch_mark(t_blobwatch *tbw, const t_blob_observation *tbo, t_constellation_device_id_t device_id)
{
	FakeBlobwatch *bw = (FakeBlobwatch *)tbw;
	for (auto it = bw->labels.begin(); it != bw->labels.end();) {
		it = it->second.first == device_id ? bw->labels.erase(it) : std::next(it);
	}
	for (uint32_t i = 0; i < tbo->num_blobs; i++) {
		const t_blob &b = tbo->blobs[i];
		if (b.matched_device_id != device_id || b.matched_device_led_id == XRT_CONSTELLATION_INVALID_LED_ID) {
			continue;
		}
		bw->labels[b.blob_id] = {device_id, b.matched_device_led_id};
		if (bw->csv != nullptr) {
			std::fprintf(bw->csv, "M,%" PRIi64 ",%d,%u,%u,%.4f,%.4f\n", tbo->timestamp_ns, (int)device_id,
			             bw->camera, (unsigned)b.matched_device_led_id, b.center.x, b.center.y);
		}
	}
}

void
fake_blobwatch_destroy(t_blobwatch *tbw)
{
	(void)tbw;
}

//! Stands in for the Sense driver: its tracking source is the history of the poses the tracker pushed.
struct FakeDevice
{
	t_constellation_tracker_device base;
	t_constellation_tracker_tracking_source source;
	std::vector<t_constellation_tracker_led> leds; // OpenXR convention, as a driver provides them
	t_constellation_device_id_t recorded_id{XRT_CONSTELLATION_INVALID_DEVICE_ID};
	t_constellation_device_id_t id{XRT_CONSTELLATION_INVALID_DEVICE_ID};
	m_relation_history *history{nullptr};
	FILE *csv{nullptr};
	uint32_t pushes{0};
	std::map<uint32_t, uint32_t> pushes_per_camera;
	Stats rms_px;
};

FakeDevice *
fake_device_of_source(t_constellation_tracker_tracking_source *source)
{
	return (FakeDevice *)((char *)source - offsetof(FakeDevice, source));
}

void
fake_device_push(t_constellation_tracker_device *device, t_constellation_tracker_sample *sample)
{
	FakeDevice *fake = (FakeDevice *)device;
	fake->pushes++;
	fake->pushes_per_camera[(uint32_t)sample->camera_index]++;
	fake->rms_px.add(sample->metrics.reprojection_error);

	xrt_space_relation relation = XRT_SPACE_RELATION_ZERO;
	relation.pose = sample->pose;
	relation.relation_flags =
	    (xrt_space_relation_flags)(XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
	                               XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
	                               XRT_SPACE_RELATION_POSITION_VALID_BIT | XRT_SPACE_RELATION_POSITION_TRACKED_BIT);
	m_relation_history_push(fake->history, &relation, sample->timestamp_ns);

	if (fake->csv != nullptr) {
		xrt_pose cv;
		math_pose_convert_from_opencv(&sample->pose, &cv); // The conversion is its own inverse.
		std::fprintf(fake->csv, "R,%" PRIi64 ",%d,%u,%u,%.4f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
		             sample->timestamp_ns, (int)fake->recorded_id, (unsigned)sample->camera_index,
		             sample->metrics.matched_blob_count, sample->metrics.reprojection_error, cv.position.x,
		             cv.position.y, cv.position.z, cv.orientation.x, cv.orientation.y, cv.orientation.z,
		             cv.orientation.w);
	}
}

void
fake_device_get(t_constellation_tracker_tracking_source *source, int64_t when_ns, xrt_space_relation *out)
{
	FakeDevice *fake = fake_device_of_source(source);
	*out = XRT_SPACE_RELATION_ZERO;
	m_relation_history_get(fake->history, when_ns, out);
}

int
usage(const char *name)
{
	std::fprintf(stderr,
	             "usage: %s DATASET.ctd OUT.csv [--min-leds-without-prior N] [--min-leds-with-prior N]\n"
	             "Replays the dataset through this tree's constellation tracker and writes frontend records.\n",
	             name);
	return 1;
}

} // namespace

int
main(int argc, char **argv)
{
	if (argc < 3) {
		return usage(argv[0]);
	}
	// The Sense driver's defaults in this tree.
	t_constellation_tracker_led_model_match_parameters match = DEFAULT_MATCH_PARAMETERS;
	(void)T_led_imu_left;
	(void)T_led_imu_right;
	for (int i = 3; i < argc; i++) {
		std::string arg = argv[i];
		if (arg == "--min-leds-without-prior" && i + 1 < argc) {
			match.min_leds_for_correspondence_search_without_prior = (uint32_t)std::atoi(argv[++i]);
		} else if (arg == "--min-leds-with-prior" && i + 1 < argc) {
			match.min_leds_for_correspondence_search_with_prior = (uint32_t)std::atoi(argv[++i]);
		} else {
			return usage(argv[0]);
		}
	}

	std::unique_ptr<DatasetReader> dataset;
	try {
		dataset = std::make_unique<DatasetReader>(argv[1]);
	} catch (const std::exception &e) {
		std::fprintf(stderr, "failed to load %s: %s\n", argv[1], e.what());
		return 1;
	}
	if (!dataset->stop_reason.empty()) {
		std::fprintf(stderr, "warning: reading stopped early: %s\n", dataset->stop_reason.c_str());
	}
	if (dataset->mosaics.empty() || dataset->samples.empty()) {
		std::fprintf(stderr, "nothing to replay\n");
		return 1;
	}
	const DatasetMosaic &mosaic = dataset->mosaics[0];
	const size_t camera_count = mosaic.camera_calibrations.size();

	FILE *csv = std::fopen(argv[2], "w");
	if (csv == nullptr) {
		std::fprintf(stderr, "cannot write %s\n", argv[2]);
		return 1;
	}
	std::fprintf(csv, "# constellation frontend records v1: %s replayed by constellation_upstream_replay\n",
	             argv[1]);

	// Rig geometry from the first exposure that has every camera's pose.
	std::vector<const CameraSample *> order;
	for (const CameraSample &sample : dataset->samples) {
		order.push_back(&sample);
	}
	std::stable_sort(order.begin(), order.end(), [](const CameraSample *a, const CameraSample *b) {
		return a->timestamp_ns != b->timestamp_ns ? a->timestamp_ns < b->timestamp_ns
		                                          : a->camera_index < b->camera_index;
	});
	std::vector<std::optional<xrt_pose>> first_world(camera_count);
	{
		std::vector<std::optional<xrt_pose>> world(camera_count);
		int64_t group_ns = INT64_MIN;
		for (const CameraSample *sample : order) {
			if (sample->timestamp_ns - group_ns > 2'000'000) {
				world.assign(camera_count, std::nullopt);
				group_ns = sample->timestamp_ns;
			}
			if (sample->camera_index < camera_count && sample->Txr_world_cam.has_value()) {
				world[sample->camera_index] = sample->Txr_world_cam;
			}
			if (std::all_of(world.begin(), world.end(), [](const auto &w) { return w.has_value(); })) {
				first_world = world;
				break;
			}
		}
	}
	if (!first_world[0].has_value()) {
		std::fprintf(stderr, "no exposure with every camera's pose\n");
		return 1;
	}

	FakeOrigin origin{};
	origin.base.get_tracked_pose = fake_origin_get;
	for (const CameraSample *sample : order) {
		if (sample->camera_index == 0 && sample->Txr_world_cam.has_value()) {
			origin.poses.push_back({sample->timestamp_ns, sample->Txr_world_cam.value()});
		}
	}

	t_constellation_tracker_params params{};
	params.flags = T_CONSTELLATION_TRACKER_FLAGS_DETERMINISTIC;
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

	xrt_frame_context xfctx{};
	t_constellation_tracker *tracker = nullptr;
	if (t_constellation_tracker_create(&xfctx, &params, &tracker) != 0) {
		std::fprintf(stderr, "failed to create tracker\n");
		return 1;
	}

	std::vector<std::unique_ptr<FakeDevice>> fakes;
	std::map<t_constellation_device_id_t, t_constellation_device_id_t> recorded_from_tracker;
	for (const DatasetDevice &device : dataset->devices) {
		auto fake = std::make_unique<FakeDevice>();
		fake->base.push_constellation_tracker_sample = fake_device_push;
		fake->source.get_tracked_pose = fake_device_get;
		fake->recorded_id = device.id;
		fake->csv = csv;
		m_relation_history_create(&fake->history);
		// Datasets store the model in the tracker's OpenCV convention; drivers hand over OpenXR.
		fake->leds = device.leds;
		for (t_constellation_tracker_led &led : fake->leds) {
			led.position.y = -led.position.y;
			led.position.z = -led.position.z;
			led.normal.y = -led.normal.y;
			led.normal.z = -led.normal.z;
		}
		t_constellation_tracker_device_params dparams{};
		dparams.led_model.leds = fake->leds.data();
		dparams.led_model.led_count = fake->leds.size();
		dparams.led_model.match_parameters = match;
		dparams.led_model.compute_led_visibility = nullptr;
		dparams.tracking_source = &fake->source;
		if (t_constellation_tracker_add_device(tracker, &dparams, &fake->base, &fake->id) != 0) {
			std::fprintf(stderr, "failed to add device %d\n", (int)device.id);
			return 1;
		}
		fakes.push_back(std::move(fake));
	}

	std::vector<FakeBlobwatch> blobwatches(camera_count);
	for (size_t c = 0; c < camera_count; c++) {
		blobwatches[c].base.mark_blob_device = fake_blobwatch_mark;
		blobwatches[c].base.destroy = fake_blobwatch_destroy;
		blobwatches[c].camera = (uint32_t)c;
		blobwatches[c].csv = csv;
	}

	/*
	 * Labels carry across frames under the recorded device ids, which may differ from the ids this tracker assigns:
	 * map them, as a blob detector would hold the tracker's own.
	 */
	std::map<t_constellation_device_id_t, t_constellation_device_id_t> tracker_from_recorded;
	for (const auto &fake : fakes) {
		tracker_from_recorded[fake->recorded_id] = fake->id;
	}

	Stats frame_us;
	std::vector<Stats> frame_us_per_camera(camera_count);
	std::vector<t_blob> blobs;
	auto start = std::chrono::steady_clock::now();
	for (const CameraSample *sample : order) {
		if (sample->camera_index >= camera_count) {
			continue;
		}
		FakeBlobwatch &bw = blobwatches[sample->camera_index];
		blobs.assign(sample->blobs, sample->blobs + sample->blob_count);
		for (t_blob &b : blobs) {
			// Only this replay's own labels: the recording's come from another frontend.
			b.matched_device_id = XRT_CONSTELLATION_INVALID_DEVICE_ID;
			b.matched_device_led_id = XRT_CONSTELLATION_INVALID_LED_ID;
			auto it = bw.labels.find(b.blob_id);
			if (it != bw.labels.end()) {
				b.matched_device_id = it->second.first;
				b.matched_device_led_id = it->second.second;
			}
		}

		t_blob_observation observation{};
		observation.source = &bw.base;
		observation.id = sample->id;
		observation.timestamp_ns = sample->timestamp_ns;
		observation.blobs = blobs.data();
		observation.num_blobs = (uint32_t)blobs.size();
		t_blob_sink *sink = params.mosaics[0].cameras[sample->camera_index].blob_sink;

		auto frame_start = std::chrono::steady_clock::now();
		sink->push_blobs(sink, &observation);
		double us =
		    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - frame_start).count();
		frame_us.add(us);
		frame_us_per_camera[sample->camera_index].add(us);
		std::fprintf(csv, "F,%" PRIi64 ",%u,%.1f\n", sample->timestamp_ns, sample->camera_index, us);

		// A label lives as long as its blob: drop those whose blob did not reach this frame.
		for (auto it = bw.labels.begin(); it != bw.labels.end();) {
			bool present = std::any_of(blobs.begin(), blobs.end(),
			                           [&](const t_blob &b) { return b.blob_id == it->first; });
			it = present ? std::next(it) : bw.labels.erase(it);
		}
	}
	double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

	xrt_frame_context_destroy_nodes(&xfctx);
	std::fclose(csv);

	// Live, a camera frame that takes longer than the frame interval delays or drops the next.
	double interval_us = 0.0;
	if (order.size() > camera_count) {
		interval_us = (double)(order.back()->timestamp_ns - order.front()->timestamp_ns) / 1000.0 /
		              ((double)order.size() / (double)camera_count);
	}
	size_t over = 0;
	for (double us : frame_us.values) {
		over += us > interval_us ? 1 : 0;
	}
	std::printf(
	    "upstream tracker replay: %zu camera frames in %.2f s; per frame us p50 %.0f p95 %.0f max %.0f; "
	    "%zu (%.1f%%) over the %.1f ms frame interval\n",
	    order.size(), seconds, frame_us.pct(0.5), frame_us.pct(0.95), frame_us.pct(1.0), over,
	    frame_us.values.empty() ? 0.0 : 100.0 * (double)over / (double)frame_us.values.size(),
	    interval_us / 1000.0);
	for (auto &fake : fakes) {
		std::printf("  device %d: %u poses pushed; per camera:", (int)fake->recorded_id, fake->pushes);
		for (const auto &[camera, count] : fake->pushes_per_camera) {
			std::printf(" %u:%u", camera, count);
		}
		std::printf("; rms px p50 %.3f p95 %.3f\n", fake->rms_px.pct(0.5), fake->rms_px.pct(0.95));
		m_relation_history_destroy(&fake->history);
	}
	(void)recorded_from_tracker;
	return 0;
}
