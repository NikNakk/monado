// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Shared CSV files for the PS VR2 timing traces.
 * @ingroup aux_util
 */

#include "xrt/xrt_config_os.h"

#include "util/u_timing_trace.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include <string.h>

#ifdef XRT_OS_WINDOWS
#include <process.h>
#define u_timing_trace_getpid _getpid
#else
#include <unistd.h>
#include <stdatomic.h>
#include <sys/stat.h>
#define u_timing_trace_getpid getpid
#endif

#define U_TIMING_TRACE_FULLY_BUFFERED_SIZE (16u * 1024u * 1024u)

DEBUG_GET_ONCE_BOOL_OPTION(timing_trace, "PSVR2_TIMING_TRACE", false)
DEBUG_GET_ONCE_BOOL_OPTION(timing_trace_fully_buffered, "PSVR2_TIMING_TRACE_FULLY_BUFFERED", false)
DEBUG_GET_ONCE_OPTION(timing_trace_dir, "PSVR2_TIMING_TRACE_DIR", "/tmp")

bool
u_timing_trace_enabled(void)
{
	return debug_get_bool_option_timing_trace();
}

bool
u_timing_trace_fully_buffered(void)
{
	return debug_get_bool_option_timing_trace_fully_buffered();
}

void
u_timing_trace_poll_flush_request(void)
{
#ifdef XRT_OS_OSX
	if (!u_timing_trace_enabled() || !u_timing_trace_fully_buffered()) {
		return;
	}
	/* No helper thread, signal handler, or trace write during measurement.
	 * Poll cached filesystem metadata at most ten times per second. */
	static atomic_flag busy = ATOMIC_FLAG_INIT;
	static uint64_t last_poll_ns;
	if (atomic_flag_test_and_set_explicit(&busy, memory_order_acquire)) {
		return;
	}
	uint64_t now_ns = os_monotonic_get_ns();
	if (now_ns - last_poll_ns < 100000000) {
		atomic_flag_clear_explicit(&busy, memory_order_release);
		return;
	}
	last_poll_ns = now_ns;
	const char *dir = debug_get_option_timing_trace_dir();
	if (dir == NULL || dir[0] == '\0') {
		dir = "/tmp";
	}
	char request[1024], ack[1024];
	int n = snprintf(request, sizeof(request), "%s/monado_trace_%d.flush-request", dir, (int)getpid());
	int m = snprintf(ack, sizeof(ack), "%s/monado_trace_%d.flush-complete", dir, (int)getpid());
	struct stat info;
	if (n > 0 && (size_t)n < sizeof(request) && m > 0 && (size_t)m < sizeof(ack) && lstat(request, &info) == 0 &&
	    S_ISREG(info.st_mode) && info.st_uid == geteuid()) {
		/* stdio serializes its streams; acknowledge only a successful flush.
		 * Renaming the owned marker creates no file from untrusted contents. */
		if (fflush(NULL) == 0) {
			(void)rename(request, ack);
		}
	}
	atomic_flag_clear_explicit(&busy, memory_order_release);
#endif
}

FILE *
u_timing_trace_open(const char *name, size_t buffer_size)
{
	const char *dir = debug_get_option_timing_trace_dir();
	if (dir == NULL || dir[0] == '\0') {
		dir = "/tmp";
	}

	size_t dir_len = strlen(dir);
	const char *separator = dir[dir_len - 1] == '/' ? "" : "/";

	char path[1024];
	snprintf(path, sizeof(path), "%s%smonado_psvr2_%d_%s.csv", dir, separator, (int)u_timing_trace_getpid(), name);

	FILE *file = fopen(path, "w");
	if (file == NULL) {
		U_LOG_W("Could not open timing trace '%s'", path);
		return NULL;
	}

	if (u_timing_trace_fully_buffered()) {
		if (buffer_size < U_TIMING_TRACE_FULLY_BUFFERED_SIZE) {
			buffer_size = U_TIMING_TRACE_FULLY_BUFFERED_SIZE;
		}
	}
	if (setvbuf(file, NULL, _IOFBF, buffer_size) != 0) {
		U_LOG_W("Could not allocate %zu-byte timing trace buffer for '%s'", buffer_size, path);
		fclose(file);
		return NULL;
	}

	U_LOG_I("Timing trace: %s", path);

	return file;
}
