<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Common Chromium / Monado GPU traces

Status: 2026-10-12. This is the tracing migration record for the authorised M3
investigation, not a claim that M3's synchronization or performance gates pass.

The macOS CSV instrumentation is fork-local diagnostic code. Chromium's
`TRACE_EVENT` instrumentation already feeds Perfetto. Monado's `u_trace_marker`
interface supports Percetto, a C wrapper producing Perfetto packets, but the
local build does not enable that backend. We should use Perfetto for new
cross-process investigation and gradually move existing diagnostics behind the
same event model. Retain CSV verification during migration; do not replace the
existing hardware evidence with unvalidated new measurements.

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
Chromium's tracing groundwork only: real Dawn/ANGLE SharedImage handoffs,
Monado backend capture, trace joining and performance comparisons remain open.

## Capture backend

The inspected upstream Percetto revision is
`8f4968309af74bd9d4f29b8859dad167ec37e6b8` (outside this repository). Its
`src/percetto.cc` references Linux-only `CLOCK_BOOTTIME` and coarse clock
constants without platform guards, and initializes `perfetto::kSystemBackend`
only. Thus enabling `XRT_HAVE_PERCETTO` is not a tested macOS capture solution.
It is also not installed in this local build.

The official [Perfetto SDK](https://perfetto.dev/docs/instrumentation/tracing-sdk)
supports in-process capture on macOS. The next native step is a macOS-compatible
Percetto integration or a small C-facing SDK adapter behind Monado's tracing
interface, with an explicit capture lifecycle. Keep third-party dependency
sources and generated SDK/build artifacts outside this checkout. Preserve the
existing Linux backend and keep native tracing opt-in.

One common format does not automatically mean one recording service. Validate
collection and joining of the browser's multi-process capture with the native
service/client captures. Do not concatenate protobuf files and assume packet
sequence IDs, interned strings or clocks remain unambiguous.

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
Those SharedImage correctness checks remain M3 work.

## Clocks and interpretation

Monado uses `clock_gettime(CLOCK_MONOTONIC)` via `os_monotonic_get_ns`;
Chromium's macOS `TimeTicks` uses converted Mach absolute time. Metal supplies
another timestamp domain. Include clock snapshots or a measured correlation,
with uncertainty and capture boundaries, before comparing absolute timestamps
across these domains. Do not infer alignment merely because all values are
monotonic. CPU readiness observation and listener delivery latency are distinct
from when the GPU actually completed.

Use native timestamps and clock identifiers in emitted packets. Query nanosecond
durations in the trace processor; convert to microseconds for reporting.
Never wait for GPU timestamps or read pixels synchronously just to trace a
normal frame. Count dropped packets/incomplete slices so loss cannot look like
an improved latency tail.

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
