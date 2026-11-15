// Copyright 2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Small utility for keeping track of the history of an xrt_space_relation, ie. for knowing where a HMD or
 * controller was in the past.
 * @author Moshi Turner <moshiturner@protonmail.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup aux_math
 */

#include "xrt/xrt_defines.h"

#include "util/u_logging.h"
#include "util/u_trace_marker.h"

#include "glue/g_catch_guard.hpp"

#include "m_relation_history.hpp"

#include <memory>
#include <stdint.h>


using namespace xrt::auxiliary::math;

static RelationHistory *
from_mrh(m_relation_history *mrh)
{
	return reinterpret_cast<RelationHistory *>(mrh);
}

static const RelationHistory *
from_mrh(const m_relation_history *mrh)
{
	return reinterpret_cast<const RelationHistory *>(mrh);
}

/*
 *
 * 'Exported' functions.
 *
 */

void
m_relation_history_create(m_relation_history **rh_ptr)
{
	auto ret = std::make_unique<RelationHistory>();

	*rh_ptr = reinterpret_cast<m_relation_history *>(ret.release());
}

bool
m_relation_history_push(m_relation_history *mrh, xrt_space_relation const *in_relation, int64_t timestamp)
try {
	XRT_TRACE_MARKER();
	RelationHistoryEntry rhe;
	rhe.relation = *in_relation;
	rhe.timestamp = timestamp;

	auto rh = from_mrh(mrh);
	return rh->pushOrUpdate(rhe, false);
}
G_CATCH_GUARDS_WITH_RETURN(false)

bool
m_relation_history_push_or_update(m_relation_history *mrh, xrt_space_relation const *in_relation, int64_t timestamp)
try {
	XRT_TRACE_MARKER();
	RelationHistoryEntry rhe;
	rhe.relation = *in_relation;
	rhe.timestamp = timestamp;

	auto rh = from_mrh(mrh);
	return rh->pushOrUpdate(rhe, true);
}
G_CATCH_GUARDS_WITH_RETURN(false)

enum m_relation_history_result
m_relation_history_get(const m_relation_history *mrh, int64_t at_timestamp_ns, xrt_space_relation *out_relation)
try {
	XRT_TRACE_MARKER();

	auto rh = from_mrh(mrh);

	RelationHistoryEntry entry{};
	auto result = rh->get(at_timestamp_ns, entry);
	*out_relation = entry.relation;

	return result;
}
G_CATCH_GUARDS_WITH_RETURN(M_RELATION_HISTORY_RESULT_INVALID)

bool
m_relation_history_push_with_motion_estimation(m_relation_history *mrh,
                                               xrt_space_relation const *in_relation,
                                               int64_t timestamp)
try {
	auto rh = from_mrh(mrh);

	RelationHistoryEntry final = {
	    .relation = *in_relation,
	    .timestamp = timestamp,
	};

	return rh->pushWithMotionEstimation(final, false);
}
G_CATCH_GUARDS_WITH_RETURN(false)

bool
m_relation_history_get_latest(const m_relation_history *mrh, int64_t *out_time_ns, xrt_space_relation *out_relation)
try {
	auto rh = from_mrh(mrh);

	auto entry = rh->getLatest();
	if (!entry.has_value()) {
		return false;
	}

	*out_relation = entry->relation;
	*out_time_ns = entry->timestamp;

	return true;
}
G_CATCH_GUARDS_WITH_RETURN(false)

bool
m_relation_history_get_exact(const m_relation_history *mrh, int64_t at_timestamp_ns, xrt_space_relation *out_relation)
try {
	auto rh = from_mrh(mrh);

	auto entry = rh->getExact(at_timestamp_ns);
	if (!entry.has_value()) {
		return false;
	}

	*out_relation = entry->relation;

	return true;
}
G_CATCH_GUARDS_WITH_RETURN(false)

bool
m_relation_history_get_nearest(const m_relation_history *mrh,
                               int64_t at_timestamp_ns,
                               int64_t max_slop_ns,
                               int64_t *out_time_ns,
                               xrt_space_relation *out_relation)
try {
	auto rh = from_mrh(mrh);

	auto entry = rh->getNearest(at_timestamp_ns, max_slop_ns);
	if (!entry.has_value()) {
		return false;
	}

	*out_relation = entry->relation;
	*out_time_ns = entry->timestamp;

	return true;
}
G_CATCH_GUARDS_WITH_RETURN(false)

uint32_t
m_relation_history_get_size(const m_relation_history *mrh)
try {
	auto rh = from_mrh(mrh);

	return rh->size();
}
G_CATCH_GUARDS_WITH_RETURN(0)

void
m_relation_history_clear(m_relation_history *mrh)
try {
	auto rh = from_mrh(mrh);

	rh->clear();
}
G_CATCH_GUARDS_WITH_RETURN()

void
m_relation_history_destroy(m_relation_history **mrh_ptr)
try {
	auto *rh = from_mrh(*mrh_ptr);
	if (rh == NULL) {
		// Do nothing, it's likely already been destroyed
		return;
	}

	*mrh_ptr = NULL;

	delete rh;
}
G_CATCH_GUARDS_WITH_RETURN()
