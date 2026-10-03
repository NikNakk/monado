// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS PS VR2 diagnostic mapping from system compositor frames to client frames.
 *
 * Included by comp_multi_system.c on Apple builds only. The latch trace leaves
 * the normal multi-compositor latch behaviour unchanged and records which delivered
 * client frame supplied each system-compositor refresh. Joining system_frame_id to
 * late_render.csv and present.csv/timeline_value lets us verify asynchronous
 * 60 -> 120 Hz reprojection directly rather than infer it from cadence.
 */

#pragma once

#include "xrt/xrt_session.h"
#include "multi/comp_multi_private.h"
#include "os/os_time.h"
#include "util/u_debug.h"
#include "util/u_timing_trace.h"

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

DEBUG_GET_ONCE_BOOL_OPTION(macos_client_frame_trace, "PSVR2_TIMING_TRACE", false)
DEBUG_GET_ONCE_NUM_OPTION(macos_client_frame_min_hold, "XRT_MACOS_CLIENT_FRAME_MIN_HOLD", 0)
DEBUG_GET_ONCE_BOOL_OPTION(macos_compositor_time_constraint, "XRT_MACOS_COMPOSITOR_TIME_CONSTRAINT", true)
DEBUG_GET_ONCE_NUM_OPTION(macos_compositor_computation_pct, "XRT_MACOS_COMPOSITOR_COMPUTATION_PCT", 35)
DEBUG_GET_ONCE_NUM_OPTION(macos_compositor_constraint_pct, "XRT_MACOS_COMPOSITOR_CONSTRAINT_PCT", 70)

static FILE *g_macos_client_frame_trace = NULL;
static uint64_t g_macos_client_frame_trace_rows = 0;
static bool g_macos_client_frame_trace_failed = false;
static bool g_macos_client_frame_trace_atexit_registered = false;
static bool g_macos_client_frame_trace_have_last[MULTI_MAX_CLIENTS];
static int64_t g_macos_client_frame_trace_last_frame[MULTI_MAX_CLIENTS];
static uint32_t g_macos_client_frame_trace_source_use_ordinal[MULTI_MAX_CLIENTS];
// Mach scheduling policy belongs to each compositor thread. A replacement
// hosted compositor must apply it even when the display period is unchanged.
static _Thread_local int64_t g_macos_compositor_time_constraint_period_ns = 0;

/* Elastic minimum-hold state, indexed by multi-system client slot. */
static bool g_macos_client_frame_hold_initialized[MULTI_MAX_CLIENTS];
static int64_t g_macos_client_frame_hold_frame_id[MULTI_MAX_CLIENTS];
static int64_t g_macos_client_frame_hold_first_system_frame[MULTI_MAX_CLIENTS];

static int
macos_client_frame_min_hold(void)
{
	int hold = debug_get_num_option_macos_client_frame_min_hold();
	if (hold <= 1) {
		return 0;
	}
	if (hold > 16) {
		hold = 16;
	}
	return hold;
}

static size_t
macos_client_slot_for_mc(struct multi_compositor *mc)
{
	if (mc == NULL || mc->msc == NULL) {
		return MULTI_MAX_CLIENTS;
	}
	for (size_t i = 0; i < MULTI_MAX_CLIENTS; i++) {
		if (mc->msc->clients[i] == mc) {
			return i;
		}
	}
	return MULTI_MAX_CLIENTS;
}

static void
macos_client_frame_trace_close(void)
{
	if (g_macos_client_frame_trace == NULL) {
		return;
	}
	fflush(g_macos_client_frame_trace);
	fclose(g_macos_client_frame_trace);
	g_macos_client_frame_trace = NULL;
}

static FILE *
macos_client_frame_trace_get(void)
{
	if (!debug_get_bool_option_macos_client_frame_trace() || g_macos_client_frame_trace_failed) {
		return NULL;
	}
	if (g_macos_client_frame_trace != NULL) {
		return g_macos_client_frame_trace;
	}

	g_macos_client_frame_trace = u_timing_trace_open("client_frame_map", 64u * 1024u);
	if (g_macos_client_frame_trace == NULL) {
		g_macos_client_frame_trace_failed = true;
		return NULL;
	}


	fputs(
	    "system_frame_id,system_display_time_ns,latch_ns,client_slot,client_frame_id,"
	    "client_display_time_ns,display_time_delta_ns,reused,source_use_ordinal,layer_count,focused,visible\n",
	    g_macos_client_frame_trace);

	if (!g_macos_client_frame_trace_atexit_registered) {
		atexit(macos_client_frame_trace_close);
		g_macos_client_frame_trace_atexit_registered = true;
	}

	return g_macos_client_frame_trace;
}

/*
 * Optional source-cadence stabiliser. It does not alter the application's
 * xrWaitFrame pacing or the system compositor's physical cadence.
 *
 * XRT_MACOS_CLIENT_FRAME_MIN_HOLD=2 (elastic minimum hold): a
 * newly delivered client frame must remain delivered for at least two system
 * compositor ticks. Once that minimum has elapsed, the next GPU-complete frame
 * is accepted immediately, so a late 60 Hz source frame produces a 3-refresh
 * hold and shifts phase instead of being forced to wait for a fixed even/odd
 * boundary and becoming a 4-refresh hold.
 */
static inline void
macos_deliver_client_frame_cadenced(struct multi_compositor *mc, int64_t display_time_ns, int64_t system_frame_id)
{
	int min_hold = macos_client_frame_min_hold();
	if (min_hold != 0 && mc != NULL) {
		size_t client_slot = macos_client_slot_for_mc(mc);
		if (client_slot == MULTI_MAX_CLIENTS) {
			multi_compositor_deliver_any_frames(mc, display_time_ns);
			return;
		}

		bool active_before = mc->delivered.active;
		int64_t frame_before = active_before ? mc->delivered.data.frame_id : -1;

		if (!g_macos_client_frame_hold_initialized[client_slot]) {
			multi_compositor_deliver_any_frames(mc, display_time_ns);
			if (mc->delivered.active) {
				g_macos_client_frame_hold_initialized[client_slot] = true;
				g_macos_client_frame_hold_frame_id[client_slot] = mc->delivered.data.frame_id;
				g_macos_client_frame_hold_first_system_frame[client_slot] = system_frame_id;
			}
			return;
		}

		/* Recover cleanly if delivered changed outside this diagnostic wrapper. */
		if (active_before && frame_before != g_macos_client_frame_hold_frame_id[client_slot]) {
			g_macos_client_frame_hold_frame_id[client_slot] = frame_before;
			g_macos_client_frame_hold_first_system_frame[client_slot] = system_frame_id;
		}

		int64_t first_system_frame = g_macos_client_frame_hold_first_system_frame[client_slot];
		if (!active_before || system_frame_id - first_system_frame >= min_hold) {
			multi_compositor_deliver_any_frames(mc, display_time_ns);
			if (mc->delivered.active && (!active_before || mc->delivered.data.frame_id != frame_before)) {
				g_macos_client_frame_hold_frame_id[client_slot] = mc->delivered.data.frame_id;
				g_macos_client_frame_hold_first_system_frame[client_slot] = system_frame_id;
			}
		}
		return;
	}

	multi_compositor_deliver_any_frames(mc, display_time_ns);
}

static inline void
macos_trace_multi_compositor_latch_frame_locked(struct multi_compositor *mc,
                                                int64_t when_ns,
                                                int64_t system_frame_id,
                                                int64_t system_display_time_ns)
{
	/* Preserve the real compositor behaviour exactly. */
	multi_compositor_latch_frame_locked(mc, when_ns, system_frame_id);

	FILE *file = macos_client_frame_trace_get();
	if (file == NULL || mc == NULL || mc->msc == NULL || !mc->delivered.active) {
		return;
	}

	size_t client_slot = macos_client_slot_for_mc(mc);
	if (client_slot == MULTI_MAX_CLIENTS) {
		return;
	}

	int64_t client_frame_id = mc->delivered.data.frame_id;
	bool reused = g_macos_client_frame_trace_have_last[client_slot] &&
	              g_macos_client_frame_trace_last_frame[client_slot] == client_frame_id;
	if (reused) {
		if (g_macos_client_frame_trace_source_use_ordinal[client_slot] < UINT32_MAX) {
			g_macos_client_frame_trace_source_use_ordinal[client_slot]++;
		}
	} else {
		g_macos_client_frame_trace_source_use_ordinal[client_slot] = 1;
	}
	g_macos_client_frame_trace_have_last[client_slot] = true;
	g_macos_client_frame_trace_last_frame[client_slot] = client_frame_id;

	int64_t client_display_time_ns = mc->delivered.data.display_time_ns;
	int64_t display_time_delta_ns = system_display_time_ns - client_display_time_ns;

	flockfile(file);
	fprintf(file, "%lld,%lld,%lld,%zu,%lld,%lld,%lld,%u,%u,%u,%u,%u\n", (long long)system_frame_id,
	        (long long)system_display_time_ns, (long long)when_ns, client_slot, (long long)client_frame_id,
	        (long long)client_display_time_ns, (long long)display_time_delta_ns, reused ? 1u : 0u,
	        g_macos_client_frame_trace_source_use_ordinal[client_slot], mc->delivered.layer_count,
	        mc->state.focused ? 1u : 0u, mc->state.visible ? 1u : 0u);
	g_macos_client_frame_trace_rows++;

	if (!u_timing_trace_fully_buffered() && g_macos_client_frame_trace_rows % 512 == 0) {
		fflush(file);
	}
	funlockfile(file);
}

static inline uint32_t
macos_compositor_ns_to_mach_ticks(uint64_t ns, const mach_timebase_info_data_t *timebase)
{
	__uint128_t ticks = (__uint128_t)ns * (__uint128_t)timebase->denom / (__uint128_t)timebase->numer;
	return ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks;
}

/*
 * Apply the Mach time-constraint policy to the Multi Client Module thread using
 * the period Monado just predicted for the physical display. The computation
 * and constraint budgets are percentages of that period so switching between
 * 120 Hz and 90 Hz automatically retunes the scheduler policy with no second
 * source of truth for the refresh rate.
 */
static inline void
macos_compositor_update_time_constraint(int64_t period_ns)
{
	if (!debug_get_bool_option_macos_compositor_time_constraint() || period_ns <= 0) {
		return;
	}

	if (g_macos_compositor_time_constraint_period_ns == period_ns) {
		return;
	}

	int computation_pct = debug_get_num_option_macos_compositor_computation_pct();
	int constraint_pct = debug_get_num_option_macos_compositor_constraint_pct();
	if (computation_pct <= 0 || constraint_pct <= 0 || computation_pct > constraint_pct || constraint_pct > 100) {
		U_LOG_W(
		    "macOS diagnostic: invalid compositor time constraint percentages: computation_pct=%d "
		    "constraint_pct=%d",
		    computation_pct, constraint_pct);
		g_macos_compositor_time_constraint_period_ns = period_ns;
		return;
	}

	uint64_t computation_ns = ((uint64_t)period_ns * (uint64_t)computation_pct) / 100u;
	uint64_t constraint_ns = ((uint64_t)period_ns * (uint64_t)constraint_pct) / 100u;

	mach_timebase_info_data_t timebase = {0};
	kern_return_t kr = mach_timebase_info(&timebase);
	if (kr != KERN_SUCCESS || timebase.numer == 0 || timebase.denom == 0) {
		U_LOG_W("macOS diagnostic: mach_timebase_info failed for compositor time constraint: %d", kr);
		g_macos_compositor_time_constraint_period_ns = period_ns;
		return;
	}

	thread_time_constraint_policy_data_t policy = {
	    .period = macos_compositor_ns_to_mach_ticks((uint64_t)period_ns, &timebase),
	    .computation = macos_compositor_ns_to_mach_ticks(computation_ns, &timebase),
	    .constraint = macos_compositor_ns_to_mach_ticks(constraint_ns, &timebase),
	    .preemptible = TRUE,
	};

	thread_t thread = mach_thread_self();
	kr = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy,
	                       THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	mach_port_deallocate(mach_task_self(), thread);
	g_macos_compositor_time_constraint_period_ns = period_ns;

	double refresh_hz = 1000000000.0 / (double)period_ns;
	if (kr == KERN_SUCCESS) {
		U_LOG_I(
		    "macOS diagnostic: Multi Client Module time constraint updated: refresh_hz=%.3f "
		    "period_ns=%lld computation_ns=%llu constraint_ns=%llu computation_pct=%d constraint_pct=%d",
		    refresh_hz, (long long)period_ns, (unsigned long long)computation_ns,
		    (unsigned long long)constraint_ns, computation_pct, constraint_pct);
	} else {
		U_LOG_W(
		    "macOS diagnostic: failed to set Multi Client Module THREAD_TIME_CONSTRAINT_POLICY: kr=%d "
		    "refresh_hz=%.3f period_ns=%lld computation_ns=%llu constraint_ns=%llu",
		    kr, refresh_hz, (long long)period_ns, (unsigned long long)computation_ns,
		    (unsigned long long)constraint_ns);
	}
}

static inline void
macos_xrt_comp_predict_frame_with_time_constraint(struct xrt_compositor *xc,
                                                  int64_t *out_frame_id,
                                                  int64_t *out_wake_up_time_ns,
                                                  int64_t *out_predicted_gpu_time_ns,
                                                  int64_t *out_predicted_display_time_ns,
                                                  int64_t *out_predicted_display_period_ns)
{
	xrt_comp_predict_frame(xc, out_frame_id, out_wake_up_time_ns, out_predicted_gpu_time_ns,
	                       out_predicted_display_time_ns, out_predicted_display_period_ns);

	if (out_predicted_display_period_ns != NULL) {
		macos_compositor_update_time_constraint(*out_predicted_display_period_ns);
	}
}
