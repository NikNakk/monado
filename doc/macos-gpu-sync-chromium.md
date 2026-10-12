<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Chromium SharedImage synchronization: M3

Investigation date: 2026-10-12. M3 is authorised; M4/Wine remains deferred.
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

These fixtures test producer/backing behavior, not decoder buffering or an
end-to-end Chromium/Monado image handoff. Browser scenarios still need playing,
pause, buffering, resume, prolonged buffering, seeking, rapid transitions,
GPU/decoder termination and WebXR cancellation coverage.

## Executed results

Chromium fix/test commit: **`956c93da4c1`**. Incremental `gl_tests` support
dependencies took about 16 minutes; subsequent rebuilds took 6–9 seconds.
The changed OpenXR render-loop object compiles, and GN dependency checks pass
for `//gpu:gl_tests` and `//device/vr:vr`.

- **15/15 selected SharedImage tests PASS**, no skips in the final selection.
- **11/11 real Metal waiter tests PASS**.
- Negative control: restoring only the old query/absent-mailbox behavior makes
  **four of five targeted tests FAIL**. Restoring the fixes returns the selected
  suite to PASS. Cancellation wiring is compiled; its generic weak-lifetime
  contract is tested, with updated-browser process-loss validation pending.

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
The updated `autoninja -C out/mac-webxr chrome` build is still running. A finite
post-build job is queued for both direct and forced-copy cases, including the
concurrent overlay and trace validation. Its status/results are in
`/tmp/m3-updated-browser-validation.json`; raw build/results stay outside Git.
The older bundle's baseline is not a matched performance baseline: the checkout
also carries other uncommitted transport/media changes.

### Pixel and performance fixture validation

The follow-up fixture has two separate modes, selected with
`M3_CHROMIUM_MODE=pixels` or `benchmark`. Pixel mode writes exact red, green,
blue and red-again values into real XR transport images and checks Monado's
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
retained outside Git. The corrected switch is
`--enable-features=OpenXR,WebXRWebGPUBinding`, and the assertions compare the
left/right eye interiors. These are diagnostic corrections, not runtime fixes.
The updated-browser pixel matrix is queued after its video/failure cases.

Benchmark mode uses 3 seconds of warmup and a 20-second normal video/WebXR
window, with readbacks disabled by `run-gpu-sync-service.py --benchmark`.
It requires continued decoding and reports callback/fresh-latch throughput,
CPU readiness, IPC handler duration, handler-entry-to-latch delay, Vulkan
queue submission and prior-fence wait distributions. Chromium trace durations
remain distinct from GPU execution durations. Foundation thermal state and
low-power mode are sampled every five seconds by the native test's GPU-free
`telemetry` mode. The wrapper also records public power/thermal settings.
A fixture-development measurement passes at 59.99 XR callbacks/s; it is **not**
a matched before/after comparison. The matched control will reverse only
Chromium commit `956c93da4c1` against the same inherited working tree, then
restore it, using incremental builds and A–B–B–A measurements without compiler
contention. A reverse-patch dry run succeeds; no control edit has yet been made.

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

## Milestone gates

| Criterion | Evidence required before completion |
| --- | --- |
| M3.1 Actual Dawn/ANGLE mechanisms | PASS for the producer/backing fixtures; updated-browser handoff evidence remains pending. |
| M3.2 Pause/buffering safety | PARTIAL: fixtures and real-video baseline pass; updated-browser cases are queued. |
| M3.3 Handoff ownership and ordering | PARTIAL: producer pixel tests pass; exact Chromium → Monado pixels/ordering and reuse checks remain open. |
| M3.4 Timeout/cancellation/termination | PARTIAL: retry/callback regressions pass; baseline process-loss/replacement passes; updated-browser validation pending. |
| M3.5 Monado responsiveness/recovery | PARTIAL: quiet baseline has concurrent fresh latches without restart; updated-browser cases and the native device-loss failure remain open. |
| M3.6 Reproducibility/performance | PARTIAL: regression tools/results exist; matched normal-path A–B–B–A measurements and telemetry remain open. |

None of the native tracing results alone closes these gates. Physical display
checks are user-run, as required by this checkout's agent instructions.
