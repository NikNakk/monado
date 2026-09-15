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


/*!
 * Waits until the given time using the @ref os_precise_sleeper.
 *
 * @ingroup aux_util
 */
static inline void
u_wait_until(struct os_precise_sleeper *sleeper, uint64_t until_ns)
{
	uint64_t now_ns = os_monotonic_get_ns();

	// Lets hope its not to late.
	bool fuzzy_in_the_past = time_is_less_then_or_within_range(until_ns, now_ns, U_TIME_1MS_IN_NS);

	// When we should wake up is in the past:ish.
	if (fuzzy_in_the_past) {
		return;
	}

	// Sufficiently in the future.
	uint32_t delay = (uint32_t)(until_ns - now_ns - U_WAIT_MEASURED_SCHEDULER_LATENCY_NS);

#if defined(XRT_OS_OSX)
	const char *wait_timing_env = getenv("XRT_MACOS_WAIT_TIMING");
	bool trace_wait = wait_timing_env != NULL && wait_timing_env[0] != '\0' && wait_timing_env[0] != '0';
	const char *wait_spin_env = getenv("XRT_MACOS_WAIT_SPIN");
	bool spin_wait = wait_spin_env != NULL && wait_spin_env[0] != '\0' && wait_spin_env[0] != '0';
	uint64_t wait_begin_ns = (trace_wait || spin_wait) ? os_monotonic_get_ns() : 0;

	if (spin_wait) {
		while (os_monotonic_get_ns() < until_ns) {
			/* Diagnostic only: stay runnable instead of parking in mach_wait_until(). */
		}
	} else {
		os_precise_sleeper_nanosleep(sleeper, delay);
	}
#else
	os_precise_sleeper_nanosleep(sleeper, delay);
#endif

#if defined(XRT_OS_OSX)
	if (trace_wait) {
		uint64_t wait_end_ns = os_monotonic_get_ns();
		uint64_t actual_ns = wait_end_ns >= wait_begin_ns ? wait_end_ns - wait_begin_ns : 0;
		int64_t lateness_ns = (int64_t)wait_end_ns - (int64_t)until_ns;
		fprintf(stderr,
		        "MACOS_WAIT_TIMING mode=%s requested_ns=%u actual_ns=%llu lateness_ns=%lld until_ns=%llu begin_ns=%llu end_ns=%llu\n",
		        spin_wait ? "spin" : "mach", delay, (unsigned long long)actual_ns, (long long)lateness_ns,
		        (unsigned long long)until_ns, (unsigned long long)wait_begin_ns, (unsigned long long)wait_end_ns);
	}
#endif
}
