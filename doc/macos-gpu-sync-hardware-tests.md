<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Outstanding GPU synchronization headset tests

Hardware runs are user-operated. Record the commit, launch settings, observed
behavior and output directory. Wear the headset for the visual checks: recorded
presentation callbacks alone do not establish a stable view. Close other USB
owners and stop an older Monado service before a private native fixture claims
the headset. Use a new output directory per test.

## 1. Native three versus four swapchain images

From `~/Code/monado-2`, after the normal native incremental build:

```sh
python3 scripts/macos/run-three-image-comparison.py \
  --headset --frames 2400 --output /tmp/monado-three-image-headset-new
```

This runs GPU checks followed by four–three–three–four timing runs. Look for a
stable panel, first still and then with gentle head motion; record black frames,
checkerboards, judder and stalls in each mode. The fixture rejects an absent
headset rather than accepting offscreen results. See the
[full procedure and offscreen baseline](macos-three-image-comparison.md).

## 2. SteamVR fixed-allocation copy versus three-image zero-copy

Fully stop SteamVR between trials, and ensure the installed service has loaded
the compatible updated binary. Use the usual hardware launcher:

```sh
MWXR_STEAMVR_ZERO_COPY=0 MWXR_STEAMVR_THREE_IMAGES=0 ~/Windows/run-mwxr.zsh

# After stopping VR and the previous session:
MWXR_STEAMVR_ZERO_COPY=1 MWXR_STEAMVR_THREE_IMAGES=1 ~/Windows/run-mwxr.zsh
```

Use the same Home/dashboard scene and comparable still/moving-head windows.
For the second run, verify `zero-copy (first image ...)` in the driver's log
and `Three-image compatibility` in Monado's service log. Flags alone do not
prove the chosen transport. Record Home/dashboard behavior, black frames,
checkerboards, stalls and any crashes separately. The zeroCopy setting persists;
the three-image flag is launch-specific. Follow the
[detailed SteamVR procedure](macos-psvr2-timing-diagnostics.md#three-image-steamvr-compatibility-prepared-2026-10-10).
The [backend GPU test](macos-steamvr-resolve-copy-test.md) now proves that the
tested format can remove two copies per stereo frame; this hardware trial must
validate the actual SteamVR deployment and workload.

## Other remaining hardware coverage

The previous [native synchronization timing comparison](macos-gpu-sync-headset-comparison.md)
has no headset-worn visual judgement. The tests above provide a new current-build
visual record, but do not retrospectively validate its older captures.
Chromium ANGLE/Dawn video, pause/buffering/recovery on the physical display,
hosted Wine/Game Mode full-rate presentation, 90 Hz and Wine client failure
recovery remain separate coverage gaps in the
[M5 limits](macos-gpu-sync-cross-stack.md#exit-criteria-and-practical-limits).
There is not yet a validated single hardware runner for those cases. The
headless M3–M5 scripts/results must not be relabelled as headset validation.

## Headset attempts (2026-10-12)

**Resolved: the startup GPU hang was a render-resource lifetime race**
(`c/main: wait for the previous frame before reusing its render resources`).
`comp_renderer_draw` reset the shared command pool and rewrote descriptor sets
and uniform buffers before waiting for the previous frame's fence. It also
reset descriptor pools straight after submitting. macOS skips the end-of-frame
queue-idle wait that makes this safe elsewhere, so a pending frame could read
resources the next frame was rewriting. The fix waits on the existing fence
before recording, and defers each frame's resource release until after that
wait. The queue-idle wait stays disabled. Codex identified the ordering.

| Check | Before fix | After fix |
| --- | --- | --- |
| Offscreen startups with Metal GPU capture | 15 / 15 hung | 0 / 12 |
| Headset startups, display awake | about 20% hung (6 / 30) | 0 / 30 |
| Headset comparison, enqueue-to-display p50 | 19.23 ms | 19.23–19.24 ms |
| Headset comparison, shown gaps > 12.5 ms per ~2335 | 2–10 | 1–7 |

The earlier investigation notes follow; the items they leave open are answered
by this fix.

Four user-run attempts at test 1 on `c2703cc0f` (binaries from 14:43) did not
reach the timing runs.

1. **Installed service running.** The LaunchAgent service held the headset, so
   the private fixture service could not claim it. Boot out
   `org.freedesktop.monado.service` before a headset fixture run.
2. **USB status interface stuck.** With no other owner, the fixture service
   reported `Failed to set USB status interface alt 1`, then
   `Failed to claim USB status interface`. This is the
   [post-reboot failure](macos-service-direct-xpc.md#post-reboot-usb-initialization-failure-2026-10-11).
   A full headset/adapter power cycle and USB reconnect cleared it.
3. **Startup device loss.** The service lost the Vulkan device
   (`VK_ERROR_DEVICE_LOST`) on the first composed frame that waited on the
   client's readiness signal. Nothing was presented. An immediate repeat of the
   same `array-four` case, with `MVK_CONFIG_LOG_LEVEL=3`, passed: 240 frames
   were presented, and samples in both eyes matched. The same binaries also
   pass offscreen. Crash reports show the same sequence at 2026-10-11 00:42 and
   2026-10-12 08:02. This is the previously recorded unexplained startup
   device loss, and its cause is still open. Keep `MVK_CONFIG_LOG_LEVEL=3` set
   for headset runs, so that a recurrence logs MoltenVK's Metal error.
4. **Pixel check.** The four-image proof run had no device loss, but only one
   of the then-two required pixel samples matched. Frames 3–6 are blank in
   every headset run, so few samples could ever count. Composed frame 122
   latched a fresh client frame (119, not reused), yet its sample was fully
   transparent: `(0,0,0,0)`, including alpha. Either the target was not written
   that frame, or the readback read another image. It was not determined which.

5. **Head motion, not black frames.** With the denser sampling, on `ec2c61ecb`,
   2 of 7 samples matched. There was again no device loss, and the service
   exited cleanly. Recorded head pose explains every miss. Pitch moved from
   −40° to −11°, and yaw from +18° to −7°, during the 2 s run. Both eyes
   matched only near yaw 12° and pitch −14°. At frame 211, the left eye had
   lost the quad but the right still had it. The fixture's 1 m quad is fixed
   1.5 m along LOCAL −Z, so rotating the headset moves it off the sample
   points. Frame 122 in attempt 4 is consistent with the same cause.

6. **GPU hang captured.** In run 6, on `77aea62e6`, proof-four passed: all 6
   samples matched with the head-locked quad. Proof-three then lost the device.
   MoltenVK reported a GPU hang on the compositor's first submission with client
   content:

   > `Lost VkDevice after MTLCommandBuffer "vkQueueSubmit MTLCommandBuffer on Queue 0-0" execution failed (code 2): Caused GPU Hang Error (kIOGPUCommandBufferCallbackErrorHang)`

   The producer was not late. The service observed ready value 4 at 594.7 ms
   and latched the frame at 599.6 ms. The zero-timeout readiness check before
   submission reads the imported Vulkan semaphore, so MoltenVK already reported
   value 4 when the GPU wait was queued. The unified log has no GPU messages
   from that time. The descriptor and window fixes held: the service shut down
   cleanly, with no crash report.

7. **Not a semaphore deadlock.** In run 8, on `66ba54c3d`, the new log showed
   proof-three's hung first submission. Producer wait 4 was already at 4, and
   it signalled `render_complete` 3→4 and the reuse timeline 0→1. This
   matches the passing proof-four exactly. Twenty offscreen startups of the
   same case did not hang. On the headset, 4 of 14 startups with client
   content have hung, each on the first submission that read a client image.

8. **Presentation experiments: first results invalid.** Startup loops on
   `a4eb13ddf` (15 three-image startups of 120 frames each, in
   `/tmp/monado-startup-*`):

   | Loop | Slot worker | Presenter wait | GPU hangs | Display awake |
   | --- | --- | --- | --- | --- |
   | Baseline | on | GPU (shared event) | 4 / 15 | all |
   | `XRT_MACOS_PRIME_PRODUCER_EVENT=1` | on | GPU | 1 / 15 | all |
   | `MTL_SHADER_VALIDATION=1 MTL_DEBUG_LAYER=1` | on | GPU | 0 / 15 | all |
   | `XRT_MACOS_DRAWABLE_SLOT=0` | off | GPU | 0 / 6 valid | starts 1–6 only |
   | `XRT_MACOS_FORCE_CPU_RENDER_WAIT=1` | off | CPU | no valid data | none |

   Each hang was on the first submission that read a client image, with every
   wait met. Priming refutes the first-GPU-wait-on-the-event theory: one
   startup hung after its priming completed. Validation reported no errors, but
   slowed each composed frame to about 10.5 ms, so it may hide a timing race.

   The last two loops ran unattended. The service blocks display sleep only
   while a client is connected, so macOS's idle timer put the displays to sleep
   between startups. From start 7 of the slot-off loop, the PS VR2 produced no
   vblanks, and Core Animation reported no frame as presented. Those startups
   cannot show a presentation-path hang. A conclusion briefly recorded here,
   that the slot worker was the trigger, did not account for this and is
   withdrawn. The loop now holds `caffeinate -d` and wakes the display at the
   start. It classifies a headset startup with no displayed frame as
   `not_presented`, and stops.

   Rerun with the display held awake, on `53c4f9017`:

   | Loop | Slot worker | Presenter wait | GPU hangs |
   | --- | --- | --- | --- |
   | `XRT_MACOS_DRAWABLE_SLOT=0` | off | GPU | 2 / 15 |
   | `XRT_MACOS_FORCE_CPU_RENDER_WAIT=1` | off | CPU | 0 / 15, then 0 / 30 |

   **The presenter's GPU-side wait on `render_complete` is involved.** No
   startup hung in 45 with the CPU wait, against 6 of 30 with the GPU-side wait
   (the baseline and slot-off loops). At a 20% rate, 45 clean startups have
   about a 0.004% chance. The slot worker is not the trigger: the hang persists
   with it off. In completed startups, the CPU wait matched the baseline:
   presented interval median 8.342 ms, maximum 8.360 ms, no gap over 12.5 ms,
   and enqueue-to-display median 27.52 ms in both. The slot-off loop was
   faster to display (19.20 ms) but had 4 gaps over 12.5 ms in 1153. These are
   2 s startups, not sustained pacing evidence.

   **Correction: the CPU wait does not prevent the hang.** Four full
   comparisons on `cbeedbc75` (2400-frame timing runs, display held
   awake, in `/tmp/monado-cmp-{gpu,cpu}-{light,load}`) gave one hang with
   `XRT_MACOS_FORCE_CPU_RENDER_WAIT=1`. It was at the start of the cpu-light
   A1-four step, in the same way as before, with the presenter in CPU (`timeline`)
   mode. The other 23 steps completed. The `--background-load` runs looped the M5
   stress fixture (1024², 64 copies, uploads) for about 77 s of each roughly 90 s
   comparison.

   | Timing run | Enqueue-to-display p50 | Shown gaps > 12.5 ms |
   | --- | --- | --- |
   | GPU wait, light (4 runs) | 19.23 ms in all 4 | 2–10 per ~2330 |
   | GPU wait, loaded (4 runs) | 19.35–19.37 ms in all 4 | 1–19 per ~2330 |
   | CPU wait, light (4 runs) | 19.24 ms in 1, 27.56 ms in 3 | 0–3 per ~2340 |
   | CPU wait, loaded (4 runs) | 27.58–27.72 ms in all 4 | 5–12 per ~2333 |

   Throughput was 119.78–119.89 Hz throughout, and medians of readiness,
   dispatch and entry-to-latch were similar. The CPU wait usually delivered
   frames one refresh (about 8.3 ms) later: 7 of 8 runs, against 0 of 8 with
   the GPU-side wait. Keep the GPU-side shared-event wait as the default.
   `XRT_MACOS_FORCE_CPU_RENDER_WAIT` stays a diagnostic. The startup hang
   remains open. Its rate varies between sessions: 6 of 30 short loop
   startups, but 1 of 24 comparison steps.

   **Metal GPU capture reproduces the hang offscreen** (before the fix). With
   `XRT_MACOS_GPU_CAPTURE_DIR` and `MTL_CAPTURE_ENABLED=1`, the service
   captures its MTLDevice from the first producer-waiting submission. The
   startup loop's `--gpu-capture` sets both. It stops after four submissions,
   or at device loss. Offscreen, where startups have never hung otherwise, 9
   of 9 captured startups hung, with the same
   `kIOGPUCommandBufferCallbackErrorHang` on the compositor's queue. Capture
   slows the GPU, as validation does, but here that makes the hang far more
   likely rather than hiding it. The diagnostic
   `XRT_MACOS_SKIP_SATISFIED_PRODUCER_GPU_WAIT=1` (since removed) dropped the producer GPU wait
   once the CPU has seen the value. With it, 5 of 6 captured startups still
   hung. Offscreen command buffers then contain no GPU wait at all, so under
   capture the stall is in the buffer's own work, such as the client texture
   reads, not in a wait. Whether this capture hang is the headset startup hang
   is not established. Hung captures, for Xcode, are in
   `/tmp/monado-gpu-captures/`.

   Ruled out so far:
   - **CPU ordering.** Hung and passing startups follow the same CPU sequence
     through frame 4: Vulkan submit, present handoff, Metal present commit
     (GPU wait on `render_complete`), then the slot worker's `nextDrawable`.
   - **Present committed before the Vulkan buffer.** MoltenVK 1.4.2 reports
     `synchronousQueueSubmits=1` and `prefillMetalCommandBuffers=0` through
     `vkGetMoltenVKConfigurationMVK`. The compositor's Metal command buffer is
     therefore committed before `vkQueueSubmit` returns, ahead of the present.


- The comparison now samples every 30 frames, after a 30-frame warmup. It
  records each blank frame in `pixel-check.json`, and fails on more than one
  blank.
- Headset proof runs lock the quad to the head (VIEW space, 2 m at 1.5 m).
  Offscreen, all seven samples match, including the startup frames. Timing
  runs are unchanged.
- The first 8 submissions that wait on a producer now log every wait and
  signal semaphore, when `XRT_COMPOSITOR_LOG_SWAPCHAIN_GPU_REUSE=1` (proof
  runs). Each line gives the target value and the current counter at submission
  (`Swapchain GPU reuse submit state`). An unmet wait is marked `UNMET`. A
  signal that does not exceed its current value is marked `NOT_INCREASING`. If
  every wait was met at submission, the hang is in the command buffer's work or
  its signals, not in a wait.
- `scripts/macos/run-headset-startup-loop.py` repeats short isolated startups,
  by default 15 three-image startups of 120 frames. It counts `gpu_hang`,
  `device_lost`, `usb` and other outcomes in `summary.json`. It stops early on
  a USB claim failure. `--service-env XRT_NAME=VALUE` passes a setting to
  each service. The `XRT_MACOS_PRIME_PRODUCER_EVENT` experiment run with it
  has since been removed.
- A failed compute or graphics dispatch now releases its descriptor sets. A
  lost device previously leaked them each frame until allocation failed. A null
  set then crashed MoltenVK in `vkUpdateDescriptorSets`; this produced all
  three device-loss crash reports. Upstream has the same early return.
- The PS VR2 window is now created with `releasedWhenClosed` off. Teardown both
  closed and released it. That left the main loop's tracked reference dangling,
  and its release crashed at shutdown (2026-10-11 21:32, 2026-10-12 19:58).

Measure the startup hang rate, with a setting under test passed through.
Stop the installed service first.

```sh
MVK_CONFIG_LOG_LEVEL=3 python3 scripts/macos/run-headset-startup-loop.py \
  --output /tmp/monado-startup-baseline-2
MVK_CONFIG_LOG_LEVEL=3 python3 scripts/macos/run-headset-startup-loop.py \
  --output /tmp/monado-startup-noslot-2 --service-env XRT_MACOS_DRAWABLE_SLOT=0
```
