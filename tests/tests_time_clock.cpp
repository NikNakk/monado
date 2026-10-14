// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Monado's monotonic clock and its timespec conversion.
 */
#include "os/os_time.h"
#include "util/u_time.h"
#include "catch_amalgamated.hpp"

#include <cstdlib>
#include <time.h>

#ifdef XRT_OS_OSX
#include <mach/mach_time.h>
#endif

namespace {
constexpr int64_t kTolerance = 2 * U_TIME_1MS_IN_NS;

int64_t
clock_monotonic_ns()
{
	struct timespec ts;
	REQUIRE(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
	return os_timespec_to_ns(&ts);
}
} // namespace

TEST_CASE("timespec conversion uses CLOCK_MONOTONIC")
{
	struct time_state *state = time_state_create(os_monotonic_get_ns());
	REQUIRE(state != nullptr);

	struct timespec ts;
	int64_t before = clock_monotonic_ns();
	time_state_to_timespec(state, time_state_get_now(state), &ts);
	int64_t after = clock_monotonic_ns();
	CHECK(os_timespec_to_ns(&ts) >= before - kTolerance);
	CHECK(os_timespec_to_ns(&ts) <= after + kTolerance);

	os_ns_to_timespec(clock_monotonic_ns(), &ts);
	int64_t now = time_state_get_now(state);
	CHECK(std::llabs(time_state_from_timespec(state, &ts) - now) <= kTolerance);

	time_state_destroy(&state);
}

#ifdef XRT_OS_OSX
TEST_CASE("macOS monotonic clock is mach absolute time")
{
	mach_timebase_info_data_t timebase;
	REQUIRE(mach_timebase_info(&timebase) == KERN_SUCCESS);
	int64_t mach_ns =
	    (int64_t)(((__uint128_t)mach_absolute_time() * timebase.numer) / timebase.denom);
	CHECK(std::llabs(os_monotonic_get_ns() - mach_ns) <= kTolerance);
}
#endif
