// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Compositor-thread realtime scheduling trace for the Multi Client Module.
 *
 * Include this after comp_multi_system_macos_trace.h. After each
 * xrt_comp_predict_frame(), when PSVR2_TIMING_TRACE=1 and the Mach
 * time-constraint policy is enabled, compositor_rt.csv records CPU-time budget
 * use and samples the effective scheduling policy every frame. Detailed public
 * Mach information is captured on policy transitions, budget overruns, the
 * first frame, and every 60 frames.
 */
#pragma once

#include "os/os_time.h"

#include <mach/mach.h>
#include <mach/thread_info.h>
#include <mach/thread_policy.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static FILE *g_macos_compositor_rt_trace = NULL;
static bool g_macos_compositor_rt_trace_failed = false;
static bool g_macos_compositor_rt_trace_atexit_registered = false;
static uint64_t g_macos_compositor_rt_trace_rows = 0;
static uint64_t g_macos_compositor_rt_last_cpu_ns = 0;
static uint64_t g_macos_compositor_rt_last_sample_ns = 0;
static uint64_t g_macos_compositor_rt_thread_id = 0;
static int g_macos_compositor_rt_last_observed_basic_policy = -1;
static int g_macos_compositor_rt_last_policy_get_kr = -1;
static int g_macos_compositor_rt_last_policy_default = -1;
static uint32_t g_macos_compositor_rt_last_period_ticks = 0;
static uint32_t g_macos_compositor_rt_last_computation_ticks = 0;
static uint32_t g_macos_compositor_rt_last_constraint_ticks = 0;
static int g_macos_compositor_rt_last_extended_kr = -1;
static int g_macos_compositor_rt_last_extended_policy = -1;
static int g_macos_compositor_rt_last_curpri = -1;
static int g_macos_compositor_rt_last_priority = -1;
static int g_macos_compositor_rt_last_maxpriority = -1;

static inline bool
macos_compositor_rt_trace_enabled(void)
{
	const char *value = getenv("PSVR2_TIMING_TRACE");
	return value != NULL && strcmp(value, "1") == 0;
}

static inline bool
macos_compositor_rt_trace_fully_buffered(void)
{
	const char *value = getenv("PSVR2_TIMING_TRACE_FULLY_BUFFERED");
	return value != NULL && strcmp(value, "1") == 0;
}

static inline const char *
macos_compositor_rt_trace_dir(void)
{
	const char *dir = getenv("PSVR2_TIMING_TRACE_DIR");
	return dir != NULL && dir[0] != '\0' ? dir : "/tmp";
}

static void
macos_compositor_rt_trace_close(void)
{
	if (g_macos_compositor_rt_trace != NULL) {
		fflush(g_macos_compositor_rt_trace);
		fclose(g_macos_compositor_rt_trace);
		g_macos_compositor_rt_trace = NULL;
	}
}

static FILE *
macos_compositor_rt_trace_get(void)
{
	/* The time-constraint getter is defined by the preceding macOS trace header. */
	if (!macos_compositor_rt_trace_enabled() || !debug_get_bool_option_macos_compositor_time_constraint() ||
	    g_macos_compositor_rt_trace_failed) {
		return NULL;
	}
	if (g_macos_compositor_rt_trace != NULL) {
		return g_macos_compositor_rt_trace;
	}

	const char *dir = macos_compositor_rt_trace_dir();
	char path[1024];
	size_t len = strlen(dir);
	const char *separator = len > 0 && dir[len - 1] == '/' ? "" : "/";
	snprintf(path, sizeof(path), "%s%smonado_psvr2_%d_compositor_rt.csv", dir, separator, (int)getpid());
	g_macos_compositor_rt_trace = fopen(path, "w");
	if (g_macos_compositor_rt_trace == NULL) {
		g_macos_compositor_rt_trace_failed = true;
		return NULL;
	}
	setvbuf(g_macos_compositor_rt_trace, NULL, _IOFBF,
	        macos_compositor_rt_trace_fully_buffered() ? 16u * 1024u * 1024u : 64u * 1024u);
	fputs("sample,frame_id,sample_ns,wall_since_previous_predict_ns,thread_id,display_period_ns,"
	      "cpu_since_previous_predict_ns,configured_computation_ns,configured_constraint_ns,"
	      "over_computation_budget,basic_info_kr,basic_policy,basic_cpu_usage,basic_run_state,basic_flags,"
	      "policy_transition,previous_basic_policy,detail_sampled,extended_info_kr,extended_policy,"
	      "cur_priority,base_priority,max_priority,policy_get_kr,policy_get_default,tc_period_ticks,"
	      "tc_computation_ticks,tc_constraint_ticks\n",
	      g_macos_compositor_rt_trace);
	if (!g_macos_compositor_rt_trace_atexit_registered) {
		atexit(macos_compositor_rt_trace_close);
		g_macos_compositor_rt_trace_atexit_registered = true;
	}
	fprintf(stderr, "macOS compositor realtime trace: %s\n", path);
	return g_macos_compositor_rt_trace;
}

static inline uint64_t
macos_current_thread_cpu_ns(void)
{
	struct timespec ts = {0};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void
macos_compositor_rt_sample_details(thread_t thread)
{
	if (g_macos_compositor_rt_thread_id == 0) {
		thread_identifier_info_data_t identifier = {0};
		mach_msg_type_number_t identifier_count = THREAD_IDENTIFIER_INFO_COUNT;
		if (thread_info(thread, THREAD_IDENTIFIER_INFO, (thread_info_t)&identifier, &identifier_count) == KERN_SUCCESS) {
			g_macos_compositor_rt_thread_id = identifier.thread_id;
		}
	}

	thread_extended_info_data_t extended = {0};
	mach_msg_type_number_t extended_count = THREAD_EXTENDED_INFO_COUNT;
	kern_return_t extended_kr =
	    thread_info(thread, THREAD_EXTENDED_INFO, (thread_info_t)&extended, &extended_count);
	g_macos_compositor_rt_last_extended_kr = extended_kr;
	if (extended_kr == KERN_SUCCESS) {
		g_macos_compositor_rt_last_extended_policy = extended.pth_policy;
		g_macos_compositor_rt_last_curpri = extended.pth_curpri;
		g_macos_compositor_rt_last_priority = extended.pth_priority;
		g_macos_compositor_rt_last_maxpriority = extended.pth_maxpriority;
	} else {
		g_macos_compositor_rt_last_extended_policy = -1;
		g_macos_compositor_rt_last_curpri = -1;
		g_macos_compositor_rt_last_priority = -1;
		g_macos_compositor_rt_last_maxpriority = -1;
	}

	thread_time_constraint_policy_data_t tc = {0};
	mach_msg_type_number_t tc_count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
	boolean_t get_default = FALSE;
	kern_return_t tc_kr = thread_policy_get(thread, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tc,
	                                        &tc_count, &get_default);
	g_macos_compositor_rt_last_policy_get_kr = tc_kr;
	g_macos_compositor_rt_last_policy_default = tc_kr == KERN_SUCCESS ? (get_default ? 1 : 0) : -1;
	g_macos_compositor_rt_last_period_ticks = tc_kr == KERN_SUCCESS ? tc.period : 0;
	g_macos_compositor_rt_last_computation_ticks = tc_kr == KERN_SUCCESS ? tc.computation : 0;
	g_macos_compositor_rt_last_constraint_ticks = tc_kr == KERN_SUCCESS ? tc.constraint : 0;
}

static inline void
macos_compositor_rt_trace_record(int64_t frame_id, int64_t display_period_ns)
{
	FILE *file = macos_compositor_rt_trace_get();
	if (file == NULL || display_period_ns <= 0) {
		return;
	}

	uint64_t sample_ns = os_monotonic_get_ns();
	uint64_t wall_delta_ns =
	    g_macos_compositor_rt_last_sample_ns != 0 && sample_ns >= g_macos_compositor_rt_last_sample_ns
	        ? sample_ns - g_macos_compositor_rt_last_sample_ns
	        : 0;
	g_macos_compositor_rt_last_sample_ns = sample_ns;

	uint64_t cpu_now_ns = macos_current_thread_cpu_ns();
	uint64_t cpu_delta_ns = g_macos_compositor_rt_last_cpu_ns != 0 && cpu_now_ns >= g_macos_compositor_rt_last_cpu_ns
	                            ? cpu_now_ns - g_macos_compositor_rt_last_cpu_ns
	                            : 0;
	g_macos_compositor_rt_last_cpu_ns = cpu_now_ns;

	int computation_pct = debug_get_num_option_macos_compositor_computation_pct();
	int constraint_pct = debug_get_num_option_macos_compositor_constraint_pct();
	uint64_t computation_ns = computation_pct > 0 ? ((uint64_t)display_period_ns * (uint64_t)computation_pct) / 100u : 0;
	uint64_t constraint_ns = constraint_pct > 0 ? ((uint64_t)display_period_ns * (uint64_t)constraint_pct) / 100u : 0;
	bool over_budget = cpu_delta_ns != 0 && computation_ns != 0 && cpu_delta_ns > computation_ns;

	thread_t thread = mach_thread_self();
	thread_basic_info_data_t basic = {0};
	mach_msg_type_number_t basic_count = THREAD_BASIC_INFO_COUNT;
	kern_return_t basic_kr = thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&basic, &basic_count);
	int basic_policy = basic_kr == KERN_SUCCESS ? basic.policy : -1;
	int previous_policy = g_macos_compositor_rt_last_observed_basic_policy;
	bool policy_transition = previous_policy >= 0 && basic_policy >= 0 && basic_policy != previous_policy;
	g_macos_compositor_rt_last_observed_basic_policy = basic_policy;

	g_macos_compositor_rt_trace_rows++;
	bool detail_sampled = g_macos_compositor_rt_trace_rows == 1 ||
	                      (g_macos_compositor_rt_trace_rows % 60) == 0 || over_budget || policy_transition;
	if (detail_sampled) {
		macos_compositor_rt_sample_details(thread);
	}
	mach_port_deallocate(mach_task_self(), thread);

	if (policy_transition) {
		fprintf(stderr,
		        "INFO: macOS compositor scheduler policy transition: frame=%lld thread_id=%llu policy=%d->%d "
		        "wall_delta_ms=%.3f cpu_delta_ms=%.3f over_budget=%u ext_policy=%d curpri=%d basepri=%d maxpri=%d\n",
		        (long long)frame_id, (unsigned long long)g_macos_compositor_rt_thread_id,
		        previous_policy, basic_policy, (double)wall_delta_ns / 1000000.0,
		        (double)cpu_delta_ns / 1000000.0, over_budget ? 1u : 0u,
		        g_macos_compositor_rt_last_extended_policy, g_macos_compositor_rt_last_curpri,
		        g_macos_compositor_rt_last_priority, g_macos_compositor_rt_last_maxpriority);
	}

	fprintf(file,
	        "%llu,%lld,%llu,%llu,%llu,%lld,%llu,%llu,%llu,%u,%d,%d,%d,%d,%d,%u,%d,%u,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u\n",
	        (unsigned long long)g_macos_compositor_rt_trace_rows,
	        (long long)frame_id,
	        (unsigned long long)sample_ns,
	        (unsigned long long)wall_delta_ns,
	        (unsigned long long)g_macos_compositor_rt_thread_id,
	        (long long)display_period_ns,
	        (unsigned long long)cpu_delta_ns,
	        (unsigned long long)computation_ns,
	        (unsigned long long)constraint_ns,
	        over_budget ? 1u : 0u,
	        (int)basic_kr,
	        basic_policy,
	        basic_kr == KERN_SUCCESS ? basic.cpu_usage : -1,
	        basic_kr == KERN_SUCCESS ? basic.run_state : -1,
	        basic_kr == KERN_SUCCESS ? basic.flags : -1,
	        policy_transition ? 1u : 0u,
	        previous_policy,
	        detail_sampled ? 1u : 0u,
	        g_macos_compositor_rt_last_extended_kr,
	        g_macos_compositor_rt_last_extended_policy,
	        g_macos_compositor_rt_last_curpri,
	        g_macos_compositor_rt_last_priority,
	        g_macos_compositor_rt_last_maxpriority,
	        g_macos_compositor_rt_last_policy_get_kr,
	        g_macos_compositor_rt_last_policy_default,
	        g_macos_compositor_rt_last_period_ticks,
	        g_macos_compositor_rt_last_computation_ticks,
	        g_macos_compositor_rt_last_constraint_ticks);
	if (!macos_compositor_rt_trace_fully_buffered() && (g_macos_compositor_rt_trace_rows % 256) == 0) {
		fflush(file);
	}
}

/*!
 * Called after each xrt_comp_predict_frame() on the compositor thread: keeps
 * its Mach time constraint in step with the display period, and records it
 * in compositor_rt.csv when tracing.
 */
static inline void
macos_multi_system_after_predict_frame(int64_t frame_id, int64_t predicted_display_period_ns)
{
	macos_compositor_update_time_constraint(predicted_display_period_ns);
	macos_compositor_rt_trace_record(frame_id, predicted_display_period_ns);
}
