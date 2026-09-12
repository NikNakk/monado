// Copyright 2020 Jan Schmidt
// Copyright 2026 Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Ab-initio blob<->LED correspondence search
 * @author Jan Schmidt <jan@centricular.com>
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup tracking
 */

#pragma once

#include "tracking/t_constellation.h"

#include "pose_metrics.h"
#include "camera_model.h"


#ifdef __cplusplus
extern "C" {
#endif

#define MAX_BLOB_SEARCH_DEPTH 5

enum correspondence_search_flags
{
	CS_FLAG_NONE = 0x0,
	//! Do quick search @ depth 1-2 neighbour depth
	CS_FLAG_SHALLOW_SEARCH = 0x1,
	//! do deeper searches @ up to MAX_LED_SEARCH_DEPTH/MAX_BLOB_SEARCH_DEPTH
	CS_FLAG_DEEP_SEARCH = 0x2,
	//! Stop searching if a strong match is found, otherwise search all and return best match
	CS_FLAG_STOP_FOR_STRONG_MATCH = 0x4,
	//! Allow matching against all blobs, not just unlabelled ones or for the current device
	CS_FLAG_MATCH_ALL_BLOBS = 0x8,
	//! If the input obj_cam_pose contains a valid prior
	CS_FLAG_HAVE_POSE_PRIOR = 0x10,
	//! Use the input pose orientation and provided gravity vector to check pose verticality.
	CS_FLAG_MATCH_GRAVITY = 0x20,
};

struct cs_image_point
{
	struct t_blob *blob;

	double point_homog[3]; // Homogeneous version of the point
	double size[2];        // w/h of the blob, in homogeneous coordinates
	double max_dist;       // norm of (W/H) for distance checks

	/* List of the nearest blobs, filtered for the active model */
	int num_neighbours;
	struct cs_image_point *neighbours[MAX_BLOB_SEARCH_DEPTH];
};

struct cs_model_info
{
	t_constellation_device_id_t id;

	struct t_constellation_search_model *model;

	double best_pose_found_time; /* Time (in secs) at which the best pose was found */
	int best_pose_blob_depth;    /* Blob neighbor depth the best pose is from */
	int best_pose_led_depth;     /* LED neighbour depth the best pose is from */
	struct xrt_pose best_pose;
	float best_pose_gravity_error_rad;
	enum pose_match_flags match_flags;

	struct pose_metrics best_score;

	/* Search parameters */
	double search_start_time;
	int led_depth;
	int led_index;
	int blob_index;

	enum correspondence_search_flags search_flags;
	int min_led_depth, max_led_depth;
	int max_blob_depth;

	/* Full pose prior fields are valid when CS_FLAG_HAVE_POSE_PRIOR is set. */
	/* pose_prior.orientation is also valid when CS_FLAG_MATCH_GRAVITY is set. */
	struct xrt_pose pose_prior;
	struct xrt_vec3 *pos_error_thresh;
	struct xrt_vec3 *rot_error_thresh;

	/* Used when CS_FLAG_MATCH_GRAVITY is set */
	struct xrt_vec3 gravity_vector;
	struct xrt_quat gravity_swing;
	float gravity_tolerance_rad;

	/* Diagnostic counters for orientation/gravity-constrained bootstrap searches. */
	struct
	{
		uint32_t p3p_solutions;
		uint32_t gravity_checked;
		uint32_t within_30_deg;
		uint32_t within_45_deg;
		uint32_t within_60_deg;
		uint32_t passed_active_tolerance;
		uint32_t passed_gravity_failed_scoring;
		float min_error_rad;
		uint32_t best_failed_matched_blobs;
		uint32_t best_failed_visible_leds;
		float best_failed_reprojection_error;
	} gravity_diag;

	/* Detailed diagnostics for hypotheses which pass the active gravity tolerance. */
	struct
	{
		bool enabled;
		uint32_t evaluated;
		uint32_t matched_lt3;
		uint32_t matched_lt5;
		uint32_t visible_lt5;
		uint32_t reprojection_fail;
		uint32_t coverage_fail;
		uint32_t shape_ok;
		uint32_t shape_reprojection_ok;
		uint32_t shape_reprojection_coverage_ok;
		uint32_t prior_match;
		uint32_t good;

		bool have_closest_gravity_pose;
		float closest_gravity_error_rad;
		struct xrt_pose closest_gravity_pose;

		bool have_best_failed_pose;
		float best_failed_gravity_error_rad;
		struct xrt_pose best_failed_pose;
		struct pose_metrics best_failed_score;
	} score_diag;
};

struct correspondence_search
{
	const enum u_logging_level *ct_log_level;

	int num_points;
	struct cs_image_point *points;
	struct t_blob *blobs; /* Original blobs structs [num_points] */

	unsigned int num_trials;
	unsigned int num_pose_checks;

	struct camera_model *calib;

	/* List of the nearest blobs for each blob */
	struct cs_image_point
	    *blob_neighbours[XRT_CONSTELLATION_MAX_BLOBS_PER_FRAME][XRT_CONSTELLATION_MAX_BLOBS_PER_FRAME];
};

struct correspondence_search *
correspondence_search_new(const enum u_logging_level *ct_log_level, struct camera_model *camera_calib);

void
correspondence_search_free(struct correspondence_search *cs);

void
correspondence_search_set_blobs(struct correspondence_search *cs, struct t_blob *blobs, int num_blobs);

bool
correspondence_search_find_one_pose(struct correspondence_search *cs,
                                    struct t_constellation_search_model *model,
                                    enum correspondence_search_flags search_flags,
                                    struct xrt_pose *pose,
                                    struct xrt_vec3 *pos_error_thresh,
                                    struct xrt_vec3 *rot_error_thresh,
                                    struct xrt_vec3 *gravity_vector,
                                    float gravity_tolerance_rad,
                                    struct pose_metrics *score);

#ifdef __cplusplus
}
#endif
