// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "util/u_gpu_trace_present.h"
#include "os/os_time.h"
#include <string.h>

static struct u_gpu_trace_present_entry *
lookup(struct u_gpu_trace_present *state, uint64_t timeline)
{
	struct u_gpu_trace_present_entry *entry = &state->entries[timeline % U_GPU_TRACE_PRESENT_CAPACITY];
	return !state->closed && entry->valid && entry->frame.timeline_value == timeline ? entry : NULL;
}

static void
cancel_entry(struct u_gpu_trace_present_entry *entry, const char *reason)
{
	if (!entry->valid || entry->terminal) {
		return;
	}
	if (entry->native_started) {
		u_gpu_trace_signpost_gpu_to_glass_end(&entry->frame, entry->gpu_end_ns, 0, os_monotonic_get_ns(),
		                                      reason);
	}
	entry->native_started = false;
	entry->terminal = true;
}

void
u_gpu_trace_present_init(struct u_gpu_trace_present *state, uint64_t scope_id)
{
	memset(state, 0, sizeof(*state));
	state->scope_id = scope_id;
	state->depth = -1;
}

void
u_gpu_trace_present_prepare(struct u_gpu_trace_present *state, int64_t frame, uint32_t image, uint64_t timeline)
{
	if (state->closed) {
		return;
	}
	struct u_gpu_trace_present_entry *entry = &state->entries[timeline % U_GPU_TRACE_PRESENT_CAPACITY];
	if (entry->valid && entry->frame.timeline_value == timeline) {
		return;
	}
	cancel_entry(entry, "evicted");
	memset(entry, 0, sizeof(*entry));
	entry->frame = (struct u_gpu_trace_frame){state->scope_id, u_gpu_trace_new_scope(), frame, image, timeline};
	entry->valid = true;
	entry->measured_ns = -1;
}

static void
finish(struct u_gpu_trace_present_entry *entry, int64_t observed_ns)
{
	if (entry->gpu_end_ns <= 0 || entry->presented_ns <= 0 || entry->terminal) {
		return;
	}
	if (entry->presented_ns < entry->gpu_end_ns) {
		cancel_entry(entry, "invalid_clock");
		return;
	}
	entry->measured_ns = entry->presented_ns - entry->gpu_end_ns;
	if (entry->native_started) {
		u_gpu_trace_signpost_gpu_to_glass_end(&entry->frame, entry->gpu_end_ns, entry->presented_ns,
		                                      observed_ns, "presented");
	}
	u_gpu_trace_event("presentation", "compositor_gpu_end_to_display", entry->frame.scope_id, entry->frame.frame_id,
	                  entry->frame.image_index, entry->frame.timeline_value, entry->presented_ns,
	                  entry->measured_ns);
	entry->native_started = false;
	entry->terminal = true;
}

void
u_gpu_trace_present_gpu_end(struct u_gpu_trace_present *state,
                            uint64_t timeline,
                            int64_t gpu_end_ns,
                            int64_t observed_ns)
{
	struct u_gpu_trace_present_entry *entry = lookup(state, timeline);
	if (entry == NULL || entry->terminal || entry->gpu_end_ns != 0 || gpu_end_ns <= 0 || observed_ns < gpu_end_ns) {
		return;
	}
	entry->gpu_end_ns = gpu_end_ns;
	if (entry->presented_ns == 0) {
		entry->native_started = u_gpu_trace_signpost_gpu_to_glass_begin(&entry->frame, gpu_end_ns, observed_ns);
	}
	finish(entry, observed_ns);
}

void
u_gpu_trace_present_reported(struct u_gpu_trace_present *state,
                             const struct u_gpu_trace_frame *frame,
                             int64_t presented_ns,
                             int64_t observed_ns,
                             int64_t depth)
{
	if (state->closed) {
		return;
	}
	if (presented_ns > 0 && depth >= 0 && frame->frame_id > 0 &&
	    (uint64_t)frame->frame_id > state->latest_depth_frame) {
		if (state->depth != depth) {
			u_gpu_trace_present_depth_change(frame, state->depth, depth, presented_ns);
			state->depth = depth;
		}
		state->latest_depth_frame = (uint64_t)frame->frame_id;
	}
	struct u_gpu_trace_present_entry *entry = lookup(state, frame->timeline_value);
	if (entry == NULL || entry->terminal || entry->presented_ns != 0) {
		return;
	}
	if (presented_ns <= 0 || observed_ns < presented_ns) {
		cancel_entry(entry, "no_valid_present_time");
		return;
	}
	entry->presented_ns = presented_ns;
	finish(entry, observed_ns);
}

void
u_gpu_trace_present_cancel(struct u_gpu_trace_present *state, uint64_t timeline, const char *reason)
{
	struct u_gpu_trace_present_entry *entry = lookup(state, timeline);
	if (entry != NULL) {
		cancel_entry(entry, reason);
	}
}

void
u_gpu_trace_present_close(struct u_gpu_trace_present *state)
{
	for (unsigned i = 0; i < U_GPU_TRACE_PRESENT_CAPACITY; i++) {
		cancel_entry(&state->entries[i], "target_closed");
	}
	state->closed = true;
}
