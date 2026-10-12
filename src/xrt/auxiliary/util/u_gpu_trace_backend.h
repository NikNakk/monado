// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "util/u_gpu_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_FEATURE_MACOS_PERFETTO
bool
u_gpu_trace_perfetto_enabled(void);
void
u_gpu_trace_perfetto_init_from_environment(void);
void
u_gpu_trace_perfetto_emit(const char *domain,
                          const char *event,
                          uint64_t scope_id,
                          uint64_t owner_scope_id,
                          int64_t frame_id,
                          uint32_t image_index,
                          uint64_t timeline_value,
                          int64_t timestamp_ns,
                          int64_t duration_ns);
void
u_gpu_trace_perfetto_depth_change(const struct u_gpu_trace_frame *frame,
                                  int64_t previous,
                                  int64_t depth,
                                  int64_t event_ns);
#endif

#ifdef __cplusplus
}
#endif
