<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->
# Cross-stack GPU synchronization: M5, 2026-10-12

**M5's five criteria pass within the tested headless routes.** Native Metal,
Windows x86-64 D3D11/DXMT, and Chromium ANGLE/Dawn preserve the synchronization
and ownership contract below. Extended tests and paired measurements pass;
SteamVR and physical presentation constraints remain explicit. This completes
the synthetic M1–M5 investigation, not a claim of complete headset/game coverage.

This report builds on [M1–M2](macos-gpu-sync-completion.md),
[M3](macos-gpu-sync-chromium.md), [M4](macos-gpu-sync-wine.md), and the
[common Perfetto capture model](macos-gpu-sync-perfetto.md).
Implementation: `4fed5fb51` fixes the browser capture scope and checks utility
coverage; `7c95e5e8d` adds the bounded cross-stack runner and longer Wine timeout.
No production GPU synchronization or presentation changes were needed.
Unrelated Wine/DXMT/SteamVR edits were preserved.

## One contract, three producers

| Obligation | Native Metal / Monado | Chromium | Windows / DXMT |
| --- | --- | --- | --- |
| Write-before-signal | Metal queue writes precede its monotonic shared-event signal; separate queues join explicitly. | ANGLE resolve/end-write and Dawn end-access export the actual Metal write obligations. SyncToken alone proves command handling. | Windows D3D11 writes/signals/flushes against the exact imported allocations and event. |
| Observe readiness | CPU verifies the selected expected value before admitting a consumer GPU wait. | Asynchronous waiter verifies exported fences; absent mailbox is failure, valid completed/no-write image is distinct. | Native consumer reopens the event and checks the imported Vulkan timeline value. |
| Keep memory ordering | Retain the imported already-satisfied Vulkan semaphore dependency at the first consumer stage. | Monado keeps its independent CPU readiness and Vulkan dependency after Chromium readiness. Copy fallback completes its transfer before release. | Same Vulkan consumer and dependency; the test uses TRANSFER for readback. |
| Authorize reuse | Observed consumer GPU completion, not elapsed time or logical release. | Backing/claim lifetime remains separate from callback/session lifetime; repeated completed images need no new signal. | Producer waits for consumer acknowledgement and completion before recycling three images. |
| Failure/cancellation | Unresolved storage stays retained/quarantined, charged to its budget. A timeout never proves completion. | Keep unresolved write obligations across failed queries; invalidate retired submission callbacks even if frame IDs repeat. | Pending/completed producer exit, missing/future signals and delayed consumer completion follow the same test contract. |

Truthful producer ordering is required. A signal issued before the write is a
contract violation; the broken-queue negative control detects wrong GPU pixels.
Neither a CPU observation nor a logical reference release can repair that
ordering. Likewise, cancelling a callback does not cancel already-issued GPU
work. Production service quarantine/budget enforcement remains the M1/M2 path;
the standalone Wine fixture is not a new proof of Wine OpenXR service recovery.

## Deterministic and extended validation

- Final restored native build: **69/69 CTests PASS**, 37.31 s, with host GPU,
  XPC and socket access. The initial M5 run also passed 69/69.
- Chromium production SharedImage/ANGLE/Dawn regressions: **15/15 PASS**;
  real Metal waiter regressions: **11/11 PASS**. No skips.
- Two native CPU-upload producers, 1024×1024 textures, 64 GPU copies per
  transfer: **24,000 transfers PASS in 392.434 s**.
- Two genuine Windows/DXMT producers, same texture/copy settings:
  **24,000 transfers PASS in 274.732 s**. Every transfer's entire native
  readback matches frame, image and producer identity before reuse.
- Real service under the independent Wine GPU stress: **PASS**. A healthy
  native client contributes **25 new frames during the missing-signal wait**;
  failed storage is retained, replacement has **53 fresh latches**, and there
  is **one service startup**. Session-loss recovery endpoint: **538.909 ms**.
  These are offscreen service results, not display cadence measurements.
- Extended ANGLE and Dawn stereo tests each run four 15 s color phases.
  ANGLE performs **3,717 image acquisitions** with **30 sampled stereo output
  checks**; Dawn performs **3,723 acquisitions** with **31 checks**. All sampled
  GPU-completed eye colors match. ANGLE actually uses direct IOSurface storage;
  Dawn actually uses atlas-to-array copy. These samples complement the exhaustive
  native/Wine patterns; they do not check every browser output pixel/frame.
- ANGLE stress claims **7,432/7,432**, with one quarantine/reclaim; Dawn claims
  **7,442/7,442**, with two quarantines/reclaims. Complete captures have no
  outstanding logical claims and no claims at reclamation; GPU ordering is
  established separately by readback and reuse tests.
- Browser-wide ANGLE fault/recovery capture: **PASS**, all nine transitions,
  including GPU/decoder replacement. Independent native client: **1,200 frames**,
  **1,058 fresh latches**, **128** during prolonged buffering and **95** between
  GPU crash and replacement. Four quarantines/reclaims; no service restart.

All collected stress thermal samples were **Nominal (0)**. Throttling was
permitted, but did not occur in these samples; these runs therefore do not
establish safety under an observed throttled state. Initial stress matrices and
both timing pairs are separate. Compiler activity had ended before measurements.
Game Mode was not requested; AC power, charged battery, Low Power Mode off.
CPU/GPU power and frequency counters were not collected.

## Trace-scope defect and correction

The first long ANGLE fixture passes its sampled pixels but **fails capture
validation** with **273 `traced_chunks_discarded`**. Both Monado captures are
clean. Chromium's central buffers are not full and have no per-buffer drops.
Temporary service diagnostics identify its XR utility producer committing to
invalid **buffer zero**.

The fixture previously called `Tracing.start` on a page debugger session.
Chromium's `TracingHandler` applies that session's process filter, which excludes
the XR utility process. The child starts startup tracing but never binds those
writers to the real recording; after 30 s, Perfetto aborts the reservations to
invalid buffer zero. Short M3 captures do not expose this timeout. Their GPU
tests, native ownership observations and CSV measurements remain valid, but
their page-level Chromium trace does not prove XR utility coverage.

The fixture now uses the **browser debugger endpoint** for start/end/export of
the process-wide capture, retaining the page endpoint for script control.
Validation additionally requires actual `OpenXrRenderLoop::GetFrameData` and
`CompositeAndSubmit` events when `traceScope=browser`. A bounded reproduction
then passes with **zero discarded chunks**, **4,086 frame-data events**,
**2,042 compositor submissions**, and balanced native claims. The final long
ANGLE and Dawn runs also pass strict joined-trace loss checks and utility
coverage: **7,433/3,716** and **7,444/3,722** frame-data/submission events.

A native SDK namespace-isolation trial did not change the failure and was
removed. Temporary Chromium diagnostics were removed and the ordinary browser
rebuilt incrementally before final tests. No SDK relocation, namespace change,
trace-loss relaxation or larger central buffer is retained. Native binaries were
rebuilt while testing/removing the trial; final artifact hashes and native checks
are recorded. The meaningful retained fix is the fixture's debugger scope.

## Comparable measurements, with separate workloads

There is no fair single ranking of native uploads, D3D11 translation and browser
video decode/rendering. M5 uses two matched A–B–B–A pairs on the same machine.
Each run has a 20 s measured interval and continuous Foundation thermal samples;
all samples are Nominal with Low Power Mode off. Durations use differences
within their recorded clocks. Joined browser/native traces retain explicit
clock correlation; no uncalibrated cross-clock latency is asserted.

### Native / Wine upload pair

Both generate the same 128×128 BGRA pattern, GPU-upload it into three recycled
images and use the identical native Vulkan consumer. Each checks **1,200 frames**
at 60 Hz. Native and Wine upload/queue/transport implementations still differ.

| Run | Readiness p50 µs | Vulkan submit p50/p95 µs | GPU-completed handoff p50 µs | Missed initial cutoff |
| --- | ---: | ---: | ---: | ---: |
| A1 native | 1062.166 | 60.709 / 81.000 | 1695.333 | 0/1200 |
| B1 Wine | 919.584 | 66.375 / 91.666 | 1364.416 | 0/1200 |
| B2 Wine | 920.000 | 65.833 / 94.125 | 1367.875 | 1/1200 |
| A2 native | 775.583 | 37.125 / 75.417 | 1409.000 | 1/1200 |

All **4,800 transfers pass**. The paired average readiness medians are almost
equal (~919 µs), but the native runs vary by ~287 µs and their empty-submit
medians also shift. Nominal thermal state does not fix CPU/core/frequency or
scheduler behavior. This cannot establish a universal Wine overhead or speedup;
it complements, rather than replaces, M4's different 64×64/120 Hz comparison.
Late cutoffs defer the selected target; they never authorize premature reuse.

### ANGLE / Dawn video pair

Same H264 MediaSource fixture, 15 fps decoded video, three-second warmup,
20 s normal interval, simulated HMD/debug-image service and 20% render scale.
Diagnostic GPU readback/per-image logging is off; common traces remain on.
ANGLE uses direct transport, while Dawn's array path uses its existing copy.
Those distinct renderer/transport costs are not synchronization-fix overhead.

| Run | XR callbacks/s | Fresh latches/s | Service readiness p50/p95 µs | Vulkan submit p50/p95 µs | Handler-entry → latch p50/p95 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| A1 ANGLE | 60.019 | 49.491 | 7 / 12 | 120 / 175 | 13.643 / 16.295 |
| B1 Dawn | 60.030 | 47.846 | 7 / 12 | 110 / 154 | 13.463 / 16.343 |
| B2 Dawn | 59.819 | 46.092 | 5 / 11 | 82 / 141 | 12.967 / 16.397 |
| A2 ANGLE | 59.968 | 47.240 | 7 / 13 | 118 / 197 | 13.497 / 16.348 |

All four cases pass transport, trace completeness and ownership checks.
Browser-wide traces now also measure the actual Metal waiter: ANGLE p50
**242/228 µs**, Dawn **311/384 µs**. These durations include scheduling and
notification processing, not just GPU execution.

One B2 renderer observation has **36.819 ms previous-fence wait** and
**37.045 ms draw-dispatch interval**, despite p99 previous-fence wait of 2 µs.
The run still completes correctly. This is an unresolved isolated offscreen
renderer stall, not proof of a Dawn defect or a physical-display regression.
The latching distributions overlap; no statistical tail guarantee is claimed.

## SteamVR and presentation constraints carried forward

These are existing findings, not fixes performed during M5:

1. **Keyed mutex ownership is a different contract.** GPU shared-event readiness
   does not prove the caller owns the correct D3DKMT key, nor permit releasing it
   before image reads are complete. Existing Wine ownership-history tests and
   same-key contenders do not reproduce the Home GPU workload.
2. **Protected D3D11 contexts must cover the full Signal/Wait operation.** The
   preceding DXMT work found real premature signalling around an application's
   protected rendering section. M5 uses installed matched artifacts and does
   not claim an isolated-source deployment or new validation of that fix.
3. **Split the long handoff costs.** Existing Home observations include
   17.4–193 ms ReleaseSync intervals and acquire timeouts. Attribute context-lock,
   flush, command-capacity, CPU-fence, frame-latency, application ownership and
   owner-thread handoff separately; overlapping device-wide counters cannot be
   summed as if they were disjoint CPU/GPU execution times.
4. **Avoid allocation while holding the sync texture.** Existing dynamic-resolution
   evidence associated repeated destination swapchain creation with timeouts.
   The separate driver work sizes from the source allocation and changes image
   rectangles without reallocating. Its headset visual result remains separate.
5. **Deploy a matched renderer set.** A previous D3D11-only deployment failed
   on an older WineMetal entry point. Texture-only tests do not validate window
   presentation. Match D3D11, DXGI and WineMetal PE/native artifacts, then test
   ordinary/child/sibling/resize presentation before changing Steam's runtime.
6. **Scheduling and drawable acquisition remain separate.** The Game Mode
   ledger records external service demotion; XPC importance does not undo it.
   Client-side compositing is the existing direction. The newest-frame worker
   keeps drawable acquisition off the compositor thread; M5's debug-image target
   exercises neither physical drawable acquisition nor hosted layer handoff.
7. **Visual and cadence limits remain open.** Existing Wine hosted output was
   60 Hz on a 120 Hz display; black frames/checkerboards and user-worn confirmation
   remain unresolved in the separate SteamVR work. Static/offscreen traces do
   not establish rotational smoothness or physical presentation latency.

Sources: [presentation defaults](macos-psvr2-timing-diagnostics.md),
[judder evidence](macos-psvr2-judder-evidence.md),
[Game Mode architecture](macos-client-compositor-design.md),
[layer hosting](macos-remote-layer-hosting.md), and the current local
`~/Code/macos-wine-xr/docs/steamvr-home.md`,
`tools/keyedmutex_test/README.md`, and DXMT `docs/native-macos-sharing.md`.
The external notes contain unrelated uncommitted work; their observations are
not presented as newly executed M5 headset tests.

## Exit criteria and practical limits

| Criterion | Result | Evidence |
| --- | --- | --- |
| M5.1 Consistent model | PASS | Shared write/readiness/dependency/completion contract, distinct logical ownership, pause reuse and truthful-producer requirement. |
| M5.2 Determinism and stress | PASS | 69 native tests, 15 backing tests, 11 waiters, 48,000 exact native/Wine transfers, long browser eye/reuse checks and healthy service under Wine load. |
| M5.3 Comparable measurements | PASS | Separate 20 s paired uploads/video, nominal continuous telemetry, recorded artifacts/workloads and explicit variation/tail limits. |
| M5.4 SteamVR constraints | PASS | Ownership, protected-context ordering, deployment, allocation, scheduling, drawable and visual constraints identified above. |
| M5.5 Final report | PASS | Tested guarantees, rejected trace captures, one retained harness correction, unresolved stalls and untested routes distinguished. |

**Not verified here:** production Wine OpenXR/service failure recovery, hosted
Game Mode behavior, physical PS VR2 presentation or visual non-regression,
90 Hz, XRMediaBinding sparse layers, production sandbox compatibility for
isolated test endpoints, pathological driver/GPU resets, or Linux CI. The prior
native-only device-loss startup remains an unresolved observation. A capture
processed late does not identify GPU execution delay by itself. Hardware runs
remain user-run; no Steam/SteamVR restart, prefix reconfiguration or headset
claim is part of this milestone.

## Reproduction and evidence

The existing builds/checkouts are reused. Run with normal host GPU/XPC access:

```sh
python3 scripts/macos/run-gpu-sync-cross-stack.py \
  --output /tmp/m5-new-run \
  --wine-artifacts /tmp/m4-wine-gpu-sync \
  --wine-runtime "$HOME/Code/wine-crossover/runtime" \
  --chrome "$HOME/Code/chromium/src/out/mac-webxr/Chromium.app/Contents/MacOS/Chromium" \
  --processor "$HOME/Code/perfetto-sdk-v58.2/tools/mac-arm64/trace_processor_shell"
```

The runner bounds each stress case, samples thermal state off the frame threads,
waits for nominal state before each measurement, records source/artifact/power
metadata, and validates strict joined-trace loss and native claim replay.
`--section chromium` reruns the browser section into a fresh output directory
after preserving completed native/Wine evidence. `--pixel-ms` (or
`M3_CHROMIUM_PIXEL_MS`) controls each finite color phase; its old 3 s default
is unchanged. Wine's per-case timeout is configurable and remains finite.

Local untracked evidence: `/tmp/m5-cross-stack-20261012` contains successful
stress/upload pairs and the first rejected long capture;
`/tmp/m5-chromium-final-20261012` contains all twelve successful browser run/check
steps; `/tmp/m5-service-under-wine-stress` and
`/tmp/m5-root-trace-fault-recovery` contain the additional real-service checks.
`/tmp/m5-final-build-manifest.json` identifies the final ordinary binaries.
The diagnostic and unsuccessful namespace-trial captures remain in `/tmp` as
rejected evidence. Raw recordings and generated binaries are not committed.
