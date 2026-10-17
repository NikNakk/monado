// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include "util/u_gpu_trace.h"
#include "util/u_gpu_trace_backend.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include <perfetto.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unistd.h>
#include <mach/mach_time.h>

PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("monado.gpu").SetDescription("GPU readiness, ownership and presentation"));
PERFETTO_TRACK_EVENT_STATIC_STORAGE();

namespace {
std::atomic<uint64_t> capture_state{0};
uint64_t capture_generation = 0; // Guarded by capture_lock.
std::shared_mutex capture_lock;
std::unique_ptr<perfetto::TracingSession> capture_session;
int output_fd = -1;
bool initialized = false;
bool shutdown_registered = false;
std::once_flag environment_once;

// SDKs on macOS label Mach uptime as builtin clocks 3/6. Native Monado's
// clock is Mach uptime on macOS too, but other platforms differ, so a
// namespaced global clock keeps the domain explicit. IDs 64..127
// would be writer-sequence-local and would need snapshots on every thread.
constexpr uint32_t kNativeClock = 0x4d4f4e00;
perfetto::TraceTimestamp
timestamp(int64_t ns)
{
	return {kNativeClock, static_cast<uint64_t>(ns)};
}


void
clock_correlation()
{
	mach_timebase_info_data_t timebase;
	mach_timebase_info(&timebase);
	const int64_t before = os_monotonic_get_ns();
	const uint64_t mach_ns =
	    static_cast<uint64_t>((static_cast<__uint128_t>(mach_absolute_time()) * timebase.numer) / timebase.denom);
	const int64_t after = os_monotonic_get_ns();
	const int64_t middle = before + (after - before) / 2;
	// Explicit mapping from native monotonic time to Mach absolute nanoseconds.
	// The SDK default trace clock (6) is Mach uptime in the pinned macOS SDK.
	perfetto::TrackEvent::Trace([&](perfetto::TrackEvent::TraceContext ctx) {
		auto packet = ctx.NewTracePacket();
		auto snapshot = packet->set_clock_snapshot();
		auto native = snapshot->add_clocks();
		native->set_clock_id(kNativeClock);
		native->set_timestamp(middle);
		auto mach = snapshot->add_clocks();
		mach->set_clock_id(5);
		mach->set_timestamp(mach_ns);
		auto sdk = snapshot->add_clocks();
		sdk->set_clock_id(6);
		sdk->set_timestamp(mach_ns);
	});
	TRACE_EVENT_INSTANT("monado.gpu", "MonadoClockCorrelation", timestamp(middle), "native_ns", middle,
	                    "native_clock_id", kNativeClock, "mach_absolute_ns", mach_ns, "bracket_ns", after - before,
	                    "uncertainty_ns", 1000 + (after - before + 1) / 2);
}

void
shutdown_capture()
{
	u_gpu_trace_stop();
	std::unique_lock<std::shared_mutex> guard(capture_lock);
	if (initialized)
		perfetto::Tracing::Shutdown();
}
} // namespace

extern "C" bool
u_gpu_trace_start(const char *path, uint32_t buffer_mb)
{
	if (path == nullptr || path[0] == '\0' || buffer_mb == 0 || buffer_mb > 256)
		return false;
	std::unique_lock<std::shared_mutex> guard(capture_lock);
	if (capture_session)
		return false;
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		U_LOG_W("Perfetto capture open failed: %s", path);
		return false;
	}
	if (!initialized) {
		perfetto::TracingInitArgs args;
		args.backends = perfetto::kInProcessBackend;
		args.use_monotonic_clock = true;
		// Writers drop chunks once the shared buffer between them and the
		// in-process service is full. The SDK default overflowed in bursts of
		// a few thousand events; 4 MiB absorbs bursts such as teardown.
		args.shmem_size_hint_kb = 4096;
		perfetto::Tracing::Initialize(args);
		perfetto::TrackEvent::Register();
		initialized = true;
	}
	if (!shutdown_registered) {
		std::atexit(shutdown_capture);
		shutdown_registered = true;
	}
	perfetto::TraceConfig config;
	config.add_buffers()->set_size_kb(buffer_mb * 1024);
	config.set_write_into_file(true);
	// Write-into-file mode flushes on every write period; flush_period_ms is ignored.
	config.set_file_write_period_ms(250);
	config.set_max_file_size_bytes(256u * 1024u * 1024u);
	config.set_flush_timeout_ms(1000);
	config.set_data_source_stop_timeout_ms(1000);
	auto source = config.add_data_sources()->mutable_config();
	source->set_name("track_event");
	perfetto::protos::gen::TrackEventConfig events;
	events.add_disabled_categories("*");
	events.add_enabled_categories("monado.gpu");
	source->set_track_event_config_raw(events.SerializeAsString());
	capture_session = perfetto::Tracing::NewTrace(perfetto::kInProcessBackend);
	const uint64_t active_state = (++capture_generation << 1) | 1;
	capture_session->SetOnStopCallback([active_state] {
		uint64_t expected = active_state;
		capture_state.compare_exchange_strong(expected, active_state & ~UINT64_C(1), std::memory_order_acq_rel);
	});
	capture_session->Setup(config, fd);
	capture_session->StartBlocking();
	output_fd = fd;
	capture_state.store(active_state, std::memory_order_release);
	clock_correlation();
	char boot_id[128] = {};
	size_t boot_length = sizeof(boot_id);
	if (sysctlbyname("kern.bootsessionuuid", boot_id, &boot_length, nullptr, 0) != 0)
		boot_id[0] = '\0';
	TRACE_EVENT_INSTANT("monado.gpu", "MonadoCaptureContext", "boot_id", boot_id, "pid", getpid(),
	                    "native_clock_id", kNativeClock, "native_clock_source", "CLOCK_UPTIME_RAW", "sdk_timeline",
	                    "Mach absolute nanoseconds");
	U_LOG_I("Perfetto capture started: %s (buffer %u MiB)", path, buffer_mb);
	return true;
}

extern "C" bool
u_gpu_trace_stop(void)
{
	std::unique_lock<std::shared_mutex> guard(capture_lock);
	if (!capture_session)
		return false;
	capture_state.fetch_and(~UINT64_C(1), std::memory_order_acq_rel);
	clock_correlation();
	perfetto::TrackEvent::Flush();
	const bool flushed = capture_session->FlushBlocking(1000);
	TRACE_EVENT_INSTANT("monado.gpu", "MonadoCaptureEnd", timestamp(os_monotonic_get_ns()), "flush_acknowledged",
	                    flushed);
	perfetto::TrackEvent::Flush();
	capture_session->StopBlocking();
	capture_session.reset();
	struct stat info{};
	const bool inspected = fstat(output_fd, &info) == 0;
	const bool closed = close(output_fd) == 0;
	output_fd = -1;
	U_LOG_I("Perfetto capture stopped: bytes=%lld flushed=%d closed=%d", (long long)info.st_size, flushed, closed);
	return flushed && inspected && closed;
}

extern "C" bool
u_gpu_trace_perfetto_enabled(void)
{
	return (capture_state.load(std::memory_order_acquire) & 1) != 0;
}

extern "C" void
u_gpu_trace_perfetto_init_from_environment(void)
{
	std::call_once(environment_once, [] {
		const char *directory = std::getenv("XRT_MACOS_PERFETTO_TRACE_DIR");
		if (directory == nullptr || directory[0] == '\0')
			return;
		std::string path = std::string(directory) + "/monado_" + std::to_string(getpid()) + "_" +
		                   std::to_string(os_monotonic_get_ns()) + ".pftrace";
		(void)u_gpu_trace_start(path.c_str(), 16);
	});
}

extern "C" void
u_gpu_trace_perfetto_emit(const char *domain,
                          const char *event,
                          uint64_t scope_id,
                          uint64_t owner_scope_id,
                          int64_t frame_id,
                          uint32_t image_index,
                          uint64_t timeline_value,
                          int64_t timestamp_ns,
                          int64_t duration_ns)
{
	const uint64_t generation = capture_state.load(std::memory_order_acquire);
	if ((generation & 1) == 0 || timestamp_ns <= 0 || domain == nullptr || event == nullptr)
		return;
	std::shared_lock<std::shared_mutex> guard(capture_lock);
	// A writer waiting across stop/start must not enter the next capture.
	if (capture_state.load(std::memory_order_acquire) != generation)
		return;
	auto track = perfetto::NamedTrack(perfetto::DynamicString(domain), scope_id);
	TRACE_EVENT_INSTANT("monado.gpu", perfetto::DynamicString(event), track, timestamp(timestamp_ns), "record_type",
	                    "observation", "domain", domain, "scope_id", scope_id, "owner_scope_id", owner_scope_id,
	                    "frame_id", frame_id, "image_index", image_index, "timeline_value", timeline_value,
	                    "event_ns", timestamp_ns, "duration_ns", duration_ns);
	if (duration_ns > 0 && duration_ns <= timestamp_ns) {
		// Unique request tracks prevent overlapping retrospective intervals sharing
		// a stack. Perfetto may compact sequential non-overlapping same-name tracks.
		auto interval = perfetto::NamedTrack(perfetto::DynamicString(event), u_gpu_trace_new_scope());
		TRACE_EVENT_BEGIN("monado.gpu", perfetto::DynamicString(event), interval,
		                  timestamp(timestamp_ns - duration_ns), "record_type", "duration", "domain", domain,
		                  "scope_id", scope_id, "frame_id", frame_id, "owner_scope_id", owner_scope_id,
		                  "image_index", image_index, "timeline_value", timeline_value, "event_ns",
		                  timestamp_ns, "duration_ns", duration_ns);
		TRACE_EVENT_END("monado.gpu", interval, timestamp(timestamp_ns));
	}
}

extern "C" void
u_gpu_trace_perfetto_depth_change(const struct u_gpu_trace_frame *frame,
                                  int64_t previous,
                                  int64_t depth,
                                  int64_t event_ns)
{
	const uint64_t generation = capture_state.load(std::memory_order_acquire);
	if ((generation & 1) == 0 || event_ns <= 0)
		return;
	std::shared_lock<std::shared_mutex> guard(capture_lock);
	if (capture_state.load(std::memory_order_acquire) != generation)
		return;
	auto track = perfetto::NamedTrack("presentation", frame->scope_id);
	TRACE_EVENT_INSTANT("monado.gpu", "present_queue_depth_changed", track, timestamp(event_ns), "record_type",
	                    "observation", "domain", "presentation", "scope_id", frame->scope_id, "owner_scope_id", 0,
	                    "frame_id", frame->frame_id, "image_index", frame->image_index, "timeline_value",
	                    frame->timeline_value, "event_ns", event_ns, "duration_ns", 0, "previous_depth_known",
	                    previous >= 0, "previous_depth", previous >= 0 ? previous : 0, "depth", depth);
}
