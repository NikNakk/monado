// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "util/u_gpu_trace.h"
#include "os/os_time.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <mach/mach_time.h>

// Backend fixture, separate from production service/ownership tests. No GPU
// completion is inferred from these synthetic observations.
int
main(int argc, char **argv)
{
	if (argc != 4)
		return 2;
	const char *mode = argv[1];
	const bool tracing = strcmp(mode, "disabled") != 0;
	const bool csv = strcmp(mode, "dual") == 0;
	std::vector<char> csv_buffer(16u * 1024u * 1024u);
	FILE *rows = fopen(argv[3], "w");
	if (!rows)
		return 3;
	setvbuf(rows, csv_buffer.data(), _IOFBF, csv_buffer.size());
	fprintf(rows, "scope_id,frame_id,event_ns,timeline_value,duration_ns\n");
	if (tracing && !u_gpu_trace_start(argv[2], 16))
		return 4;
	if (tracing && u_gpu_trace_start(argv[2], 16))
		return 5;
	uint64_t scopes[] = {u_gpu_trace_new_scope(), u_gpu_trace_new_scope()};
	std::vector<int64_t> costs;
	mach_timebase_info_data_t timebase;
	mach_timebase_info(&timebase);
	for (int i = 0; i < 10000; ++i) {
		int64_t now = os_monotonic_get_ns();
		uint64_t scope = scopes[i % 2];
		uint64_t start = mach_absolute_time();
		u_gpu_trace_event("fixture", "fixture_observation", scope, i / 2, i % 2, i + 1, now, 100);
		if (csv)
			fprintf(rows, "%llu,%d,%lld,%d,100\n", (unsigned long long)scope, i / 2, (long long)now, i + 1);
		costs.push_back(static_cast<int64_t>(
		    (static_cast<__uint128_t>(mach_absolute_time() - start) * timebase.numer) / timebase.denom));
	}
	if (strcmp(mode, "crash") == 0) {
		os_nanosleep(1000000000LL); // Allow the SDK background writer to drain.
		std::_Exit(0);              // No stop/atexit: retained packets must still be readable.
	}
	// Preserve exact expected rows outside the timed section in Perfetto-only mode.
	// Dual mode is used for timestamp parity; disabled mode benchmarks the guard.
	fclose(rows);
	if (tracing && !u_gpu_trace_stop())
		return 6;
	if (tracing) {
		std::string second = std::string(argv[2]) + ".restart";
		if (!u_gpu_trace_start(second.c_str(), 1))
			return 8;
		u_gpu_trace_event("fixture", "capture_restart", u_gpu_trace_new_scope(), 0, 0, 1, os_monotonic_get_ns(),
		                  0);
		if (!u_gpu_trace_stop())
			return 9;
	}
	if (tracing && strcmp(mode, "dual") == 0) {
		std::string threaded = std::string(argv[2]) + ".threaded";
		if (!u_gpu_trace_start(threaded.c_str(), 4))
			return 10;
		std::vector<std::thread> workers;
		for (int worker = 0; worker < 4; ++worker) {
			uint64_t scope = u_gpu_trace_new_scope();
			workers.emplace_back([scope] {
				for (int frame = 0; frame < 1000; ++frame)
					u_gpu_trace_event("fixture", "threaded_observation", scope, frame, 0, frame + 1,
					                  os_monotonic_get_ns(), 0);
			});
		}
		for (auto &worker : workers)
			worker.join();
		if (!u_gpu_trace_stop())
			return 11;
	}
	if (u_gpu_trace_enabled() || u_gpu_trace_stop())
		return 7;
	std::sort(costs.begin() + 1000, costs.end());
	auto first = costs.begin() + 1000;
	size_t count = costs.end() - first;
	printf("mode=%s samples=%zu p50_ns=%lld p95_ns=%lld p99_ns=%lld\n", mode, count, (long long)first[count / 2],
	       (long long)first[count * 95 / 100], (long long)first[count * 99 / 100]);
	return 0;
}
