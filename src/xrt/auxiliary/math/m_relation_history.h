// Copyright 2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Small utility for keeping track of the history of an xrt_space_relation, ie. for knowing where a HMD or
 * controller was in the past
 * @author Moshi Turner <moshiturner@protonmail.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup aux_util
 */
#pragma once

#include "xrt/xrt_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque type for storing the history of a space relation in a ring buffer
 *
 * @note Unlike the bare C++ data structure @ref HistoryBuffer this wraps, **this is a thread safe interface**,
 * and is safe for concurrent access from multiple threads.
 * (It is using a simple mutex, not a reader/writer lock, but that is fine until proven to be a bottleneck.)
 *
 * @ingroup aux_util
 */
struct m_relation_history;

/**
 * @brief Describes how the resulting space relation for the desired time stamp was generated.
 *
 * @relates m_relation_history
 */
enum m_relation_history_result
{
	M_RELATION_HISTORY_RESULT_INVALID = 0,       //!< The supplied timestamp was invalid (0) or buffer was empty
	M_RELATION_HISTORY_RESULT_EXACT,             //!< The exact desired timestamp was found
	M_RELATION_HISTORY_RESULT_INTERPOLATED,      //!< The desired timestamp was between two entries
	M_RELATION_HISTORY_RESULT_PREDICTED,         //!< The desired timestamp was newer than the most recent entry
	M_RELATION_HISTORY_RESULT_REVERSE_PREDICTED, //!< The desired timestamp was older than the oldest entry
};

/*!
 * Creates an opaque relation_history object.
 *
 * @public @memberof m_relation_history
 */
void
m_relation_history_create(struct m_relation_history **rh);

/*!
 * Pushes a new pose to the history.
 *
 * If the history is full, it will also pop a pose out of the other side of the buffer.
 *
 * @return false if the timestamp is earlier than the most recent timestamp already recorded
 *
 * @public @memberof m_relation_history
 */
bool
m_relation_history_push(struct m_relation_history *rh, struct xrt_space_relation const *in_relation, int64_t timestamp);

/*!
 * Pushes a new pose to the history, or updates an existing one.
 *
 * If the history is full, it will also pop a pose out of the other side of the buffer.
 *
 * @return false if the timestamp is not new or doesn't match an older timestamp
 *
 * @public @memberof m_relation_history
 */
bool
m_relation_history_push_or_update(struct m_relation_history *rh,
                                  struct xrt_space_relation const *in_relation,
                                  int64_t timestamp);

/*!
 * Pushes a new pose to the history, estimating linear and angular velocity based on the previous entry if the input
 * relation does not have valid velocity data.
 *
 * If the history is full, it will also pop a pose out of the other side of the buffer.
 *
 * @return false if the timestamp is earlier than the most recent timestamp already recorded
 *
 * @public @memberof m_relation_history
 */
bool
m_relation_history_push_with_motion_estimation(struct m_relation_history *rh,
                                               struct xrt_space_relation const *in_relation,
                                               int64_t timestamp);

/*!
 * Interpolates or extrapolates to the desired timestamp.
 *
 * Read-only operation - doesn't remove anything from the buffer or anything like that - you can call this as often
 * as you want.
 *
 * @public @memberof m_relation_history
 */
enum m_relation_history_result
m_relation_history_get(const struct m_relation_history *rh,
                       int64_t at_timestamp_ns,
                       struct xrt_space_relation *out_relation);

/*!
 * Get the latest report in the buffer, if any.
 *
 * @param rh self
 * @param[out] out_time_ns Populated with the latest report time, if any
 * @param[out] out_relation Populated with the latest relation, if any
 *
 * @return false if the history is empty.
 *
 * @public @memberof m_relation_history
 */
bool
m_relation_history_get_latest(const struct m_relation_history *rh,
                              int64_t *out_time_ns,
                              struct xrt_space_relation *out_relation);

/*!
 * Get the relation stored at exactly the given timestamp, if any.
 *
 * Unlike @ref m_relation_history_get, this never interpolates or extrapolates, and fails if there is no entry with
 * precisely this timestamp.
 *
 * @param rh self
 * @param at_timestamp_ns The exact timestamp to look up
 * @param[out] out_relation Populated with the stored relation, if found
 *
 * @return false if there is no entry at that timestamp.
 *
 * @public @memberof m_relation_history
 */
bool
m_relation_history_get_exact(const struct m_relation_history *rh,
                             int64_t at_timestamp_ns,
                             struct xrt_space_relation *out_relation);

/*!
 * Get the stored relation nearest to the given timestamp, if one is within @p max_slop_ns of it.
 *
 * Like @ref m_relation_history_get_exact, this never interpolates or extrapolates, but accepts entries that are up
 * to @p max_slop_ns away (inclusive) from the requested timestamp. If two entries are equally close, the older one
 * is returned.
 *
 * @param rh self
 * @param at_timestamp_ns The timestamp to look up
 * @param max_slop_ns Maximum allowed distance between @p at_timestamp_ns and the found entry, must be >= 0
 * @param[out] out_time_ns Populated with the found entry's timestamp, if found
 * @param[out] out_relation Populated with the stored relation, if found
 *
 * @return false if there is no entry within @p max_slop_ns of the timestamp.
 *
 * @public @memberof m_relation_history
 */
bool
m_relation_history_get_nearest(const struct m_relation_history *rh,
                               int64_t at_timestamp_ns,
                               int64_t max_slop_ns,
                               int64_t *out_time_ns,
                               struct xrt_space_relation *out_relation);

/*!
 * Returns the number of items in the history.
 *
 * @public @memberof m_relation_history
 */
uint32_t
m_relation_history_get_size(const struct m_relation_history *rh);

/*!
 * Clears the history from all of the items.
 *
 * @public @memberof m_relation_history
 */
void
m_relation_history_clear(struct m_relation_history *rh);

/*!
 * Destroys an opaque relation_history object.
 *
 * @public @memberof m_relation_history
 */
void
m_relation_history_destroy(struct m_relation_history **rh);

#ifdef __cplusplus
}
#endif
