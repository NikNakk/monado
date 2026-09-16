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
