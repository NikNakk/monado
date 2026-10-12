// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "util/u_gpu_trace_backend.h"
#include "util/u_debug.h"

#include <os/log.h>
#include <os/signpost.h>
#include <pthread.h>
#include <string.h>

DEBUG_GET_ONCE_BOOL_OPTION(macos_signpost, "XRT_MACOS_SIGNPOST", false)

static pthread_once_t signpost_once = PTHREAD_ONCE_INIT;
static os_log_t signpost_log;
static os_log_t renderer_log;
static os_log_t presentation_log;
static os_log_t gpu_log;

static os_log_t
log_for_domain(const char *domain)
{
	return strcmp(domain, "renderer") == 0 ? renderer_log :
	       strcmp(domain, "presentation") == 0 ? presentation_log : gpu_log;
}

static void
init_signposts(void)
{
	if (debug_get_bool_option_macos_signpost()) {
		signpost_log = os_log_create("org.freedesktop.monado", OS_LOG_CATEGORY_POINTS_OF_INTEREST);
		renderer_log = os_log_create("org.freedesktop.monado", "renderer");
		presentation_log = os_log_create("org.freedesktop.monado", "presentation");
		gpu_log = os_log_create("org.freedesktop.monado", "gpu");
	}
}

bool
u_gpu_trace_signpost_enabled(void)
{
	pthread_once(&signpost_once, init_signposts);
	return signpost_log != NULL && (os_signpost_enabled(signpost_log) || os_signpost_enabled(renderer_log) ||
	                               os_signpost_enabled(presentation_log) || os_signpost_enabled(gpu_log));
}

void
u_gpu_trace_init_from_environment(void)
{
	pthread_once(&signpost_once, init_signposts);
#ifdef XRT_FEATURE_MACOS_PERFETTO
	u_gpu_trace_perfetto_init_from_environment();
#endif
}

bool
u_gpu_trace_enabled(void)
{
	bool enabled = u_gpu_trace_signpost_enabled();
#ifdef XRT_FEATURE_MACOS_PERFETTO
	enabled = enabled || u_gpu_trace_perfetto_enabled();
#endif
	return enabled;
}

static void
emit(const char *domain,
     const char *event,
     uint64_t scope_id,
     uint64_t owner_scope_id,
     int64_t frame_id,
     uint32_t image_index,
     uint64_t timeline_value,
     int64_t timestamp_ns,
     int64_t duration_ns)
{
	if (domain == NULL || event == NULL || timestamp_ns <= 0) {
		return;
	}
#ifdef XRT_FEATURE_MACOS_PERFETTO
	u_gpu_trace_perfetto_emit(domain, event, scope_id, owner_scope_id, frame_id, image_index, timeline_value,
	                          timestamp_ns, duration_ns);
#endif
	if (!u_gpu_trace_signpost_enabled()) {
		return;
	}
	/* Signposts timestamp the observation now. Retrospective event times and
	 * durations stay in the payload: do not fabricate GPU execution intervals. */
	os_log_t log = strcmp(event, "latency_guard_drain") == 0 ? signpost_log : log_for_domain(domain);
	os_signpost_id_t id = scope_id != 0 ? (os_signpost_id_t)scope_id : OS_SIGNPOST_ID_EXCLUSIVE;
#define EMIT(NAME)                                                                                                      \
	os_signpost_event_emit(log, id, NAME,                                                                   \
	                       "domain=%{public}s event=%{public}s scope=%{public}llu owner=%{public}llu "                      \
	                       "frame_known=%{public}u frame=%{public}llu image=%{public}u timeline=%{public}llu "                \
	                       "observed_ns=%{public}lld duration_ns=%{public}lld",                                             \
	                       domain, event, (unsigned long long)scope_id, (unsigned long long)owner_scope_id,                 \
	                       frame_id >= 0 ? 1u : 0u, frame_id >= 0 ? (unsigned long long)frame_id : 0,                       \
	                       image_index, (unsigned long long)timeline_value,                                                \
	                       (long long)timestamp_ns, (long long)duration_ns)
	if (strcmp(event, "latency_guard_drain") == 0) {
		EMIT("LatencyGuardDrain");
	} else if (strcmp(event, "latency_guard_drop") == 0) {
		EMIT("LatencyGuardDrop");
	} else if (strcmp(event, "compositor_gpu_end_to_display") == 0) {
		EMIT("CompositorGPUToGlassMeasurement");
	} else if (strcmp(domain, "presentation") == 0) {
		EMIT("Presentation");
	} else if (strcmp(domain, "renderer") == 0) {
		EMIT("RendererStage");
	} else if (strcmp(domain, "ownership") == 0) {
		EMIT("ImageOwnership");
	} else if (strcmp(domain, "readiness") == 0) {
		EMIT("Readiness");
	} else {
		EMIT("GPUObservation");
	}
#undef EMIT
}

void
u_gpu_trace_event(const char *domain,
                  const char *event,
                  uint64_t scope_id,
                  int64_t frame_id,
                  uint32_t image_index,
                  uint64_t timeline_value,
                  int64_t timestamp_ns,
                  int64_t duration_ns)
{
	emit(domain, event, scope_id, 0, frame_id, image_index, timeline_value, timestamp_ns, duration_ns);
}

void
u_gpu_trace_owner_event(const char *event,
                        uint64_t scope_id,
                        uint64_t owner_scope_id,
                        int64_t frame_id,
                        uint32_t image_index,
                        uint64_t timeline_value,
                        int64_t timestamp_ns,
                        int64_t duration_ns)
{
	emit("ownership", event, scope_id, owner_scope_id, frame_id, image_index, timeline_value, timestamp_ns,
	     duration_ns);
}

void
u_gpu_trace_present_depth_change(const struct u_gpu_trace_frame *frame,
                                 int64_t previous,
                                 int64_t depth,
                                 int64_t event_ns)
{
	if (frame == NULL || event_ns <= 0 || depth < 0) {
		return;
	}
#ifdef XRT_FEATURE_MACOS_PERFETTO
	u_gpu_trace_perfetto_depth_change(frame, previous, depth, event_ns);
#endif
	if (!u_gpu_trace_signpost_enabled()) {
		return;
	}
	os_signpost_event_emit(signpost_log, frame->scope_id, "PresentLatencyStateChanged",
	                       "domain=presentation event=present_queue_depth_changed scope=%{public}llu owner=0 "
	                       "frame=%{public}lld image=%{public}u timeline=%{public}llu "
	                       "observed_ns=%{public}lld duration_ns=0 previous_depth_known=%{public}u "
	                       "previous_depth=%{public}llu depth=%{public}lld",
	                       (unsigned long long)frame->scope_id, (long long)frame->frame_id, frame->image_index,
	                       (unsigned long long)frame->timeline_value, (long long)event_ns,
	                       previous >= 0 ? 1u : 0u, previous >= 0 ? (unsigned long long)previous : 0,
	                       (long long)depth);
}

bool
u_gpu_trace_signpost_gpu_to_glass_begin(const struct u_gpu_trace_frame *frame, int64_t gpu_end_ns, int64_t observed_ns)
{
	if (frame == NULL || frame->interval_id == 0 || gpu_end_ns <= 0 || observed_ns < gpu_end_ns ||
	    !u_gpu_trace_signpost_enabled()) {
		return false;
	}
	os_signpost_interval_begin(presentation_log, frame->interval_id, "CompositorGPUToGlassObserved",
	                           "scope=%{public}llu frame=%{public}lld image=%{public}u timeline=%{public}llu "
	                           "gpu_end_ns=%{public}lld observed_ns=%{public}lld",
	                           (unsigned long long)frame->scope_id, (long long)frame->frame_id, frame->image_index,
	                           (unsigned long long)frame->timeline_value, (long long)gpu_end_ns, (long long)observed_ns);
	return true;
}

void
u_gpu_trace_signpost_gpu_to_glass_end(const struct u_gpu_trace_frame *frame,
                                      int64_t gpu_end_ns,
                                      int64_t presented_ns,
                                      int64_t observed_ns,
                                      const char *reason)
{
	if (frame == NULL || frame->interval_id == 0 || observed_ns <= 0 || !u_gpu_trace_signpost_enabled()) {
		return;
	}
	bool valid = presented_ns >= gpu_end_ns && gpu_end_ns > 0;
	uint64_t measured_ns = valid ? (uint64_t)(presented_ns - gpu_end_ns) : 0;
	os_signpost_interval_end(presentation_log, frame->interval_id, "CompositorGPUToGlassObserved",
	                         "scope=%{public}llu frame=%{public}lld image=%{public}u timeline=%{public}llu "
	                         "gpu_end_ns=%{public}lld presented_ns=%{public}lld measurement_valid=%{public}u "
	                         "measured_ns=%{public}llu "
	                         "observed_ns=%{public}lld reason=%{public}s",
	                         (unsigned long long)frame->scope_id, (long long)frame->frame_id, frame->image_index,
	                         (unsigned long long)frame->timeline_value, (long long)gpu_end_ns,
	                         (long long)presented_ns, valid ? 1u : 0u, (unsigned long long)measured_ns, (long long)observed_ns,
	                         reason != NULL ? reason : "unknown");
}

// These names are literals because Instruments requires static interval names.
#define CPU_STAGES(OP, LOG, ID, STAGE, ...) \
	do { \
		if (strcmp(STAGE, "previous_fence_wait") == 0) { OP(LOG, ID, "PreviousFenceWait", __VA_ARGS__); } \
		else if (strcmp(STAGE, "previous_gpu_feedback") == 0) { OP(LOG, ID, "PreviousGPUFeedback", __VA_ARGS__); } \
		else if (strcmp(STAGE, "draw_dispatch") == 0) { OP(LOG, ID, "DrawDispatch", __VA_ARGS__); } \
		else if (strcmp(STAGE, "queue_submit") == 0) { OP(LOG, ID, "QueueSubmit", __VA_ARGS__); } \
		else if (strcmp(STAGE, "present_enqueue") == 0) { OP(LOG, ID, "PresentEnqueue", __VA_ARGS__); } \
		else if (strcmp(STAGE, "target_acquire") == 0) { OP(LOG, ID, "TargetAcquire", __VA_ARGS__); } \
		else if (strcmp(STAGE, "slot_acquire") == 0) { OP(LOG, ID, "NextDrawableSlot", __VA_ARGS__); } \
		else if (strcmp(STAGE, "slot_fallback") == 0) { OP(LOG, ID, "NextDrawableFallback", __VA_ARGS__); } \
		else if (strcmp(STAGE, "late_delay") == 0) { OP(LOG, ID, "CompositorLateDelay", __VA_ARGS__); } \
		else { OP(LOG, ID, "NextDrawable", __VA_ARGS__); } \
	} while (0)

uint64_t
u_gpu_trace_cpu_begin(const char *domain, const char *stage, uint64_t scope, int64_t frame, uint64_t timeline)
{
	if (!u_gpu_trace_signpost_enabled()) {
		return 0;
	}
	os_log_t log = log_for_domain(domain);
	if (!os_signpost_enabled(log)) {
		return 0;
	}
	uint64_t id = u_gpu_trace_new_scope();
	CPU_STAGES(os_signpost_interval_begin, log, id, stage,
	           "scope=%{public}llu frame_known=%{public}u frame=%{public}llu timeline=%{public}llu",
	           (unsigned long long)scope, frame >= 0 ? 1u : 0u, frame >= 0 ? (unsigned long long)frame : 0,
	           (unsigned long long)timeline);
	return id;
}

void
u_gpu_trace_cpu_end(const char *domain, const char *stage, uint64_t id, int32_t result)
{
	if (id == 0) {
		return;
	}
	CPU_STAGES(os_signpost_interval_end, log_for_domain(domain), id, stage, "result=%{public}d", result);
}
#undef CPU_STAGES
