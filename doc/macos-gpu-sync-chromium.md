<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Chromium SharedImage synchronization: M3

Completion date: 2026-10-12. **M3's six exit criteria pass for the tested
offscreen Chromium/Monado routes.** M4/Wine remains deferred. The limits below
include unverified XRMediaBinding sparse layers and physical presentation.
The native M1/M2 evidence remains in [the completion record](macos-gpu-sync-completion.md).
Common capture and clock validation are in [the Perfetto record](macos-gpu-sync-perfetto.md).

## Scope and baseline

Chromium checkout: `~/Code/chromium/src`, branch
`codex/macos-openxr-upstream-2026-10-04`, tracing groundwork `8fbb7c43c01`.
The checkout contains unrelated uncommitted transport, sandbox, media and Blink
work. This investigation preserves it and records only its own changes.
Builds use incremental `autoninja -C out/mac-webxr`, with the existing build
configuration. Raw captures, build logs and result files stay outside Git.

## Defects and changes

1. `IOSurfaceImageBacking::GetExclusiveSharedEventFences()` previously cleared
   every external write obligation when queried. A timed-out or cancelled query
   could therefore make an unresolved texture look ready on retry. Queries now
   retain unresolved events, preserve the highest value per event, and prune
   only events whose `signaledValue` proves completion. A completed paused
   texture needs no new signal; a resumed write establishes a new obligation.
2. `SharedImageFactory::GetMetalSharedEventFences()` previously skipped missing
   mailboxes. An absent or destroyed image could masquerade as a successful
   empty query. The factory now returns an optional result and the GPU stub
   fails the callback when any requested mailbox is absent. A valid empty
   query remains distinct from an invalid image.
3. Metal submission callbacks previously used the render-loop object's weak
   lifetime. That object survives session/context replacement, while frame
   indices can repeat. A dedicated weak-pointer factory now invalidates Metal
   submissions on presentation exit, runtime stop and context loss. Teardown
   callbacks retain their independent lifetime.

The changes add no GPU waits to frame loops and do not change Monado's
already-signalled Vulkan semaphore wait. Retained pending events describe
obligations; a timeout or logical cancellation does not prove GPU completion.

## Chromium-to-Monado contract

A Chromium SyncToken proves that the GPU process has handled the producer's
commands; it does not itself prove completion on the Metal queue. ANGLE's
end-write access and Dawn's SharedTextureMemory end-access export the actual
Metal shared-event obligations. The GPU stub snapshots those obligations after
the SyncTokens and uses the asynchronous 250 ms waiter. The timeout reports
failed readiness to the caller; it never means the texture is complete.

For a matching pending frame, failed readiness ends the WebXR presentation
without marking that image rendered or submitting it as a completed frame.
Queries retain unresolved obligations for retries. Completed unchanged images
need no new signal. Session/context retirement invalidates outstanding Metal
submission callbacks even if the render-loop object remains alive.

After successful Chromium readiness, Monado retains its independent CPU
readiness gate and the imported, already-satisfied Vulkan semaphore wait for
memory ordering. Logical claim release is separate from GPU completion;
quarantine/reclaim and image reuse continue to use the M1/M2 GPU contract.
The checker replays service-side ownership observations, rejects negative or
unbalanced claims, and rejects reclamation while CPU consumer claims remain.
It separately rejects trace discard/overwrite/loss statistics. These logical
checks supplement GPU pixel/reuse tests; they cannot replace them.

The existing Metal copy fallback synchronously completes its own transfer
before releasing the OpenXR image. This milestone adds no new frame-thread
wait. Direct ANGLE transport is the normal-path performance comparison; the
Dawn array path's copy and existing transfer diagnostics are identified as
separate costs, not attributed to these three synchronization fixes.

## Regression coverage

- Production IOSurface backing with real Metal events: repeated missing-signal
  timeout/query, completed pause reuse and a resumed higher value, deduplicated
  maximum values, and two independent producer queues.
- Production SharedImage factory: absent mailbox, destroyed mailbox in either
  query order, valid live image without writes, and an explicitly empty query.
- Actual Dawn Metal SharedTextureMemory and ANGLE Metal GL representations:
  alternating GPU-written colors, end-access fence export, completion query,
  Skia/GL pixel reads, repeated completed pause queries and resumed writes.
  Perfetto assertions require exported write events, rather than accepting a
  pixel pass that bypassed the intended synchronization path.
- Real asynchronous Metal waiter: retiring weak callbacks while the target
  remains alive, then submitting a replacement with the same frame ID.

These producer/backing fixtures are complemented by the real decoder/browser
scenarios and end-to-end stereo pixel checks below. The final tests cover
playing, pause, buffering, resume, prolonged buffering, seeking, rapid
transitions, GPU/decoder termination and WebXR cancellation/replacement.

## Executed results

Chromium fix/test commit: **`956c93da4c1`**. Incremental `gl_tests` support
dependencies took about 16 minutes; subsequent rebuilds took 6–9 seconds.
The changed OpenXR render-loop object compiles, and GN dependency checks pass
for `//gpu:gl_tests` and `//device/vr:vr`.

- **15/15 selected SharedImage tests PASS**, no skips in the final selection.
- **11/11 real Metal waiter tests PASS**.
- Negative control: restoring only the old query/absent-mailbox behavior makes
  **four of five targeted tests FAIL**. Restoring the fixes returns the selected
  suite to PASS. Cancellation wiring is compiled; its weak-lifetime
  contract and updated-browser process-loss/session replacement both pass.

```sh
autoninja -C out/mac-webxr gl_tests metal_shared_event_sync_unittests \
  obj/device/vr/vr/openxr_render_loop.o
out/mac-webxr/gl_tests --use-gl=angle --use-angle=metal \
  --gtest_filter='IOSurfaceExternalSyncTest.*:SharedImageFactoryTest.*:IOSurfaceImageBackingFactoryTest.ANGLEOpenXRWritePauseResume:*OpenXRWritePauseResume/Metal:*Dawn_SkiaGL/Metal:*Dawn_WriteReadReadOnThreeDevices/Metal:*GL_Dawn_Skia_UnclearTexture/Metal' \
  --test-launcher-jobs=1
out/mac-webxr/metal_shared_event_sync_unittests --test-launcher-jobs=1
```

### Real video/browser fixture baseline

The existing 2026-10-10 browser bundle was cloned outside Git for fixture
development while the updated browser builds. Its framework SHA-256 is
`27a3d5a8c0dffddb2b9567db2ed95b276e75009bcc37a5b189fa30f48d370281`;
its exact source revision is not asserted. These runs **do not validate the
new browser fixes**. They exercise the real decoder, ANGLE, SharedImage,
OpenXR and Monado paths without a fake XR device.

The new fixture generates a small H264 fragmented stream, appends two seconds
through MediaSource, lets actual decoder starvation occur, then appends more
data. It covers playing, pause, buffering, prolonged buffering, resume, seek,
rapid pause/resume, session cancellation/replacement and a deliberate GPU-process
crash. A terminated decoder is explicitly replaced; this is not automatic
recovery of the old stream. WebGL resources are recreated after context loss.
An independent healthy native overlay renders through the same service.

The quiet baseline run passes the fixture and SQL/CSV validation:

- Native overlay: **1,200 frames**, no session loss; **1,043 fresh latches**.
- **122** fresh native latches during prolonged buffering; **95** between GPU
  crash and replacement-session progress. Browser XR frames continue during
  buffering with zero newly decoded frames.
- **2,524** producer obligations, **7,392** observed consumer completions,
  **four** quarantines and **four** reclaims; one service startup.
- Four joined captures have no reported trace error/data loss. The service
  capture has an acknowledged graceful footer. Some process-exit captures can
  remain partial; they are not completion proof.
- The service's measured Mach/native rate difference is **−10.47 ppm** during
  this run. CSV latch times use that capture's start/end calibration rather
  than assuming a constant cross-process clock offset. These derived counts
  are not microsecond latency claims.

The offscreen runtime recommends 256×144 transport images at 20% render scale.
The legacy base layer uses the same recommendation. Independently requesting
5% framebuffer scale in the old bundle crashed its renderer with an incomplete
framebuffer; matching attachment dimensions removes that fixture obstacle.
This does not fix or validate general framebuffer resizing support.

One concurrent-build run completed the video scenarios but failed its native
1,800-frame/60-second deadline. Another native-only startup failed with
`VK_ERROR_DEVICE_LOST` before Chromium launched. The cause remains unresolved.
With the compiler paused, a 120-frame native repeat and the concurrent
1,200-frame baseline both pass. Do not attribute the failed startup to Chromium,
or discard it as harmless noise. The build was resumed after the quiet runs.

The isolated socket/Mach service names require this fixture to disable the
Chromium sandbox. It uses a temporary profile and localhost-generated media;
production sandbox compatibility is a separate gate. The tests use the
simulated HMD/debug image target, not the physical headset.

```sh
python3 scripts/macos/run-gpu-sync-service.py --perfetto --scale-percentage 20 \
  --client-timeout 150 --output /tmp/new-m3-video-run \
  --client /usr/bin/env \
  M3_CHROMIUM_PATH=/path/to/Chromium.app/Contents/MacOS/Chromium \
  M3_CHROMIUM_OUTPUT=/tmp/new-m3-video-run \
  M3_HEALTHY_NATIVE_PATH=/absolute/path/to/build/arm64/tests/tests_macos_service_gpu_sync \
  node scripts/macos/run-chromium-gpu-sync.mjs
python3 scripts/macos/check-chromium-gpu-sync.py /tmp/new-m3-video-run \
  --processor $HOME/Code/perfetto-sdk-v58.2/tools/mac-arm64/trace_processor_shell
```

Set `M3_CHROMIUM_FORCE_COPY=1` to exercise the transfer fallback.
The updated incremental browser build succeeds after 31,572 build steps
(2 h 32 min with brief pauses for fixture checks). Builds reuse the existing output directory and dependency cache; subsequent
control/restoration builds need only 39 steps each. Updated-browser video/failure
cases both pass; the results are below. The older bundle remains a fixture
baseline rather than the matched performance control.

### Pixel and performance fixture validation

The follow-up fixture has two separate modes, selected with
`M3_CHROMIUM_MODE=pixels` or `benchmark`. Pixel mode writes distinct exact red/blue, green/red,
blue/green and red/blue-again eye pairs into real XR transport images and checks Monado's
existing Vulkan output readback after its completion fence. It validates both
eye interiors, excluding the transparent distorted seam at the output center.
`M3_CHROMIUM_PRODUCER=dawn` uses a real XR-compatible Dawn device, WebGPU
projection layer and render passes; the default uses ANGLE/WebGL.
Both modes require repeated acquisition of at least three transport images.
The pixel matrix covers default and forced-copy requests separately. ANGLE's
ordinary 2D image uses direct IOSurface transport by default; Dawn's array
projection image uses the render-copy fallback even without the force-copy
switch. The checker requires the actual transport label in Chromium's log;
a default Dawn case must not be described as a direct-image pass.

Fixture-development runs with the **older bundle** pass ANGLE pixel checks and
Dawn pixel checks (four array slices repeatedly recycled). The initial Dawn
attempt used the wrong feature switch; the next attempt incorrectly required
opaque pixels at the inter-eye distortion seam. Both failed attempts are
retained outside Git. The strengthened fixture draws only each eye's viewport with different
left/right colors, because whole-atlas clears with identical colors could hide
slice/viewport mistakes. The older bundle's Dawn stereo fixture passes all four
pairs. These are fixture-development results; the rebuilt matrix separately passes.
The corrected switch is
`--enable-features=OpenXR,WebXRWebGPUBinding`, and the assertions compare the
left/right eye interiors. These are diagnostic corrections, not runtime fixes.
The updated-browser pixel matrix passes all four cases, as recorded below.

Benchmark mode uses 3 seconds of warmup and a 20-second normal video/WebXR
window, with readbacks disabled by `run-gpu-sync-service.py --benchmark`.
It requires continued decoding and reports callback/fresh-latch throughput,
CPU readiness, IPC handler duration, handler-entry-to-latch delay, Vulkan
queue submission and prior-fence wait distributions. Chromium trace durations
remain distinct from GPU execution durations. Foundation thermal state and
low-power mode are sampled every five seconds by the native test's GPU-free
`telemetry` mode. The wrapper also records public power/thermal settings.
A fixture-development measurement passes at 59.99 XR callbacks/s; it is **not**
a matched before/after comparison. The matched control reverses only
Chromium commit `956c93da4c1` against the same inherited working tree, then
restores it. Both incremental control/restoration builds succeed in 39 steps
and about 31 seconds each. Source fingerprints cover the M3 files and every
inherited modified/untracked source. The original working tree is restored
before A–B–B–A measurements without compiler contention. Results follow.

A decoder-backed `XRMediaBinding` probe cannot create its layers session in
this runtime configuration. Chromium requires cylinder, equirect2 **and cube**
extensions for the `layers` feature; this Monado build intentionally has cube
support disabled. The attempt fails explicitly before any session frames.
Its raw failure is retained, and the exploratory fixture branch was removed.
`XRMediaBinding` sparse-layer playback is therefore **NOT VERIFIED** here.
Real decoder starvation remains covered by the video/ANGLE fixture, while
completed SharedImage reuse without a new write is covered by the actual
ANGLE/Dawn representation tests. No unsupported OpenXR extension was enabled
to bypass the capability check.

Five further native startup/300-frame runs under compiler load pass with
MoltenVK diagnostic logging: 1,500 frames total, no device-loss or queue errors.
The earlier native-only device-loss startup has not reproduced. Its cause
remains unproven; these repeats do not retroactively turn it into a pass.

### Final native build check

The service/runtime and all native test binaries were incrementally rebuilt
against the committed `d80f86dc0` 4 MiB Perfetto shared-buffer fix and the
persistent SDK at `$HOME/Code/perfetto-sdk-v58.2`. Earlier fixture-development
runs used the older service binary; the final browser matrix uses the rebuilt
one. All **69 native CTests pass**. The restricted first run passes 50 and
fails 19 at local shared-memory/Unix/Mach IPC setup; rerunning those 19 with the
required endpoint access passes all of them. The Perfetto test passes with the
current backend. The execution restriction is distinct from a GPU test pass
or a GPU failure.

## Updated-browser results

Fixed framework SHA-256:
`e5dac7a61a265e286867522c43b7a49f788b516c2464e3594fb79a4f3c1a67ca`.
Chromium synchronization fixes: `956c93da4c1`; Monado fixtures: `7b840e454`.
The rebuilt native binaries include the committed Perfetto buffer fix.

| Video/failure route | Native completed frames | Fresh native latches | Prolonged buffering latches | GPU crash → replacement latches | Quarantine / reclaim |
| --- | ---: | ---: | ---: | ---: | ---: |
| ANGLE direct IOSurface | 1,200 | 962 | 129 | 80 | 4 / 4 |
| ANGLE forced render-copy | 1,200 | 1,035 | 124 | 82 | 4 / 4 |

Both runs pass all nine phase checks, including zero new decoder frames during
prolonged buffering, explicit session replacement, GPU-process/decoder failure
and successful replacement media/context/session rendering. Both keep one
service startup, balance every consumer claim (2,656 and 2,652 respectively),
and report no service device loss or queue failure. Reclamation occurs with
zero outstanding CPU consumer claims; GPU completion remains separately
protected by the production Vulkan reuse wait. Joined traces have no reported
error/data loss/discard/overwrite statistics, and the service footers acknowledge
complete capture.

The two-point Mach/native clock rates differ by +236 and +853 ppm in these
captures. The phase-window latch counts use their own calibration and are
coarse evidence of continuing progress, not microsecond cross-stack latency
claims. Independent native stdout frame counts also progress in every phase.

| Stereo pixel request | Actual route | Repeatedly acquired transport images | GPU pixel/ownership result |
| --- | --- | --- | --- |
| ANGLE default | Direct IOSurface | 3 images, 280–281 acquisitions each | PASS |
| ANGLE forced copy | Render-copy | 3 images, 281 acquisitions each | PASS |
| Dawn default | Atlas → array render-copy | 4 array images, 210–211 acquisitions each | PASS |
| Dawn forced copy | Atlas → array render-copy | 4 array images, 210–211 acquisitions each | PASS |

Each case checks exact left/right red-blue, green-red, blue-green and red-blue
again after Monado's Vulkan GPU completion fence. Different eye values detect
swapped/stale atlas slices as well as stale content. All consumer claims
balance; all one/two quarantined swapchains are reclaimed. A default Dawn
request is **not** a direct-IOSurface pass: arrays use the actual copy path.

Raw reports: `/tmp/m3-updated-browser-validation.json` and
`/tmp/m3-updated-pixel-validation.json`; each case has its own JSON, CSV and
native/Chromium traces outside Git. A final quiet native-only startup repeat
on the rebuilt service completes 300 frames with MoltenVK diagnostics and no
device loss. The earlier failed startup remains unexplained; it occurred
before Chromium launched and is recorded as a separate platform/runtime risk,
not erased or attributed to these fixes.

## Matched normal-path performance

A = the inherited Chromium working tree with only `956c93da4c1` reversed.
B = the same working tree with that commit present. Both use the same rebuilt
Monado service, fixture, 256×144 recommendation, ANGLE/direct transport, buffered
CSV plus Perfetto, 3-second warmup and 20-second measurement window. Diagnostic
readbacks and per-frame verbose logs are disabled. Each run uses a fresh profile
and service. All 26 public Foundation thermal samples are **nominal**; low-power
mode is false, and public power telemetry reports AC power. Game Mode is not
requested. GPU/CPU frequency and privileged power measurements are unavailable.

A framework SHA-256:
`817cf5277efa26cc4ef6f0ce7a346cceac2d5680fd4a578924a21b1e0d7e9c0d`.
B framework SHA-256 is the validated fixed framework above. The finite runner
verifies source restoration before measurement; inherited edits are preserved.

| Run | XR callbacks/s | Fresh latches/s | CPU readiness p50 / p95 (µs) | IPC handler p50 / p95 (µs) | Vulkan submit p50 / p95 (µs) | Handler entry → latch p50 / p95 (ms) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| A1 | 60.023 | 50.791 | 6 / 12 | 28 / 56 | 109 / 164 | 13.751 / 16.217 |
| B1 | 59.968 | 50.840 | 7 / 12 | 29 / 59 | 111 / 168 | 14.486 / 16.352 |
| B2 | 59.919 | 49.991 | 6 / 12 | 29 / 58 | 117 / 169 | 13.671 / 16.172 |
| A2 | 59.860 | 49.590 | 6 / 12 | 28 / 55 | 111 / 165 | 14.185 / 16.293 |

All four runs pass real decoding, transport, image reuse, trace completeness,
ownership and throughput checks. B avoids an asynchronous waiter for 131/1,200
and 79/1,200 already-completed queries; both A runs create a waiter for every
query. There are no readiness timeouts or failed mailbox queries. The measured
Metal waiter duration includes GPU completion observation and CPU dispatch;
it is not isolated GPU execution time. Remaining wait p50/p95 values are
337/567, 221/555, 323/569 and 188/493 µs in run order.

These short runs show comparable throughput and roughly 0–1 µs higher median
CPU readiness/IPC cost. Vulkan submit medians span 109–111 µs for A and
111–117 µs for B. Latch distributions overlap; the B1 median is higher while
B2 is lower than either A run. Prior-fence wait p50/p95 is 1/2 µs in every run;
A2 has one 7.162 ms maximum, while B maxima are 3 µs. The four runs provide a measured comparison. Statistical tail
non-regression and physical headset latency require separate validation. No additional optimization is justified by these four
samples alone. Raw distributions/telemetry/source fingerprints are in
`/tmp/m3-matched-performance.json` and its four case directories.

## Milestone exit report

| Criterion | Outcome | Evidence |
| --- | --- | --- |
| M3.1 Actual Dawn/ANGLE mechanisms | PASS | 15 actual backing/producer tests, 11 real Metal waiter tests; end-to-end ANGLE and Dawn GPU pixel cases use the actual transport. |
| M3.2 Pause/buffering safety | PASS | Completed-image query reuse tests plus both rebuilt-browser real decoder pause/starvation/resume/seek/rapid-transition runs; prolonged buffering has zero new decoded frames and continued XR/native progress. |
| M3.3 Handoff ownership and ordering | PASS | Exact differing eye colors through GPU writers, SharedImage/event transport and Vulkan-completed output readback; repeated three/four-image recycling, balanced claims and existing mandatory Vulkan/reuse waits. |
| M3.4 Timeout/cancellation/termination | PASS | Missing/future signal and retry regressions, absent/dead mailbox failure, retired callback target with the same replacement frame ID, session cancellation and GPU-process/decoder replacement; timeout never establishes completion. |
| M3.5 Monado responsiveness/recovery | PASS | Both rebuilt-browser failure cases keep the independent native client rendering 1,200 frames in one service; replacement browser sessions render successfully, quarantines reclaim, no service GPU failures. |
| M3.6 Reproducibility/performance | PASS | Finite local fixture/checker, negative controls, source-scoped commits, full native checks and matched A–B–B–A distributions with recorded thermal/power state. |

These outcomes cover the tested simulated-HMD/debug-image routes and truthful
producer synchronization. **NOT VERIFIED:** XRMediaBinding sparse-layer playback
(blocked by cube capability), production sandbox compatibility for these
isolated endpoint names, physical PS VR2 display/visual behavior, sustained
cross-stack stress and pathological driver/GPU resets. The earlier native-only
device-loss startup remains a known unresolved observation. These limits do not
substitute for any passing test and are not evidence of physical presentation
non-regression. M4/Wine and broader M5 work remain separately authorized stages.

Final verification: 15/15 selected SharedImage tests and 11/11 waiter tests
pass again with no skips after the fixed source is restored; GN checks pass
for both `//gpu:gl_tests` and `//device/vr:vr`. All 69 native tests pass as
recorded above. Raw captures and build logs remain outside Git.
