// Copyright 2022, Collabora, Ltd.
// Copyright 2024-2025, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Tiny file to implement precise waiting functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_config_os.h"
#include "os/os_time.h"

#if defined(XRT_OS_OSX)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <stdio.h>
#include <stdlib.h>
#endif

#if defined(XRT_DOXYGEN)

/*!
 * OS specific tweak to wait time.
 *
 * @todo Measure on Windows.
 * @ingroup aux_util
 */
#define U_WAIT_MEASURED_SCHEDULER_LATENCY_NS (uint64_t)(0)

#elif defined(XRT_OS_LINUX) || defined(XRT_OS_ANDROID)
#define U_WAIT_MEASURED_SCHEDULER_LATENCY_NS (uint64_t)(50 * 1000)
#elif defined(XRT_OS_OSX)
//! @TODO Measure
#define U_WAIT_MEASURED_SCHEDULER_LATENCY_NS (uint64_t)(0)
#elif defined(XRT_OS_WINDOWS)
#define U_WAIT_MEASURED_SCHEDULER_LATENCY_NS (uint64_t)(0)
#else
#error "Unsupported platform!"
#endif

#if defined(XRT_OS_OSX)
static inline bool
u_wait_macos_env_enabled(const char *name)
{
	const char *value = getenv(name);
	return value != NULL && value[0] != '\0' && value[0] != '0';
}

static inline uint64_t
u_wait_macos_env_u64(const char *name, uint64_t default_value)
{
	const char *value = getenv(name);
	if (value == NULL || value[0] == '\0') {
		return default_value;
	}

	char *end = NULL;
	unsigned long long parsed = strtoull(value, &end, 10);
	return end != value && *end == '\0' ? (uint64_t)parsed : default_value;
}

static inline uint32_t
u_wait_macos_ns_to_mach_ticks(uint64_t ns, const mach_timebase_info_data_t *timebase)
{
	__uint128_t ticks = (__uint128_t)ns * (__uint128_t)timebase->denom / (__uint128_t)timebase->numer;
	return ticks > UINT32_MAX ? UINT32_MAX : (uint32_t)ticks;
}

/*
 * Diagnostic macOS real-time scheduling experiment. This is intentionally tied
 * to the process environment: in service-mode testing the variable is supplied
 * only to monado-service, where u_wait_until() is used by the timing-critical
 * Multi Client Module loop. Keep the full-spin mode available as the known-good
 * control while evaluating whether a real time constraint lets us park safely.
 */
static inline void
u_wait_macos_apply_time_constraint_if_requested(void)
{
	static bool checked = false;
	if (checked) {
		return;
	}
	checked = true;

	if (!u_wait_macos_env_enabled("XRT_MACOS_COMPOSITOR_TIME_CONSTRAINT")) {
		return;
	}

	uint64_t period_us = u_wait_macos_env_u64("XRT_MACOS_COMPOSITOR_PERIOD_US", 8342);
	uint64_t computation_us = u_wait_macos_env_u64("XRT_MACOS_COMPOSITOR_COMPUTATION_US", 3000);
	uint64_t constraint_us = u_wait_macos_env_u64("XRT_MACOS_COMPOSITOR_CONSTRAINT_US", 6000);

	if (period_us == 0 || computation_us == 0 || constraint_us == 0 || computation_us > constraint_us ||
	    constraint_us > period_us || period_us > UINT64_MAX / 1000 || computation_us > UINT64_MAX / 1000 ||
	    constraint_us > UINT64_MAX / 1000) {
		fprintf(stderr,
		        "WARN [u_wait] invalid macOS time constraint: period_us=%llu computation_us=%llu constraint_us=%llu\n",
		        (unsigned long long)period_us, (unsigned long long)computation_us,
		        (unsigned long long)constraint_us);
		return;
	}

	mach_timebase_info_data_t timebase = {0};
	kern_return_t kr = mach_timebase_info(&timebase);
	if (kr != KERN_SUCCESS || timebase.numer == 0 || timebase.denom == 0) {
		fprintf(stderr, "WARN [u_wait] mach_timebase_info failed for macOS time constraint: %d\n", kr);
		return;
	}

	thread_time_constraint_policy_data_t policy = {
	    .period = u_wait_macos_ns_to_mach_ticks(period_us * 1000, &timebase),
	    .computation = u_wait_macos_ns_to_mach_ticks(computation_us * 1000, &timebase),
	    .constraint = u_wait_macos_ns_to_mach_ticks(constraint_us * 1000, &timebase),
	    .preemptible = TRUE,
	};

	thread_t thread = mach_thread_self();
	kr = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&policy,
	                       THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	mach_port_deallocate(mach_task_self(), thread);

	if (kr == KERN_SUCCESS) {
		fprintf(stderr,
		        "INFO [u_wait] macOS compositor time constraint enabled: period_us=%llu computation_us=%llu constraint_us=%llu\n",
		        (unsigned long long)period_us, (unsigned long long)computation_us,
		        (unsigned long long)constraint_us);
	} else {
		fprintf(stderr,
		        "WARN [u_wait] failed to set macOS THREAD_TIME_CONSTRAINT_POLICY: kr=%d period_us=%llu computation_us=%llu constraint_us=%llu\n",
		        kr, (unsigned long long)period_us, (unsigned long long)computation_us,
		        (unsigned long long)constraint_us);
	}
}
#endif

/*!
 * Waits until the given time using the @ref os_precise_sleeper.
 *
 * @ingroup aux_util
 */
static inline void
u_wait_until(struct os_precise_sleeper *sleeper, uint64_t until_ns)
{
	uint64_t now_ns = os_monotonic_get_ns();

#if defined(XRT_OS_OSX)
	u_wait_macos_apply_time_constraint_if_requested();
#endif

	// Lets hope its not to late.
	bool fuzzy_in_the_past = time_is_less_then_or_within_range(until_ns, now_ns, U_TIME_1MS_IN_NS);

	// When we should wake up is in the past:ish.
	if (fuzzy_in_the_past) {
		return;
	}

	// Sufficiently in the future.
	uint32_t delay = (uint32_t)(until_ns - now_ns - U_WAIT_MEASURED_SCHEDULER_LATENCY_NS);

#if defined(XRT_OS_OSX)
	bool trace_wait = u_wait_macos_env_enabled("XRT_MACOS_WAIT_TIMING");
	bool spin_wait = u_wait_macos_env_enabled("XRT_MACOS_WAIT_SPIN");
	uint64_t hybrid_us = u_wait_macos_env_u64("XRT_MACOS_WAIT_HYBRID_US", 0);
	uint64_t hybrid_ns = hybrid_us <= UINT64_MAX / 1000 ? hybrid_us * 1000 : 0;
	bool hybrid_wait = !spin_wait && hybrid_ns > 0;
	uint64_t wait_begin_ns = trace_wait ? os_monotonic_get_ns() : 0;
	uint64_t park_requested_ns = 0;
	uint64_t park_actual_ns = 0;
	uint64_t spin_actual_ns = 0;

	if (spin_wait) {
		uint64_t spin_begin_ns = os_monotonic_get_ns();
		while (os_monotonic_get_ns() < until_ns) {
			/* Diagnostic control: stay runnable for the whole wait. */
		}
		spin_actual_ns = os_monotonic_get_ns() - spin_begin_ns;
	} else if (hybrid_wait) {
		uint64_t before_park_ns = os_monotonic_get_ns();
		if (until_ns > before_park_ns + hybrid_ns) {
			uint64_t requested = until_ns - before_park_ns - hybrid_ns;
			park_requested_ns = requested;
			uint64_t park_begin_ns = os_monotonic_get_ns();
			os_precise_sleeper_nanosleep(sleeper, requested);
			uint64_t park_end_ns = os_monotonic_get_ns();
			park_actual_ns = park_end_ns >= park_begin_ns ? park_end_ns - park_begin_ns : 0;
		}

		uint64_t spin_begin_ns = os_monotonic_get_ns();
		while (os_monotonic_get_ns() < until_ns) {
			/* Short final spin removes residual wake jitter without a full-frame busy wait. */
		}
		spin_actual_ns = os_monotonic_get_ns() - spin_begin_ns;
	} else {
		park_requested_ns = delay;
		uint64_t park_begin_ns = trace_wait ? os_monotonic_get_ns() : 0;
		os_precise_sleeper_nanosleep(sleeper, delay);
		if (trace_wait) {
			uint64_t park_end_ns = os_monotonic_get_ns();
			park_actual_ns = park_end_ns >= park_begin_ns ? park_end_ns - park_begin_ns : 0;
		}
	}
#else
	os_precise_sleeper_nanosleep(sleeper, delay);
#endif

#if defined(XRT_OS_OSX)
	if (trace_wait) {
		uint64_t wait_end_ns = os_monotonic_get_ns();
		uint64_t actual_ns = wait_end_ns >= wait_begin_ns ? wait_end_ns - wait_begin_ns : 0;
		int64_t lateness_ns = (int64_t)wait_end_ns - (int64_t)until_ns;
		const char *mode = spin_wait ? "spin" : (hybrid_wait ? "hybrid" : "mach");
		fprintf(stderr,
		        "MACOS_WAIT_TIMING mode=%s requested_ns=%u park_requested_ns=%llu park_actual_ns=%llu spin_actual_ns=%llu actual_ns=%llu lateness_ns=%lld until_ns=%llu begin_ns=%llu end_ns=%llu\n",
		        mode, delay, (unsigned long long)park_requested_ns, (unsigned long long)park_actual_ns,
		        (unsigned long long)spin_actual_ns, (unsigned long long)actual_ns, (long long)lateness_ns,
		        (unsigned long long)until_ns, (unsigned long long)wait_begin_ns, (unsigned long long)wait_end_ns);
	}
#endif
}
