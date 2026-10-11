<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# macOS GPU synchronization: first-run checkpoint, 2026-10-11

Current native milestone status is recorded in the [M1–M2 completion report](macos-gpu-sync-completion.md). The results below remain evidence for their recorded source versions.

This is the historical first checkpoint. The subsequent
[headset-free service follow-up](macos-gpu-sync-service-tests.md) supersedes its
service-startup, recovery and performance findings.

The native cross-process harness and targeted production fixes are implemented.
**M1 and M2 are not complete.** Actual service GPU rendering, recovery and a
representative before/after service performance comparison remain unverified.
Chromium, Wine and DXMT were inspected only; no implementation was started there.

## Source and review scope

Baseline: `macos-upstream-clean`, `805316bebe976256a16c591ee135a5422ffe73a3`.
Working branch: `codex/macos-gpu-sync-harness`.
Remote integration branch at inspection: `bc9591f0313508cdb19fcb3ddf8e2c39c1e2232b`;
remote historical GPU-reuse WIP: `e364f85eaf8d40513f93dbf702d9c073522899b9`.
The current port documentation selected the integration branch, rather than the WIP.

Commits:

- `3a347d87c`: native two-process harness, runner and CTest scenarios.
- `e7bd14556`: readiness dependencies, failed-wait handling and quarantine.

The checkout initially contained substantial unrelated, uncommitted changes.
Commits exclude those changes. Added local-session regression tests depend on the
existing modified fixture and remain uncommitted; their isolated review diff is
[local-session-regressions.patch](results/macos-gpu-sync-20261011/local-session-regressions.patch).
[committed-code.patch](results/macos-gpu-sync-20261011/committed-code.patch) contains
the two committed changes against the pinned baseline. Tests used the dirty
integration checkout, not an otherwise pristine baseline tree.

Status-document links and the hung-threshold reference were updated in the
working tree while preserving prior document edits. Their isolated diff is
`status-doc-updates.patch`; these overlapping documents remain uncommitted.

Existing IOSurface fixtures, Vulkan utilities, shared-event semaphore import,
XPC probe transport and CTest infrastructure were reused. A shared-event handle
requires NSXPC transport: an NSKeyedArchiver attempt failed because the handle
requires NSXPCCoder. The harness uses an isolated temporary launchd/XPC producer
and private Unix command socket. It does not restart installed Monado services.
The loaderless OpenXR and simulated/null compositor infrastructure was used for
an attempted service check. Existing latency probes and producer tests were
reviewed; allocation/transport-only checks are not GPU consumption evidence.

Companion inventory: Chromium `codex/macos-openxr-upstream-2026-10-04`;
macos-wine-xr `claude/steamvr-openxr-driver`; DXMT
`codex/cross-process-presentation-hardening`. The legacy Windows producer is now
under macos-wine-xr's `legacy/monado-tree/tests/windows/`. Chromium's waiter has a
250 ms timeout; its caller semantics remain deferred.

## Contract and implementation

A Metal compute shader writes every pixel of a 64×64 IOSurface texture, including
producer PID, full frame number and texture index. Its completion shared event
is transported to another process and imported into a Vulkan timeline semaphore
on the matching Metal device. The consumer polls readiness on the CPU, verifies
the imported counter and submits a wait at the selected, already-observed value.
GPU copies and image barriers precede fence completion and pixel validation.
Producer reuse requires a consumer ACK sent only after that fence completes.
Readback validation occurs outside the measured critical path.

Correct multi-queue production explicitly joins the write queue before signaling.
The deliberately broken queue case signals before writing; PASS means the test
**detects incorrect pixels**, not that a faulty producer is safe. Monado cannot
infer proper queue ordering from an observed signal. Producers must honor the
monotonic event and write-before-signal contract.

Production changes register the producer event/value on every referenced tracked
Metal image before the existing readiness worker processes it. Submission checks
readiness again with timeout zero and supplies deduplicated imported semaphore
waits at the selected values with ALL_COMMANDS stage masks. Existing consumer
completion waits and barriers remain. Image acquisition also refuses reuse while
a producer obligation remains incomplete. Type checks reject arbitrary semaphore
objects before casting to native semaphore internals.

The multi-compositor worker now preserves wait errors. It never marks GPU work
done or schedules the failed frame. On macOS it polls with finite 5 ms waits and
triggers failure after 500 ms from the specific submitted obligation, rather than
from client inactivity. Failure is sticky; subsequent layer_begin returns an
error and a running session receives a lost event. Existing layer_begin
backpressure remains. The 6 ms readiness selection policy is implemented only
in the synthetic presentation clock; production deadline selection is outstanding.

`XRT_MACOS_CLIENT_GPU_HUNG_MS` configures the worker threshold (1–10000 ms;
invalid values fall back to 500). Harness defaults are interval 8333 µs, readiness
margin 6000 µs and hung threshold 500 ms; GPU_SYNC_* configuration is printed.
Normal Linux wait cadence is preserved; failed waits are now rejected there too.
Linux CI and headset regression runs were not executed.

## Quarantine and known limits

Tracked swapchain destruction cancels logical ownership and quarantines resources
without waiting on Vulkan. Collection polls producer and consumer completion
without blocking and reclaims only when both complete and CPU claims are zero.
The retained swapchain holds imported images and native resources. Collection
runs on compositor activity and cleanup. Unresolved device teardown contexts are
detached and retained until process exit; timeouts never authorize image reuse.

A global 1 GiB Vulkan-memory-requirement budget and 256 tracked-swapchain limit
include live and quarantined objects. Tests prove repeated failure reaches the
limit, rejects the next tracker and recovers capacity after completion. This is
not a complete allocation policy: there is no per-client quota and checking occurs
after import allocation, allowing a temporary allocation peak. Actual device-loss
and teardown with GPU work in flight remain unverified. Other presentation waits,
including the macOS presentation drain, still need audit. Therefore the full
nonblocking teardown requirement is not claimed satisfied.

## Evidence and commands

Evidence is in [the result directory](results/macos-gpu-sync-20261011/).

```sh
cmake --build build/arm64 --parallel
ctest --test-dir build/arm64 --output-on-failure
python3 scripts/macos/run-gpu-sync.py --output <directory> --repeats 2 --concurrent 2
python3 scripts/macos/run-gpu-sync.py --output <directory> --scenario normal --frames 1200 --repeats 2 --keep-awake
```

Real GPU/XPC tests require normal host access: the sandbox reports no Metal
device or Vulkan incompatible-driver errors, and restricts shared memory/socket
checks. Runs outside the sandbox exercise the intended path. The macOS build
passed. The full suite passed 63 tests, including 13 native GPU scenarios; the
latest targeted local-session/reuse checks also passed. Final rebuild/full-suite
output is retained separately in `final-build.txt` and `ctest-final.txt`; the
final full run also passed 63/63 (27.85 s).
Twenty-six fault runs (two concurrent independent producer/consumer pairs)
passed in `final-faults/summary.json`. These pairs do not share one consumer
Vulkan device or service.

The new failed-wait test was run against the original multi-compositor source:
three assertions failed because the frame was scheduled, progress references
were cleared and failure was not sticky. Restoring the modified source made it
pass. See `baseline-regression.txt` and `targeted-final.txt`. Mock tests verify
exact Stage B value/stage, refusal of future values and reuse, asynchronous
producer/consumer quarantine, oversized allocations and the 256-object cap.
A same-system CPU/mock test checks healthy-client progress during a failed wait
and a new healthy client after failure; it is not rendering recovery evidence.

Scenario limits matter: exit-before terminates before GPU writes, exit-after
terminates after signaling but before consumer submission, and consumer-delay
holds CPU completion observation rather than delaying GPU execution. Nonmonotonic
sequences are rejected by the protocol before enqueue; malicious event reset is
not tested. Late-callback testing uses a real Metal listener with retained state
and verifies cancellation prevents reactivation. Missing/future signals refuse
GPU submission. Idle with no obligation remains healthy beyond 500 ms.
Repeated quarantine is tested with mocked Vulkan, not real stalled GPU objects.
No sustained high-load thermal stress or randomized mixture run was completed.

An isolated real service attempt selected the simulated HMD, but the null
compositor failed Vulkan initialization with VK_ERROR_INCOMPATIBLE_DRIVER.
Both loaderless clients failed. See `null-service.log`, `null-service-results.json`
and client logs. This did not render and does not prove service recovery. The
native display frontend ordinarily requires the PS VR2 display; hardware runs
remain user-owned. No automatic headset test was performed.

## Timing and telemetry

Authoritative native latency runs are `awake-latency`: two 1200-frame runs, about
10 seconds each, nominal 120 Hz, small textures, AC power, Low Power Mode off,
thermal state nominal (0) at beginning/end, Apple M5 Mac17,4, macOS 26.7.1.
Temporary `caffeinate -i` prevents idle system sleep; no permanent power settings,
Game Mode or priority boosts were changed. Background CPU snapshots and power
settings are in telemetry.json. GPU power/frequency and powermetrics counters
were unavailable/not collected. CPU readiness includes IPC, GPU production and
observation delay; GPU timestamp and callback-processing components are not
separately measured. All timestamps are host monotonic nanoseconds.

| Native measurement, µs | Run 1 p50 / p95 / p99 / max | Run 2 p50 / p95 / p99 / max |
|---|---|---|
| Obligation to CPU readiness | 641.208 / 906.750 / 1816.542 / 3463.875 | 642.958 / 902.000 / 2128.000 / 38337.250 |
| Texture Vulkan submission | 63.625 / 107.583 / 151.917 / 852.625 | 64.416 / 110.459 / 143.166 / 734.125 |
| Empty submission, no wait | 14.584 / 20.208 / 25.375 / 32.334 | 14.459 / 20.000 / 24.084 / 85.667 |
| Empty submission, satisfied wait | 15.083 / 25.167 / 33.583 / 53.667 | 15.000 / 21.042 / 25.250 / 29.875 |

Empty controls alternate order and never read shared textures without a wait.
Median added satisfied-wait submission cost is 0.499 µs (3.42%) and 0.541 µs
(3.74%). This is a CPU submission comparison, not GPU blocking latency or a
production before/after comparison. Obligation-to-consumer-completion medians
are 1110.125 and 1115.500 µs; higher quantiles were not exported. Both runs
consumed 1200 frames without refusal, with 3 and 10 synthetic missed deadlines.
The 38 ms readiness outlier remains in the results; nominal thermal state does
not rule out CPU scheduling delay.

An earlier `final-latency` run failed at frame 794 after a 19-second readiness
observation gap, correctly refusing the texture. The host power log showed idle
sleep during that interval. The failure is preserved, not counted as a pass.
Earlier `baseline`, `latency` and `faults` data predate shader/pacing/control
corrections and are superseded. There is **no comparable baseline versus modified
actual-service workload**, so no production non-regression claim is made.

## Exit criteria

PASS below applies to the stated tested scope; incomplete end-to-end guarantees
are marked NOT VERIFIED rather than inferred from source or mock results.

| Criterion | Status | Evidence or outstanding work |
|---|---|---|
| M1.1 | PASS | Real two-process Metal→MoltenVK reads. |
| M1.2 | PASS | Every harness texture read follows observed exact-value readiness. |
| M1.3 | PASS | Imported timeline wait included on every consuming submit. |
| M1.4 | PASS | Full GPU-generated pixel pattern validation. |
| M1.5 | PASS | Missing/future refused; delayed safely deferred. |
| M1.6 | PASS | ACK/reuse follows consumer fence completion. |
| M1.7 | NOT VERIFIED | Tested exits/callbacks pass; in-flight termination/device loss not covered. |
| M1.8 | PASS | Deterministic named cases, structured traces, finite deadlines, repeated runs; scenario coverage limits above. |
| M1.9 | PASS | Existing fixtures/utilities/XPC/CTest reused. |
| M1.10 | PASS | Native latency baseline and paired wait-cost measurements above. |
| M2.1 | NOT VERIFIED | Source and mock integration; actual service GPU path did not run. |
| M2.2 | NOT VERIFIED | Tracked Metal submit gate tested; complete service execution unverified. |
| M2.3 | PASS | Reproduced failed-wait regression; failed wait cannot mark GPU done. |
| M2.4 | PASS | No added frame-loop wait; bounded macOS readiness worker; existing backpressure retained. |
| M2.5 | NOT VERIFIED | Producer/consumer reuse checks pass mocks; actual service ownership unverified. |
| M2.6 | NOT VERIFIED | Tracker teardown fixed; other waits/device-loss lifetime remain unaudited/unproven. |
| M2.7 | NOT VERIFIED | Global cap passes; per-client/pre-allocation controls incomplete. |
| M2.8 | FAIL | Isolated service failed startup, preventing render/recovery verification. |
| M2.9 | NOT VERIFIED | Mock isolation passes; healthy concurrent rendering/presentation not exercised. |
| M2.10 | NOT VERIFIED | No representative service before/after measurements. |
| M2.11 | NOT VERIFIED | Available tests pass; real service fault/recovery and hardware cases outstanding. |
| M2.12 | PASS | Scoped commits, diffs, traces and this handover. |

## Next review steps

First provide a real renderer target with simulated display, or perform a
user-run PS VR2 native Metal client test. Validate acquire/wait/release, producer
completion, compositor reads and reuse; then inject a client disconnect and
missing signal while a healthy client renders. Require a new client to render
without restarting the service, and measure healthy-frame interruption and
recovery duration. Audit presentation/device teardown and add per-client budgets
before claiming M2 complete. Capture pinned, comparable A–B–B–A service timing
with power/thermal context. Test actual in-flight producer termination and
consumer completion delay without submitting unsafe future dependencies.

Only after separate authorization, trace Chromium's existing waiter failure
propagation and Dawn/ANGLE SharedImage ownership through pause/buffering/reuse.
Then extend the same scenarios across the genuine x86-64 Wine/DXMT producer.
No M3–M5 work was started in this run.
