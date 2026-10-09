// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/* Replay PSVR2 prediction captures without a headset. */

#include "psvr2_prediction.h"
#include "psvr2_prediction_capture.h"
#include "math/m_api.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

struct prediction_difference_stats
{
	uint64_t records;
	uint64_t divergent;
	double max_orientation;
	double max_position;
	double max_linear_velocity;
	double max_angular_velocity;
	double sum_orientation;
	double sum_position;
	double sum_linear_velocity;
	double sum_angular_velocity;
	double sum_square_orientation;
	double sum_square_position;
	double sum_square_linear_velocity;
	double sum_square_angular_velocity;
};

struct forecast_stats
{
	uint64_t count;
	double sum_deg;
	double sum_square_deg;
	 double max_deg;
};

struct replay_prediction
{
	struct psvr2_capture_record record;
	struct xrt_space_relation ordinary;
	struct xrt_space_relation explicit;
	uint64_t newest_gyro_ns;
};

struct timing_stats
{
	uint64_t count;
	int64_t min_ns;
	int64_t max_ns;
	int64_t sum_ns;
};

static double
orientation_error_deg(const struct xrt_quat *a, const struct xrt_quat *b)
{
	double dot = fabs((double)a->x * b->x + (double)a->y * b->y + (double)a->z * b->z + (double)a->w * b->w);
	if (dot > 1.0) {
		dot = 1.0;
	}
	return 2.0 * acos(dot) * 180.0 / M_PI;
}

static void
add_forecast_error(struct forecast_stats *stats, double error_deg)
{
	stats->count++;
	stats->sum_deg += error_deg;
	stats->sum_square_deg += error_deg * error_deg;
	if (error_deg > stats->max_deg) {
		stats->max_deg = error_deg;
	}
}

static void
add_timing(struct timing_stats *stats, int64_t value_ns)
{
	stats->count++;
	stats->sum_ns += value_ns;
	if (value_ns < stats->min_ns) stats->min_ns = value_ns;
	if (value_ns > stats->max_ns) stats->max_ns = value_ns;
}

static void
print_timing(const char *name, const struct timing_stats *stats)
{
	if (stats->count == 0) return;
	fprintf(stdout, "%s count=%" PRIu64 " mean_ms=%.6g min_ms=%.6g max_ms=%.6g\n", name, stats->count,
	        (double)stats->sum_ns / (double)stats->count / 1000000.0, (double)stats->min_ns / 1000000.0,
	        (double)stats->max_ns / 1000000.0);
}

static double
max_abs_difference(const float *a, const float *b, size_t count)
{
	double max = 0.0;
	for (size_t i = 0; i < count; i++) {
		double difference = fabs((double)a[i] - (double)b[i]);
		if (difference > max) {
			max = difference;
		}
	}
	return max;
}

static struct prediction_difference_stats
prediction_difference(const struct xrt_space_relation *a, const struct xrt_space_relation *b)
{
	return (struct prediction_difference_stats){
	    .max_orientation = max_abs_difference(&a->pose.orientation.x, &b->pose.orientation.x, 4),
	    .max_position = max_abs_difference(&a->pose.position.x, &b->pose.position.x, 3),
	    .max_linear_velocity = max_abs_difference(&a->linear_velocity.x, &b->linear_velocity.x, 3),
	    .max_angular_velocity = max_abs_difference(&a->angular_velocity.x, &b->angular_velocity.x, 3),
	};
}

static void
accumulate_difference(struct prediction_difference_stats *total, struct prediction_difference_stats difference)
{
	total->records++;
	if (difference.max_orientation > 1e-7 || difference.max_position > 1e-7 ||
	    difference.max_linear_velocity > 1e-7 || difference.max_angular_velocity > 1e-7) {
		total->divergent++;
	}
	if (difference.max_orientation > total->max_orientation) {
		total->max_orientation = difference.max_orientation;
	}
	if (difference.max_position > total->max_position) {
		total->max_position = difference.max_position;
	}
	if (difference.max_linear_velocity > total->max_linear_velocity) {
		total->max_linear_velocity = difference.max_linear_velocity;
	}
	if (difference.max_angular_velocity > total->max_angular_velocity) {
		total->max_angular_velocity = difference.max_angular_velocity;
	}
	total->sum_orientation += difference.max_orientation;
	total->sum_position += difference.max_position;
	total->sum_linear_velocity += difference.max_linear_velocity;
	total->sum_angular_velocity += difference.max_angular_velocity;
	total->sum_square_orientation += difference.max_orientation * difference.max_orientation;
	total->sum_square_position += difference.max_position * difference.max_position;
	total->sum_square_linear_velocity += difference.max_linear_velocity * difference.max_linear_velocity;
	total->sum_square_angular_velocity += difference.max_angular_velocity * difference.max_angular_velocity;
}

static void
print_difference_stats(const char *name, const struct prediction_difference_stats *stats)
{
	if (stats->records == 0) {
		fprintf(stdout, "slam_%s records=0\n", name);
		return;
	}
	double count = (double)stats->records;
	fprintf(stdout,
	        "slam_%s records=%" PRIu64 " divergent=%" PRIu64
	        " max_orientation=%.9g max_position=%.9g max_linear_velocity=%.9g max_angular_velocity=%.9g"
	        " mean_orientation=%.9g mean_position=%.9g mean_linear_velocity=%.9g mean_angular_velocity=%.9g"
	        " rms_orientation=%.9g rms_position=%.9g rms_linear_velocity=%.9g rms_angular_velocity=%.9g\n",
	        name, stats->records, stats->divergent, stats->max_orientation, stats->max_position,
	        stats->max_linear_velocity, stats->max_angular_velocity, stats->sum_orientation / count,
	        stats->sum_position / count, stats->sum_linear_velocity / count, stats->sum_angular_velocity / count,
	        sqrt(stats->sum_square_orientation / count), sqrt(stats->sum_square_position / count),
	        sqrt(stats->sum_square_linear_velocity / count), sqrt(stats->sum_square_angular_velocity / count));
}

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s CAPTURE\n", argv[0]);
		return 2;
	}
	FILE *file = fopen(argv[1], "rb");
	if (file == NULL) {
		perror(argv[1]);
		return 1;
	}

	struct psvr2_capture_header header = {0};
	if (fread(&header, sizeof(header), 1, file) != 1 ||
	    memcmp(header.magic, PSVR2_CAPTURE_MAGIC, sizeof(header.magic)) != 0 ||
	    header.endian != 0x01020304 || header.relation_size != sizeof(struct xrt_space_relation) ||
	    header.record_size != sizeof(struct psvr2_capture_record) ||
	    header.sample_size != sizeof(struct psvr2_capture_sample)) {
		fprintf(stderr, "Invalid or incompatible PSVR2 prediction capture\n");
		fclose(file);
		return 1;
	}

	struct m_ff_vec3_f32 *gyro_ff = NULL;
	m_ff_vec3_f32_alloc(&gyro_ff, PSVR2_CAPTURE_MAX_SAMPLES);

	uint64_t records = 0;
	uint64_t explicit_records = 0;
	uint64_t divergent_records = 0;
	double max_mode_difference = 0.0;
	double max_record_difference = 0.0;
	struct prediction_difference_stats by_slam[2] = {0};
	struct replay_prediction replay[PSVR2_CAPTURE_MAX_RECORDS];
	struct timing_stats slam_to_gyro = {.min_ns = INT64_MAX};
	struct timing_stats gyro_to_target = {.min_ns = INT64_MAX};
	struct timing_stats slam_to_target = {.min_ns = INT64_MAX};
	int64_t previous_base_ns = 0;
	bool have_previous_base = false;
	for (;;) {
		struct psvr2_capture_record record;
		size_t got = fread(&record, 1, sizeof(record), file);
		if (got == 0 && feof(file)) {
			break;
		}
		if (got != sizeof(record) || record.sample_count == 0 || record.sample_count > PSVR2_CAPTURE_MAX_SAMPLES) {
			fprintf(stderr, "Truncated or invalid record at index %" PRIu64 "\n", records);
			m_ff_vec3_f32_free(&gyro_ff);
			fclose(file);
			return 1;
		}
		/* The FIFO has no reset operation: recreate it for each independent call. */
		m_ff_vec3_f32_free(&gyro_ff);
		m_ff_vec3_f32_alloc(&gyro_ff, PSVR2_CAPTURE_MAX_SAMPLES);
		for (uint32_t i = 0; i < record.sample_count; i++) {
			struct psvr2_capture_sample sample;
			if (fread(&sample, sizeof(sample), 1, file) != 1) {
				fprintf(stderr, "Truncated sample data at record index %" PRIu64 "\n", records);
				m_ff_vec3_f32_free(&gyro_ff);
				fclose(file);
				return 1;
			}
			m_ff_vec3_f32_push(gyro_ff, &sample.gyro, sample.timestamp_ns);
		}
		uint64_t newest_gyro_ns = 0;
		m_ff_vec3_f32_get_timestamp(gyro_ff, 0, &newest_gyro_ns);

		struct xrt_space_relation ordinary = XRT_SPACE_RELATION_ZERO;
		struct xrt_space_relation explicit = XRT_SPACE_RELATION_ZERO;
		psvr2_predict_mode(gyro_ff, NULL, NULL, record.target_ns, &record.base, record.base_ns, &ordinary, false);
		psvr2_predict_mode(gyro_ff, NULL, NULL, record.target_ns, &record.base, record.base_ns, &explicit, true);
		replay[records].record = record;
		replay[records].ordinary = ordinary;
		replay[records].explicit = explicit;
		replay[records].newest_gyro_ns = newest_gyro_ns;
		add_timing(&slam_to_gyro, (int64_t)newest_gyro_ns - record.base_ns);
		add_timing(&gyro_to_target, record.target_ns - (int64_t)newest_gyro_ns);
		add_timing(&slam_to_target, record.target_ns - record.base_ns);
		struct prediction_difference_stats mode_difference = prediction_difference(&ordinary, &explicit);
		double mode_max = mode_difference.max_orientation;
		if (mode_difference.max_position > mode_max) mode_max = mode_difference.max_position;
		if (mode_difference.max_linear_velocity > mode_max) mode_max = mode_difference.max_linear_velocity;
		if (mode_difference.max_angular_velocity > mode_max) mode_max = mode_difference.max_angular_velocity;
		struct xrt_space_relation *selected = record.explicit_integration ? &explicit : &ordinary;
		struct prediction_difference_stats selected_difference = prediction_difference(selected, &record.result);
		double selected_max = selected_difference.max_orientation;
		if (selected_difference.max_position > selected_max) {
			selected_max = selected_difference.max_position;
		}
		if (selected_difference.max_linear_velocity > selected_max) {
			selected_max = selected_difference.max_linear_velocity;
		}
		if (selected_difference.max_angular_velocity > selected_max) {
			selected_max = selected_difference.max_angular_velocity;
		}
		if (mode_max > max_mode_difference) {
			max_mode_difference = mode_max;
		}
		if (selected_max > max_record_difference) {
			max_record_difference = selected_max;
		}
		if (mode_max > 1e-7) {
			divergent_records++;
		}
		bool slam_advanced = have_previous_base && record.base_ns != previous_base_ns;
		accumulate_difference(&by_slam[slam_advanced ? 1 : 0], mode_difference);
		previous_base_ns = record.base_ns;
		have_previous_base = true;
		explicit_records += record.explicit_integration != 0;
		records++;
	}

	fclose(file);
	m_ff_vec3_f32_free(&gyro_ff);
	if (records == 0) {
		fprintf(stderr, "Capture contains no records\n");
		return 1;
	}
	fprintf(stdout,
	        "records=%" PRIu64 " explicit=%" PRIu64 " divergent=%" PRIu64
	        " max_mode_diff=%.9g max_record_diff=%.9g\n",
	        records, explicit_records, divergent_records, max_mode_difference, max_record_difference);
	print_difference_stats("same", &by_slam[0]);
	print_difference_stats("advanced", &by_slam[1]);
	print_timing("timing_slam_to_newest_gyro", &slam_to_gyro);
	print_timing("timing_newest_gyro_to_target", &gyro_to_target);
	print_timing("timing_slam_to_target", &slam_to_target);

	static const int64_t horizon_ns[] = {
	    5 * U_TIME_1MS_IN_NS,
	    10 * U_TIME_1MS_IN_NS,
	    20 * U_TIME_1MS_IN_NS,
	    30 * U_TIME_1MS_IN_NS,
	    40 * U_TIME_1MS_IN_NS,
	};
	static const char *horizon_names[] = {"0_5ms", "5_10ms", "10_20ms", "20_30ms", "30_40ms", "over_40ms"};
	struct forecast_stats forecast[2][6] = {0};
	uint64_t forecast_pairs = 0;
	for (uint64_t i = 0; i < records; i++) {
		const struct psvr2_capture_record *record = &replay[i].record;
		int64_t previous = -1;
		int64_t next = -1;
		for (uint64_t j = 0; j < records; j++) {
			int64_t base_ns = replay[j].record.base_ns;
			if (base_ns <= record->target_ns && (previous < 0 || base_ns > replay[previous].record.base_ns)) {
				previous = (int64_t)j;
			}
			if (base_ns > record->target_ns && (next < 0 || base_ns < replay[next].record.base_ns)) {
				next = (int64_t)j;
			}
		}
		if (previous < 0 || next < 0 || replay[next].record.base_ns <= replay[previous].record.base_ns) {
			continue;
		}
		float alpha = (float)((double)(record->target_ns - replay[previous].record.base_ns) /
		                     (double)(replay[next].record.base_ns - replay[previous].record.base_ns));
		struct xrt_pose reference;
		math_pose_interpolate(&replay[previous].record.base.pose, &replay[next].record.base.pose, alpha, &reference);
		int bin = 5;
		int64_t horizon = record->target_ns - record->base_ns;
		for (int h = 0; h < 5; h++) {
			if (horizon <= horizon_ns[h]) {
				bin = h;
				break;
			}
		}
		add_forecast_error(&forecast[0][bin], orientation_error_deg(&replay[i].ordinary.pose.orientation,
		                                                            &reference.orientation));
		add_forecast_error(&forecast[1][bin], orientation_error_deg(&replay[i].explicit.pose.orientation,
		                                                            &reference.orientation));
		forecast_pairs++;
	}
	fprintf(stdout, "self_reference_pairs=%" PRIu64 " (interpolated future SLAM pose; not ground truth)\n", forecast_pairs);
	for (size_t predictor = 0; predictor < 2; predictor++) {
		fprintf(stdout, "self_reference_predictor=%s\n", predictor == 0 ? "ordinary" : "explicit");
		for (size_t bin = 0; bin < 6; bin++) {
			const struct forecast_stats *stats = &forecast[predictor][bin];
			if (stats->count == 0) {
				continue;
			}
			double count = (double)stats->count;
			fprintf(stdout, "  horizon_%s count=%" PRIu64 " mean_deg=%.9g rms_deg=%.9g max_deg=%.9g\n",
			        horizon_names[bin],
			        stats->count, stats->sum_deg / count, sqrt(stats->sum_square_deg / count), stats->max_deg);
		}
	}
	return 0;
}
