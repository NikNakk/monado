// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Experimental CAMetalDisplayLink trigger for the Multi Client Module.
 *
 * Force-include this after comp_multi_system_macos_trace.h. It does not replace
 * multi_main_loop(): it changes only where CAMetalDisplayLink participates in
 * the established predict/wait/render sequence.
 *
 * Driven mode must consume the CAMetal tick before xrt_comp_predict_frame(): the
 * native compositor uses that callback's target/presentation timestamps and its
 * callback-owned drawable for the frame.
 *
 * Hybrid mode is deliberately narrower. Prediction remains completely native/
 * legacy. The ordinary u_wait_until(wake_up_time_ns) call inside wait_frame() is
 * replaced with a condition-variable wait for the independent child-layer
 * CAMetalDisplayLink callback. Thus the only intended difference from legacy is
 * the CPU wake primitive at the existing wake point.
 *
 * When PSVR2_TIMING_TRACE=1, hybrid writes hybrid_phase.csv so the callback phase
 * can be compared directly with the native pacer's wake target. If the optional
 * compositor Mach time-constraint experiment is enabled, compositor_rt.csv also
 * records CPU-time budget overruns and periodically samples the effective/current
 * scheduler policy exposed by the public Mach interfaces.
 */
#pragma once

#include "multi/comp_multi_macos_displaylink.h"
#include "os/os_time.h"
#include "util/u_wait.h"

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

static FILE *g_macos_hybrid_phase_trace = NULL;
static bool g_macos_hybrid_phase_trace_failed = false;
static bool g_macos_hybrid_phase_trace_atexit_registered = false;
static uint64_t g_macos_hybrid_phase_trace_rows = 0;

static FILE *g_macos_compositor_rt_trace = NULL;
static bool g_macos_compositor_rt_trace_failed = false;
static bool g_macos_compositor_rt_trace_atexit_registered = false;
static uint64_t g_macos_compositor_rt_trace_rows = 0;
static uint64_t g_macos_compositor_rt_last_cpu_ns = 0;
static int64_t g_macos_compositor_rt_last_frame_id = -1;
static int g_macos_compositor_rt_last_basic_policy = -1;
static int g_macos_compositor_rt_last_policy_get_kr = -1;
static int g_macos_compositor_rt_last_policy_default = -1;
static uint32_t g_macos_compositor_rt_last_period_ticks = 0;
static uint32_t g_macos_compositor_rt_last_computation_ticks = 0;
static uint32_t g_macos_compositor_rt_last_constraint_ticks = 0;

static inline bool
macos_displaylink_trace_enabled(void)
{
	const char *value = getenv("PSVR2_TIMING_TRACE");
	return value != NULL && strcmp(value, "1") == 0;
}

static inline bool
macos_displaylink_trace_fully_buffered(void)
{
	const char *value = getenv("PSVR2_TIMING_TRACE_FULLY_BUFFERED");
	return value != NULL && strcmp(value, "1") == 0;
}

static inline const char *
macos_displaylink_trace_dir(void)
{
	const char *dir = getenv("PSVR2_TIMING_TRACE_DIR");
	return dir != NULL && dir[0] != '\0' ? dir : "/tmp";
}

static void
macos_hybrid_phase_trace_close(void)
{
	if (g_macos_hybrid_phase_trace != NULL) {
		fflush(g_macos_hybrid_phase_trace);
		fclose(g_macos_hybrid_phase_trace);
		g_macos_hybrid_phase_trace = NULL;
	}
}

static FILE *
macos_hybrid_phase_trace_get(void)
{
	if (!macos_displaylink_trace_enabled() || g_macos_hybrid_phase_trace_failed) {
		return NULL;
	}
	if (g_macos_hybrid_phase_trace != NULL) {
		return g_macos_hybrid_phase_trace;
	}

	const char *dir = macos_displaylink_trace_dir();
	char path[1024];
	size_t len = strlen(dir);
	const char *separator = len > 0 && dir[len - 1] == '/' ? "" : "/";
	snprintf(path, sizeof(path), "%s%smonado_psvr2_%d_hybrid_phase.csv", dir, separator, (int)getpid());
	g_macos_hybrid_phase_trace = fopen(path, "w");
	if (g_macos_hybrid_phase_trace == NULL) {
		g_macos_hybrid_phase_trace_failed = true;
		return NULL;
	}
	setvbuf(g_macos_hybrid_phase_trace, NULL, _IOFBF,
	        macos_displaylink_trace_fully_buffered() ? 16u * 1024u * 1024u : 64u * 1024u);
	fputs("sample,wait_entry_ns,native_wake_ns,callback_ns,callback_minus_native_wake_ns,wait_return_ns,"
	      "wait_return_minus_native_wake_ns,target_ns,target_minus_callback_ns,presentation_ns,"
	      "presentation_minus_callback_ns,presentation_minus_target_ns\n",
	      g_macos_hybrid_phase_trace);
	if (!g_macos_hybrid_phase_trace_atexit_registered) {
		atexit(macos_hybrid_phase_trace_close);
		g_macos_hybrid_phase_trace_atexit_registered = true;
	}
	fprintf(stderr, "macOS hybrid phase trace: %s\n", path);
	return g_macos_hybrid_phase_trace;
}

static inline void
macos_hybrid_phase_trace_record(int64_t native_wake_ns,
                                uint64_t wait_entry_ns,
                                uint64_t callback_ns,
                                uint64_t target_ns,
                                uint64_t presentation_ns,
                                uint64_t wait_return_ns)
{
	FILE *file = macos_hybrid_phase_trace_get();
	if (file == NULL) {
		return;
	}
	g_macos_hybrid_phase_trace_rows++;
	fprintf(file, "%llu,%llu,%lld,%llu,%lld,%llu,%lld,%llu,%lld,%llu,%lld,%lld\n",
	        (unsigned long long)g_macos_hybrid_phase_trace_rows,
	        (unsigned long long)wait_entry_ns,
	        (long long)native_wake_ns,
	        (unsigned long long)callback_ns,
	        (long long)((int64_t)callback_ns - native_wake_ns),
	        (unsigned long long)wait_return_ns,
	        (long long)((int64_t)wait_return_ns - native_wake_ns),
	        (unsigned long long)target_ns,
	        (long long)((int64_t)target_ns - (int64_t)callback_ns),
	        (unsigned long long)presentation_ns,
	        (long long)((int64_t)presentation_ns - (int64_t)callback_ns),
	        (long long)((int64_t)presentation_ns - (int64_t)target_ns));
	if (!macos_displaylink_trace_fully_buffered() && (g_macos_hybrid_phase_trace_rows % 256) == 0) {
		fflush(file);
	}
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
	if (!macos_displaylink_trace_enabled() || !debug_get_bool_option_macos_compositor_time_constraint() ||
	    g_macos_compositor_rt_trace_failed) {
		return NULL;
	}
	if (g_macos_compositor_rt_trace != NULL) {
		return g_macos_compositor_rt_trace;
	}

	const char *dir = macos_displaylink_trace_dir();
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
	        macos_displaylink_trace_fully_buffered() ? 16u * 1024u * 1024u : 64u * 1024u);
	fputs("sample,frame_id,sample_ns,display_period_ns,cpu_since_previous_predict_ns,configured_computation_ns,"
	      "configured_constraint_ns,over_computation_budget,policy_sampled,basic_policy,policy_get_kr,"
	      "policy_get_default,tc_period_ticks,tc_computation_ticks,tc_constraint_ticks\n",
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
macos_compositor_rt_sample_policy(void)
{
	thread_t thread = mach_thread_self();

	thread_basic_info_data_t basic = {0};
	mach_msg_type_number_t basic_count = THREAD_BASIC_INFO_COUNT;
	kern_return_t basic_kr = thread_info(thread, THREAD_BASIC_INFO, (thread_info_t)&basic, &basic_count);
	g_macos_compositor_rt_last_basic_policy = basic_kr == KERN_SUCCESS ? basic.policy : -1;

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

	mach_port_deallocate(mach_task_self(), thread);
}

static inline void
macos_compositor_rt_trace_record(int64_t frame_id, int64_t display_period_ns)
{
	FILE *file = macos_compositor_rt_trace_get();
	if (file == NULL || display_period_ns <= 0) {
		return;
	}

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

	g_macos_compositor_rt_trace_rows++;
	bool policy_sampled = g_macos_compositor_rt_trace_rows == 1 ||
	                      (g_macos_compositor_rt_trace_rows % 60) == 0 || over_budget;
	if (policy_sampled) {
		macos_compositor_rt_sample_policy();
	}

	fprintf(file, "%llu,%lld,%llu,%lld,%llu,%llu,%llu,%u,%u,%d,%d,%d,%u,%u,%u\n",
	        (unsigned long long)g_macos_compositor_rt_trace_rows,
	        (long long)frame_id,
	        (unsigned long long)os_monotonic_get_ns(),
	        (long long)display_period_ns,
	        (unsigned long long)cpu_delta_ns,
	        (unsigned long long)computation_ns,
	        (unsigned long long)constraint_ns,
	        over_budget ? 1u : 0u,
	        policy_sampled ? 1u : 0u,
	        g_macos_compositor_rt_last_basic_policy,
	        g_macos_compositor_rt_last_policy_get_kr,
	        g_macos_compositor_rt_last_policy_default,
	        g_macos_compositor_rt_last_period_ticks,
	        g_macos_compositor_rt_last_computation_ticks,
	        g_macos_compositor_rt_last_constraint_ticks);
	g_macos_compositor_rt_last_frame_id = frame_id;
	if (!macos_displaylink_trace_fully_buffered() && (g_macos_compositor_rt_trace_rows % 256) == 0) {
		fflush(file);
	}
}

/* The trace header has already defined its predict wrapper and source macro. */
#ifdef xrt_comp_predict_frame
#undef xrt_comp_predict_frame
#endif

static inline void
macos_xrt_comp_predict_frame_from_displaylink(struct xrt_compositor *xc,
                                              int64_t *out_frame_id,
                                              int64_t *out_wake_up_time_ns,
                                              int64_t *out_predicted_gpu_time_ns,
                                              int64_t *out_predicted_display_time_ns,
                                              int64_t *out_predicted_display_period_ns)
{
	/* Driven mode needs the callback timing before native prediction. Hybrid does
	 * not: it predicts first and substitutes its callback for u_wait_until below. */
	if (comp_multi_macos_displaylink_active() && comp_multi_macos_displaylink_driven_mode()) {
		(void)comp_multi_macos_displaylink_wait_tick(NULL, NULL, NULL);
	}
	macos_xrt_comp_predict_frame_with_time_constraint(xc, out_frame_id, out_wake_up_time_ns,
	                                                  out_predicted_gpu_time_ns,
	                                                  out_predicted_display_time_ns,
	                                                  out_predicted_display_period_ns);

	if (out_frame_id != NULL && out_predicted_display_period_ns != NULL) {
		macos_compositor_rt_trace_record(*out_frame_id, *out_predicted_display_period_ns);
	}
}

#define xrt_comp_predict_frame(xc, out_frame_id, out_wake_up_time_ns, out_predicted_gpu_time_ns,                     \
                               out_predicted_display_time_ns, out_predicted_display_period_ns)                       \
	macos_xrt_comp_predict_frame_from_displaylink((xc), (out_frame_id), (out_wake_up_time_ns),                       \
	                                              (out_predicted_gpu_time_ns),                                         \
	                                              (out_predicted_display_time_ns),                                     \
	                                              (out_predicted_display_period_ns))

/*
 * Preserve the source-level wait point so hybrid differs from legacy only in the
 * primitive used to release wait_frame():
 *
 *   legacy: u_wait_until(sleeper, native_wake_time)
 *   hybrid: predict native timing, then wait for the child CAMetal callback here
 *   driven: callback was already consumed before prediction, so do not wait twice
 *
 * Hybrid phase tracing records the exact callback which released this wait against
 * the native wake target. The bridge's 100 ms timeout remains only a failure/
 * teardown escape hatch; healthy CAMetal cadence is callback-driven.
 */
static inline void
macos_u_wait_until_displaylink(struct os_precise_sleeper *sleeper, int64_t wake_up_time_ns)
{
	if (comp_multi_macos_displaylink_active()) {
		if (comp_multi_macos_displaylink_hybrid_mode()) {
			uint64_t callback_ns = 0;
			uint64_t target_ns = 0;
			uint64_t presentation_ns = 0;
			uint64_t wait_entry_ns = os_monotonic_get_ns();
			bool got_tick = comp_multi_macos_displaylink_wait_tick(&callback_ns, &target_ns, &presentation_ns);
			uint64_t wait_return_ns = os_monotonic_get_ns();
			if (got_tick) {
				macos_hybrid_phase_trace_record(wake_up_time_ns, wait_entry_ns, callback_ns, target_ns,
				                                presentation_ns, wait_return_ns);
			}
		}
		/* Driven already waited before prediction. Hybrid just waited above. */
		(void)sleeper;
		(void)wake_up_time_ns;
		return;
	}
	u_wait_until(sleeper, wake_up_time_ns);
}

#define u_wait_until(sleeper, wake_up_time_ns) macos_u_wait_until_displaylink((sleeper), (wake_up_time_ns))
