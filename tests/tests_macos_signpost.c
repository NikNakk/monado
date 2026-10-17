// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "util/u_gpu_trace.h"
#include "util/u_gpu_trace_present.h"
#include "os/os_time.h"
#include <stdio.h>
#include <stdlib.h>

/* Synthetic observations only; this fixture neither opens a GPU nor a headset.
 * An optional output path exercises simultaneous Perfetto/signpost capture. */
int
main(int argc, char **argv)
{
	/* Fixture capture is selected by argv, independently of a developer's
	 * production capture directory inherited from their shell. */
	unsetenv("XRT_MACOS_PERFETTO_TRACE_DIR");
	u_gpu_trace_init_from_environment();
	if (argc > 2 || (argc == 2 && !u_gpu_trace_start(argv[1], 1))) {
		return 1;
	}
	uint64_t scope = u_gpu_trace_new_scope();
	uint64_t owner = u_gpu_trace_new_scope();
	struct u_gpu_trace_present presentation;
	u_gpu_trace_present_init(&presentation, scope);
	for (int64_t frame = 1; frame <= 8; frame++) {
		int64_t now = os_monotonic_get_ns();
		u_gpu_trace_event("readiness", "ready", scope, frame, 0, frame, now, 100);
		u_gpu_trace_owner_event("consumer_claimed", scope, owner, frame, 0, frame, now, 0);
		u_gpu_trace_event("renderer", "queue_submit", scope, frame, 0, frame, now, 100);
		u_gpu_trace_event("presentation", "enqueued", scope, frame, 0, frame, now, 0);
		u_gpu_trace_event("presentation", "metal_complete_observed", scope, frame, 0, frame, now, 100);
		u_gpu_trace_event("presentation", "drawable_presented_observed", scope, frame, 0, frame, now, 0);
		u_gpu_trace_event("presentation", "latency_guard_drain", scope, frame, 0, frame, now, 0);
		u_gpu_trace_event("presentation", "latency_guard_drop", scope, frame, 0, frame, now, 0);
		u_gpu_trace_present_prepare(&presentation, frame, 0, frame);
		u_gpu_trace_present_gpu_end(&presentation, frame, now, os_monotonic_get_ns());
		os_nanosleep(1000000);
		int64_t physical_ns = os_monotonic_get_ns();
		struct u_gpu_trace_frame reported = {scope, 0, frame, 0, frame};
		u_gpu_trace_present_reported(&presentation, &reported, physical_ns, os_monotonic_get_ns(),
		                             frame % 2 + 1);
		const char *stages[] = {"previous_fence_wait", "previous_gpu_feedback", "draw_dispatch",
		                        "queue_submit",        "present_enqueue",       "target_acquire",
		                        "slot_acquire",        "slot_fallback",         "next_drawable"};
		for (unsigned i = 0; i < sizeof(stages) / sizeof(stages[0]); i++) {
			const char *domain = i < 6 ? "renderer" : "presentation";
			uint64_t interval = u_gpu_trace_cpu_begin(domain, stages[i], scope, frame, frame);
			os_nanosleep(100000);
			u_gpu_trace_cpu_end(domain, stages[i], interval, 0);
		}
		os_nanosleep(10000000);
	}
	/* A discarded synthetic frame closes its native interval as cancelled,
	 * without producing a measured GPU-to-display completion. */
	u_gpu_trace_present_prepare(&presentation, 99, 0, 99);
	int64_t cancelled_gpu_ns = os_monotonic_get_ns();
	u_gpu_trace_present_gpu_end(&presentation, 99, cancelled_gpu_ns, os_monotonic_get_ns());
	u_gpu_trace_present_cancel(&presentation, 99, "fixture_cancel");
	u_gpu_trace_present_close(&presentation);
	/* Invalid observations must be harmless in either backend. */
	u_gpu_trace_event(NULL, "invalid", scope, 0, 0, 0, os_monotonic_get_ns(), 0);
	u_gpu_trace_event("fixture", NULL, scope, 0, 0, 0, os_monotonic_get_ns(), 0);
	u_gpu_trace_event("fixture", "invalid", scope, 0, 0, 0, 0, 0);
	if (argc == 2 && !u_gpu_trace_stop()) {
		return 2;
	}
	if (u_gpu_trace_enabled() != u_gpu_trace_signpost_enabled()) {
		return 3;
	}
	printf("{\"frames\":8,\"observations\":80,\"signpost_enabled\":%s}\n",
	       u_gpu_trace_signpost_enabled() ? "true" : "false");
	return 0;
}
