// Copyright 2021, Collabora, Ltd.
// Copyright 2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Small utility for keeping track of the history of an xrt_space_relation, ie. for knowing where a HMD or
 *        controller was in the past, and predicting where it will be.
 * @author Moshi Turner <moshiturner@protonmail.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup aux_math
 */

#pragma once

#include "xrt/xrt_defines.h"

#include "os/os_threading.h"

#include "util/u_template_historybuf.hpp"
#include "util/u_logging.h"
#include "util/u_time.h"

#include "math/m_api.h"
#include "math/m_vec3.h"
#include "math/m_predict.h"

#include "m_relation_history.h"

#include <algorithm>
#include <mutex>
#include <optional>
#include <assert.h>
#include <stdint.h>


namespace xrt::auxiliary::math {

struct RelationHistoryEntry
{
	xrt_space_relation relation XRT_SPACE_RELATION_ZERO;
	int64_t timestamp{0};

	//! Predicts the relation history entry to the specified delta, returning the new one.
	RelationHistoryEntry
	predict(time_duration_ns delta_ns) const
	{
		double delta_s = time_ns_to_s(delta_ns);

		RelationHistoryEntry final = *this;
		final.timestamp += delta_ns;
		m_predict_relation(&this->relation, delta_s, &final.relation);

		return final;
	}

	//! Interpolates between the two relation history entries, returning a new one.
	RelationHistoryEntry
	interpolate(timepoint_ns to_ns, RelationHistoryEntry successor) const
	{
		RelationHistoryEntry final = *this;
		final.timestamp = to_ns;

		int64_t diff_before = to_ns - this->timestamp;
		int64_t diff_after = successor.timestamp - to_ns;

		float amount_to_lerp = (float)diff_before / (float)(diff_before + diff_after);

		auto &result_relation = final.relation;

		// Copy intersection of relation flags
		result_relation.relation_flags =
		    (enum xrt_space_relation_flags)(this->relation.relation_flags & successor.relation.relation_flags);

		// First-order implementation - lerp between the before and after
		if (result_relation.relation_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) {
			result_relation.pose.position =
			    m_vec3_lerp(this->relation.pose.position, successor.relation.pose.position, amount_to_lerp);
		}
		if (result_relation.relation_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) {

			math_quat_slerp(&this->relation.pose.orientation, &successor.relation.pose.orientation,
			                amount_to_lerp, &result_relation.pose.orientation);
		}

		//! @todo Does interpolating the velocities make any sense?
		if (result_relation.relation_flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT) {
			result_relation.angular_velocity = m_vec3_lerp(
			    this->relation.angular_velocity, successor.relation.angular_velocity, amount_to_lerp);
		}
		if (result_relation.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) {
			result_relation.linear_velocity = m_vec3_lerp(
			    this->relation.linear_velocity, successor.relation.linear_velocity, amount_to_lerp);
		}

		return final;
	}

	/*!
	 * Estimates motion from this relation history to the end entry, modifying the end entry with the new
	 * velocities.
	 */
	void
	estimateMotion(RelationHistoryEntry &in_out_end) const
	{
		const auto &old_relation = this->relation;
		const auto &new_relation = in_out_end.relation;

		xrt_vec3 &out_linear_velocity = in_out_end.relation.linear_velocity;
		xrt_vec3 &out_angular_velocity = in_out_end.relation.angular_velocity;
		enum xrt_space_relation_flags &out_flags = in_out_end.relation.relation_flags;

		double dt = time_ns_to_s(in_out_end.timestamp - this->timestamp);

		assert(dt != 0.0f);

		enum xrt_space_relation_flags shared_flags =
		    (enum xrt_space_relation_flags)(old_relation.relation_flags & new_relation.relation_flags);

		// Don't do anything if the new relation already has linear velocity
		if (new_relation.relation_flags & XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT) {
			out_flags =
			    (enum xrt_space_relation_flags)(out_flags | XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT);
			out_linear_velocity = new_relation.linear_velocity;
		}
		// If both relations have position data, estimate linear velocity
		else if (shared_flags & XRT_SPACE_RELATION_POSITION_VALID_BIT) {
			out_flags =
			    (enum xrt_space_relation_flags)(out_flags | XRT_SPACE_RELATION_LINEAR_VELOCITY_VALID_BIT);

			out_linear_velocity = (new_relation.pose.position - old_relation.pose.position) / dt;
		}

		// Don't do anything if the new relation already has angular velocity
		if (new_relation.relation_flags & XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT) {
			out_flags =
			    (enum xrt_space_relation_flags)(out_flags | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);
			out_angular_velocity = new_relation.angular_velocity;
		}
		// If both relations have orientation data, estimate angular velocity
		else if (shared_flags & XRT_SPACE_RELATION_ORIENTATION_VALID_BIT) {
			out_flags =
			    (enum xrt_space_relation_flags)(out_flags | XRT_SPACE_RELATION_ANGULAR_VELOCITY_VALID_BIT);

			math_quat_finite_difference(&old_relation.pose.orientation, &new_relation.pose.orientation, dt,
			                            &out_angular_velocity);
		}
	}
};

/*!
 * The base type for a relation history.
 *
 * T must be a type compatible with @ref RelationHistoryEntry (fields & functions).
 */
template <typename T> struct RelationHistoryBase
{
public: // Constants
	static constexpr size_t kBufLen = 4096;

public: // Fields
	util::HistoryBuffer<T, kBufLen> impl;
	mutable os::Mutex mutex;

public: // Methods (thread-unsafe)
	/*!
	 * Pushes a new entry (if newest) or updates an old one if allowed.
	 *
	 * Call with @ref mutex locked.
	 */
	bool
	pushOrUpdateLocked(const T &rhe, bool allow_update)
	{
		// if we aren't empty, we can compare against the latest timestamp.
		if (this->impl.empty() || rhe.timestamp > this->impl.back().timestamp) {
			/*
			 * Everything explodes if the timestamps in relation_history aren't monotonically increasing. If
			 * we get a timestamp that's before the most recent timestamp in the buffer, don't put it
			 * in the history.
			 */
			this->impl.push_back(rhe);
			return true;
		}

		// Don't do any updating if it's not enabled
		if (!allow_update) {
			return false;
		}

		for (auto it = this->impl.end(); it != this->impl.begin();) {
			--it;
			auto &item = *it;

			// Poses are monotonic, so if we're newer than this item than it will never reach
			if (rhe.timestamp > item.timestamp) {
				return false;
			}

			// If that timestamp is in here, update the pose
			if (item.timestamp == rhe.timestamp) {
				item = rhe;
				return true;
			}
		}

		return false;
	}

	/*!
	 * Gets the latest entry, or nullopt if none exists.
	 *
	 * Call with @ref mutex locked.
	 */
	std::optional<T>
	getLatestLocked() const
	{
		if (this->impl.empty()) {
			return std::nullopt;
		}

		return this->impl.back();
	}

	/*!
	 * Gets the device's relation at the specified timestamp. May predict or interpolate to get the closest value.
	 *
	 * Call with @ref mutex held.
	 */
	m_relation_history_result
	getLocked(timepoint_ns timestamp_ns, T &out_entry) const
	{
		if (this->impl.empty() || timestamp_ns == 0) {
			// Do nothing. You push nothing to the buffer you get nothing from the buffer.
			out_entry = {};
			return M_RELATION_HISTORY_RESULT_INVALID;
		}

		const auto b = this->impl.begin();
		const auto e = this->impl.end();

		// Find the first element *not less than* our value. the lambda we pass is the comparison
		// function, to compare against timestamps.
		const auto it = std::lower_bound(
		    b, e, timestamp_ns, [](const T &rhe, int64_t timestamp) { return rhe.timestamp < timestamp; });

		if (it == e) {
			// lower bound is at the end:
			// The desired timestamp is after what our buffer contains.
			// (pose-prediction)
			// Output flags match the most recent buffer entry.
			time_duration_ns delta_ns = timestamp_ns - this->impl.back().timestamp;
			double delta_s = time_ns_to_s(delta_ns);

			U_LOG_T("Extrapolating %f s past the back of the buffer!", delta_s);

			out_entry = this->impl.back().predict(delta_ns);
			return M_RELATION_HISTORY_RESULT_PREDICTED;
		}
		if (timestamp_ns == it->timestamp) {
			// exact match:
			// Flags copied directly along with everything else.
			U_LOG_T("Exact match in the buffer!");
			out_entry = *it;
			return M_RELATION_HISTORY_RESULT_EXACT;
		}
		if (it == b) {
			// lower bound is at the beginning (and it's not an exact match):
			// The desired timestamp is before what our buffer contains.
			// (an edge case where somebody asks for a really old pose and we do our best)
			// Output flags are the same as the input flags for the history entry we use
			time_duration_ns delta_ns = timestamp_ns - this->impl.front().timestamp;
			double delta_s = time_ns_to_s(delta_ns);
			U_LOG_T("Extrapolating %f s before the front of the buffer!", delta_s);
			out_entry = this->impl.front().predict(delta_ns);

			return M_RELATION_HISTORY_RESULT_REVERSE_PREDICTED;
		}
		U_LOG_T("Interpolating within buffer!");

		// We precede *it and follow *(it - 1) (which we know exists because we already handled
		// the it = begin() case)
		const auto &predecessor = *(it - 1);
		const auto &successor = *it;

		// Do the thing.
		out_entry = predecessor.interpolate(timestamp_ns, successor);
		return M_RELATION_HISTORY_RESULT_INTERPOLATED;
	}

	/*!
	 * Gets an exact entry, if it exists.
	 *
	 * Call with @ref mutex held.
	 */
	std::optional<T>
	getExactLocked(timepoint_ns timestamp_ns) const
	{
		if (this->impl.empty()) {
			return std::nullopt;
		}

		const auto b = this->impl.begin();
		const auto e = this->impl.end();

		const auto it = std::lower_bound(
		    b, e, timestamp_ns, [](const T &rhe, int64_t timestamp) { return rhe.timestamp < timestamp; });

		if (it == e || it->timestamp != timestamp_ns) {
			return std::nullopt;
		}

		return *it;
	}

	/*!
	 * Gets the stored entry nearest to the given timestamp, if one exists within @p max_slop_ns of it.
	 *
	 * Never interpolates or extrapolates, the returned entry is exactly as stored (check its timestamp to see
	 * how far off it is). If two entries are equally close, the older one is returned.
	 *
	 * Call with @ref mutex held.
	 */
	std::optional<T>
	getNearestLocked(timepoint_ns timestamp_ns, time_duration_ns max_slop_ns) const
	{
		if (this->impl.empty() || max_slop_ns < 0) {
			return std::nullopt;
		}

		const auto b = this->impl.begin();
		const auto e = this->impl.end();

		// First element not less than our timestamp, the nearest is either it or its predecessor.
		const auto it = std::lower_bound(
		    b, e, timestamp_ns, [](const T &rhe, int64_t timestamp) { return rhe.timestamp < timestamp; });

		std::optional<T> best = std::nullopt;
		time_duration_ns best_diff = max_slop_ns;

		if (it != b) {
			const auto &before = *(it - 1);
			time_duration_ns diff = timestamp_ns - before.timestamp;
			if (diff <= best_diff) {
				best = before;
				best_diff = diff;
			}
		}

		if (it != e) {
			const auto &after = *it;
			time_duration_ns diff = after.timestamp - timestamp_ns;
			// Strictly less, so ties go to the older entry.
			if (diff < best_diff || (!best.has_value() && diff <= best_diff)) {
				best = after;
			}
		}

		return best;
	}

public: // Methods (thread-safe)
	/*!
	 * @copydoc pushOrUpdateLocked
	 *
	 * Thread safe, holds @ref mutex.
	 */
	bool
	pushOrUpdate(const T &entry, bool allow_update)
	{
		std::unique_lock<os::Mutex> lock(this->mutex);
		return this->pushOrUpdateLocked(entry, allow_update);
	}

	/*!
	 * @copydoc getLatestLocked
	 *
	 * Thread safe, holds @ref mutex.
	 */
	std::optional<T>
	getLatest() const
	{
		std::unique_lock<os::Mutex> lock(this->mutex);
		return this->getLatestLocked();
	}

	/*!
	 * Pushes a new entry, doing motion estimation if required.
	 *
	 * Thread safe, holds @ref mutex.
	 */
	bool
	pushWithMotionEstimation(const T &new_entry, bool allow_update)
	{
		std::unique_lock<os::Mutex> lock(this->mutex);
		const auto begin_entry = this->getLatestLocked();

		T end_entry = new_entry;
		if (begin_entry.has_value() && end_entry.timestamp > begin_entry->timestamp) {
			begin_entry->estimateMotion(end_entry);
		}

		return this->pushOrUpdateLocked(end_entry, allow_update);
	}

	/*!
	 * Gets the amount of entries contained within the history.
	 *
	 * Thread safe, holds @ref mutex.
	 */
	uint32_t
	size() const
	{
		std::unique_lock<os::Mutex> lock(this->mutex);
		return static_cast<uint32_t>(this->impl.size());
	}

	/*!
	 * Clears the relation history.
	 *
	 * Thread safe, holds @ref mutex.
	 */
	void
	clear()
	{
		std::unique_lock<os::Mutex> lock(this->mutex);
		this->impl.clear();
	}

	/*!
	 * @copydoc getLocked
	 *
	 * Thread safe, holds @ref mutex.
	 */
	m_relation_history_result
	get(timepoint_ns timestamp_ns, T &out_entry) const
	{
		std::unique_lock<os::Mutex> lock(this->mutex);
		return this->getLocked(timestamp_ns, out_entry);
	}

	/*!
	 * @copydoc getExactLocked
	 *
	 * Thread safe, holds @ref mutex.
	 */
	std::optional<T>
	getExact(timepoint_ns timestamp_ns) const
	{
		std::unique_lock<os::Mutex> lock(this->mutex);
		return this->getExactLocked(timestamp_ns);
	}

	/*!
	 * @copydoc getNearestLocked
	 *
	 * Thread safe, holds @ref mutex.
	 */
	std::optional<T>
	getNearest(timepoint_ns timestamp_ns, time_duration_ns max_slop_ns) const
	{
		std::unique_lock<os::Mutex> lock(this->mutex);
		return this->getNearestLocked(timestamp_ns, max_slop_ns);
	}
};

using RelationHistory = RelationHistoryBase<RelationHistoryEntry>;

}; // namespace xrt::auxiliary::math
