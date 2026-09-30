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

#include <string.h>

#ifdef XRT_OS_WINDOWS
#include <process.h>
#define u_timing_trace_getpid _getpid
#else
#include <unistd.h>
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
		buffer_size = U_TIMING_TRACE_FULLY_BUFFERED_SIZE;
	}
	setvbuf(file, NULL, _IOFBF, buffer_size);

	U_LOG_I("Timing trace: %s", path);

	return file;
}
