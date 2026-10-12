// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "xrt/xrt_config_build.h"
#include "xrt/xrt_config_os.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef XRT_OS_OSX
uint64_t
u_gpu_trace_new_scope(void);
#else
static inline uint64_t
u_gpu_trace_new_scope(void)
{
	return 0;
}
#endif

#ifdef XRT_FEATURE_MACOS_PERFETTO
//! Capture lifecycle calls belong on an ordinary control thread, not a GPU callback.
bool
u_gpu_trace_start(const char *path, uint32_t buffer_mb);
bool
u_gpu_trace_stop(void);
#else
static inline bool
u_gpu_trace_start(const char *path, uint32_t buffer_mb)
{
	(void)path;
	(void)buffer_mb;
	return false;
}
static inline bool
u_gpu_trace_stop(void)
{
	return false;
}

#endif

#if defined(XRT_OS_OSX) || defined(XRT_FEATURE_MACOS_PERFETTO)
void
u_gpu_trace_init_from_environment(void);
bool
u_gpu_trace_enabled(void);
//! timestamp_ns is CLOCK_MONOTONIC; duration_ns describes an interval ending there.
void
u_gpu_trace_event(const char *domain,
                  const char *event,
                  uint64_t scope_id,
                  int64_t frame_id,
                  uint32_t image_index,
                  uint64_t timeline_value,
                  int64_t timestamp_ns,
                  int64_t duration_ns);
void
u_gpu_trace_owner_event(const char *event,
                        uint64_t scope_id,
                        uint64_t owner_scope_id,
                        int64_t frame_id,
                        uint32_t image_index,
                        uint64_t timeline_value,
                        int64_t timestamp_ns,
                        int64_t duration_ns);
//! Whether the independent opt-in Instruments backend can currently emit.
bool
u_gpu_trace_signpost_enabled(void);
#else
static inline void
u_gpu_trace_init_from_environment(void)
{}
static inline bool
u_gpu_trace_enabled(void)
{
	return false;
}
static inline void
u_gpu_trace_event(const char *domain,
                  const char *event,
                  uint64_t scope_id,
                  int64_t frame_id,
                  uint32_t image_index,
                  uint64_t timeline_value,
                  int64_t timestamp_ns,
                  int64_t duration_ns)
{
	(void)domain;
	(void)event;
	(void)scope_id;
	(void)frame_id;
	(void)image_index;
	(void)timeline_value;
	(void)timestamp_ns;
	(void)duration_ns;
}
static inline void
u_gpu_trace_owner_event(const char *event,
                        uint64_t scope_id,
                        uint64_t owner_scope_id,
                        int64_t frame_id,
                        uint32_t image_index,
                        uint64_t timeline_value,
                        int64_t timestamp_ns,
                        int64_t duration_ns)
{
	(void)owner_scope_id;
	u_gpu_trace_event("ownership", event, scope_id, frame_id, image_index, timeline_value, timestamp_ns,
	                  duration_ns);
}
static inline bool
u_gpu_trace_signpost_enabled(void)
{
	return false;
}

#endif

#ifdef XRT_OS_OSX
struct u_gpu_trace_frame
{
	uint64_t scope_id;
	uint64_t interval_id;
	int64_t frame_id;
	uint32_t image_index;
	uint64_t timeline_value;
};

void
u_gpu_trace_present_depth_change(const struct u_gpu_trace_frame *frame,
                                 int64_t previous,
                                 int64_t depth,
                                 int64_t event_ns);
bool
u_gpu_trace_signpost_gpu_to_glass_begin(const struct u_gpu_trace_frame *frame, int64_t gpu_end_ns, int64_t observed_ns);
void
u_gpu_trace_signpost_gpu_to_glass_end(const struct u_gpu_trace_frame *frame,
                                      int64_t gpu_end_ns,
                                      int64_t presented_ns,
                                      int64_t observed_ns,
                                      const char *reason);
#endif

#ifdef __cplusplus
}
#endif
