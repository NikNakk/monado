<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Monado GPU service and headset tests — 2026-10-11

Current native milestone status is recorded in the [M1–M2 completion report](macos-gpu-sync-completion.md). The results below remain evidence for their recorded source versions.

The actual native Metal OpenXR → IPC/XPC → Vulkan/MoltenVK compositor path now
runs without a headset. Missing-signal and client-kill recovery tests pass,
including concurrent healthy rendering, replacement-client rendering and
quarantine. The initial [first-run checkpoint](macos-gpu-sync-first-run.md) is
historical; this follow-up supersedes its service-startup and recovery findings.
Full M1/M2 completion is still not claimed: device-loss/in-flight teardown,
per-client allocation limits, production deadline policy, thermal stress and
sustained physical presentation remain outstanding. The short hardware follow-up
below now verifies actual PS VR2 presentation during both fault scenarios.

## Implementation and reproducibility

Implementation commit: **`ba46ccd91`**. Review diff: `followup-code.patch` in the
evidence directory. Overlapping status documents are updated in the working
tree; their isolated task diff is `status-doc-updates.patch`.

Working branch remains `codex/macos-gpu-sync-harness`, following `dfff38184`,
`1c7b515d6` and `59f520813`. Baseline synchronization code is pinned to
`805316bebe976256a16c591ee135a5422ffe73a3`, with the initial integration checkout's
pre-existing multi-compositor edits retained in both comparisons. Existing
unrelated working-tree changes remain separate from scoped follow-up commits.

`XRT_COMPOSITOR_FORCE_DEBUG_IMAGE=1` explicitly selects Monado's existing
`debug_image` target. This runs the normal renderer into ordinary offscreen
images. Their final layout is GENERAL rather than WSI PRESENT_SRC; the target
also exposes its actual transfer-source capability to existing opt-in readback
diagnostics. Normal headset target selection is unchanged.

The new runner creates a unique temporary launchd service, XPC Mach service,
Unix socket and configuration directory. `SIMULATED_ENABLE=1` selects a simulated
HMD even with the physical headset connected. `XRT_NO_STDIN=1` prevents launchd's
stdin EOF from terminating the daemon. It does not replace, stop or restart the
installed Monado service. The test requires a macOS GUI launchd domain and real
Metal/MoltenVK access; sandbox-restricted runs cannot substitute for host runs.

The small loaderless Metal OpenXR client reuses runtime negotiation from the
existing loaderless probe. It creates a real Metal session and 64×64 swapchain,
acquires/waits/releases images, clears them on the GPU and submits quad layers.
A forwarding command-queue wrapper suppresses only the completion event signal
at frame 11. It still commits all texture writes and never submits an unsignalled
GPU wait. The faulty client is an overlay, keeping the healthy main session
visible. The kill scenario terminates that client after its fault marker.

The missing-signal run reproduced a new notification defect: XRT_SESSION_EVENT_LOST
sets the OpenXR session's error state, but alone does not emit a LOSS_PENDING
state event to an idle client. The readiness worker now emits LOSS_PENDING with
a monotonic timestamp before LOST. This permits an idle polling client to learn
of failure after the bounded obligation timeout. Initial unsuccessful runs are
preserved in the evidence directory.

An existing optional reuse diagnostic now records the selected, already-ready
producer timeline wait as it is inserted into VkSubmitInfo. The zero-time gate,
exact imported payload/value, ALL_COMMANDS wait and consumer reuse timeline
remain mandatory. Successful GPU output checks use existing compositor readback
at its fence-completion point; no new normal-path readback was added.

```sh
cmake --build build/arm64 --parallel
python3 scripts/macos/run-gpu-sync-service.py --output <new-directory> --suite
python3 scripts/macos/run-gpu-sync-service.py --output <new-directory> --suite --fault disconnect
ctest --test-dir build/arm64 --output-on-failure -R tests_macos_service_gpu_sync
python3 scripts/macos/run-gpu-sync-service.py --output <new-directory> --benchmark \
  --client build/arm64/tests/tests_macos_service_gpu_sync normal 600
python3 scripts/macos/summarize-gpu-sync-service.py <comparison-directory>
```

The CTest entries are opt-in: configure with
`-DXRT_MACOS_SERVICE_GPU_TESTS=ON -DXRT_FEATURE_MACOS_TIMING_DIAGNOSTICS=ON`.
They bootstrap a launchd job in the user's GUI domain and need a GPU, and their
verification reads the timing-diagnostics traces, so default and CI builds do
not register them.

Use a new result directory per manual run. CTest uses unique per-run directories
under the existing build tree. Both scenarios have a 60-second test deadline,
finite client/bootstrap deadlines and automatic removal of their temporary
launchd registration. Benchmark mode temporarily uses `caffeinate -i`, disables
readback/per-image logging and retains fully buffered timing traces.

## Recovery evidence

Evidence: [macos-gpu-service-20261011](results/macos-gpu-service-20261011/).
`recovery-verified` and `disconnect` are the first complete, timestamp-correlated
runs. Earlier `recovery-suite` used the C++ steady clock for the client and
CLOCK_MONOTONIC for service traces: their epochs differ on macOS, so that run
cannot support cross-process timing joins. The client now explicitly uses
CLOCK_MONOTONIC, matching Monado.

| Check | Missing signal | Client killed with missing signal |
|---|---|---|
| Healthy client completed | 240 frames | 240 frames |
| Replacement client completed | 60 frames | 60 frames |
| Recovery endpoint | Session-loss observation | Replacement's first submitted frame |
| Injection to endpoint | 511.249 ms | 249.424 ms |
| New healthy frames consumed during interval | 26 | 14 |
| Largest gap between those healthy latches | 33.348 ms | 16.682 ms |
| Service starts | 1 | 1 |
| Failed storage retained | Yes | Yes |
| GPU output samples matched | 6 | 5 |

At 60 Hz, a 33 ms gap represents a missed fresh frame; these results do not
claim perfect cadence. The disconnect run's pre-fault maximum was 33.345 ms.
The service frame-map traces establish that healthy frames were actually
latched for GPU composition during failure, rather than merely accepted by IPC.
GPU output has the expected sRGB-encoded color (137,188,224,255) in each eye.
The first blank output sample is initialization, not a client-content match.

The failed swapchain retains 49,536 bytes of Vulkan-reported storage and its
unsignalled producer references. Its quarantine entry has no matching reclaim.
Healthy and replacement storage is reclaimed after both ownership obligations
complete. The process exits normally when the isolated test is removed;
retention is bounded by the existing global tracker limits. Actual repeated
real failed-client cap exhaustion is not tested; the 256-object cap regression
remains mocked. There is still no per-client quota or pre-import allocation gate.

The final CTest suites additionally verify readback matches, imported producer
wait diagnostics, one service start, healthy progress during the fault,
replacement success and absence of failed-storage reclamation. Full suite
results and build output are retained as `ctest-final.txt` and `final-build.txt`.
Final full suite: **65/65 passed** (35.78 s), including both service scenarios.
The strengthened service checks subsequently passed 2/2 again (8.95 s), with
replacement fresh latches and both-eye pixels explicitly verified. The repeat
missing-signal recovery was 533.670 ms with 25 fresh healthy frames and 50
replacement latches; disconnect recovery to first replacement frame was
170.623 ms with 9 fresh healthy frames and 44 replacement latches. These
repeats retain the same failure quarantine behavior. Their complete
traces are retained in `final-missing` and `final-disconnect`.
These offscreen runs did not include the headset. The subsequent hardware results
are recorded below. No Linux CI run, Chromium change or Wine/DXMT change was performed.

## Normal-path comparison

Four 600-frame runs used A–B–B–A, with 30 warmup frames excluded. A uses the pinned
original production synchronization sources; B uses the fixes. The offscreen
selection/layout, test client and tracing setup are identical. Original source
files were temporarily substituted, then restored and all targets rebuilt in a
finally block. The comparison script and baseline fixture/source diff are
included in the evidence directory for inspection.

Workload: simulated HMD at 60 Hz, real compositor output 1280×720, one 64×64
Metal quad. This is a low-load native service workload, not representative
120 Hz headset imagery. Submission timings are CPU timings; readiness includes
GPU execution, worker wake and CPU observation. GPU execution and notification
processing are not separately measured. Frame-to-latch excludes consumer GPU
completion and physical presentation. Client CLOCK_MONOTONIC timestamps permit
service joins without clock-offset estimation.

Every run completed 600 frames at 59.988–59.998 Hz. Results in microseconds:

| Metric (p50 / p95 / p99 / max) | A0 | B0 | B1 | A1 |
|---|---|---|---|---|
| xrEndFrame call | 135 / 264 / 390 / 433 | 131 / 219 / 320 / 396 | 125 / 193 / 279 / 362 | 121 / 220 / 274 / 392 |
| Readiness worker wait | 354 / 824 / 1101 / 1517 | 405 / 618 / 698 / 1213 | 375 / 598 / 671 / 774 | 296 / 602 / 770 / 3081 |
| Vulkan queue submission | 159 / 220 / 255 / 278 | 158 / 214 / 234 / 303 | 146 / 198 / 222 / 266 | 140 / 216 / 243 / 265 |
| Renderer dispatch CPU time | 275 / 362 / 412 / 433 | 273 / 356 / 389 / 476 | 249 / 337 / 379 / 431 | 241 / 363 / 403 / 433 |
| Previous consumer fence wait | 1 / 2 / 3 / 7 | 1 / 3 / 3 / 7 | 1 / 2 / 3 / 8 | 1 / 2 / 3 / 7 |
| xrEndFrame entry to first fresh latch | 16107 / 16853 / 17263 / 17645 | 16110 / 16577 / 17156 / 17383 | 16154 / 16466 / 16972 / 17255 | 1679 / 16421 / 17149 / 17306 |

Averaging per-run medians, Vulkan submission increases from 149.5 to 152 µs:
+2.5 µs (+1.67%). The modified path includes the mandatory already-satisfied
producer semaphore wait and checks. Native paired empty-submit controls in the
initial report measure about 0.5 µs overhead; that control is not interchangeable
with this full service cost. Readiness median increases from 325 to 390 µs:
+65 µs (+20%), while average p95 falls from 713 to 608 µs and p99 from 935.5 to
684.5 µs. These differences are reported, not attributed solely to the code.
The xrEndFrame mean median is unchanged at 128 µs.

Latch medians vary dramatically even between A0 and A1. Modified median latch
latency is about one frame; A1 frequently arrives in an earlier scheduling phase.
This phase variation prevents a clean end-to-end non-regression claim. Reused
latches (including startup/warmup) were A0=88, B0=87, B1=72, A1=140. Reuse is not
itself a failed deadline: a completed frame may be reprojected safely. Explicit
production readiness deadlines/missed-deadline classification are not yet
implemented, so those counts cannot be relabelled as deadline metrics.

Telemetry: Apple M5 MacBook Air, macOS 26.7.1; thermal state nominal and Low Power
Mode false at each run's start. End thermal state was inadvertently omitted from
these four result records; that output is corrected for subsequent runs.
Power/settings/background CPU snapshots are present for later comparison runs;
the first runs predate that addition. No powermetrics CPU/GPU power/frequency
samples were collected. No Game Mode or priority workaround was enabled.
Therefore this comparison supports measured low-load costs and throughput,
**not a thermal-controlled end-to-end performance guarantee**.

## Exit criteria at this checkpoint (superseded)

| Criterion | Status | Current evidence or remaining limit |
|---|---|---|
| M1.1 | PASS | Native two-process GPU harness. |
| M1.2 | PASS | Exact observed-value gate on consuming reads. |
| M1.3 | PASS | Imported, already-satisfied Vulkan wait. |
| M1.4 | PASS | Native full-pattern GPU pixel validation. |
| M1.5 | PASS | Missing/future refused; delayed deferred safely. |
| M1.6 | PASS | Consumer completion before reuse. |
| M1.7 | NOT VERIFIED | In-flight producer termination/device-loss coverage incomplete. |
| M1.8 | PASS | Deterministic structured scenarios and repeat runs; coverage limits retained. |
| M1.9 | PASS | Existing fixtures, renderer, target, XPC and CTest reused. |
| M1.10 | PASS | Native latency and paired satisfied-wait cost. |
| M2.1 | PASS | Real Metal OpenXR service rendering with both synchronization stages. |
| M2.2 | PASS | Unready frame excluded; selected consuming submissions recheck and wait on ready values. |
| M2.3 | PASS | Failed-wait regression and real missing-signal session failure. |
| M2.4 | PASS | No added frame-loop wait; bounded readiness worker; existing backpressure retained. |
| M2.5 | PASS | Real repeated native service acquire/wait/release, consumer timeline and quarantine. |
| M2.6 | NOT VERIFIED | Other teardown waits/device-loss lifetime remain unaudited/unproven. |
| M2.7 | NOT VERIFIED | Global caps tested; per-client/pre-allocation limits incomplete. |
| M2.8 | PASS | Replacement renders in same healthy service after timeout and client kill. |
| M2.9 | PASS | Healthy client latched offscreen and physically presented during both failure/recovery intervals. |
| M2.10 | NOT VERIFIED | Offscreen and headset A–B–B–A recorded; typical headset cadence unchanged but rare-gap variation prevents strict non-regression conclusion. |
| M2.11 | PASS | Available deterministic native/mock/service tests pass; hardware tests listed below. |
| M2.12 | PASS | Commands, source scope, traces, timing and criteria documented. |

PASS applies to the tested native/offscreen and short PS VR2 service paths,
not arbitrary malicious producers, sustained presentation or device loss. The global cap and no-future-wait
contract do not authorize premature reclamation when completion never arrives.

## Remaining work before Chromium or Wine

1. Audit all compositor/IPC/device teardown waits and validate actual submitted
   GPU work during producer termination without deliberately wedging the GPU.
2. Add proportionate per-client accounting and an allocation gate before import;
   validate repeated real failed sessions against the global and client budgets.
3. Implement production deadline selection (predicted presentation minus the
   configurable 6 ms margin), retaining safe completed frames and ownership.
4. Repeat service timing with controlled submission phase, complete begin/end
   telemetry and optional supported power counters; quantify GPU completion
   separately from delayed CPU observation, including sustained-load resilience.
5. User-worn PS VR2 visual regression and sustained-load validation. Short automated
   acquire/release, timeout, kill, replacement and concurrent physical presentation
   checks now pass; visual comfort and long-session resilience remain unverified.

Keep the headset-free suites as routine regression coverage. Physical runs
should verify the presentation boundary rather than replacing these tests.
Chromium/Dawn/ANGLE and genuine x86-64 Wine/DXMT work remain deferred until
separately authorized.

## PS VR2 hardware follow-up — 2026-10-11

Tested HEAD: **`8ba2bf496`**, synchronization implementation **`ba46ccd91`**,
plus the uncommitted `--headset` runner change and the pre-existing integration
working tree. Exact HEAD and complete working diff are retained in
[macos-gpu-headset-20261011](results/macos-gpu-headset-20261011/). The hardware
run was explicitly requested by the user. No further production synchronization
change was required to pass these tests.

The physical target detected PS VR2 at **4000×2040, 120 Hz**, using the normal
CA/Metal presentation path. The runner uses `SIMULATED_ENABLE=0`, disables forced
`debug_image`, keeps camera streams off, prevents display sleep, and uses the
service compositor (`XRT_MACOS_CLIENT_COMPOSITOR=0`). This tests the native Metal
client → XPC/IPC → Vulkan → Metal display path; it does not test client-hosted
Game Mode compositing. Each client clears a 64×64 GPU texture and submits a quad.
Diagnostic pixel readbacks and per-image logging are enabled, so this is a
correctness run rather than a production performance comparison.

The older installed LaunchAgent was temporarily removed to release USB ownership.
Both isolated services were then run serially with finite deadlines. A finally
block restored the original registration and started its original executable;
launchctl confirms it is running again. Its plist and environment were not
modified. `run-headset-tests.py` records that orchestration; invoking `--headset`
directly requires the operator to ensure exclusive USB ownership and restore
any installed service they stop. The default CTests remain headset-free.

| Check | Missing signal | Killed client with missing signal |
|---|---|---|
| Result | PASS | PASS |
| Healthy / replacement client frames | 240 / 60 | 240 / 60 |
| Injection to session loss / replacement first frame | 501.015 ms | 164.964 ms |
| Fresh healthy latches during fault interval | 60 | 19 |
| Fresh healthy frames physically presented inside interval | 58 | 18 |
| Maximum gap between those physical frames | 8.349 ms | 8.342 ms |
| Replacement fresh frames physically presented | 56 | 58 |
| Successful presentation GPU completions | 240 | 242 |
| Presentation GPU errors | 0 | 0 |
| Both-eye matching GPU output samples | 5 | 5 |
| Service starts / failed swapchain reclaimed | 1 / no | 1 / no |

Physical counts join fresh client latches to the same compositor frame ID in
CAMetalDrawable presentation callbacks, requiring a nonzero presented timestamp.
For the healthy fault-window count, the presented timestamp must also fall inside
the fault interval. Zero-timestamp startup callbacks are excluded (two per run).
Each presentation command buffer completed with status 4 and a shared-event wait;
none used a no-event fallback. The suppressed value 22 was never submitted as a
producer Vulkan dependency. Failed storage stayed quarantined; healthy and
replacement storage was reclaimed after completion.

After discarding the first 30 nonzero presentations, cadence p50/p95/p99/max
was **8.342 / 8.343 / 16.683 / 41.707 ms** for missing-signal and
**8.342 / 8.343 / 8.343 / 8.349 ms** for disconnect. The 41.707 ms gap lies outside
the bounded fault interval, between compositor frames 193 and 194. Startup also
had a 25.025 ms gap in the first run. These short runs therefore establish healthy
progress through failure and recovery, not perfect presentation cadence.

All client thermal records begin/end at nominal state 0, with Low Power Mode
false. No GPU power/frequency samples or sustained thermal workload were collected.
The user was **not wearing the headset**, so visual appearance, moving-head
judder and comfort are not verified. No in-flight producer kill/device-loss claim
is added: the kill occurs after the deliberately omitted completion signal.

The build passed. The default offscreen service suites passed **2/2 (8.97 s)**
after adding the hardware option; the previous full **65/65** result remains the
latest full-suite run. Raw traces, client/service logs, machine summaries,
build output, restored-service state and working-tree/source scope are retained.

## Matched presentation comparison

A subsequent four-run PS VR2 A–B–B–A capture with readback/per-image logging off
shows similar typical cadence and median latency, but slightly worse rare gaps
in the modified samples (0.276% versus 0.234%; maximum 33 ms versus 25 ms).
Background load varied, so causation and strict tail non-regression remain
unresolved. See [the matched comparison](macos-gpu-sync-headset-comparison.md)
for exact variants, physical timestamp joins, full distributions and scope.
