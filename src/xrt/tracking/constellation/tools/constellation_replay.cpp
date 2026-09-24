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

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <map>
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

} // namespace

int
main(int argc, char **argv)
{
	if (argc < 2) {
		std::fprintf(stderr, "usage: %s DATASET.ctd\n", argv[0]);
		return 2;
	}

	try {
		DatasetReader dataset(argv[1]);
		return summarise(dataset);
	} catch (const std::exception &e) {
		std::fprintf(stderr, "failed to load %s: %s\n", argv[1], e.what());
		return 1;
	}
}
