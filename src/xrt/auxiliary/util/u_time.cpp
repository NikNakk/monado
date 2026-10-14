// Copyright 2019-2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of a steady, convertible clock.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup aux_util
 */

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_compiler.h"

#include "os/os_time.h"

#include "u_time.h"

#include <new>
#include <assert.h>
#include <stdlib.h>
#include <time.h>


/*
 *
 * Structs.
 *
 */

struct time_state
{
	timepoint_ns offset;
};


/*
 *
 * Helpers.
 *
 */

/*!
 * Timespec times are CLOCK_MONOTONIC (XR_KHR_convert_timespec_time). On macOS
 * the monotonic clock is CLOCK_UPTIME_RAW, which CLOCK_MONOTONIC is slewed
 * against, so convert through their current offset.
 */
static int64_t
clock_monotonic_minus_monotonic_ns(void)
{
#if defined(XRT_OS_OSX)
	int64_t before = os_monotonic_get_ns();
	int64_t clock_monotonic = (int64_t)clock_gettime_nsec_np(CLOCK_MONOTONIC);
	int64_t after = os_monotonic_get_ns();
	return clock_monotonic - (before + (after - before) / 2);
#else
	return 0;
#endif
}


/*
 *
 * 'Exported' functions.
 *
 */

extern "C" struct time_state *
time_state_create(int64_t offset)
{
	time_state *state = new (std::nothrow) time_state;
	if (!state) {
		return state;
	}
	state->offset = offset;
	return state;
}

extern "C" void
time_state_destroy(struct time_state **state_ptr)
{
	struct time_state *state = *state_ptr;

	if (state == NULL) {
		return;
	}

	delete state;
	*state_ptr = NULL;
}

extern "C" timepoint_ns
time_state_get_now(struct time_state const *state)
{
	assert(state != NULL);

	return time_state_monotonic_to_ts_ns(state, os_monotonic_get_ns());
}

extern "C" timepoint_ns
time_state_get_now_and_update(struct time_state *state)
{
	assert(state != NULL);

	return time_state_get_now(state);
}

extern "C" void
time_state_to_timespec(struct time_state const *state, timepoint_ns timestamp, struct timespec *out)
{
	assert(state != NULL);
	assert(out != NULL);

	int64_t ns = time_state_ts_to_monotonic_ns(state, timestamp) + clock_monotonic_minus_monotonic_ns();

	out->tv_sec = ns / (U_1_000_000_000);
	out->tv_nsec = ns % (U_1_000_000_000);
}

extern "C" timepoint_ns
time_state_from_timespec(struct time_state const *state, const struct timespec *timespecTime)
{
	assert(state != NULL);
	assert(timespecTime != NULL);

	int64_t ns = 0;
	ns += (int64_t)timespecTime->tv_nsec;
	ns += (int64_t)timespecTime->tv_sec * U_1_000_000_000;

	return time_state_monotonic_to_ts_ns(state, ns - clock_monotonic_minus_monotonic_ns());
}

extern "C" timepoint_ns
time_state_monotonic_to_ts_ns(struct time_state const *state, int64_t monotonic_ns)
{
	assert(state != NULL);

	return monotonic_ns - state->offset;
}

extern "C" int64_t
time_state_ts_to_monotonic_ns(struct time_state const *state, timepoint_ns timestamp)
{
	assert(state != NULL);

	return timestamp + state->offset;
}

#ifdef XRT_OS_WINDOWS
extern "C" void
time_state_to_win32perfcounter(struct time_state const *state, timepoint_ns timestamp, LARGE_INTEGER *out_qpc_ticks)
{
	assert(state != NULL);
	assert(out_qpc_ticks != NULL);

	int64_t ns = time_state_ts_to_monotonic_ns(state, timestamp);

	out_qpc_ticks->QuadPart = ns / os_ns_per_qpc_tick_get();
}

extern "C" timepoint_ns
time_state_from_win32perfcounter(struct time_state const *state, const LARGE_INTEGER *qpc_ticks)
{
	assert(state != NULL);
	assert(qpc_ticks != NULL);

	int64_t ns = qpc_ticks->QuadPart * os_ns_per_qpc_tick_get();

	return time_state_monotonic_to_ts_ns(state, ns);
}
#endif // XRT_OS_WINDOWS
