// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Shared CSV files for the PS VR2 timing traces.
 * @ingroup aux_util
 *
 * The timing traces are enabled by PSVR2_TIMING_TRACE and written to
 * `<PSVR2_TIMING_TRACE_DIR>/monado_psvr2_<pid>_<name>.csv` (default directory
 * `/tmp`). PSVR2_TIMING_TRACE_FULLY_BUFFERED gives every trace a 16 MiB
 * minimum buffer (larger when requested), so short captures do not flush while they run.
 */

#pragma once

#include "xrt/xrt_compiler.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * True if the timing traces are enabled (PSVR2_TIMING_TRACE).
 */
bool
u_timing_trace_enabled(void);

/*!
 * True if the traces should avoid flushing until they are closed
 * (PSVR2_TIMING_TRACE_FULLY_BUFFERED).
 */
bool
u_timing_trace_fully_buffered(void);

/*!
 * macOS capture control: occasionally check for an owned flush request in the
 * trace directory and acknowledge it after fflush(NULL). Call only from an
 * ordinary thread, never a signal handler. Inactive without fully buffered tracing.
 */
void
u_timing_trace_poll_flush_request(void);

/*!
 * Open the trace file called @p name, fully buffered with @p buffer_size bytes
 * (at least 16 MiB when fully buffered). Logs the path; returns NULL, and warns, on failure.
 * The caller writes the header and closes the file.
 */
FILE *
u_timing_trace_open(const char *name, size_t buffer_size);


#ifdef __cplusplus
}
#endif
