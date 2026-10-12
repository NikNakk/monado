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

Follow-up changes:

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
  a USB claim failure. `XRT_MACOS_PRIME_PRODUCER_EVENT=1`, passed with
  `--service-env`, primes each client event on the GPU before first use.
  Compare a baseline loop with a primed loop.
- A failed compute or graphics dispatch now releases its descriptor sets. A
  lost device previously leaked them each frame until allocation failed. A null
  set then crashed MoltenVK in `vkUpdateDescriptorSets`; this produced all
  three device-loss crash reports. Upstream has the same early return.
- The PS VR2 window is now created with `releasedWhenClosed` off. Teardown both
  closed and released it. That left the main loop's tracked reference dangling,
  and its release crashed at shutdown (2026-10-11 21:32, 2026-10-12 19:58).

Measure the startup hang rate with and without priming. Stop the installed
service first.

```sh
MVK_CONFIG_LOG_LEVEL=3 python3 scripts/macos/run-headset-startup-loop.py \
  --output /tmp/monado-startup-baseline-1
MVK_CONFIG_LOG_LEVEL=3 python3 scripts/macos/run-headset-startup-loop.py \
  --output /tmp/monado-startup-primed-1 \
  --service-env XRT_MACOS_PRIME_PRODUCER_EVENT=1
```
