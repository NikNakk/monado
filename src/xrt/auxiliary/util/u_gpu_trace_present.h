// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "util/u_gpu_trace.h"

#define U_GPU_TRACE_PRESENT_CAPACITY 128

struct u_gpu_trace_present_entry
{
	struct u_gpu_trace_frame frame;
	int64_t gpu_end_ns;
	int64_t presented_ns;
	int64_t measured_ns;
	bool valid;
	bool terminal;
	bool native_started;
};

/* Caller serializes access. The bounded table joins compositor timeline values,
 * not client frame IDs or the target's independent presentation frame counter. */
struct u_gpu_trace_present
{
	uint64_t scope_id;
	uint64_t latest_depth_frame;
	int64_t depth;
	bool closed;
	struct u_gpu_trace_present_entry entries[U_GPU_TRACE_PRESENT_CAPACITY];
};

#ifdef __cplusplus
extern "C" {
#endif
void
u_gpu_trace_present_init(struct u_gpu_trace_present *state, uint64_t scope_id);
void
u_gpu_trace_present_prepare(struct u_gpu_trace_present *state, int64_t frame, uint32_t image, uint64_t timeline);
void
u_gpu_trace_present_gpu_end(struct u_gpu_trace_present *state,
                            uint64_t timeline,
                            int64_t gpu_end_ns,
                            int64_t observed_ns);
void
u_gpu_trace_present_reported(struct u_gpu_trace_present *state,
                             const struct u_gpu_trace_frame *frame,
                             int64_t presented_ns,
                             int64_t observed_ns,
                             int64_t depth);
void
u_gpu_trace_present_cancel(struct u_gpu_trace_present *state, uint64_t timeline, const char *reason);
void
u_gpu_trace_present_close(struct u_gpu_trace_present *state);
#ifdef __cplusplus
}
#endif
