<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Common Chromium / Monado GPU traces

Status: 2026-10-12. This records the tracing migration. The completed M3
producer/fence fixes, real-video/pixel fixtures and matched measurements are
recorded in
[the M3 evidence ledger](macos-gpu-sync-chromium.md).

The macOS CSV instrumentation is fork-local diagnostic code. Chromium's
`TRACE_EVENT` instrumentation already feeds Perfetto. Monado's `u_trace_marker`
interface supports Percetto, a C wrapper producing Perfetto packets, but the
local macOS capture now uses a small C-facing official SDK adapter. It is
compiled with `XRT_FEATURE_MACOS_PERFETTO`, independently of the existing
upstream tracing backends, and records category `monado.gpu`. The first readiness,
ownership and presentation-observation hooks are implemented. Retain CSV
verification during migration; do not replace the existing hardware evidence
with unvalidated new measurements.

## Chromium groundwork validated

Chromium commit `8fbb7c43c01` adds per-request Metal completion-wait durations,
ready/timeout outcomes, cancelled-callback status and an explicit no-wait event.
The focused target compiles the production waiter and uses real Metal events
and command queues. All **10 tests pass** (0.998 s in the recorded run), including
three Perfetto SQL tests; GN header dependency checks pass. Initial test-support
dependencies took about nine minutes to build incrementally in the existing
`out/mac-webxr`; the final two-step rebuild took 5.00 s. The browser executable
was not rebuilt for this step.

The SQL tests verify overlapping requests with equal frame indices have distinct
tracks, a late signal cannot produce a second completion after timeout, a
cancelled callback never activates its target, and an empty request is recorded
separately from GPU completion. Perfetto can merge non-overlapping same-name
tracks; this is expected trace-processor behaviour, not a request-ID collision.

An individual test also exported a 108 KiB binary `.pftrace` capture successfully.
It and the build/test logs remain under `/tmp`, outside Git. This validates
Chromium's tracing groundwork only. Native capture, trace joining and offscreen
cost measurements are recorded below; real Dawn/ANGLE SharedImage handoffs and
physical-display comparisons remain open.

## Capture backend

The inspected upstream Percetto revision is
`8f4968309af74bd9d4f29b8859dad167ec37e6b8` (outside this repository). Its
`src/percetto.cc` references Linux-only `CLOCK_BOOTTIME` and coarse clock
constants without platform guards, and initializes `perfetto::kSystemBackend`
only. Thus enabling `XRT_HAVE_PERCETTO` is not a tested macOS capture solution.
It is also not installed in this local build.

The macOS adapter uses the pinned official **Perfetto v58.2 C++ SDK**, with
hidden SDK symbols so loading Monado into another traced process does not
interpose on that process's tracing implementation. SDK sources and tools stay
outside this checkout. The feature defaults off; recording additionally requires
`XRT_MACOS_PERFETTO_TRACE_DIR` or an explicit control-thread start call.

The in-process recorder uses a 16 MiB buffer, background file draining and
flushing requested every 250 ms, and a 256 MiB file cap. It writes unique 0600
files and refuses existing paths. Stop/flush/shutdown and capture-generation
changes occur on control threads; normal events never read GPU timestamps or
wait for GPU completion. A late recording call cannot enter a later capture.
File-cap exhaustion disables recording. SIGKILL or `_Exit` can leave a readable
partial recording; buffered tail loss is possible. Missing or unacknowledged `MonadoCaptureEnd`
marks it as incomplete, never proof of a completed GPU obligation.

Readiness observations share the same source timestamp and duration as the CSV
row. CSV gained a trailing `scope_id`; named-column readers remain compatible.
Image events retain a stable tracker ID and the allocating client's
`owner_scope_id`, including after that client dies and its storage is quarantined.
Presentation worker, scheduled and completed/failed callback observations use
the same API. GPU execution and physical display timestamp migration still
requires hardware/clock validation; those CSV measurements remain available.

### Reproduction

Keep the SDK in a persistent directory outside the checkout: `/tmp` is cleared
on reboot, and a build configured with this option then fails to reconfigure.
The script verifies the pinned archives and skips the download when they are
already present.

```sh
python3 scripts/macos/prepare-perfetto.py --sdk-dir $HOME/Code/perfetto-sdk-v58.2 --tools
cmake -S . -B build/arm64 -DXRT_FEATURE_MACOS_PERFETTO=ON \
  -DXRT_PERFETTO_SDK_DIR=$HOME/Code/perfetto-sdk-v58.2 \
  -DXRT_PERFETTO_TRACE_PROCESSOR=$HOME/Code/perfetto-sdk-v58.2/tools/mac-arm64/trace_processor_shell
cmake --build build/arm64 --parallel
ctest --test-dir build/arm64 --output-on-failure
python3 scripts/macos/run-gpu-sync-service.py --suite --fault missing --perfetto \
  --output /tmp/new-perfetto-service-capture
python3 scripts/macos/check-perfetto.py --service /tmp/new-perfetto-service-capture \
  --processor $HOME/Code/perfetto-sdk-v58.2/tools/mac-arm64/trace_processor_shell
```

For an ordinary run, create the output directory first, then set
`XRT_MACOS_PERFETTO_TRACE_DIR` in the process/LaunchAgent environment. The installed
service is not replaced by these tests. Capture-only mode works without
`PSVR2_TIMING_TRACE`; `run-gpu-sync-service.py --perfetto` deliberately enables
both formats to check parity. The native SQL CTest is registered only when the
backend is compiled and `XRT_PERFETTO_TRACE_PROCESSOR` points to an external tool.

Use `merge-perfetto.py --processor <tool> --output <capture.tar> <native files>
<Chromium file>` for same-host captures. It generates a provenance/clock manifest,
uses the namespace-aware archive importer and rejects unrelatable/dropped events.
Other SDK/platform clock domains require an explicitly calibrated `--manifest`.
Do not concatenate protobuf traces.

## Event and identity model

Use process, session generation, request generation and image identity together.
WebXR frame indices wrap; Monado frame IDs belong to a different namespace.
Neither frame IDs nor raw object addresses alone identify an end-to-end flow.
Record the mapping at each transport boundary rather than equating those IDs.

| Existing instrumentation | Perfetto adaptation |
| --- | --- |
| Chromium `OpenXRSyncTokenSignaled` | CPU command-processing milestone; attach request generation and SharedImage mailbox, distinct from GPU completion. |
| Chromium Metal event listener | Per-request duration, frame index, reduced fence count, ready/timeout outcome; distinct no-wait event. Late signals cannot close a failed wait again. |
| Chromium SharedImage BeginAccess/EndAccess | Mailbox/IOSurface, producer API (Dawn/ANGLE), access mode, expected event value and ownership transition. |
| Monado `client_gpu` CSV in `comp_multi_compositor.c` | Client/session-scoped producer wait and scheduling slices; readiness deferral, submission age and hung classification as events. |
| Monado GPU reuse logs in `comp_swapchain_gpu_reuse.c` | Stable tracker ID, image index, producer/consumer timeline values; claim, submission, completion, quarantine and reclamation events. |
| Monado presentation worker/Metal completion CSVs in `comp_window_macos.m` | Queueing, commit, scheduled/completed callbacks and failure retirement; GPU execution timestamps on separate correlated tracks. |
| Monado vblank/presented CSVs | Physical display timestamps, callback observations and prediction as separate event types; zero timestamps remain unavailable evidence. |
| Harness JSON and service restart checks | Run/scenario/seed/config metadata, PID lifecycle, restart and replacement-client events. Preserve compact assertion summaries. |

The new Chromium waiter instrumentation uses process-scoped, monotonically
allocated track IDs instead of the repeating frame index. It distinguishes
`OpenXRMetalSharedEventNoWait` from a measured GPU wait. No-wait success is
permitted only when the caller has established there is no outstanding write
obligation; it must not mask a missing mailbox or a consumed unresolved fence.
Those SharedImage correctness checks pass in the completed M3 record.

The committed burst follow-up `d80f86dc0` requests a separate 4 MiB shared
buffer between SDK writers and the in-process tracing service, and removes the
ignored flush-period setting. The recording buffer remains 16 MiB by default.
The final M3 service/runtime are rebuilt with this fix; their joined captures
pass loss/discard/overwrite checks. The standalone Perfetto regression passes
with the current backend.

## Clocks and interpretation

Monado's `clock_gettime(CLOCK_MONOTONIC)` includes accumulated sleep on this
Mac; Chromium's SDK timestamps use Mach uptime. The initial concurrent join
exposed a **27,106.684 s** difference. A superficially successful archive merge
was therefore not sufficient clock validation.

Native source timestamps now use namespaced global clock **`0x4d4f4e00`**;
builtin IDs used internally by the macOS SDK must not be reused for them.
Start/end snapshots map that clock to Mach absolute nanoseconds (`MONOTONIC_RAW`,
ID 5) and the SDK's Mach-based `BOOTTIME` alias (ID 6). Correlation events record
both readings and an uncertainty floor of 1 µs plus half the bracket, covering
the native clock's microsecond quantization. Boot-session provenance is recorded.
The merge manifest selects Mach time and explicitly relates the pinned SDKs'
ID-6 aliases to it; automatic graph discovery alone dropped one SDK metadata
packet. With that relation, strict validation reports no dropped events.

A concurrent real Monado service/Chromium Metal test archive contains **4,941
native events and six Chromium events**; their time ranges overlap on the
correct shared timeline. This validates capture/clock/namespace joining, not a
SharedImage pixel handoff. Caller-supplied provenance is still needed for a
Chromium capture which lacks native boot metadata. Native `event_ns` arguments
preserve the original CLOCK_MONOTONIC value exactly, even when trace `ts` is
normalized to Mach time. CPU observation latency remains separate from actual
GPU execution or physical display latency.

### Native validation and measured cost

- Native capture fixture: 10,000 exact CSV-matching observations and 10,000
  duration slices; four concurrent writers contribute 4,000 additional events.
  Capture restart and readable abrupt-exit partial traces pass SQL checks.
- Real service missing-signal run: **1,865** readiness/scheduling observations
  match CSV exactly. Ownership IDs map to all three client scopes; 311 producer
  obligations/claims/releases, three quarantined objects and two reclaimed
  objects are recorded. Session loss 516.389 ms; 29 healthy fresh latches during
  the fault, 55 replacement latches, one service start, failed storage retained.
- Real disconnect run: SIGKILL exit -9, recovery to replacement first frame
  181.379 ms, eight healthy fresh latches during recovery, 56 replacement latches,
  one service start. Paired observations match exactly. This run preceded the
  final owner-ID addition; the final missing-signal run covers that addition.
- Feature-on incremental build and all **69 native CTests PASS**, 38.00 s.
  The default feature-off build passes **68/68 CTests**, 31.48 s, and has
  no SDK symbols in the runtime. Linux CI and physical presentation hooks are not
  hardware-validated by these offscreen runs.

The API-cost probe uses 10,000 synthetic observations with a 100 ns duration
annotation, excludes 1,000 warmup calls, uses Mach ticks for timing, buffers CSV
at 16 MiB, and alternates disabled / Perfetto / Perfetto / disabled / dual / dual /
disabled. It times instrumentation only, excluding startup, stop and export.
Disabled median is below the 41.7 ns tick resolution; p99 is one tick.
Perfetto-only p50 **1.166–1.833 µs**, p95 3.166–5.083 µs, p99 4.083–6.291 µs.
Dual-format p50 **1.125–1.250 µs**, p95 3.000–3.125 µs, p99 3.458–3.833 µs.
Scheduling/cache noise prevents treating the lower dual result as an improvement.
These are per-call CPU costs, not normal-frame or headset latency comparisons.
Tracing remains disabled by default, and CSV writers are retained pending the
remaining physical-display measurement migration.

### Normal offscreen frame-path comparison

An A–B–B–A comparison also submits **1,800 frames per run** (~30 s each), with
30 warmup frames excluded. All runs use the same compiled feature-on code,
buffered CSV, simulated 60 Hz HMD and real Vulkan output. A leaves Perfetto
recording disabled; B additionally records native service and client events.
Readback/per-image logs are disabled, Low Power Mode is off and thermal state
is nominal (0) in every run. This measures enabling capture beside CSV, not the
cost of changing GPU synchronization or a physical-display comparison.

Averaging per-run p50 values, microseconds:

| Metric | CSV control A | CSV + Perfetto B | Difference |
| --- | ---: | ---: | ---: |
| xrEndFrame | 136 | 151 | +15 (+11.03%) |
| Readiness wait | 479.5 | 449.5 | −30 (−6.26%) |
| Vulkan CPU submission | 128 | 127 | −1 (−0.78%) |
| Entry to fresh latch | 14369 | 14593.5 | +224.5 (+1.56%) |

Throughput stays **59.986–60.001 Hz**. Capture adds measurable CPU work; the
lower readiness/submission samples include scheduling noise and are not claimed
as improvements. Entry-to-latch is an offscreen scheduling measure, not display
latency. The final export-footer acknowledgement addition is outside these
normal-frame timings. Native tracing remains opt-in and CSV retirement awaits
physical-display measurement parity.

## Validation and migration sequence

1. Exercise real Metal completion, absent signals, duplicate/max-value fences,
   already-completed paused frames and late completion after timeout. Validate
   the emitted slices and outcomes with Chromium's `TestTraceProcessor` SQL.
2. Enable and test a native macOS backend with readiness and ownership events,
   keeping capture lifecycle work outside completion handlers and frame loops.
3. Record paired CSV/Perfetto fixtures and compare event counts, durations,
   frame/image mapping and failure outcomes. Keep expected failures distinct
   from successful empty/no-write paths.
4. Validate clock correlation and capture joining with multiple clients, session
   restart and process exit. Exercise real Dawn/ANGLE/Monado image handoffs.
5. Measure tracing disabled, Perfetto-only and dual-capture overhead with the
   same workload and A–B–B–A ordering. Move regression queries to Perfetto before
   removing redundant CSV writers; retain concise summaries in tracked docs.

Raw `.pftrace`, CSV, logs and SQL outputs belong under ignored `doc/results/`
or a temporary capture directory. Commit instrumentation, SQL/test assertions,
capture tooling and short result summaries, never raw recordings.

## Browser-wide capture scope correction (M5, 2026-10-12)

Use the browser DevTools endpoint for cross-process Chromium tracing. Page
sessions apply a process filter that excludes the XR utility process; its
unbound startup writers expire after 30 s and commit to invalid buffer zero.
The first extended fixture therefore had 273 global discarded chunks despite
ample central-buffer capacity and clean Monado captures. It was rejected.

The retained fixture correction (`4fed5fb51`) uses browser-level start/end/export
and verifies real render-loop frame-data and composition events. Long ANGLE and
Dawn captures and GPU/decoder replacement pass strict joined-trace loss and
ownership checks. Page commands still use the page endpoint. The native SDK's
persistent location, hidden symbols, shared-memory hint and file-write policy
are unchanged. A namespace-isolation trial did not resolve the issue and was
removed. No loss checks were relaxed. See the [M5 evidence and remaining
limits](macos-gpu-sync-cross-stack.md).
