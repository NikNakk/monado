<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# macOS PS VR2 timing diagnostics

> **Buffering correction, 2026-10-03:** earlier captures described as fully
> buffered set the option, but several compositor, app-pacing and IPC CSV writers
> still flushed periodically. Their matched comparisons remain useful, but those
> windows were not free of trace writes. The moving-head runner below verifies
> fixed required-file sizes during measurement, followed by acknowledged flushes.

> **Optimisation comparison pending, 2026-10-04:** the Sense integration now
> has a full-runtime `RelWithDebInfo` preset with verified `-O2` solver, driver
> and compositor flags. Optimisation is mandatory for 6DoF throughput. Compare
> it with cameras/Sense disabled first, using the same UE workload and buffered
> traces, then measure added tracking load. No new display-pacing result has
> been established. See [the integration note](macos-pssense-6dof-integration.md).

## Current presentation defaults

Presentation uses CADisplayLink pacing with timed Metal presents.
CVDisplayLink remains available with `XRT_MACOS_DISPLAY_LINK=cv` and as an
automatic fallback when CADisplayLink cannot be created. The
CAMetalDisplayLink driven and hybrid modes, and the child-layer
CAMetalDisplayLink probe, have been removed. On hardware both modes showed
display-link thread delays and more late frames than this path (~0.87% late
cadence intervals vs driven ~5.2–5.8%), and the configuration below restored
~119.88 Hz. Later sections describe them historically. See
`doc/macos-env-toggles.md` for the evidence behind each default.

| Control | Default |
| --- | --- |
| `XRT_MACOS_DRAWABLE_SLOT` | `1`: newest-frame worker acquires drawables off the compositor thread |
| `XRT_MACOS_PRESENT_MIN_DURATION_US` | `8000`: `presentDrawable:afterMinimumDuration:` (known-good 120 Hz value; `0` restores absolute timed presents) |
| `XRT_MACOS_COMPOSITOR_TIME_CONSTRAINT` | `1`, with `_COMPUTATION_PCT=35` and `_CONSTRAINT_PCT=70` of the display period |
| `XRT_MACOS_CLIENT_FRAME_MIN_HOLD` | `0` (set `2` to hold each app frame for at least two refreshes, e.g. a 60 Hz app on the 120 Hz headset) |
| `XRT_MACOS_APP_RELEASE_SHARED_EVENT_WAIT_THREAD` | `1` in all builds; read in the **app** process |
| `XRT_MACOS_WAIT_SPIN` | `0`; opt-in busy-wait diagnostic |
| `XRT_MACOS_DISPLAY_LINK` | `ca`: CADisplayLink refresh timestamps and measured period. `cv` selects the legacy CVDisplayLink path; see [the A/B below](#cvdisplaylink-against-cadisplaylink) |

Always on, with the old toggles removed: asynchronous presentation, the Metal
shared-event handoff (automatic CPU-wait fallback), deferred GPU timestamp
readback, display-link vblank feedback, three drawables, and no
late-render wait. The removed experiments were the present worker, stale
substitution, early drawable, immediate present, unique present slots, the
display-rate, client-frame and app-pacer divisors, compositor QoS, the hybrid
wait, the process-activity assertion and the CAMetalLayer display-sync and
framebuffer-only switches. `XRT_MACOS_METAL_XPC_EXTERNAL_BROKER=1` remains
available for a manually started service;
`scripts/macos/run-wine-openvr-native-trace.zsh` depends on it.

With a non-zero minimum present duration, `XRT_MACOS_PRESENT_PRELATCH_US` and
`XRT_MACOS_PRESENT_MIN_LEAD_US` only affect the `target_output_ns` and
`metal_request_ns` trace columns, because the requested present time is
discarded.

The sections below record earlier experiments and their historical defaults.


This branch adds passive timing traces for comparing the macOS Monado PS VR2 path with Sony's Windows SteamVR driver. The trace is disabled by default and does not change pose prediction or presentation scheduling.

Enable it for a run with:

```sh
export PSVR2_TIMING_TRACE=1
```

By default CSV files are written to `/tmp`. To use another existing directory:

```sh
export PSVR2_TIMING_TRACE_DIR="$HOME/psvr2-trace"
mkdir -p "$PSVR2_TIMING_TRACE_DIR"
```

## Trace files

The process ID is included in every filename. A normal service/compositor run should produce some or all of:

- `monado_psvr2_<PID>_imu.csv`
- `monado_psvr2_<PID>_slam.csv`
- `monado_psvr2_<PID>_pose.csv`
- `monado_psvr2_<PID>_present.csv`
- `monado_psvr2_<PID>_vblank.csv`
- `monado_psvr2_<PID>_late_render.csv`

### IMU / DisplayPort scanout

`imu.csv` records each parsed high-rate status/IMU sample with both device and host timing information:

- estimated host sample timestamp;
- VTS and IMU clocks;
- their current host-clock mappings;
- raw `vts_us` and `imu_ts_us`;
- `dp_frame_cnt` and `dp_line_cnt`;
- gyro and accelerometer values.

The frame/line pair is also kept atomically as the most recent headset-reported DisplayPort raster position so it can be stamped onto SLAM and pose-query rows without racing the USB thread.

### SLAM

`slam.csv` records the host receipt timestamp and VTS timestamp for every SLAM relation, the latest IMU/VTS state and DisplayPort frame/line at that instant, the raw SLAM pose, the corrected pose inserted into Monado, and the motion-estimated linear/angular velocity returned by the relation history.

This is intended to reveal whether a new 60 Hz SLAM observation causes a visible correction after Monado has extrapolated beyond it.

### Pose queries

`pose.csv` records every generic HMD pose query after tracking is ready:

- host time at which the query is serviced;
- requested host-domain timestamp;
- the same requested time converted back to VTS;
- latest SLAM and IMU VTS timestamps;
- current host/VTS clock offset;
- latest DisplayPort frame/line;
- final returned pose and velocities.

### Presentation

`present.csv` records a monotonically increasing frame ID plus:

- compositor `desired_present_time_ns` and `present_slop_ns`;
- target image index and timeline value;
- timestamps before/after the Vulkan idle wait;
- drawable acquisition time;
- timestamps around `presentDrawable`, command-buffer commit, and completion;
- latest CVDisplayLink output timestamp;
- Metal `presentedTime`, `GPUStartTime`, and `GPUEndTime` where available.

The existing presentation behavior is deliberately left unchanged on this diagnostics branch: `desired_present_time_ns` is logged but is still not used to schedule the Metal present.

### CVDisplayLink / vblank

`vblank.csv` records both `inNow.hostTime` and `inOutputTime.hostTime` from CVDisplayLink, together with the actual callback time and the time Monado consumes the update. It also records:

- `inOutputTime - inNow`;
- callback time minus output time;
- successive output intervals;
- the nominal display period.

This is important because the current macOS target feeds `inOutputTime.hostTime` into `u_pc_update_vblank_from_display_control`. The trace lets us determine empirically whether that value represents the phase Monado expects, whether it is a future output timestamp, and whether its clock is aligned with `os_monotonic_get_ns()`.

## Suggested recording

Use the same movement pattern for the Sony/Windows and Monado/macOS captures where possible:

1. hold the headset still for roughly 3 seconds;
2. slow, nearly constant yaw for roughly 5 seconds;
3. hold still for roughly 2 seconds;
4. slow lateral translation for roughly 5 seconds;
5. hold still for roughly 3 seconds.

Exit Monado cleanly after the recording so buffered rows are flushed. The trace also flushes periodically, so a partial capture should survive an abnormal exit.

The first comparisons to make are:

- SLAM receipt/VTS age at each pose request;
- requested prediction horizon relative to latest SLAM and IMU;
- whether extrapolated pose overshoots immediately before a SLAM correction;
- `dp_frame_cnt`/`dp_line_cnt` phase against CVDisplayLink output events;
- `desired_present_time_ns` versus actual Metal submission/completion/presented time;
- whether CVDisplayLink `inOutputTime` is offset from the host monotonic clock or from the physical DP raster by approximately one refresh interval.

## Clock-domain and presentation fixes

The diagnostic branch now also fixes the timing defects exposed by the first capture:

- CoreVideo host timestamps (Mach absolute time) are translated into Monado's `CLOCK_MONOTONIC` domain using a bridge sampled on every display-link callback, so sleep-time epoch differences cannot leak into compositor pacing.
- `inOutputTime` is treated as a future output target. The display period is used to project it backwards to the most recent refresh boundary before feeding `u_pc_update_vblank_from_display_control()`.
- `desired_present_time_ns` is translated back into Mach absolute seconds and supplied to Metal with `presentDrawable:atTime:`. Late frames naturally fall back to earliest possible presentation according to Metal semantics.
- Actual screen presentation is recorded asynchronously from `addPresentedHandler:` in `*_presented.csv`; reading `presentedTime` immediately after GPU completion is no longer used.
- `XRT_MACOS_CVDISPLAYLINK_PACING=0` was originally available as an A/B diagnostic to disable display-link feedback. It has since been removed.

A post-fix capture therefore produces six CSVs: `imu`, `slam`, `pose`, `present`, `presented`, and `vblank`.

## Next-output scheduling and narrow render wait

After the second capture showed presentation landing one output later than the upcoming CoreVideo slot, the macOS target now:

- waits for the compositor's `render_complete` timeline semaphore at the exact frame value passed to `present()`, rather than idling the entire Vulkan queue; a queue-idle fallback remains for configurations without a timeline semaphore;
- selects the latest CoreVideo `inOutputTime` as the presentation slot and advances by whole display periods only if that slot is stale or has less than the minimum lead time;
- defaults to 2 ms minimum lead (`XRT_MACOS_PRESENT_MIN_LEAD_US=2000`) so Metal has enough time for the IOSurface-to-drawable blit without gratuitously adding a whole refresh;
- records the selected `target_output_ns`, wait mode, and actual-present-minus-target error in the trace.

## Timeline semaphore and Metal N-1 request

The macOS target now creates a Vulkan timeline semaphore for `render_complete` during post-Vulkan initialization when timeline semaphores are available. The renderer signals the current frame ID and the Metal target waits only for that value before touching the IOSurface. The semaphore is destroyed with the target.

The intended physical output remains `target_output_ns`, but Metal's `presentDrawable:atTime:` request is now one display period earlier (`metal_request_ns = target_output_ns - display_period_ns`). This is an evidence-driven calibration from the previous capture, where requesting output N landed on N+1 in ~90% of frames. Both timestamps are logged separately so the next capture can verify whether requesting N-1 lands on N.

## Tunable Metal pre-latch bias

The fixed one-refresh bias was already in the past by the time Metal was called. The target now requests a tunable offset before the intended output slot. `XRT_MACOS_PRESENT_PRELATCH_US` defaults to 2000. `metal_request_minus_call_ns` records whether the requested Metal time is still in the future at the call site.

## Actual presentation feedback into fake pacing

`CAMetalDrawable.presentedTime` is the observed onscreen host time, while the fake pacer previously retained its default 4 ms present-to-display offset. The macOS target now measures `presented_monotonic_ns - desired_present_time_ns` for completed frames and feeds a smoothed estimate back through `u_pc_update_present_offset`.

The Metal callback only publishes an atomic sample. The compositor thread consumes it in `update_timings`, applies a 1/8 EMA, rejects samples outside 0..4 display periods, and waits for eight samples before updating the pacer. This avoids calling the non-thread-safe pacing object from Metal's callback queue.

The intended effect is to align `predicted_display_time_ns` (and therefore late pose sampling/ATW prediction) with the display time actually reported by CAMetalLayer, even if CoreAnimation retains a stable one-refresh presentation pipeline. `presented.csv` now records `observed_present_offset_ns`.

## Linux/Fusion tracking baseline and macOS comparison

A separate `monado-cli pose-dump` diagnostic was used to test whether the visible macOS judder could originate before the compositor, for example because macOS receives older SLAM poses or because PSVR2 pose prediction behaves differently from Linux.

### Test environments

The same PS VR2 headset and diagnostic code were compared in two environments:

- **macOS:** native Apple Silicon macOS build from this PSVR2 work;
- **Linux reference:** **Ubuntu ARM64 running under VMware Fusion on the same Mac**, with the PS VR2 USB device passed through to the guest.

The Linux result is therefore a useful implementation/reference comparison, not a bare-metal Linux latency benchmark. USB virtualization and guest scheduling can add latency and jitter of their own. The Linux VM was used only to exercise the PSVR2 USB/tracking path; it was not expected to drive the headset display through Fusion.

### 200 Hz pose-prediction sweep

The first diagnostic sampled `xrt_device_get_tracked_pose()` at 200 Hz. For each common base query time it requested poses at 0, +5, +10, +15, and +20 ms. This tests the boundary after the PSVR2 driver/SLAM/dead-reckoning path but before compositor presentation timing.

The Linux/Fusion run contained 6,656 rows over 33.31 s. Its median sampling interval was 5.000 ms and p99 was about 5.18 ms. After initial tracking acquisition, calls succeeded and returned fully valid tracking. Translational prediction scaled almost perfectly with the requested horizon, and rotational prediction likewise matched angular velocity times horizon. At +20 ms, rotational correlation was about 0.99998.

The comparable macOS run contained 6,203 rows over about 31 s. The headset was moved somewhat faster on macOS, so raw predicted displacement was larger, but after accounting for movement speed the prediction behaviour was very similar. Median sampling interval was about 4.996 ms. macOS had somewhat more ordinary scheduler jitter at 200 Hz, but no corresponding tracking discontinuity. Rotational prediction correlation was greater than 0.99997.

Measurable backwards movement between increasing prediction horizons was negligible. The largest relevant macOS reversal was only around hundredths of a millimetre. Linux/Fusion actually showed a larger sequential-call update artefact in one row, confirming that tiny non-monotonic within-row changes can occur when a new underlying tracker state arrives between successive horizon queries.

A further comparison of each +5 ms prediction against the subsequently observed 0 ms pose initially showed larger absolute corrections on macOS, but the macOS run used faster movements. Stratifying by translational speed made the two platforms very similar:

| Translational speed | macOS median correction | Linux/Fusion median correction |
| --- | ---: | ---: |
| 0.02-0.10 m/s | ~0.54 mm | ~0.51 mm |
| 0.10-0.20 m/s | ~1.22 mm | ~1.04 mm |
| 0.20-0.40 m/s | ~2.04 mm | ~2.06 mm |
| 0.40-0.80 m/s | ~3.34 mm | ~3.88 mm |

The prediction sweep therefore did **not** find a macOS-specific defect in `xrt_device_get_tracked_pose()` prediction that resembles the visible backwards judder.

### 1000 Hz SLAM availability diagnostic

Prediction can look correct even when its underlying SLAM estimate is older on one platform. A second diagnostic therefore exposed the PSVR2 driver's raw VTS timing through a small diagnostics API and sampled it at 1000 Hz.

For each sample the CLI recorded the latest SLAM VTS timestamp, that timestamp mapped into Monado's monotonic host clock, the latest IMU VTS timestamp in the same clock domain, the age of the latest SLAM pose at query time, and the latency when the polling CLI first observed a new SLAM timestamp.

`slam_first_seen_latency_ns` is an upper bound on true availability latency because a newly published pose is discovered on the next poll. At 1000 Hz the observation uncertainty is approximately 1 ms apart from occasional scheduler stalls. Startup samples are excluded from the latency summary because the VTS-to-host mapping is still settling and can briefly produce impossible ages.

The two platforms were essentially indistinguishable:

| Metric | Linux/Fusion | macOS |
| --- | ---: | ---: |
| Median SLAM update interval | 16.683 ms | 16.683 ms |
| p95 SLAM update interval | ~16.96 ms | ~17.00 ms |
| Median first-seen SLAM latency | ~23.97-24.00 ms | ~22.95 ms |
| p95 first-seen SLAM latency | ~28.27-28.28 ms | ~27.88-27.89 ms |
| p99 first-seen SLAM latency | ~29.23 ms | ~28.52 ms |
| Median latest-SLAM age at arbitrary query | ~31.95-32.0 ms | ~31.51 ms |
| Median IMU timestamp lead over new SLAM pose | ~23.04 ms | ~22.5-22.6 ms |

The 16.683 ms interval corresponds to approximately 60 Hz SLAM output on both platforms. The approximately 23-24 ms delay from SLAM pose timestamp to first observation is independently supported by the latest-IMU-minus-SLAM timestamp difference, which is also around 23 ms and is strongly correlated with the first-seen measurement. This is consistent with Monado receiving a SLAM pose that is already tens of milliseconds old and dead-reckoning it forward with newer IMU samples.

The roughly 1 ms lower median first-seen latency on macOS is too small to treat as meaningful, particularly because the Linux reference is virtualized. The important result is that there is **no evidence for a substantial extra macOS SLAM delay**.

Both systems sustained the 1 kHz poller adequately. Median polling interval was about 1.000 ms on both. macOS showed somewhat more ordinary p95/p99 scheduling jitter, while Linux/Fusion had a few larger rare stalls in these particular runs. These differences do not resemble the persistent visible headset judder and do not materially alter the latency conclusion.

### Tracking-side interpretation

Together, these experiments substantially reduce the likelihood that the visible macOS judder originates in the PSVR2 tracking path. The following stages look broadly comparable between native macOS and the Ubuntu/Fusion reference:

```text
PS VR2 SLAM/IMU data
        |
        v
USB/PSVR2 driver
        |
        v
SLAM relation history
        |
        v
dead-reckoning prediction
        |
        v
xrt_device_get_tracked_pose()
```

Specifically:

1. macOS prediction over 0-20 ms horizons is smooth and quantitatively very similar to Linux/Fusion;
2. macOS does not receive materially older SLAM poses than the Linux/Fusion reference;
3. the PSVR2 SLAM stream is approximately 60 Hz on both;
4. newly available SLAM poses are about 23-24 ms old on both, with newer IMU data available for forward prediction.

This does **not** prove that every possible tracking-side issue is excluded, and the Linux reference is virtualized rather than bare metal. It does, however, make a large macOS-specific SLAM/prediction latency defect an unlikely explanation for the observed backwards judder.

Unless new tracking evidence appears, investigation should therefore concentrate after pose selection:

```text
predicted/view pose used for frame
        |
        v
ATW / distortion compositor
        |
        v
Vulkan render completion
        |
        v
IOSurface / Metal presentation handoff
        |
        v
CAMetalDrawable scheduling
        |
        v
actual PS VR2 presentation / vblank / scanout
```

The CLI prediction diagnostic can be run at 200 Hz; the SLAM availability comparison should use 1000 Hz for approximately 1 ms polling resolution:

```sh
./build/src/xrt/targets/cli/monado-cli pose-dump 1000 \
  > slam-latency.csv \
  2> slam-latency.log
```

The CLI accesses SLAM timing through a small PSVR2 diagnostics interface rather than including the private PSVR2 driver header, keeping libusb and other private driver dependencies confined to the driver target.

## Positional prediction diagnostics and filtered velocity A/B

The PSVR2 driver now records `monado_psvr2_<PID>_prediction.csv` at each SLAM update. It compares the position predicted from the previous SLAM relation's constant linear velocity with the newly reported SLAM position, including total and along-motion error.

A selectable filtered linear predictor is also available:

- `PSVR2_FILTERED_LINEAR_PREDICTION=0` (default): existing relation-history linear velocity.
- `PSVR2_FILTERED_LINEAR_PREDICTION=1`: use an EMA-filtered SLAM linear velocity for positional dead reckoning; the high-rate gyro angular path is unchanged.
- `PSVR2_LINEAR_VELOCITY_ALPHA=0.25` controls the EMA update coefficient (0..1).

The prediction trace contains prior relation velocity, newly estimated velocity, filtered velocity, prediction horizon between SLAM samples, 3-D error, and the error component along the previous direction of motion. This permits objective A/B comparison without changing the executable.

### CAMetalLayer drawable count

`XRT_MACOS_MAX_DRAWABLES` selects the CAMetalLayer drawable-pool depth. Valid values are `2` and `3`; the default is `3`. The effective value is logged at startup. This is intended to test whether drawable buffering contributes to the observed one-refresh presentation latency.

## Full-horizon translation and bounded acceleration

See [PSVR2 positional prediction](psvr2-position-prediction.md) for the offline
model comparison, a newly verified mismatch between gyro-only dead reckoning's
linear horizon and `horizon.csv`, opt-in `PSVR2_FULL_LINEAR_HORIZON` and
`PSVR2_ACCELERATION_PREDICTION` controls, appended diagnostic columns, and exact
headset A/B commands. Both new controls default to zero. Test full-horizon raw
first, then compare acceleration against that same horizon.

## Host-time continuity transition

[Continuity experiment](psvr2-continuity-prediction.md) documents the next opt-in
`PSVR2_CONTINUITY_PREDICTION=1` candidate, its 4 ms transition and 5 mm cap,
paired accuracy/continuity replay, limitations, and headset test commands.

## macOS late-render / late-pose experiment

`XRT_MACOS_LATE_RENDER_DESIRED_OFFSET_US` is the preferred macOS-only late-render diagnostic. It is **disabled when unset**. When explicitly set, including to `0`, the compositor waits immediately before its final graphics/compute dispatch until approximately `desired_present_time_ns + offset`. This keeps the experiment anchored to the compositor's desired-present phase instead of the learned `predicted_display_time_ns`, avoiding the positive feedback seen when a missed frame increased the learned present offset and therefore made the next predicted-relative wait even longer.

For example:

```sh
export PSVR2_TIMING_TRACE=1
export XRT_MACOS_LATE_RENDER_DESIRED_OFFSET_US=2000
```

Signed offsets are accepted. A useful initial 120 Hz sweep is `0`, `1000`, `2000`, and `3000` microseconds, keeping `XRT_MACOS_PRESENT_MIN_LEAD_US=2000` unchanged. The default path remains unchanged when the variable is unset.

The older predicted-display-relative diagnostic, `XRT_MACOS_LATE_RENDER_LEAD_US`, has been removed. `late_render.csv` keeps its `lead_us` column, which is always `0`.

Previous traces showed roughly 2-3 ms scheduler overshoot with a 0.5 ms spin margin, so the diagnostics-only wait now sleeps until 3 ms before its target and spins for the remainder. `monado_psvr2_<PID>_late_render.csv` retains the original columns and appends `desired_offset_us`, `wait_mode`, `target_minus_desired_ns`, and `pose_begin_minus_desired_ns` so the desired-relative and legacy modes can be distinguished without breaking column-name-based analysis. This remains an A/B diagnostic rather than the final late-latching design.

## Asynchronous Metal presentation / Vulkan-to-Metal shared event

`XRT_MACOS_ASYNC_PRESENT=1` is an opt-in diagnostic that removes the synchronous Metal `waitUntilCompleted` from the compositor thread. Source IOSurfaces are marked in-flight on acquire and are not reused until the Metal blit command buffer completes; with three target images this should normally avoid blocking, while remaining correct if the GPU falls behind. The default was originally `0`, preserving the prior synchronous path. The option has since been removed: presentation is always asynchronous.

When async present is enabled, `XRT_MACOS_METAL_SHARED_EVENT_WAIT=1` (default) also requests `VK_EXT_metal_objects`, creates the render-complete Vulkan timeline semaphore as exportable to Metal, exports its underlying `MTLSharedEvent`, and encodes the timeline-value wait directly into the Metal command buffer. If the extension/event export is unavailable, presentation falls back to the existing CPU Vulkan timeline wait but still avoids the Metal completion wait. `XRT_MACOS_METAL_SHARED_EVENT_WAIT=0` originally forced that intermediate mode. The option has since been removed: the shared-event handoff is always used when available, and the CPU wait is only the automatic fallback.

`present.csv` appends `async_present`, `shared_event_wait`, and `image_reuse_wait_ns`. Async runs also produce `present_complete.csv`, recording the command-buffer completion callback, GPU start/end timestamps, source image, timeline value, and whether the shared-event handoff was used. In async mode the legacy `after_metal_wait_ns` field records the immediate post-commit timestamp rather than a completion wait; use `present_complete.csv` for actual Metal completion.

A useful A/B at the previously favourable but cadence-limited `+3000 us` late-render setting is:

```sh
# Old synchronous control
XRT_MACOS_LATE_RENDER_DESIRED_OFFSET_US=3000 XRT_MACOS_ASYNC_PRESENT=0

# Remove only the Metal completion wait
XRT_MACOS_LATE_RENDER_DESIRED_OFFSET_US=3000 XRT_MACOS_ASYNC_PRESENT=1 XRT_MACOS_METAL_SHARED_EVENT_WAIT=0

# Fully asynchronous Vulkan -> Metal GPU handoff
XRT_MACOS_LATE_RENDER_DESIRED_OFFSET_US=3000 XRT_MACOS_ASYNC_PRESENT=1 XRT_MACOS_METAL_SHARED_EVENT_WAIT=1
```


## CAMetalDisplayLink yaw capture, 2026-09-17 (PID 47307)

The user recorded slow yaw while looking at SwiftXRShell's panel with the latest
Release build and `PSVR2_TIMING_TRACE=1`. Files were
`/tmp/monado_psvr2_47307_{frame_pipeline,present,presented,present_complete}.csv`.
All four streams contain matching data for 3,013 submitted frames over 28.274 s;
frame_pipeline has nine events per frame. The interval includes a roughly 2 s
gap, so its aggregate FPS is not a steady-state rate.

Joining native `predict_result.frame_id` to `present.timeline_value` gives zero
mismatches between `predicted_display_ns` and `target_output_ns`. The new
callback-to-prediction plumbing is active. Median prediction horizon is 16.175 ms.

Joining the presentation streams by `frame_id` gives:

| Outcome | Frames | Share |
| --- | ---: | ---: |
| Nonzero actual presentation, within 1 ms of target | 2,661 | 88.3% |
| Nonzero actual presentation, more than 1 ms late | 161 | 5.3% |
| Zero actual presentation timestamp | 191 | 6.3% |

Zero `presentedTime` means not presented or dropped according to Apple's
MTLDrawable documentation. The callback has already fired here, so these are
consistent with skipped drawables; they must not be counted as zero-error
presentations. For nonzero timestamps, median target error is -0.014749 ms,
p95 is +8.309292 ms, and maximum is +31.466709 ms. There are no backwards actual
presentation timestamps after sorting by frame ID and excluding zero timestamps.

All Metal command buffers report completed status (4). Completion callbacks
arrived before the CA rendering deadline for 152/161 late frames and 173/191
zero-timestamp frames. Their median completion lead was 4.632 ms and 4.530 ms,
respectively. Since callbacks can arrive after actual GPU completion, this is
strong evidence that these particular frames finished GPU work before the
reported deadline. Ordinary GPU overruns cannot explain most anomalies.

This capture establishes presentation irregularity despite correctly matched
prediction targets. It does not prove that every perceived backward step is
caused by presentation, nor identify which CA/Metal handoff stage drops frames.
There are no driver pose, late-render, or client-frame-map CSVs for this PID in
the supplied capture, so rotational pose continuity and repeated client content
cannot be assessed from it. Next instrumentation should correlate drawable IDs,
display-link callback return/Metal scheduling, and idle/stale-drain presents with
actual presentation, before changing prediction offsets again.

## CAMetalDisplayLink yaw capture with poses, 2026-09-17 (PID 57866)

The follow-up slow-yaw capture contains 2,425 complete compositor frames plus
PSVR2 pose, SLAM, IMU, prediction, horizon, late-render, client-frame-map, and
CAMetalDisplayLink traces. Joining `late_render.csv` to `pose.csv` isolates two
compositor pose requests for 2,421 frames and three for four frames. The normal
pair requests the beginning and end of scanout, separated by a median 7.735 ms.

The beginning-of-scanout orientation sequence agrees strongly with the reported
angular velocity. During motion, only two inter-frame changes greater than
0.01 degrees oppose the reported angular velocity. Both coincide with a new
SLAM update; the larger correction is 0.0919 degrees. No beginning-to-end
scanout change greater than 0.001 degrees opposes angular velocity. The capture
therefore does not show a repeated backward rotational correction in the poses
used by compositor distortion/timewarp.

Presentation remains irregular:

| Outcome | Frames | Share |
| --- | ---: | ---: |
| Nonzero actual presentation, within 1 ms of target | 2,149 | 88.6% |
| Nonzero actual presentation, more than 1 ms late | 103 | 4.2% |
| Zero actual presentation timestamp (skipped/dropped) | 173 | 7.1% |

Of the anomalous frames, 271/276 have the ordinary approximately 8.342 ms
CAMetalDisplayLink target interval. They are not primarily explained by missed
callbacks. Client-frame mapping contains only 27 reused frames out of 2,415,
while presentation anomalies total 276; reuse is likewise not the main cause.
The anomaly rate is present across angular-speed bands rather than appearing
only during rapid yaw.

The evidence now separates the paths: CAMetalDisplayLink target timestamps reach
the compositor correctly, and the PSVR2 poses selected for those timestamps are
rotationally continuous, but Core Animation reports about 11.4% of submitted
drawables as late by more than 1 ms or not presented. Instrument drawable IDs and
the scheduling/presentation lifecycle next. Avoid further pose-offset or
prediction changes unless new pose evidence contradicts this capture.


## macOS compositor scheduler mode-change finding, 2026-09-17

A Game Performance / System Trace capture of the service compositor thread
(Mach TID `0x2d2c7e5`, decimal `47368165`) materially changes the realtime
scheduler diagnosis.

At trace-relative time `00:14.881852`, the relevant scheduler event is
`MACH_SCHED_MODE_CHANGE`, not `MACH_MODE_DEMOTE_FAILSAFE`,
`MACH_MODE_DEMOTE_RT_DISALLOWED`, or `MACH_MODE_DEMOTE_THROTTLED`. Its
arguments identify the exact compositor TID and show scheduler mode
`1 (realtime) -> 3 (timeshare)`. Approximately 41 ns later the same thread's
effective priority changes `97 -> 4`. Instruments subsequently shows it
running predominantly on E cores, with occasional temporary User Interactive
priority inheritance, while its requested QoS remains Unspecified and its
effective QoS can become Background.

This means the current evidence does **not** support treating the transition as
a conventional RT failsafe or RT-disallowed demotion tracepoint. Investigation
should instead determine what policy update causes the explicit realtime to
timeshare mode replacement, including whether work-interval/workgroup policy,
task backgrounding, or another effective-policy update is responsible. The
recovery window should be checked for the inverse direct
`MACH_SCHED_MODE_CHANGE`.

For a launchd-vs-manual-service A/B without losing the Metal handle transport,
set `XRT_MACOS_METAL_XPC_EXTERNAL_BROKER=1` in a manually-started
`monado-service` and run the legacy standalone Metal XPC broker. In this
diagnostic mode the service skips its direct Mach-service listener and routes
texture import, shared-event publication, and token discard through the
standalone broker. Normal direct-XPC behaviour is unchanged when the variable is
unset.


### Active-session NSProcessInfo activity diagnostic

To test whether RunningBoard's Darwin-background / `lowpri_cpu` clamp can be
prevented using a supported Foundation process activity assertion, the service
now has an opt-in active-session diagnostic:

- `XRT_MACOS_PROCESS_ACTIVITY=user-interactive` holds
  `NSActivityUserInteractive` while at least one XR session is active.
- `XRT_MACOS_PROCESS_ACTIVITY=latency-critical` holds
  `NSActivityUserInteractive | NSActivityLatencyCritical` over the same
  lifetime.

The assertion is acquired on the first active XR session and released when the
last active session stops; mere IPC connection lifetime does not hold it. The
normal path is unchanged when the variable is unset. The latency-critical mode
is deliberately diagnostic and should be used only to determine whether the
extra timer/I/O precision changes behaviour after the RunningBoard background
clamp is addressed.

The immediate A/B is to repeat the Unreal/Game Mode workload and inspect the
Multi Client Module compositor TID in Instruments and
`compositor_rt.csv`. The key outcome is whether the explicit
realtime-to-timeshare mode change and `97 -> 4` MAXPRI_THROTTLE clamp still
occur.


#### Process activity diagnostic build-guard correction

The initial process-activity diagnostic commit used the CMake option name
`XRT_FEATURE_SERVICE` as though it were a C preprocessor definition around the
session-lifecycle calls in `ipc_server_process.c`. Monado does not export that
CMake option as a C macro, so those calls were compiled out even though the
Objective-C implementation was present. The service now defines
`XRT_FEATURE_SERVICE_ENABLED=1` for `ipc_server` when the CMake service feature
is enabled, and the lifecycle hooks use that build definition. A configured
activity mode is also logged during service startup; the assertion itself is
still acquired only when the first XR session becomes active.


#### Process-lifetime activity diagnostic, 2026-09-18

For the RunningBoard A/B, the activity assertion is now deliberately acquired
from `ipc_server_main_common()` before compositor creation and held until
service shutdown. This replaces the earlier active-session lifetime experiment:
the kernel can apply its background throttle before the first client
`wait_frame`, so session activation was an unnecessarily late and ambiguous
point for this diagnostic.

Every macOS service start now writes an unconditional stderr line:

```
MACOS_PROCESS_ACTIVITY startup raw=<value-or-unset>
```

If the value is `user-interactive` or `latency-critical`, a successful
assertion immediately adds:

```
MACOS_PROCESS_ACTIVITY began mode=<mode> options=0x... process_lifetime=1
```

This logging bypasses Monado's logging level and the XPC mainloop wrapper. It
therefore distinguishes an unexported environment variable from a build or
execution-path problem directly.


## CVDisplayLink against CADisplayLink

CVDisplayLink is deprecated from macOS 15. Its replacement is CADisplayLink,
from `-[NSScreen displayLinkWithTarget:selector:]` (macOS 14 and later).
`XRT_MACOS_DISPLAY_LINK=ca` in the compositor's environment (the service, or a
hosted client) selects it; unset or `cv` keeps CVDisplayLink.

Only the source of the vblank timestamps changes. Both feed the same values to
the pacer (`macos_display_link_tick()` in `comp_window_macos.m`), and
drawables, presentation and the present worker are untouched. This is not the
removed CAMetalDisplayLink driven mode, where the link supplied the drawables
and compositing ran from its callback: that mode's timestamps were correct
(2026-09-17 captures below) but about 11 % of its drawables were late or
dropped.

Differences to expect:

- CADisplayLink fires on a run loop, so it has a thread of its own
  (`Monado CADisplayLink`). CVDisplayLink calls back on Core Video's thread.
- CADisplayLink follows the real display. It does not fire while the display
  is asleep, where CVDisplayLink free-runs at the nominal rate. The log says
  how many times it fired in its first second, or warns if it did not.
- CA initially estimates the period from the display mode, then refines it from
  `targetTimestamp - timestamp`. Its `timestamp` is used directly as the last
  refresh, rather than projecting the target backwards with a nominal period.

**Status: corrected CADisplayLink matches CV closely in native headset timing
captures (2026-10-03).** The first A/B below exposed a CA phase/period defect;
the corrected service and hosted-client captures are recorded separately below.
CA became the default after the corrected captures and the UE/Game Mode
follow-up below; retain `cv` for comparison and fallback.

To compare, record the same scene twice with `PSVR2_TIMING_TRACE=1`, once
with `XRT_MACOS_DISPLAY_LINK=ca`, and compare in `present.csv` and
`presented.csv`: physical intervals over 12 ms, `latest_displaylink_output_ns`
against the actual presented time, late-frame counts per 240, and
`compositor_rt.csv` wake lateness. Run it under Game Mode with a hosted
client too, since the callback thread differs.

### Native headset A/B — 2026-10-03

**Historical, before the phase/period fix.** The corrected captures below
supersede this comparison as evidence about the current CA implementation.

Tested `be345261efbdc1ca2fa16b3197361740a5e9204b` with the existing dirty
worktree (the patch is saved with the traces). Rebuilt `monado-service`,
`openxr_monado` and `psvr2-openxr-test` in `build-wine`, whose
`CMAKE_HOME_DIRECTORY` points to this checkout and whose timing diagnostics are
enabled. Native Metal diagnostic scene: 862 world-locked boxes, 2800×2856 per
eye; Apple M5; PS VR2 at 4000×2040 / 120 Hz. Service compositing, with
`XRT_MACOS_CLIENT_COMPOSITOR=0` in the client; no hosted-client/Game Mode test.

Four 40-second client launches in order **cv, ca, ca, cv**, restarting the
service for each. Retained the existing service settings except the display-link
selector, trace output, idle-exit delay, and explicitly disabled camera/gaze
streams. Both sources retained the newest-frame drawable worker and 8000 µs
minimum present duration. Enabled `XRT_LOG=info` for both reverse-order runs to
verify source selection: the second CA run logged CADisplayLink selection and
132 callbacks in its first second. The first pair's global INFO markers were
suppressed, but CA's 8.333 ms nominal period and timestamp pattern distinguish
it from CV's 8.342 ms period.

Exclude the first five seconds after the first compositor submission; use valid
`presented_monotonic_ns` timestamps matched by frame ID to `present.csv`.
The remaining measured spans are 32.45–34.60 seconds. Physical intervals below
refer to Metal's reported presentation time, not callback arrival or a separate
optical scanout measurement.

| Run | Mean presentation rate | Intervals >12 ms | Interval p99 | Desired-to-present p50 | Wake lateness p99 |
| --- | --- | --- | --- | --- | --- |
| cv | 116.34 Hz | 110/3775 (2.91%) | 16.684 ms | 25.025 ms | 22.75 µs |
| ca | 103.23 Hz | 462/3497 (13.21%) | 25.025 ms | 25.026 ms | 24.29 µs |
| ca repeat | 112.31 Hz | 217/3826 (5.67%) | 16.684 ms | 25.005 ms | 24.50 µs |
| cv repeat | 117.08 Hz | 96/4051 (2.37%) | 16.683 ms | 16.683 ms | 21.42 µs |

Physical-present minus latest display-link output p50 was respectively 16.683,
24.974, 16.660 and 8.341 ms. Worker drawable-wait p99 was respectively 16.574,
23.466, 17.364 and 16.607 ms. Wake lateness is derived by joining
`frame_pipeline.csv`'s `predict_result.wake_time_ns` to `mark_woke.point_ns`;
`compositor_rt.csv` records scheduler state and does not directly contain that
measurement. Full completion-cadence late/240 windows are retained in the
summary; they are not physical interval counts and include startup.

**Interpretation:** this preliminary repeated comparison favours retaining CV.
Similar wake lateness does not implicate compositor wake scheduling; CA also
had longer drawable-wait tails. Run-to-run variation, changing app GPU durations
and unmeasured system load prevent attributing every difference solely to the
display-link implementation. No subjective head-motion judgement was collected.

All four clients received SIGINT after the capture and then exited with SIGSEGV
during cleanup. The inspected crash report points to `objc_release` during
autorelease-pool draining in `psvr2_openxr_base_main`, not the display-link
callback. This is an unresolved diagnostic teardown defect; captures were
periodically flushed and contain valid presentation data. The original
LaunchAgent plist was restored byte-for-byte and its registration verified idle.

Raw CSVs, service/client logs, `summary.json`, the stdlib `analyze.py`, test
runner, original plist and tested worktree patch are under
`/tmp/monado-display-link-ab-20261003/` (temporary local evidence).

### Corrected CA phase/period and repeated headset A/B — 2026-10-03

The original CA implementation reused CV's projection formula. CoreGraphics
reported 120 Hz (8.333333 ms), but CA's timestamp-to-target interval was
8.341708 ms (119.88 Hz). Ceiling division therefore subtracted two nominal
periods instead of one: CA's derived vblank lagged its own timestamp by another
refresh. CA now supplies `timestamp` directly as the previous refresh and
publishes its timestamp-to-target interval atomically. The compositor consumes
small period corrections, updating the existing fake pacer without resetting
frame IDs, outstanding feedback or present-offset calibration. Large interval
changes (outside ±10%) are rejected to avoid treating a callback divisor as a
physical display-mode change. CV's projection path remains unchanged.

The shutdown crash was identified with `NSZombieEnabled=YES`: a release was
sent to an already-deallocated `MTLTextureDescriptorInternal`. The runtime's
IOSurface swapchain convenience constructor returned an autoreleased descriptor,
but all cleanup paths explicitly released it without first retaining it. The
runtime now retains its reference; this was a runtime ownership bug, rather
than a display-link callback failure.

Tested the same base commit `be345261efbdc1ca2fa16b3197361740a5e9204b` plus
the worktree fixes and existing edits; saved the exact worktree patch with the
captures. Rebuilt the native service, runtime and diagnostic in `build-wine`.
Same PS VR2 / Apple M5 / 862-box scene, 4000×2040 at 120 Hz, 2800×2856 per
eye, camera/gaze streams disabled, three drawables and 8000 µs minimum present
duration. Four 40-second service captures in **cv, ca, ca, cv** order, then two
40-second captures with `XRT_MACOS_CLIENT_COMPOSITOR=1`, **cv, ca**. Restarted
the service between every capture; `XRT_LOG=info` throughout.

Same five-second compositor warm-up exclusion and valid Metal presentation
timestamps as above. Measurement spans are 33.35–34.68 seconds; hosted analysis
selects the client's presenter CSVs, rather than the service's idle presenter.

| Run | Mean presentation rate | Intervals >12 ms | Interval p99 | Desired-to-present p50 | Wake lateness p99 |
| --- | --- | --- | --- | --- | --- |
| Service cv | 119.76 Hz | 4/4079 (0.10%) | 8.343 ms | 16.683 ms | 20.54 µs |
| Service ca | 119.62 Hz | 9/4149 (0.22%) | 8.343 ms | 16.648 ms | 20.33 µs |
| Service ca repeat | 119.76 Hz | 4/4136 (0.10%) | 8.343 ms | 16.648 ms | 21.13 µs |
| Service cv repeat | 119.73 Hz | 5/4134 (0.12%) | 8.343 ms | 16.683 ms | 20.13 µs |
| Hosted cv | 118.74 Hz | 23/3960 (0.58%) | 8.343 ms | 16.683 ms | 19.33 µs |
| Hosted ca | 118.63 Hz | 29/4077 (0.71%) | 8.343 ms | 16.647 ms | 20.33 µs |

Each corrected CA capture logged the 8.341708 ms measured period and active
callbacks. Every consumed CA vblank in these captures exactly equals its
converted `timestamp`; the extra-refresh error is gone. Hosted selection and
visible layers were confirmed in client logs. All six clients exited **0**
after SIGINT, and the original LaunchAgent registration was restored.

**Interpretation:** the original A/B did not establish that CADisplayLink itself
was unsuitable. The corrected implementation is a viable migration candidate,
with closely matched rate, interval tails and median latency in these short
native service/hosted captures. The older runs also had higher, variable app GPU
durations; do not attribute the entire before/after improvement solely to the
phase fix. No subjective comparison or actual Game Mode run was collected,
and 90 Hz remains untested. This initial default decision was superseded by
the UE follow-up and default change below.

The pacing regression test checks that changing the period preserves frame IDs,
old-frame feedback, phase and present-offset calibration. The full macOS build
passed, and all 35 registered tests passed: 32 in the sandbox and the three
shared-memory/socket/CoreAnimation tests after rerunning with system access.
Linux CI was not run locally. Raw CSVs, logs, runner,
`summary.json`, `analyze.py`, worktree patch and build/test logs are under
`/tmp/monado-display-link-phase-fix-20261003/` (temporary local evidence).

### Unreal Game Mode follow-up — 2026-10-03

The user ran MonadoMacTest with the local UnrealEditor executable, project
`~/Documents/Unreal Projects/MonadoMacTest/MonadoMacTest.uproject`,
`-game -vr -NOSCREENMESSAGES -LogCmds="LogHMD VeryVerbose" -stdout -log`,
`UE_OPENXR_LOADER_LIBRARY=/usr/local/lib/libopenxr_loader.dylib`, the
`build-wine/openxr_monado-dev.json` runtime, and the app release shared-event
wait thread enabled. Requested order: **CV/off, CA/off, CA/on, CV/on**, where
on/off selects `XRT_MACOS_CLIENT_COMPOSITOR`. The user confirmed changing the
display-link selector only in the client environment. Therefore both off runs
actually used the service's default **CV**; they are repeat service runs,
not a service CV/CA comparison. Set the selector in the service launchd
environment and restart the service for that comparison.

The service logs report `v25.1.0-2073-g2276cfba9`; the current base is
`2276cfba9bbca3109e8e6ac5e4d1c12b34b0e2c9` plus worktree edits. This is a
later base than the preceding diagnostic captures. Matching client/service
PIDs: 45822/45949, 46172/46298, 46428/46555, 46690/46817. The two service
presenters wrote to `~/psvr2-trace`; the two hosted client presenters wrote to
`/tmp`. Both hosted logs confirm a visible hosted layer. Hosted CA logs the
corrected 8.341709 ms period (119.880 Hz).

Service RT traces show repeated priority **97 → 4 → 97** changes. After five
seconds from the first app-active compositor iteration, the priority-4 spans
physically present at **17.03–18.23 Hz** with median intervals **50–67 ms**.
The long priority-97 spans recover to **119.13/119.56 Hz**, with interval p99
**8.343/8.344 ms**. This reproduces the service throttling mechanism; averaging
the whole run would obscure it. The user confirms Game Mode was **on in every run**, except while the
Cmd+Esc menu was open during the middle third, temporarily suspending it.
The long priority-97 recovery spans therefore correspond to the menu interval;
the priority-4 spans on either side correspond to active Game Mode. Exact
toggle timestamps were not captured, so RT policy defines the span boundaries.

| Hosted run | Compositor prediction rate | Prediction interval p99 | Logged completion rate | Logged late completions | UE new-frame submission rate | >100 ms event-wait warnings |
| --- | --- | --- | --- | --- | --- | --- |
| CA, PID 46428 | 115.94 Hz | 13.60 ms | 112.76 Hz | 161/2640 (6.10%) | 7.62 Hz | 90 |
| CV, PID 46690 | 111.25 Hz | 16.32 ms | 109.26 Hz | 307/3120 (9.84%) | 7.44 Hz | 195 |

Prediction rates exclude five seconds after the first app-active iteration and
cover 19.10/27.11 seconds. UE submission rates come from `xrEndFrame` entries
in the project logs, excluding five seconds after the first submission;
measurement spans are 20.08/27.15 seconds. The service runs submit new UE
frames at only **12.50/12.32 Hz** over 25.11/23.12 seconds. Completion statistics
use all complete 240-frame log windows, including startup, and are command
completion measurements, **not physical presentedTime**. The hosted
`present`, `presented`, `vblank` and `compositor_rt` CSVs are zero bytes;
partially flushed client GPU CSVs also end before the full session. Thus no
hosted physical latency/cadence or RT-policy comparison can be made from these
files. Fully buffered tracing was enabled in the inherited launchd environment;
for the next short capture set `PSVR2_TIMING_TRACE_FULLY_BUFFERED=0` explicitly
in both processes and check non-empty presenter files after exit.

**Interpretation:** CA is somewhat better in this single hosted pair and the
corrected period is active. This supports continuing the CADisplayLink
migration, but is not a reason to switch the default yet. The user confirms UE normally runs at about 13–14 FPS: this was an
intentional heavy-client stress test. Both hosted runs maintain much faster
compositor presentation despite slow app updates and shared-event waits; those
waits alone do not establish a regression. Hosted UE submissions were about
7.5 Hz in these logs. A physical pacing or visual acceptance gate still needs
non-empty presentedTime traces and the subjective result. Game Mode state is user-confirmed and corroborated by service policy; no
independent flag capture or subjective comparison was recorded here;
90 Hz remains untested. The subsequent default decision is recorded below.

A fifth mini run set **CA in the service environment**, with client compositing
**off** (client/service PID **48606/48751**), leaving trace buffering unchanged.
Its service presenter CSVs flushed successfully on shutdown. The log confirms
8.341708 ms (119.880 Hz), and all 1731 consumed CA vblanks equal CA's timestamp
exactly. During active Game Mode, its two priority-4 spans physically present
at **17.77/17.69 Hz** (8.89/6.84-second measurement spans, median **58.39 ms**,
p99 **108.44 ms**). During the middle menu interval, priority 97 recovers to
**119.03 Hz** over 10.65 seconds: interval p99 **8.343 ms**, 7/1268 intervals
>12 ms (**0.55%**). This confirms that CA does not remove the service's Game
Mode throttling; it behaves similarly to CV when the menu suspends Game Mode.
The brief final priority-97 span occurs near exit and is excluded from the
steady-state comparison. Raw evidence is `~/psvr2-trace/monado_psvr2_48751_*`,
`/tmp/ue-client-ca-no-cc.log`; derived results are in `server-ca-summary.json`
in the analysis directory below. Successful service flush does not explain
why the earlier hosted clients failed to flush their presenter files.

Evidence: `/tmp/ue-client-{cv,ca,ca-cc,cv-cc}.log`, the corresponding PID CSVs,
and `~/Library/Logs/MonadoMacTest/` project logs. Analysis, summary, copied
client/service logs, provenance and worktree patch are saved under
`/tmp/monado-ue-analysis-20261003/` (temporary local evidence).

User subjective follow-up: none of the user-run configurations felt smooth;
CA with client compositing was probably best. The user made mostly rotational
head movements throughout. This gives the user runs a visual motion-validation
role that the agent's static-headset captures do not have. Matching service
IMU, SLAM, pose and prediction CSVs exist for all five user runs (service PIDs
45949, 46298, 46555, 46817, 48751), including the hosted runs whose client
presenter CSVs are empty. The retrospective motion correlation below uses
these recordings; static presentation rates alone do not establish rotational
smoothness.

### CADisplayLink promoted to default — 2026-10-03

Following the corrected native headset captures and user-run Unreal/Game Mode
stress tests, `XRT_MACOS_DISPLAY_LINK` now defaults to **ca**. Corrected CA
matches CV closely in native physical presentation, and the hosted UE pair
favours CA in compositor and completion cadence. The deliberately heavy app
is an appropriate stress condition; low app FPS is not itself a failed display
link acceptance gate. Both service CV and CA suffer the same external Game
Mode scheduler demotion, addressed by client compositing.

Keep `XRT_MACOS_DISPLAY_LINK=cv` as an explicit comparison/fallback. On macOS
before 14, or if the CA source cannot be created, the existing automatic CV
fallback remains. This default choice does not establish universal superiority:
hosted physical presentedTime traces, subjective preference and 90 Hz coverage
remain gaps. A created CA source that stops firing still warns and uses estimated
pacing; it does not automatically switch to CV. The warning now gives the correct
explicit CV override.

### Fully buffered hosted Unreal repeats — 2026-10-03

Before further comparisons, verified `PSVR2_TIMING_TRACE_FULLY_BUFFERED`
support in the built runtime and ran a hosted CA preflight. Explicitly set
tracing and fully buffered mode to 1 in UE, with a separate output directory.
Normal **Cmd+Q** shutdown flushed non-empty presenter, presentedTime, vblank,
client GPU and RT policy files. No source changes were needed for flushing.
This proves buffered hosted captures work with normal application shutdown;
it does not establish exactly why the earlier clients left empty files.

The direct executable launch from the agent was denied access to the project
in Documents. Terminal UI control is unavailable, so used normal macOS
LaunchServices (`open -n -W --env ... --args ...`) with the same executable,
project, environment controls and UE flags supplied by the user. This launch
route difference is recorded rather than claiming an identical shell launch.
The service configuration was left unchanged. The headset remained at 120 Hz,
with the original heavy scene and release shared-event wait thread enabled.

An attempted fullscreen/menu CA capture crashed during UE render-target
recreation: `OpenXRMetalMac: no compatible Metal swapchain format found`,
followed by a shared-pointer assertion in `AllocateRenderTargetTextures`.
Its buffered presenter files were lost. Exclude that attempt from pacing
comparisons; the error is not evidence of a display-link failure.

Repeated **CA then CV** without changing window mode during either run.
Successful client PIDs **54538 / 55056**, services **54695 / 55190**. Both
clients closed with Cmd+Q and flushed all required CSVs. Analysis selects
matched 40-second windows, starting ten seconds after the first app-active
compositor iteration; physical spans are 39.990 seconds each. Python runner
clock values are not aligned with trace clock values, so window selection uses
CSV timestamps exclusively. Normal startup window state was retained; no
middle-third Cmd+Esc menu interval was collected in these successful repeats.

| Fully buffered hosted run | Physical presentation rate | Physical intervals >12 ms | Physical interval p99 / max | Desired-to-physical p50 | New app submissions |
| --- | --- | --- | --- | --- | --- |
| CA | 105.18 Hz | 583/4206 (13.86%) | 16.684 / 33.367 ms | 24.990 ms | 6.612 Hz |
| CV | 100.08 Hz | 780/4002 (19.49%) | 16.684 / 50.050 ms | 25.024 ms | 6.615 Hz |

Additional analysis of those same windows (`stable-window/details.json`):
compositor wake lateness p99 is **10.17/9.83 µs** (CA/CV), so scheduling the
hosted compositor wake is healthy. Presentation Metal GPU duration p50 is
**1.07/1.09 ms**, p99 **5.46/5.55 ms**; all completed command buffers have
status 4 (completed), with no GPU error status. Renderer CPU wall duration
p50 is **2.84/3.08 ms**, p99 **12.69/12.10 ms**; occasional renderer stalls
exceed one refresh under this heavy workload.

CA/CV drawable-wait p50 is **7.16/8.33 ms**, p99 **9.35/16.68 ms**. Worker
queue-delay p50 is **0.011/3.066 ms**, p99 **4.14/8.14 ms**, and the newest-frame
worker supersedes **11/175** pending jobs. This places much of the observed
CA advantage in the downstream worker/drawable scheduling, while GPU durations
are similar. It is a correlation in one ordered pair, not a proven causal
isolation of the display-link choice. Image-reuse wait is negligible (p99 1 µs).

Every analysed compositor frame has a valid source and timewarp enabled, but
about **94%** reuse a previous app frame. Source-image timestamp age at predicted
display has median **71.7/70.7 ms**, p99 **204.8/211.2 ms**, maxima **306.7/314.5 ms**.
Median source-use ordinal is 8; maxima 36/37. This is expected stress-test
behaviour: repeated reprojection updates head orientation without creating
new scene animation frames. Pose-query duration is normally short (median
**0.100 ms**, p99 **0.267/0.263 ms**), with rare maxima **10.44/13.93 ms**.
The app-pacer GPU-ready feedback duration median is **153.0/147.9 ms**;
it is not a pure GPU execution measurement. The driver timing files stay in
the service trace directory and were not analysed for tracking accuracy here.

All 4219/4178 sampled hosted compositor iterations retain time-constraint
policy **2**, priority **97**. Policy queries show the service externally
backgrounded (`ext_darwinbg=1`) and UE foreground (`ext_darwinbg=0`, ui-focal)
in both runs, consistent with Game Mode activity. The direct Game Mode flag
requires administrator privileges and was unreadable; the user was away and
could not confirm the visible flag, so do not label this as independently
flag-verified. The client shared-event readiness median is 146.23/144.08 ms,
consistent with the very slow app-update stress condition.

**Interpretation:** CA again performs somewhat better under heavy client load,
now measured with physical presentedTime rather than command completions.
The matched app submission rates reduce one important confound. Both rates
remain below the approximately 119.88 Hz display cadence, and a single ordered
pair is not a statistical superiority claim. These runs strengthen the choice
of CA as the default and fill the hosted physical-trace gap at 120 Hz; 90 Hz,
subjective preference and direct Game Mode flag verification remain uncovered.

Evidence, runners, exact launch environments, normal-exit flush checks, logs,
analysis and summary: `/tmp/monado-ue-buffered-runs-20261003/`; successful
repeat data are under `stable-window/{ca,cv}/`, with `stable-window/summary.json`.
The preflight is separate, and the top-level `ca/` is the failed fullscreen
attempt. No UE test process was left running.

## Foreground-client XPC importance lease / Game Mode diagnostic, 2026-09-18

> **Removed 2026-09-30.** The lease never prevented the Game Mode 97→4
> demotion. Game Mode backgrounds the service from outside
> (`ext_darwinbg=1`), which an importance boost cannot override, and the good
> 18 Sep run coincided with Game Mode being off. The in-process client
> compositor ([macos-client-compositor-design.md](macos-client-compositor-design.md))
> is the fix. This section is kept as history.

The current macOS service architecture uses an ordinary Unix-domain socket and
shared memory for the high-frequency Monado protocol. The existing XPC endpoint
was used only for service activation and Metal object transfer, so RunningBoard
had no long-lived XPC relationship showing that compositor work was performed on
behalf of the foreground game.

An opt-in diagnostic now adds that relationship without moving frame traffic to
XPC:

- launchd registration uses `ProcessType=Adaptive`;
- `XRT_MACOS_XPC_IMPORTANCE=1` in the OpenXR **client process** enables the
  side-channel;
- immediately before the ordinary IPC `session_begin`, the client opens a
  persistent connection to the existing `org.freedesktop.monado.metal-ipc`
  Mach service;
- it sends `acquireXRSessionImportance`; the service retains that method's
  reply block instead of replying immediately;
- a second request on the same connection acts as a synchronous barrier so
  `xrBeginSession` does not continue until the server confirms the lease is
  installed;
- all normal prediction, frame, layer and shared-memory traffic still uses the
  existing Monado Unix IPC path;
- `session_end` completes the held acquire reply, waits for a release
  acknowledgement, then invalidates the XPC connection;
- compositor destruction also releases the lease as a fallback;
- service-side XPC connection invalidation releases any leases owned by that
  exact connection, covering client crash/abnormal teardown.

The service derives the owner PID from `NSXPCConnection.currentConnection`;
the client does not supply or authenticate its own PID for this mechanism.

A valid test must use the launchd-managed direct service. Do **not** set
`XRT_MACOS_METAL_XPC_EXTERNAL_BROKER=1`, because that diagnostic deliberately
removes the direct service Mach endpoint needed for the foreground relationship.

Expected service log markers are:

```
XR_XPC_IMPORTANCE acquired pid=<game-pid> session=0x...
XR_XPC_IMPORTANCE released pid=<game-pid> session=0x...
```

The decisive A/B is UE with Game Mode enabled and the same compositor
time-constraint instrumentation, comparing `XRT_MACOS_XPC_IMPORTANCE=0` with
`=1`. The desired signal is removal of the RunningBoard
realtime-to-timeshare / priority `97 -> 4` clamp while leaving the compositor's
existing requested time-constraint policy unchanged.


## Per-frame reprojection/source handoff trace, 2026-09-19

To diagnose a small apparent backwards reset when a low-rate client source frame
replaces a repeatedly timewarped frame, macOS timing diagnostics now emit two
joinable CSVs whenever `PSVR2_TIMING_TRACE=1` (or explicitly
`XRT_MACOS_REPROJECTION_TRACE=1`).

`monado_psvr2_<service-pid>_reprojection_source.csv` is emitted in the
multi-client compositor before the original app frame id is replaced by the
120-Hz native/system frame id. One row is written for every system compositor
frame. For the focused projection client it records:

- `system_frame_id` and target display time;
- the original client `frame_id` and client display time;
- `source_changed`, based on the actual delivered client frame;
- projection layer timestamp/type;
- left/right swapchain image and array indices;
- the exact submitted left/right projection source poses.

`monado_psvr2_<service-pid>_reprojection.csv` is emitted from the native
renderer for the same `system_frame_id`. It records every compositor pass,
including:

- predicted/desired display times and compute/graphics path;
- fast-path and ATW state;
- independently detected projection source changes;
- source layer timestamp and age at the compositor prediction;
- submitted source poses and fresh scanout-begin/end poses for both eyes;
- angular source-to-scanout deltas;
- the angular step between distinct submitted source poses at a handoff;
- frame-to-frame scanout-pose angular motion;
- the exact 4x4 left-eye timewarp matrices for scanout begin and end.

The two files should be joined on `system_frame_id`. In the UE 12-fps test,
rows with `source_changed=1` are the key events: they let us test whether the
submitted source pose itself steps inconsistently when a new UE image arrives,
whether the fresh scanout pose stays continuous, and whether the exact timewarp
transform has a discontinuity at the handoff. No rendering, prediction,
synchronization, scheduling, or presentation behaviour is changed by this
instrumentation.


### Retrospective correlation of user rotational UE runs — 2026-10-03

The five user recordings above contain matching service IMU, corrected SLAM,
and head-pose traces. No additional headset run was needed for this analysis.
These were the user recordings at `2276cfba9bbca3109e8e6ac5e4d1c12b34b0e2c9`
with the previously described dirty timing/ownership fixes, not the agent's
later static captures. The user reports rotation throughout and Game Mode
suspended during the middle-third Cmd+Esc menu.

Method: discard the first five seconds after active frame prediction starts;
join each compositor frame to its driver pose using both requested display time
(within 1 us) and the frame's recorded pose-query begin/end window. Repeated
requests for the same timestamp can return different poses as tracking advances,
so a timestamp-only join is insufficient. Require compositor/driver quaternion
agreement within 0.001 degrees. The accepted joins agree within 0.000005 degrees.
There are 1,220 / 1,149 / 1,713 / 2,376 / 1,187 accepted frames for service CV1,
service CV2, hosted CA, hosted CV, and actual service CA respectively. Missing
or ambiguous joins are excluded, so results describe this matched subset.

Use shortest-arc quaternion interpolation between corrected SLAM samples
bracketing the target hardware timestamp, with brackets shorter than 35 ms and
no extrapolation. Convert physical presentation timestamps using the matched
pose row's hardware-to-monotonic offset. This is a retrospective **internal
SLAM reference**, not independent tracking ground truth, photon timing, or a
measurement of rendered scene correctness. The measured physical timestamp is
Metal's `presentedTime`. Classify service frames using the latest preceding
compositor priority sample: 4 during throttling and 97 during the menu/recovery.
This classification includes transition edges, rather than precisely isolating
only the menu's steady middle segment.

| Service configuration | Matched physical frames, priority 4 | Physical minus pose target, median / p95 | Orientation disagreement at pose target, p95 | Orientation disagreement at physical presentation, p95 | Physical disagreement while rotating >=60 degrees/s, median / p95 |
| --- | --- | --- | --- | --- | --- |
| CV, first run | 173 | 42.63 / 87.56 ms | 0.223 degrees | 5.39 degrees | 3.31 / 7.94 degrees (29 frames) |
| CV, second run (client requested CA) | 177 | 52.36 / 95.08 ms | 0.244 degrees | 4.83 degrees | 2.88 / 8.86 degrees (26 frames) |
| Actual service CA | 156 | 40.06 / 92.36 ms | 0.218 degrees | 5.80 degrees | 2.24 / 8.76 degrees (39 frames) |

At priority 97, physical-minus-pose-target median is within 0.001 ms of zero in
all three service runs. Physical orientation disagreement p95 is 0.310 / 0.278 /
0.286 degrees respectively; while rotating >=60 degrees/s it is 0.281 / 0.303 /
0.329 degrees. This reinforces the earlier physical cadence result: delayed
service presentation under Game Mode makes a pose that fits its requested time
wrong for the eventual displayed frame. Both CV and CA exhibit this mechanism.
It does not demonstrate a defect in the normal one-refresh timestamp offset,
nor justify replacing the pose predictor.

For the hosted motion runs, the accepted compositor poses disagree with the
retrospective reference at the requested time by median / p95 / p99
**0.141 / 0.302 / 0.443 degrees (CA)** and
**0.162 / 0.432 / 0.819 degrees (CV)**. This is encouraging, but the motions and
sample populations differ, and the original client physical presenter files are
empty. These values cannot rank motion-to-photon accuracy or explain every
remaining visible hitch. Fully buffered static repeats prove physical tracing
works; they cannot supply missing physical timestamps for these motion runs.

Timewarp is enabled for every valid source row in all five runs. Hosted CA/CV
median source-pose ages are 73.62 / 72.04 ms, and p95 rotational warp angles are
12.81 / 14.19 degrees (maxima about 35 degrees). Old app frames are therefore
being reprojected through substantial rotations. Source-pose changes of several
degrees are expected at this frame rate and are not themselves proof of an
output snap. Timewarp cannot refresh scene animation or reconstruct unseen
content in such old images.

IMU rows usually arrive on a 0.5 ms mapped-host interval; pose-query IMU age is
about 0.92–1.01 ms at the median. Rare trace gaps reach 68–177 ms and query ages
reach 68–190 ms across these recordings. These original captures were not fully
buffered: a recorded gap alone cannot distinguish missing device data from
scheduler delay or trace-write perturbation. Do not classify these as headset
sensor dropouts from this evidence.

Next diagnostic priority is a **fully buffered moving-head hosted capture** with
both tracking and physical presentation timestamps, preferably CA first, then a
matched CV control. Correlate pose target to physical display, source switches,
drawable waits and presentation misses before tuning prediction. Existing data
already establish the service Game Mode delay mechanism; additional service A/Bs
are unnecessary to establish it again.

Reproducible local analysis, JSON summaries and matched service per-frame CSVs:
`/tmp/monado-rotation-analysis-20261003/`. These are temporary evidence; this
section records the method, exclusions and conclusions durably.


### CADisplayLink callback compositor experiment — 2026-10-03

The callback-body implementation below and its five-pair results are historical.
The deferred CA run-loop revision was also tested and then removed; see the
[final decision](#deferred-run-loop-results-and-retirement). Ordinary CA pacing
and client-hosted compositing remain in use.

Branch: `codex/cadisplaylink-compositor`, created from local
`macos-upstream-clean` at `2276cfba9bbca3109e8e6ac5e4d1c12b34b0e2c9`.
The existing uncommitted CA period/default, pacer, ownership and diagnostic
changes remain in the checkout. This experiment is opt-in; ordinary CA pacing
remains the default.

`XRT_MACOS_DISPLAY_LINK=ca XRT_MACOS_CA_COMPOSITOR=1` moves one complete multi
compositor frame (prediction, broadcasts, layer transfer, distortion/timewarp
rendering and asynchronous present enqueue) onto the existing dedicated CA
run-loop thread. It retains the pacer's ordinary wake sleep inside that callback
to isolate thread placement from render-phase changes. It retains the fake pacer
and measured presentation-offset
feedback, rather than substituting CA's next target as the physical pose target.
Session begin/end and shutdown remain controlled by the multi compositor thread.
The newest-frame drawable worker, minimum-duration presents, swapchains and GPU
handoff remain unchanged. This differs from the removed CAMetalDisplayLink
experiment, which also supplied callback-owned drawables.

A synchronous optional dispatcher permits only one frame in flight. The control
thread arms a frame on the CA run loop, waits for completion, and then checks
session state before arming the next frame. A paused/unavailable link uses the
ordinary path (also necessary to bootstrap the first acquire). A 100 ms callback
wait timeout cancels the pending frame on the run loop before falling back;
cancellation waits for a racing callback to finish, so it cannot duplicate a
frame or leave a dangling stack context. This timeout does not abort an already
executing renderer or impose a hard upper bound on a hung callback.

The callback thread receives the same real-time priority attempt and per-period
Mach time-constraint settings as the original render thread. The policy and
scheduler-trace caches are per-thread, so a bootstrap/fallback on the control
thread cannot suppress policy application on the callback thread. Validate the
observed policy/priority in `compositor_rt.csv` rather than assuming macOS grants
or retains it.

With timing tracing enabled, **both modes** write `ca_callback.csv` for every CA
callback, including callbacks with no frame armed. Columns record callback
entry/exit, mapped CA timestamp/target, callback/timestamp intervals, whether a
frame ran, frame begin/end, entry-minus-timestamp and time remaining to CA's next
target. Exit is sampled before writing the trace row/signalling the control
thread. Join frame-pipeline prediction events inside the frame begin/end window.
This resolves the earlier limitation where consumed vblank samples could hide
callbacks. Callback-target overrun is diagnostic; physical presentation still
uses the calibrated pacer target and must be judged from `presented.csv`.

For a client-hosted A/B, keep the existing UE project and launch arguments and
set these in the **UE environment**:

```sh
XRT_MACOS_CLIENT_COMPOSITOR=1
XRT_MACOS_APP_RELEASE_SHARED_EVENT_WAIT_THREAD=1
XRT_MACOS_DISPLAY_LINK=ca
XRT_MACOS_CA_COMPOSITOR=0 # control; change only this to 1 for callback compositing
PSVR2_TIMING_TRACE=1
PSVR2_TIMING_TRACE_FULLY_BUFFERED=1
PSVR2_TIMING_TRACE_DIR=/tmp/ca-compositor-control # separate directory per run
```

Exit normally to flush the traces. Compare equal post-startup windows: physical
intervals, presentation-to-pose-target delay, renderer and worker durations,
callback gaps/unarmed callbacks, source-frame delivery rate, and observed thread
policy. Require a moving-head buffered validation before claiming smoother
rotation. Build and all 35 CTest suites pass on macOS (IPC and remote-layer tests
require access outside the sandbox); Linux CI has not been run locally.


#### Initial headset results and phase correction

Launch used the same UE project and `-game -vr -NOSCREENMESSAGES`,
`-LogCmds="LogHMD VeryVerbose" -stdout -log` arguments, via the previously
validated LaunchServices `open --env` route. All recorded clients used client
compositing, CA, the shared-event wait thread, and fully buffered tracing. The
headset was static; no fullscreen transitions or Cmd+Esc menu intervals were
requested. UE's native Game Mode status was not independently confirmed in
these new runs. Both modes' executing compositor threads retain policy 2 and
priority 97 throughout the analysed windows. All four usable clients exited
normally and flushed the required streams.

Windows are 40 seconds starting 10 seconds after the first post-bootstrap
prediction. Client GPU source submissions give the new-app-frame rate; physical
cadence comes from nonzero Metal presentation timestamps. Source submissions
also confirm the throughput differences when anchoring the window at first
submission instead of first active prediction.

| Chronological usable run | Client PID | Physical Hz | Intervals >12 ms | New app frames/s | Pose-query-to-physical p50 / p95 | Physical minus pose target p99 |
| --- | --- | --- | --- | --- | --- | --- |
| Ordinary CA paced control 1 | 69215 | 119.53 | 7/4780 (0.146%) | 13.06 | 19.60 / 27.96 ms | 0.071 ms |
| Immediate CA callback (discarded variant) | 69641 | 108.63 | 445/4344 (10.244%) | 8.18 | 24.86 / 24.96 ms | 12.049 ms |
| Phase-matched CA callback | 72286 | 117.61 | 86/4704 (1.828%) | 12.33 | 19.59 / 19.63 ms | 8.341 ms |
| Ordinary CA paced control 2 | 72775 | 102.15 | 704/4085 (17.234%) | 6.96 | 23.60 / 36.28 ms | 14.637 ms |

The immediate variant woke about **5.33 ms before the pacer's planned wake**,
changing phase as well as thread placement. Its callback work p99 was 8.92 ms,
with 73/4716 frame callbacks completing after CA's next target. It was replaced
with the phase-matched implementation; no separate immediate-mode toggle remains.
The current mode preserves the ordinary wake wait inside the callback.

In the phase-matched run, wake lateness p99 is 9.08 us. Callback work includes the
intentional wait: p50/p99 is 5.57/5.82 ms, maximum 19.33 ms. Only 8/4786 frame
callbacks overrun CA's next target; 4794 total callbacks contain 4786 frames.
Renderer wall time p50/p99 is 0.223/0.430 ms, maximum 13.98 ms. Callback entry
lateness against CA timestamp p99 is 22.87 us, with a rare maximum 7.99 ms.
This makes missed callbacks measurable, but does not attribute all 86 long
physical intervals to those eight callback overruns: the drawable/presentation
worker remains a separate source of misses.

The first ordinary control has renderer p50/p99 0.217/0.446 ms and 27.92 us
callback entry lateness p99. The second has renderer p50/p99 2.112/11.503 ms,
while its timestamp-only CA callback still arrives with p99 28.25 us lateness
(maximum 57.29 us). It therefore reproduces the distinction between a healthy
link callback and a compositor/presentation pipeline falling behind under load.

**Interpretation:** immediate callback compositing was a poor phase choice in
this capture. Phase matching materially improves that variant. Against the first
ordinary control it has more physical misses, similar median query-to-display
latency and a shorter p95 latency; against the second it appears substantially
better, but workload/source-delivery differences and the large control variation
prevent a causal ranking. Retain only the phase-matched implementation as an
opt-in experiment on this branch, not a replacement default or a claim of smoother
head rotation. Repeat with confirmed/matched Game Mode/window state and buffered
motion traces before deciding whether its latency/cadence trade-off is useful.

Excluded captures: the initial layout defect (fixed and caught by the local-session
lifecycle test) prevented the first control from running successfully. The first
phase-matched recording, PID 71366, ran but UE then faulted in
`FModuleManager::UnloadModule` during shutdown; its presenter/callback buffers were
not flushed. Do not infer physical performance from that recording or assume
the reported UE stack establishes the crash's root cause. The repeat above did
exit and flush successfully.

Evidence and analysis: `/tmp/monado-ca-drive-20261003-verified/` for the first
control/immediate pair and saved immediate worktree patch;
`/tmp/monado-ca-drive-20261003-aligned-repeat/` for the current callback/repeated
control pair. Each contains process/environment provenance, logs, flush checks,
CSV streams and `summary.json`. Analysis scripts are `/tmp/ca-drive-analysis.py`
and `/tmp/ca-drive-analysis-repeat.py`. The failed recordings are under
`/tmp/monado-ca-drive-20261003/` and `/tmp/monado-ca-drive-20261003-aligned/`.
Final phase-matched build and all 35 macOS CTest suites pass; Linux CI and moving
headset validation remain pending.


#### Five alternating pairs with fully buffered traces — 2026-10-03

The phase-matched implementation above was held constant for five usable runs
of each mode, alternating **paced, callback**. Tested checkout: branch
`codex/cadisplaylink-compositor`, base commit
`2276cfba9bbca3109e8e6ac5e4d1c12b34b0e2c9` plus the uncommitted worktree patch
saved with the evidence. This is not a test of that base commit alone.

Both modes used the user's MonadoMacTest project and original UE arguments,
client compositing, CA, the shared-event wait thread and fully buffered traces.
Only `XRT_MACOS_CA_COMPOSITOR=0/1` changed. Each analysis window is 40 seconds,
starting 10 seconds after the first post-bootstrap prediction. Every physical
trace spans the full window, and every selected presentation has both an exact
frame-ID pose-query and renderer-pose-target match. All executing compositor
samples have policy 2 / priority 97; timewarp is enabled throughout.

| Pair | Mode | Client PID | Physical Hz | Intervals >12 ms | New app frames/s | Query-to-physical p50 / p95 (ms) |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | paced | 75472 | 119.61 | 0.167% | 13.02 | 19.59 / 19.63 |
| 1 | callback | 75922 | 106.68 | 12.351% | 7.73 | 19.60 / 27.95 |
| 2 | paced | 76708 | 117.91 | 1.612% | 8.87 | 19.59 / 19.63 |
| 2 | callback | 77825 | 104.88 | 14.231% | 7.58 | 19.60 / 27.95 |
| 3 | paced | 78468 | 105.15 | 13.915% | 7.23 | 19.62 / 36.26 |
| 3 | callback | 79028 | 105.21 | 13.926% | 7.62 | 19.60 / 27.95 |
| 4 | paced | 79528 | 106.68 | 12.280% | 7.45 | 19.61 / 28.58 |
| 4 | callback | 80972 | 90.17 | 30.782% | 5.91 | 27.87 / 36.28 |
| 5 | paced | 81737 | 95.45 | 24.391% | 6.33 | 27.91 / 36.48 |
| 5 | callback | 82095 | 103.23 | 16.057% | 7.19 | 19.60 / 27.95 |

Treat each run as the comparison unit (five per mode), rather than treating
thousands of frame intervals as independent trials. Across runs:

| Metric | Ordinary CA paced | CA callback compositor |
| --- | --- | --- |
| Physical Hz, mean / median | 108.96 / 106.68 | 102.03 / 104.88 |
| Physical Hz range | 95.45–119.61 | 90.17–106.68 |
| >12 ms interval percentage, mean / median | 10.47% / 12.28% | 17.47% / 14.23% |
| Pooled >12 ms intervals (descriptive only) | 2124/21787 (9.75%) | 3483/20405 (17.07%) |
| New app frames/s, mean / median | 8.58 / 7.45 | 7.20 / 7.58 |
| Median of run query-to-physical p50 / p95 / p99 | 19.61 / 28.58 / 36.40 ms | 19.60 / 27.95 / 27.97 ms |
| Median of run renderer p50 / p99 | 1.148 / 10.539 ms | 0.247 / 6.597 ms |
| Median of run callback entry lateness p99 | 0.031 ms | 3.441 ms |
| Median of run callback duration p50 / p99 | 0.000 / 0.003 ms | 5.579 / 11.838 ms |
| Median of run drawable-wait p95 | 15.826 ms | 15.730 ms |

Callback minus paced physical cadence by adjacent pair is **−12.93, −13.03,
+0.06, −16.51, +7.78 Hz**. Callback mode is clearly worse in three pairs,
approximately tied in one, and better in one; the approximately tied pair has
essentially identical long-interval percentages. Its latency tail is shorter
in several loaded comparisons, but there is no consistent cadence improvement.
App delivery and control performance vary substantially, including a decline
in later controls. Fixed AB order, changing workload/system state and early
interim analysis activity prevent attributing all differences to thread placement.
This is a repeated operational comparison, not a randomized benchmark.

The callback mode spends about 5.3 ms of its typical 5.6 ms callback intentionally
waiting for the ordinary pacer wake: prediction-result to actual wake medians
are 5.333–5.336 ms across its five runs (including the small intervening
bookkeeping). Under load, renderer work extends callback
p99 beyond the 8.34 ms refresh period. Between **520 and 969 frame callbacks per
40-second run** overrun CA's next target (12.18–25.70% of executed frame callbacks).
Callback arrival p99 is consequently delayed by milliseconds. In contrast,
ordinary timestamp-only callbacks retain roughly 31 us median-of-run p99 entry
lateness even when physical cadence degrades. These traces support a mechanism
where running the compositor in the callback couples renderer/wait duration to
later callback delivery. They do not establish a one-to-one attribution of each
physical miss to a callback overrun; the asynchronous drawable/present worker
still contributes independently. A zero frame-overrun count in paced mode is
not a compositor deadline result: that callback executes no compositor frames.

**Decision:** retain ordinary CADisplayLink pacing as the default. The
phase-matched callback compositor remains an opt-in branch experiment, with a
measured latency-tail trade-off but no demonstrated overall benefit in this
series. This does not reverse the earlier CA-versus-CV default decision: both
arms here use CA. Moving-head validation remains necessary before making claims
about perceived rotational smoothness.

Capture quality and shutdown handling:

- The headset remained static. Game Mode was not independently confirmed, and
  no fullscreen or Cmd+Esc transitions were requested.
- Four attempts with empty required physical/callback buffers were excluded and
  repeated in the same mode: paced PID 76263; callback PIDs 77042, 77486 and 80531.
  The first three had UE shutdown faults; their stacks do not establish the root
  cause. The last used the user's double-interrupt exit suggestion, implemented
  as two SIGINT signals; it exited without saving those fully buffered streams.
- Usable runs 4–10 explicitly flushed stdio with `fflush(0)` via LLDB **after**
  the measurement window, then detached before exiting. These debugger/shutdown
  intervals are excluded from analysis. Runs 1–3 flushed on exit. Run 7 required
  SIGTERM after two unresponsive normal-quit attempts; its post-window flush had
  already preserved the data. Runs 8–10 exited with double SIGINT after flushing.
- Early interim CSV analysis ran around some subsequent launches/windows;
  later captures avoided that concurrent analysis. The timing windows are valid,
  but this additional background load limits tightly controlled comparisons.

Evidence: `/tmp/monado-ca-drive-ten-20261003/`, with ten separate run directories,
`excluded/`, launch/process provenance, logs, flush checks, `commit.txt`,
`worktree.patch`, `summary.json`, `aggregate.json`, `wait-summary.json` and
`run-summary.csv`.
Reproduction scripts: `/tmp/ca-drive-ten.py`, `/tmp/ca-drive-ten-analysis.py`,
`/tmp/ca-drive-ten-aggregate.py` and `/tmp/ca-drive-ten-flush.py`. The table and
method above retain the conclusions if temporary traces are removed. No source
changed during this series; the preceding full build and all 35 macOS CTest
suites still apply. Linux CI and moving-head validation remain pending.


#### Deferred CA run-loop revision — 2026-10-03

This historical follow-up tested `XRT_MACOS_CA_COMPOSITOR=1` on
`codex/cadisplaylink-compositor` before retiring the dispatch experiment.
It replaced execution inside the display-link callback with immediate execution on the same dedicated CA run-loop thread,
after preparation on the multi-compositor control thread. Ordinary CA pacing
remains the default.

`renderer_stage.csv` was added before changing scheduling. A fully buffered
40-second hosted UE callback capture (PID 84893, same project/arguments) had
118.43 Hz, 1.14% physical intervals >12 ms and 12.35 new app frames/s. Previous
GPU fence-wait p50/p99 was 0/0.002 ms, GPU timing feedback p99 0.007 ms and queue
submission p99 0.100 ms. Renderer p99 was 0.423 ms but its maximum was 9.584 ms;
that frame's pose query took 9.319 ms, fence wait 0.002 ms and submit 0.108 ms.
Several other long frames also coincide with long pose retrieval, although some
stalls are outside the pose query. This capture does not establish GPU fence
waiting as the dominant bottleneck. Hosted pose retrieval uses synchronous IPC
on the application's shared connection; these timings do not yet distinguish
connection contention, service delay and client scheduling.

The revision:

1. Predict and broadcast timing on the control thread.
2. Finish previous-GPU fence/feedback work before the ordinary pacer sleep.
3. Wait for the original planned wake, then retrieve views on the control thread
   for this exact frame ID, predicted display target and scanout endpoints.
4. Dispatch immediately to the CA run loop. On that thread, transfer application
   layers, consume the prepared view result, record timewarp/distortion commands,
   submit and enqueue presentation. No additional display-link tick is awaited.

CA clock callbacks only publish refresh timestamps. The renderer can still delay
that run loop if command recording/submission itself blocks, but pose IPC and
intentional pacing sleeps no longer block its callback delivery. This is a
change in scheduling architecture, not evidence that any GPU or pose delay has
been eliminated. Pose retrieval remains after wake; moving it before layer
transfer introduces a small additional query-to-render dispatch interval, which
must be measured rather than assumed free. The pacer still chooses the physical
pose target and consumes measured presentation feedback.

Preparation and rendering are serialized by the existing control loop and
synchronous selector dispatch. Cached views match both frame ID and display
target, are consumed once, and retain failure rather than retrying blocking IPC
on the CA thread. Bootstrap/unavailable/paused CA uses control-thread rendering.
The old callback-arm timeout/cancellation code is removed. As before, synchronous
dispatch does not impose a hard timeout on a hung renderer or run loop. Defaults
and Linux keep the ordinary render path without the optional preparation hooks.

`renderer_stage.csv` records stage begin/end monotonic timestamps, current frame
ID and result. `draw_dispatch` includes nested fence/submit stages, so durations
must not be summed. `ca_render.csv` records dispatch request, render begin/end
and the last CA target; match render windows to frame-pipeline events.
`ca_callback.csv` still covers every clock callback, with its historical frame
columns zero. Judge physical timing from `presented.csv`, not the last CA target.

Build and all 35 macOS CTest suites pass after the revision. Hardware timing
validation follows below; Linux CI and moving-head validation remain pending.
Baseline evidence: `/tmp/monado-ca-stage-20261003/before-callback/`, including
process provenance, full buffered traces, saved worktree patch and stage summary.


##### Deferred run-loop results and retirement

Five new alternating pairs used the same hosted MonadoMacTest launch and buffered
40-second windows after 10 seconds of warmup. No heavy analysis, build or tests
ran concurrently with these ten measurement windows. All traces span the full
physical window. For deferred renders, analysis verifies that the prepared view
query completed before renderer entry, with matching frame IDs and predicted
display targets. Both executing UE threads retain policy 2 / priority 97.

| Pair | Mode | PID | Physical Hz | Intervals >12 ms | New app frames/s | Query-to-physical p99 (ms) |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | paced | 88164 | 119.68 | 0.104% | 11.33 | 19.63 |
| 1 | deferred-ca | 88758 | 119.18 | 0.462% | 10.11 | 24.09 |
| 2 | paced | 89275 | 119.68 | 0.146% | 10.71 | 19.64 |
| 2 | deferred-ca | 89748 | 119.51 | 0.251% | 10.13 | 23.94 |
| 3 | paced | 90216 | 119.34 | 0.356% | 10.41 | 19.66 |
| 3 | deferred-ca | 90686 | 119.35 | 0.398% | 10.09 | 24.19 |
| 4 | paced | 91140 | 118.83 | 0.821% | 9.99 | 27.94 |
| 4 | deferred-ca | 91605 | 118.75 | 0.863% | 9.58 | 24.71 |
| 5 | paced | 92058 | 119.28 | 0.419% | 10.13 | 19.64 |
| 5 | deferred-ca | 92509 | 118.93 | 0.736% | 9.69 | 24.66 |

| Across five runs | Ordinary CA pacing | Deferred CA render |
| --- | --- | --- |
| Mean physical Hz | 119.36 | 119.14 |
| Mean percentage of intervals >12 ms | 0.369% | 0.542% |
| Mean new app frames/s | 10.52 | 9.92 |
| Median of run renderer p99 | 2.569 ms | 0.237 ms |
| Median of run query-to-physical p50 / p95 / p99 | 19.59 / 19.63 / 19.64 ms | 19.60 / 21.64 / 24.19 ms |
| Median of run CA callback duration p99 | 0.003 ms | 0.003 ms |
| Median of run CA callback entry lateness p99 | 0.029 ms | 0.023 ms |
| Median of run CA run-loop render p99 | n/a | 0.328 ms |
| Median of run CA dispatch delay p99 | n/a | 0.019 ms |
| Median of run pose-query-end to renderer-entry p99 | n/a | 0.132 ms |

The deferred revision removes the intentional wait and pose retrieval from CA
callbacks and makes the actual CA render critical section short. Against its
single original-callback diagnostic pilot, renderer p99 falls from 0.423 to
0.208 ms and callback p99 from 5.808 to 0.003 ms; the revised pilot has a 12.821 ms
prepared-view stall off the CA thread. That pilot alone is not a causal ranking.
The new repeated comparison finds no end-to-end gain: physical cadence is
slightly worse, all five deferred runs have more >12 ms intervals than their
adjacent control, and the typical latency tail is longer. Mean render iterations
are also about 125.3/s deferred versus 119.8/s paced, despite roughly 119 Hz
physical output. Removing callback gating therefore changed render scheduling
as well as thread placement; this can add work without adding displayed frames.
Do not interpret the comparison as proof that every possible CA-thread design
must lose. Fixed AB order and a modest decline in source delivery across runs
also limit causal attribution.

**Decision:** remove `XRT_MACOS_CA_COMPOSITOR`, CA render dispatch, prepared-view
caching, generic dispatcher hooks and the dispatch-only tests. Keep ordinary CA
pacing, client-hosted compositing, `ca_callback.csv` and `renderer_stage.csv`.
This retires the tested compositor-thread variants, not the client-hosted Game
Mode architecture or CADisplayLink's replacement of CVDisplayLink. Shared-event
handoff, asynchronous drawable acquisition, late pose queries and timewarp remain.
The revised dispatch tests passed before removal; final build/CTest validation
also covers the remaining implementation after cleanup.

Shared-event checks: every analysed run records `semaphore_pushed`,
`semaphore_wait_start`, `semaphore_ready` and `scheduled` in `client_gpu.csv`.
The presenter explicitly reports Metal shared-event handoff and 0 ms Vulkan CPU
wait; `XRT_MACOS_APP_RELEASE_SHARED_EVENT_WAIT_THREAD=1` was set in every launch.
Median-of-run client shared-event wait p50 is about 87.68 ms paced and 93.30 ms
deferred, consistent with the slow source frame delivery. The wait thread gates
new app-frame readiness while the compositor can reproject the last ready frame.
It does not wait for every refresh to produce a new application frame. Occasional
100 ms warnings concern those client-GPU waits, not evidence of a pose timeout.

A separate read-only policy diagnostic ran after the ten comparisons, using the
same UE launch (PID 9801, service PID 9959). Eight snapshots over roughly 21
seconds show service `ext_darwinbg=1` and **all 12–13 observed service threads at
priority 4 / policy 1**. UE remains `ext_darwinbg=0`, UI-focal, with its compositor
threads at priority 97. The Game Mode bit itself is unreadable with the current
query permission; this independently verifies the external-background mechanism,
not the Game Mode bit or the state of all preceding ten runs. It confirms that
moving rendering into UE does not rescue the service's tracking/IPC priorities.
Pose queries still cross the application's serialized IPC connection into that
service. The stage evidence does not distinguish app-connection contention,
service scheduling and client scheduling for each individual pose stall.

Next architecture priority is to remove that synchronous tracking dependency:
publish timestamped tracking state at driver ingestion through shared memory and
perform equivalent prediction locally in the hosted client. Include sequence and
source timestamps, pose/relation validity and velocities; measure publication
age during Game Mode. A fresh nonblocking read avoids the service round trip,
but cannot manufacture newer tracking data if service-side ingestion/publication
itself stalls. Validate prediction equivalence and moving-head physical timing
before enabling that path. No shared-memory tracking implementation is claimed
by this experiment.

Evidence: `/tmp/monado-ca-stage-20261003/`, with diagnostic pilots, `pairs/`
(ten complete streams and per-run/aggregate JSON), `policy-check/` (thread/policy
snapshots and full buffered traces) and `retired-experiment.patch` for recovery.
Each capture saved its process/environment and worktree patch. Scripts are
`/tmp/ca-stage-capture.py`, `/tmp/ca-stage-pairs.py`, `/tmp/ca-stage-analysis.py`
and `/tmp/ca-stage-policy.py`. Base commit is
`2276cfba9bbca3109e8e6ac5e4d1c12b34b0e2c9` with the saved uncommitted patches.
All clients exited via double SIGINT after successful post-window debugger
flushing; no required stream was empty and no capture was excluded in this
series. Headsets remained static, and earlier comparison Game Mode/window state
was not independently confirmed. Source after cleanup preserves the unrelated
pacer/default, ownership and PS VR2 diagnostic changes. Linux CI and moving-head
validation remain pending.

## Shared tracking and client-local prediction — 2026-10-03

**Decision:** retain as an opt-in transport experiment, not a new default or a
proven smoothness fix. It consistently removes the hosted compositor's long
synchronous pose-query tail. Physical presentation still varies with GPU waits;
mean and median comparisons disagree. Moving-head validation is required.

Branch `codex/macos-shared-tracking`, base `2276cfba9`, with uncommitted changes
preserved from the CA pacing work. Hardware is connected PS VR2, 4000×2040 at
120 Hz. Captures: `/tmp/monado-shared-tracking-20261003/`; each directory contains
launch/environment metadata, a tracked worktree patch and archive of new files,
post-window flush output, all client CSVs, and a stage summary. The prototype
comparison is `pilot-local`, then `01-ipc` through `09-ipc`, alternating local/IPC
for five runs each. Only `XRT_MACOS_SHARED_TRACKING=1/0` changes between launches.

Launch keeps the user's UnrealEditor application and MonadoMacTest project,
`-game -vr -NOSCREENMESSAGES -LogCmds="LogHMD VeryVerbose" -stdout -log`,
`IPC_LOG=info`, `XRT_MACOS_CLIENT_COMPOSITOR=1`,
`XRT_MACOS_APP_RELEASE_SHARED_EVENT_WAIT_THREAD=1`,
`XRT_MACOS_DISPLAY_LINK=ca`, the `build-wine` OpenXR manifest and the installed
OpenXR loader. All traces use `PSVR2_TIMING_TRACE=1` and
`PSVR2_TIMING_TRACE_FULLY_BUFFERED=1`. The headset is static. Startup window state
is retained; there is no fullscreen transition or Cmd+Esc menu interval.
Game Mode's bit is unreadable, so these captures do not independently verify it.
Every post-window policy sample in the nine repeated runs shows service
`ext_darwinbg=1` and all its 12–13 sampled threads at priority 4; UE's compositor
threads remain priority 97. RT CSVs also show policy 2 / priority 97.

Each analysis window lasts 40 seconds, starting 10 seconds after the second
compositor `predict_entry`. Physical `presented_monotonic_ns` values cover both
ends. After the window, take a process-policy sample and attach LLDB only to
`fflush(0)`, verify success and detach, then send SIGINT twice, one second apart,
as the user's usual double Ctrl+C exit. **Every run flushed nonempty physical,
renderer and pose traces.** Analysis and builds run with UE closed. Source ages
are computed exclusively within Monado's host clock domain, not from Python's
monotonic clock.

| Run | PID | Physical Hz | >12 ms % | App Hz | Pose p99 ms | Previous fence p99 ms | Query→physical p99 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| pilot-local | 19583 | 117.81 | 1.719 | 11.72 | 0.016 | 0.002 | 27.94 |
| 01-ipc | 21742 | 119.70 | 0.084 | 12.93 | 0.185 | 0.002 | 19.63 |
| 02-local | 22281 | 117.35 | 2.131 | 11.27 | 0.017 | 0.006 | 27.95 |
| 03-ipc | 22645 | 90.72 | 30.687 | 6.20 | 0.612 | 14.711 | 44.95 |
| 04-local | 23011 | 103.28 | 15.908 | 6.97 | 0.018 | 11.395 | 36.78 |
| 05-ipc | 23562 | 119.81 | 0.021 | 12.38 | 0.174 | 0.002 | 19.63 |
| 06-local | 24027 | 119.61 | 0.188 | 10.66 | 0.015 | 1.638 | 19.64 |
| 07-ipc | 24493 | 119.65 | 0.146 | 10.28 | 0.184 | 1.491 | 19.63 |
| 08-local | 24948 | 119.18 | 0.525 | 10.28 | 0.016 | 3.150 | 27.90 |
| 09-ipc | 25417 | 119.30 | 0.440 | 9.96 | 0.247 | 2.225 | 19.64 |

| Five-run statistic | IPC | Local |
|---|---:|---:|
| Mean physical Hz | 113.84 | 115.44 |
| Median physical Hz | 119.65 | 117.81 |
| Mean >12 ms intervals | 6.275% | 4.094% |
| Median >12 ms intervals | 0.146% | 1.719% |
| Mean app Hz | 10.35 | 10.18 |
| Median run pose-query p99 | 0.185 ms | 0.016 ms |
| Worst measured pose query | 31.197 ms | 0.132 ms |
| Median run query→physical p99 | 19.634 ms | 27.943 ms |

A mean-only comparison would favour local tracking, but the better median
physical cadence and presentation tail favour IPC. One poor GPU-wait run in
each group strongly affects averages. There is no consistent physical benefit
here and no basis to claim subjective smoothness from a static headset.
Conversely, the pose-query improvement is consistent across all five local
runs (p99 0.015–0.018 ms), including the GPU-stressed one.

Local snapshots: 51,002 measured-window queries, **zero RPC fallbacks**, 238
bounded snapshot-read misses (0.467%, retaining the last coherent snapshot),
and all recorded result flags 63. Median of run p99 publication age is 1.023 ms,
mapped IMU age 1.527 ms, and mapped SLAM age 40.894 ms. Worst observed ages are
33.505 / 75.928 / 94.273 ms respectively. SLAM source age includes its hardware
capture/transfer timing; latest gyro permits orientation integration from that
older SLAM pose. Fast snapshot reads cannot fix the occasional old source data.
The existing 500 ms SLAM tracking-loss rule is preserved, not a newly validated
freshness guarantee.

Shared-event readiness remains active (client GPU semaphore pushed/wait/ready
and scheduled events, presenter shared-event handoff). The stressed runs have
large **previous compositor GPU-fence** waits, not merely long pose RPCs. Do not
attribute those waits to tracking transport. In clean runs, median physical
minus predicted display timestamp remains approximately −0.001 ms; this does
not reopen the settled one-refresh pose-target hypothesis. The stressed local
run's median is −2.812 ms, without a consistent one-refresh shift across runs.

After the comparison, the importer was tightened to rebuild the gyro window in
its existing allocation on each new snapshot. This preserves distinct gyro
samples with equal timestamps, which the driver's FIFO allows, and removes
query-path allocation on a timestamp-regression branch. A new test covers
those samples, replacement of old samples and empty-window reset. The ten-run
comparison above used the initial incremental importer; its timing must not be
presented as a fresh ten-run comparison of the final importer. A separate final
capture is recorded below.

The transport remains macOS-only and opt-in. The driver and consumer use the
same future-pose math, including optional acceleration/continuity state and the
tracker-to-head/recenter relation chain. Full macOS build and all 36 CTests pass.
The broad rebuild emitted existing EUROC printf-format warnings; no new tracking
warnings were reported. Linux CI and moving-head visual validation remain
pending. General IPC space-overseer calls still reach the service. See the
[design and lifecycle details](macos-client-compositor-design.md#shared-ps-vr2-tracking-experiment--2026-10-03).

### Final importer verification

Separate final capture `final-local`, UE PID 27689, service PID 27837:
physical 117.35 Hz, 2.152% intervals over 12 ms,
app 11.22 Hz, compositor pose-query p99 0.020 ms
(max 0.119 ms), and query→physical p99
27.96 ms. All fully buffered files flushed.
The service again samples externally backgrounded, all 12 threads at priority 4,
while UE's two compositor threads are priority 97.

There are 10,488 measured local queries, zero RPC fallbacks, 48
bounded read misses reusing the last coherent snapshot, and all flags 63.
Mapped IMU-age p99/max is 1.289/12.605 ms;
SLAM-age p99/max 40.914/49.244 ms;
publication-age p99/max 1.017/9.110 ms.
This verifies the final importer's normal hardware path, not moving-head
prediction quality or a new five-pair presentation comparison. UE has exited.
The final full macOS build and all 36 CTests pass; no Linux CI was run.

## Prepared moving-head freshness capture — 2026-10-03

The user will run the moving-head test later. The preparation uses shared
tracking version **2**, requiring matching rebuilt service/client binaries.
It adds actual IMU USB callback receipt, reconstructed sample time, SLAM receipt,
raw device timestamps, clock offset, and the returned quaternion/position/angular
velocity to the shared tracking trace. These are diagnostics; prediction and
clock mapping behavior are unchanged.

The preflight caught unconditional header/periodic `fflush` calls in renderer,
frame-pipeline, client-GPU, reprojection-source, Metal-release, app-pacing and
IPC traces. They now respect the buffering option. Close still flushes normally.
`u_timing_trace_open` checks buffer setup, requests at least 16 MiB, and honors
larger requests; driver IMU requests 64 MiB. Finite buffers can fill during long
runs, so the runner checks required file sizes throughout and rejects writes
before the end of measurement rather than assuming the option suffices.

The first preflight failed this size check. The second passed the buffered
window but debugger attachment timed out after measurement. The final runner
uses normal UE double-SIGINT shutdown and a temporary one-second service idle
exit to collect buffers, avoiding a debugger dependency. Launchd bootstrap has
bounded retries for its teardown race. Each attempt restored the original
registration; the original plist was not edited.

Static final preflight: `/tmp/monado-moving-tracking-20261003/preflight-3`,
UE **38505**, service **38647**, branch `codex/macos-shared-tracking`, HEAD
`2276cfba9` plus the captured uncommitted patch/new-source archive. The measured
window was **10.222 seconds**. All eight required files remained zero bytes at
warmup completion and every measurement check. Normal shutdown produced 57,126
IMU rows, 1,680 SLAM rows and nonempty compositor traces. All **1,204** measured
physical presentations matched the exact target pose in `pose-presented.csv`.
There were 2,630 local queries, no RPC fallback, 13 bounded snapshot read misses,
and flags 63 throughout. Pose-query p99 was 0.011 ms; actual callback age at query
p99 1.013 ms / max 9.042 ms. USB callback interval p99 was 1.291 ms / max 1.752 ms.
Physical presentation was 117.82 Hz with 1.746% intervals above 12 ms. Service
policy was externally backgrounded throughout sampled measurement, but Game
Mode was not independently confirmed. Motion above 0.1 rad/s was absent. This
validates instrumentation and joins, not moving-head smoothness or source
freshness in a confirmed Game Mode transition.

A full-length static verification also passed with the final automatic analysis:
`/tmp/monado-moving-tracking-20261003/preflight-60`, UE **39297**, service
**39439**, same HEAD plus saved working changes. Its **60.096-second** window had
14 checks with every required file still at zero bytes; normal closure produced
156,232 IMU and 4,653 SLAM rows overall. All **7,203** measured physical frames
joined to exact pose targets. There were 15,915 local queries, zero fallback,
94 bounded read misses, flags 63, query p99 0.009 ms / max 0.022 ms. Physical
presentation was **119.85 Hz**, with one >12 ms interval (0.0139%). Actual IMU
callback-age p99 was 1.022 ms / max 9.192 ms, and callback gaps p99 1.280 ms /
max 9.322 ms. Session-baseline delivery-excess p99 was 1.912 ms / max 9.730 ms;
client source-age p99 2.739 ms against that baseline versus mapped p99 1.333 ms.
No motion above the analysis threshold occurred. Sampled service policy remained
externally backgrounded; Game Mode was still not independently confirmed.
The original launchd plist/path was restored, and UE/service were both stopped.
This establishes full-duration buffering and automatic analysis, without
changing the outstanding moving-head/Game Mode gate.

IMU reconstructed host sample times can overlap/regress between batches.
Delivery analysis therefore uses the newest device sample per **actual USB
callback**, not reconstructed timestamps or older bundled samples as distinct
late deliveries. It compares a trailing five-second minimum host-minus-device
offset with a session-wide minimum. The former can absorb prolonged stalls;
the latter can include clock drift. Both report **excess** delay relative to
fast delivery, not absolute USB latency. Neither changes the runtime clock.
The analyser also reports mapped/source-baseline ages, actual receipt age,
publication age, motion speed, service external-background policy and physical
pose-target joins. The static preflight's session-baseline delivery excess p99
was 1.729 ms; client source age against that baseline p99 was 2.368 ms, versus
mapped age 1.380 ms. Those quantities are deliberately separate.

Run from the repository with the headset connected and UE/service stopped:

```sh
python3 scripts/macos/capture-tracking-freshness.py /tmp/monado-moving-head-01 --announce
```

This preserves the user's MonadoMacTest `-game -vr -NOSCREENMESSAGES`
`-LogCmds="LogHMD VeryVerbose" -stdout -log` arguments, with CA, client compositor,
shared tracking and shared-event wait thread enabled. After startup/warmup it
captures 60 seconds. Rotate gently throughout; with Game Mode active, open
Cmd+Esc for the middle 20 seconds and dismiss it for the final 20 seconds.
Spoken cues mark thirds, but are intended phase labels rather than evidence of
Game Mode. The runner does not change fullscreen/focus; confirm the desired
Game Mode state yourself. It stops its UE, waits for service teardown, verifies
nonempty traces, restores launchd, and writes `freshness-summary.json` plus
`pose-presented.csv`. Use a new output directory per run. Re-analyse with:

```sh
python3 scripts/macos/analyze-tracking-freshness.py /tmp/monado-moving-head-01
```

Build and all 36 macOS CTests pass; five Python analysis-method tests cover
sustained delay hidden by the rolling floor, receipt-mapping concealment,
timestamp ordering/epochs, USB batch reduction and trailing lookups. Linux CI
and the user's moving-head capture remain pending. General shared input/space
state and timely sensor production are subsequent architecture gates, not
implemented by these diagnostics.

## First buffered moving-head shared-tracking run — 2026-10-03

The user completed the prepared capture in `/tmp/monado-moving-head-01`:
UE **42448**, service **42587**, HEAD `2276cfba9` plus saved working changes,
CA/client compositor/shared tracking/shared-event wait thread enabled. The
**60.275-second** window has 120,071 IMU samples, 59,483 USB callback batches,
and 3,613 SLAM updates. Motion exceeds 0.1 rad/s for **95.37%** of callback
samples; median/p95 angular speed is **30.78/72.50 degrees/s**. All 14 buffering
checks show zero bytes for every required trace, followed by nonempty normal
shutdown traces and original service restoration.

The sampled service policy changes from externally backgrounded to unbackgrounded
at **24.69 seconds**, then back at **45.21 seconds**. This matches the requested
Cmd+Esc protocol. The user confirms **Game Mode active in the first and last
thirds**, with the middle menu interval, and reports **juddery, but better and
with a slightly different quality to the judders**. The private Game Mode bit
remains unreadable; user confirmation supplies that context. Probe spacing is about two
seconds, so these are first observed policy changes, not exact menu timestamps.
The client compositor retains time-constraint policy 2 throughout all 7,169
samples; all 119 detailed priority samples are **97**. Shared-event waits are
active on every one of the 7,079 measured presentation submissions.

There are 15,733 tracking queries, p99 **0.011 ms**, max **0.064 ms**, 75 bounded
read misses, and one historical-target RPC fallback at 0.392 seconds. The latter
has flags zero in the local trace by design; it does not establish actual
tracking loss because the RPC result is not recorded there. All other local
query flags are 63. Physical output is **117.44 Hz**, with **1.229%** intervals
above 12 ms. Exact pose-target joins succeed for **7,078/7,079** physical frames.
Median/p95 physical-minus-pose-target delay is -0.001/0.001 ms; p99 is 8.334 ms,
max **133.467 ms**. Overall rates hide substantial individual hitches.

Actual callback-age at query p99/max is **1.714/61.318 ms**. USB callback gaps
p99/max are **1.333/62.884 ms**. Excess delivery delay against the session minimum
has p99/max **2.866/74.111 ms**. The mapped clock absorbs part of delayed
production: offset above the session fast-delivery floor reaches **31.786 ms**.
Client IMU age against that floor reaches **88.670 ms**, versus mapped age
**81.515 ms**. The rolling floor changes by 1.361 ms; neither floor is an
absolute latency measurement. The service has occasional real receipt gaps
with tracing buffered; these cannot be dismissed as periodic CSV flushes.
They do not identify whether USB/device delivery, service scheduling, or another
host-wide pause causes the gap.

The largest sensor gaps finish at 0.353 s (55.931 ms), 11.792 s (57.406 ms),
24.478 s (62.193 ms) and 44.286 s (62.884 ms). The latter two occur near inferred
menu transitions; the 11.792 s gap occurs during sampled external backgrounding.
There is no clear sustained freshness collapse throughout either backgrounded
steady interval. Conservative interior windows, avoiding observed transitions:

| Interval | Observed service policy | Physical Hz | >12 ms intervals | Pose-query callback-age p99 / max |
| --- | --- | --- | --- | --- |
| 2–18 s | externally backgrounded | 118.38 | 0.317% | 1.057 / 57.333 ms |
| 27–40 s | not externally backgrounded | 117.96 | 1.567% | 1.020 / 9.047 ms |
| 48–58 s | externally backgrounded | 119.88 | 0% | 1.039 / 9.026 ms |

These intervals differ in motion and app workload; this is a mechanism diagnosis,
not an A/B performance ranking or evidence that backgrounding improves cadence.
UE submissions average roughly 12.22/11.90/10.61 Hz over the nominal thirds.

The largest physical gap, **141.809 ms** ending at 16.038 seconds, coincides with
**141.796 ms** in `nextDrawable`. CA callbacks over that interval continue with
maximum interval **8.371 ms**. Previously submitted frame **2796** completes its
Metal command buffer in **1.259 ms**, yet is physically presented about
**148.322 ms after completion**, **133.467 ms after its pose target**. The next
submitted frame shows the same late physical release. Another **100.099 ms**
physical gap ends at 43.615 seconds, with **100.102 ms** drawable acquisition;
CA callbacks remain at most **8.367 ms** apart there. Frames 6100/6101 complete
Metal work promptly, then display late. This points to presentation/layer/display
backpressure beyond command-buffer completion. Drawable blocking may be the
consequence of delayed release rather than its root cause. These hitches are
not explained by a CADisplayLink callback stall or slow shared pose queries.
The traces do not isolate WindowServer, remote layer hosting, or display handling
as the root cause, and command-buffer completion alone is not a photon timestamp.

Retrospective corrected-SLAM orientation reference: shortest-arc interpolation
in device time, brackets at most 35 ms, no extrapolation, physical host time
converted with the matched query's offset. The driver's tracker-to-head rotation
is identity and recenter is off in this capture; no orientation fit was applied.
All 7,078 exact target joins have usable reference brackets. Disagreement at the
requested pose target is median/p95/p99 **0.135/0.209/0.303 degrees**, max
**1.461 degrees**. At physical presentation it is **0.137/0.226/0.437 degrees**,
max **4.675 degrees**. This internal reference supports working moving-head
prediction most of the time, with worse pose agreement at delayed presentations.
It is not independent tracking ground truth, full scene/render correctness, or
proof of a subjective smoothness improvement. Clock mapping can also bias this
reference during delayed receipt.

Evidence: `freshness-summary.json`, `pose-presented.csv`, `motion-detail.json`,
`motion-detail.log`, `user-feedback.json`, and saved `motion-analysis-method.py`
in the capture directory.
The extra analysis runs from this checkout and uses its freshness analyser.
No new hardware run or runtime behavior change was made during analysis.

Decision: keep shared tracking opt-in and retain client-hosted CA pacing.
The moving-head transport/prediction gate has encouraging evidence, but the
smoothness gate is not passed. Next isolate the late **physical presentation
of already completed Metal frames** and its drawable backpressure, while
retaining raw receipt diagnostics. General shared device/input state plus local
space evaluation remains the longer-term query architecture. Moving sensor
acquisition out of the service is still an option if repeated traces establish
service-specific scheduling delay; this run alone does not justify that migration.

## Completed-frame presentation investigation — 2026-10-03

The first moving-head capture now has a durable lifecycle analysis through
`scripts/macos/analyze-presentation-stalls.py`. It joins actual physical frame
IDs to submission, GPU end, completion callback and available scheduled-callback
traces. GPU end is converted from host seconds with each physical row's clock
offset, not with callback arrival time; invalid clock/lifecycle conversions are
excluded. Old captures explicitly report missing new traces.

For `/tmp/monado-moving-head-01`, all **7,079** physical frames join to completion
and submission; no GPU clock validation fails. GPU execution duration p99/max
is **3.853/5.534 ms**. GPU-end-to-physical p50/p99/max is
**14.781/22.232/148.585 ms**. There are **94** frames beyond 20 ms after GPU end,
and **14** physical cadence gaps of at least 20 ms. The largest pause releases
two old frames (2796/2797), then frame 2813 returns to ordinary delay immediately
(**14.215 ms** after GPU end). The later 100 ms pause similarly returns to normal
after a few frames. This differs from the persistent hide/show latency ratchet
already diagnosed in the remote-layer probe; changing to `atTime:` had not cured
that ratchet either. Do not repeat that settled hypothesis as a presumed fix.

New passive instrumentation:

- `present_scheduled.csv` records an additional Metal scheduled callback,
  registration time, frame/image/timeline IDs, status, configured minimum
  duration, and the actual `presentsWithTransaction` value. The observer does
  **not** replace Metal's existing timed-present helper and does not timestamp
  its private callback. It is drained alongside command completion before
  trace destruction. Apple's [minimum-duration documentation](https://developer.apple.com/documentation/metal/mtlcommandbuffer/present(_:afterminimumduration:))
  explains that the convenience helper requests presentation from a scheduled
  handler; [transaction presentation](https://developer.apple.com/documentation/quartzcore/cametallayer/presentswithtransaction)
  is independent when that property is false.
- `appkit_pump.csv` records service event-pump begin/end and prior begin/end,
  NSApp presence and main-thread identity, without a timer or layer transaction.
  Its existing main loop intentionally sleeps **50 ms** between polls; the
  diagnostic must not label every such interval a scheduling defect.
- The analyser writes `presentation-lifecycle.csv` and `presentation-stalls.json`,
  correlating physical gaps with CA callbacks, drawable waits, sensor callback
  gaps, sampled service policy and available AppKit progress. Coincidence
  narrows stages; it does not prove WindowServer, the host or the driver caused
  a gap.

A valid five-second static preflight (`preflight-3`, UE **47168**, service
**47327**) saved all 15 required traces and joined all **629** physical frames,
with no missing lifecycle trace or GPU clock failure. Scheduled callback delay
p99/max was **2.104/2.707 ms**; GPU-end-to-display max **15.706 ms**; no >=20 ms
physical hitch occurred. The actual layer property was
`presentsWithTransaction=0`, with 8000 us minimum duration. Measured service
AppKit gaps reached **249.761 ms**, while physical presentation stayed steady:
ordinary service event pumping is not required for each hosted refresh.

### Reliable capture flush and source-health gate

Several later UE interrupt exits bypassed most XR/stdio cleanup, leaving client
buffers empty even after giving the first interrupt more time. They are rejected,
not used as timing evidence. The fully buffered option alone does not guarantee
that a process exit saves its buffers. The runner now uses **acknowledged flush
requests outside measurement**, without LLDB or signal handlers: each runtime
checks an owned regular `monado_trace_<pid>.flush-request` marker in its trace
directory, calls `fflush(NULL)` on an ordinary worker/main-loop thread, and renames
it to `.flush-complete` only after success. Requests contain no commands or paths.
The macOS helper is inactive unless both timing tracing and full buffering are
on. It checks cached file metadata at most **10 Hz**; there are no new background
threads, timers or default presentation-policy changes. These reads are diagnostic
overhead, and the captures do not claim zero filesystem activity.

The short flush preflight (UE **49805**, service **49952**) verified both
acknowledgements and all **628** physical lifecycle joins, with zero >=20 ms
hitches, maximum scheduled-callback delay 2.072 ms, and maximum GPU-end-to-display
15.715 ms. Traces were saved before UE teardown.

A full-length buffered presentation verification at
`/tmp/monado-presentation-stalls-20261003/flush-verification-60`
(UE **50145**, service **50273**) also saved both acknowledgements. All **7,208**
physical frames joined with no missing lifecycle trace or GPU clock failure.
Scheduled callback p99/max was **3.781/6.607 ms**, GPU-end-to-display p99/max
**15.662/23.558 ms**, with one >=20 ms physical hitch and four frames more than
20 ms after GPU end. **This is presentation-only evidence:** IMU stopped after
its initial batch, SLAM was empty, and shared prediction fell back to service
queries. Service idle exit was also slow; the original registration was restored
through bootout/bootstrap. The tracking analyser correctly rejects the empty
sensor stream. It is not a moving-head/shared-prediction validation. No long
moving-head stall was reproduced here. Headset sleep/disconnection status is
awaiting user clarification.

To prevent wasting a motion run with inactive sources, the final runner now
flushes warmup buffers first, verifies IMU and SLAM receipts within 250 ms of the
request, and refuses to start measurement if either is missing/stale. It then
allows a two-second settling interval. Measurement buffering checks require
**unchanged file sizes relative to the saved warmup baseline**, followed by a
second acknowledged flush. Warmup bytes are expected; growth inside measurement
is rejected. UE and the capture registration are stopped/restored afterwards;
slow service idle exit is recorded and bounded, rather than invalidating already
acknowledged presentation data. The 250 ms check is a capture prerequisite,
not a new runtime prediction/validity rule.

Build and all **36 macOS CTests** pass, together with five presentation-clock,
five tracking-method and three source-health/buffering Python checks. Linux CI
is pending. The next user moving-head capture retains the presentation defaults:

```sh
python3 scripts/macos/capture-tracking-freshness.py /tmp/monado-moving-head-02 --announce
```

Run with the headset awake and tracking, and UE/service stopped. Rotate gently,
with Game Mode active outside the middle Cmd+Esc interval, as before. It will
produce both freshness and presentation-stall reports automatically. The useful
next distinction is a delayed observable scheduled callback versus a drawable
that remains undisplayed after callbacks and GPU work have finished. Neither
observation alone timestamps WindowServer acceptance or determines its cause.

The final health-gate check (`health-verification`, UE 51682/service 51829)
correctly refused measurement on stale IMU. The original service registration
was restored after a further launchd teardown retry; the runner's bootstrap
retry window was extended to approximately 13 seconds. No UE/service process
was left running. Acknowledged flushing with active sensors passes in the earlier short
preflight; full-duration presentation flushing passes, but final end-to-end
warmup-plus-post-window flushing and moving-head health-gated validation require
live sensors. The new health and
baseline-size gate also passes its three Python tests.

### Missing tracking during capture 02 — 2026-10-03

The user's `/tmp/monado-moving-head-02` (UE 68723/service 68853, HEAD
`2276cfba9bbca3109e8e6ac5e4d1c12b34b0e2c9` plus the uncommitted shared-tracking
and presentation diagnostics) launched a static scene and stopped after warmup.
Both sensor CSVs have headers but zero samples, despite acknowledged warmup
flushes. The stop was the runner's source-health rejection, rather than evidence
of a UE crash. This repeats the sensor loss first seen in
`flush-verification-60`; it is not a valid presentation or prediction capture.

Two service-only checks reproduce missing IMU and SLAM without UE or a shared
tracking consumer. `/tmp/monado-usb-diagnostic-20261003` used the original
camera/gaze configuration; `/tmp/monado-usb-conservative-20261003` disabled both.
Control replies, including firmware information, succeeded. Libusb debug logs
show status endpoint 0x88 and SLAM endpoint 0x83 submitted, with no subsequent
stream completions during the check. A process sample places the USB thread in
the libusb event wait, not blocked in snapshot publication or pose prediction.
All diagnostic LaunchAgents were removed and the original registration restored.

After the user power-cycled/reconnected the headset/adapter, the service-only
check `/tmp/monado-usb-reconnected-20261003` received 13,992 IMU samples and 374
SLAM records with the original camera/gaze configuration. This establishes
recovery after physical reset, but does not establish what originally stopped
the streams or whether rapid repeated service teardown contributed. No USB
reset or stream-restart workaround has been added on this evidence.

The runner now records both streams in `sensor-health.json` even on rejection,
including row counts and last receipt age, and explains the intentional UE stop
without a Python traceback. Four source-health/buffering tests pass.

The recovered UE preflight `/tmp/monado-reconnected-ue-preflight-20261003`
(UE 70242/service 70391, five-second static measurement) passes the complete
warmup-health, unchanged-buffer-baseline and post-window-flush sequence in the
same processes. Both acknowledgements arrived at both flush points, all 15
required traces contain records, and both analysis scripts complete. The
original service registration was restored. All 603 physical frames join to
their pose targets and presentation lifecycle; cadence is 119.88 Hz with no
interval above 12 ms, and none exceeds 20 ms from GPU end to physical output.
The short run was requested as a static preflight; measured gyro motion is
present, but it has no controlled Game Mode/menu protocol and cannot replace
the full moving-head capture. All 14 Python diagnostic tests pass. This closes the active-sensor
capture-plumbing gap above; moving-head presentation validation remains pending.
Use a new directory for the next moving run:

```sh
python3 scripts/macos/capture-tracking-freshness.py /tmp/monado-moving-head-03 --announce
```

### Recovered second moving-head capture — 2026-10-03

The user reused `/tmp/monado-moving-head-02` after reset. Its new contents
(UE 70746/service 70890) replace the earlier failed capture with UE 68723/service
68853 described above. HEAD remains `2276cfba9bbca3109e8e6ac5e4d1c12b34b0e2c9`
with the recorded uncommitted patch. The 60.134-second measurement passes both
acknowledged flushes, unchanged measurement buffer sizes and all 15 required
nonempty traces. The service exits normally and its registration is restored.
Both analysers complete: all 7,071 physical frames join to pose targets and
presentation lifecycle, with no GPU-clock validation failures. User feedback
(`user-feedback.json`) reports high apparent FPS despite low application frame
rate, but very jumpy movement whenever moving the head, perhaps better than
before. Cadence does not establish perceptual smoothness.

Motion is substantial: 91.4% of IMU callbacks exceed 0.1 rad/s, with median/p95
gyro speed 29.0/85.3 degrees/s. Service external-background policy changes at
24.680 and 45.224 seconds, consistent with the requested menu interval; Game
Mode has not been independently confirmed for this particular capture. Sampled
client compositor priorities remain 97. All 15,695 shared pose queries avoid
RPC, with p99/max duration 0.012/0.071 ms and 84 bounded read misses.

| Evidence | Result |
| --- | --- |
| Physical cadence | 117.59 Hz; 1.372% of intervals >12 ms |
| UE new-frame cadence, first/middle/last thirds | 12.21 / 11.58 / 10.01 Hz |
| Physical gaps >=20 ms | 11; largest 116.78 / 100.10 / 75.07 ms |
| Completed frames displayed >20 ms after GPU end | 99; maximum 107.07 ms |
| GPU duration | p99/max 4.13/7.04 ms |
| Internal SLAM orientation disagreement at pose target | p95/max 0.224/0.679 degrees |
| Internal disagreement at physical presentation | p95/max 0.243/3.345 degrees |
| IMU/SLAM receipt gaps | maximum 56.52/60.40 ms |

At the 100.10 ms gap ending 4.163 seconds, frame 1686's scheduled callback
arrives 0.333 ms after commit and GPU work takes 0.547 ms, yet GPU end precedes
physical output by 107.07 ms. CA callbacks continue with a maximum 8.355 ms
interval, drawable acquisition waits 100.10 ms, and overlapping sensor receipt
gaps reach only 2.032 ms. This isolates a large hitch after completed rendering,
without a matching acquisition stall. The 116.78 ms gap ending 24.559 seconds
also includes a 56.52 ms sensor gap; the 75.07 ms gap at 43.669 seconds does not.
Hitches occur in both observed service-policy states. Passive scheduled
callbacks and AppKit-pump overlaps do not identify WindowServer acceptance or
prove causality.

Regular movement jumpiness warrants a separate source-transition check.
`source-transition-analysis.py` and `source-transition-summary.json` save that
method and result in the capture directory. All 7,160 valid renderer rows use
timewarp and ordinary projection layers. This build has
`XRT_FEATURE_OPENXR_LAYER_DEPTH=OFF`; `render_calc_time_warp_matrix` uses source
and destination orientations, ignoring positions. Source-frame age at the
predicted target is median/p95 50.03/108.42 ms, with occasional unchanged content
reaching 1.297 seconds. Source-to-target rotation reaches p95 8.18 degrees, but
rotation-only warp does not compensate view-centre translation: median/p95
source-to-target displacement is 3.53/24.46 mm, and view-centre steps on new
source frames are 6.65/25.31 mm. Even nominal head rotation moves the eyes around
the neck pivot. Uncorrected translation of nearby geometry and old scene content
are plausible contributors at 10–12 application FPS. Pose steps alone do not
establish rendered-image jumps or rule out a UE pose/image mismatch. Internal
SLAM comparisons remain internal references, not ground truth.

The next useful distinction is occasional late presentation versus regular
source-frame refresh/parallax. Do not replace the predictor on this evidence.
A controlled high-application-FPS or translation-isolating scene comparison,
with the same presentation traces, would separate the second mechanism before
changing depth reprojection or client pose handling. Completed-frame stalls
remain an independently reproduced presentation problem.

### Prepared UE workload A/B

The user proposes the previously effective UE low-cost preset, especially the
25% HMD render target. The capture runner now accepts `--ue-preset low-cost`,
passing this exact string as a single `-ExecCmds` argument:

```text
xr.SecondaryScreenPercentage.HMDRenderTarget 25,sg.ViewDistanceQuality 0,sg.AntiAliasingQuality 0,sg.ShadowQuality 0,sg.GlobalIlluminationQuality 0,sg.ReflectionQuality 0,sg.PostProcessQuality 0,sg.EffectsQuality 0,sg.FoliageQuality 0,sg.ShadingQuality 0
```

`--ue-preset standard` is the default and adds no ExecCmds, matching the latest
baseline launch. Both choices record the preset, exact commands and full argv
in `process.json`; the service, shared tracking, display link, scene and capture
protocol stay the same. This compares application workloads, not an isolated
resolution or graphics-feature effect. Reduced pixel density/anti-aliasing may
also change perception; inspect measured application cadence and pose/presentation
timing rather than attributing any subjective difference entirely to FPS.
The preset is prepared, not yet verified in a headset run. Commands for a fresh
pair (or use the valid recovered capture as a preliminary A baseline):

```sh
python3 scripts/macos/capture-tracking-freshness.py /tmp/monado-ue-workload-a --ue-preset standard --announce
python3 scripts/macos/capture-tracking-freshness.py /tmp/monado-ue-workload-b --ue-preset low-cost --announce
```

Use similar head movements and the same middle-third Cmd+Esc protocol. If the
regular movement jumps diminish while occasional completed-frame stalls remain,
that supports a source-refresh/load contribution without proving translation
is its sole cause. If needed, separate render-target reduction from the quality
settings in a later comparison; do not change both compositor and UE workload
in this pair.

### UE workload A/B results and effective timewarp — 2026-10-03

Both workload captures are valid and fully buffered, with acknowledged flushes,
normal service exits and restoration. A is `/tmp/monado-ue-workload-a`, UE
78439/service 78599, 60.239 seconds; B is `/tmp/monado-ue-workload-b`, UE
78910/service 79035, 61.997 seconds. HEAD is still
`2276cfba9bbca3109e8e6ac5e4d1c12b34b0e2c9` plus each saved worktree patch.
B records the exact low-cost ExecCmds in argv and its UE log. The user reports
that B's announcements all arrived at the end, so Game Mode remained active
throughout. A's observed service policy clears external background at 29.724
seconds and resumes it at 46.298 seconds; B's 19 sampled service policies all
remain externally backgrounded. Do not compare their middle thirds as a
controlled Game Mode contrast. The user reports B's movement jumps were smaller,
but still present. Feedback is saved in each `user-feedback.json`.

| Matched time windows, both observed backgrounded | A standard | B low-cost |
| --- | --- | --- |
| UE FPS, 2–18 seconds | 12.28 | 62.66 |
| Physical Hz, 2–18 seconds | 118.94 | 116.13 |
| Physical intervals >12 ms, 2–18 seconds | 0.473% | 2.746% |
| UE FPS, 48–58 seconds | 9.84 | 56.15 |
| Physical Hz, 48–58 seconds | 118.58 | 117.38 |
| Physical intervals >12 ms, 48–58 seconds | 1.013% | 2.131% |

Across the full runs, A/B physical cadence is 118.78/112.91 Hz with 9/45 gaps
>=20 ms and worst gaps 83.42/200.20 ms. B has lower presentation GPU duration
(p99 1.189 versus 4.534 ms), but longer delivery stalls: actual IMU callback
gap maxima 114.38/240.99 ms and SLAM gaps 87.27/334.56 ms. B's three largest
physical gaps around 38.2–38.8 seconds overlap long sensor gaps and renderer
`draw_dispatch` intervals reaching 191.82 ms. The resumed displayed frames
finish GPU work only about 7–8 ms before appearing, while CA intervals remain
near 8.37 ms. This differs from a frame held long after GPU completion; total
stall counts cannot all be labelled WindowServer delays. B still has 11 frames
displayed >20 ms after GPU end, versus A's 19. The higher application frame rate
did not simply improve scheduling. Motion is also faster in B (gyro p95
89.0 versus 74.0 degrees/s), so this is one useful pair, not a clean ranking of
every metric or proof of a single cause.

The speech failure is consistent with helper scheduling delay: B's policy
sample spacing grows from about 2.1 seconds to as much as 11.87 seconds, versus
A's maximum about 2.29 seconds. The runner launches `say` asynchronously and
does not record speech start or completion. The current evidence cannot separate
delayed process startup, synthesis or audio delivery. Phase printing is not
proof that a cue was heard. Future controlled pairs should use the same Game
Mode protocol (keeping it on throughout both is suitable), with independently
timed menu transitions if needed; voice cues alone are unreliable here.

#### Query poses are not always the poses used for rendering

The source-transition traces expose a concrete existing renderer issue.
`dispatch_compute` first calls `calc_pose_data` for fresh scanout target poses,
then, outside the one-projection fast path, replaces target begin/end poses
with the submitted projection poses if their timestamp lies within three
compositor frame periods of the predicted display time. This override originated
in bring-up commit `816d372cf27ca42c6c0e8658c88140311c810ed4` and is unchanged by
the current shared-tracking experiment. Both A and B use this general compute
path. The chosen source and target orientations are consequently identical on
1,960/7,155 matched A frames and 6,769/6,992 B frames (27.4%/96.8%). `do_timewarp=1`
is an enabled flag; it does not prove that a fresh target pose was applied.

This qualifies earlier conclusions based only on queried poses: the returned
shared pose can be accurate yet subsequently discarded. Applying the same
internal SLAM-reference method to the **actual renderer begin orientation**
instead gives target disagreement p95 1.675/0.999 degrees for A/B, versus
0.216/0.219 degrees for the raw queries. Physical disagreement p95 is
1.682/1.008 degrees. `effective-reprojection-analysis.py` and
`effective-reprojection-summary.json` save the frame-ID joins and per-query
clock-offset method; this remains an internal reference, not scene correctness
or independent tracking ground truth. Near-zero B source-to-target translation
in `source-transition-summary.json` also reflects the override, rather than
proving that the eyes did not move. The build still lacks depth reprojection.

The override creates an age-dependent change between using submitted poses
and fresh scanout poses, capable of stepping the effective view during source
refresh. It is now the concrete next correction to investigate on macOS,
before assuming the remaining regular judder is solely positional parallax or
replacing the predictor. Retain Linux behaviour while correcting the macOS
target-pose selection, and validate actual renderer poses as well as raw query
poses in the next moving-head capture. No renderer fix is claimed in these
captures; late-presentation and acquisition/scheduling stalls remain separate.

### Fresh scanout target fix — 2026-10-03

The macOS compute renderer now retains `calc_pose_data`'s fresh begin/end
scanout poses, eye poses and distortion/device-view FOV selection through the
render pipeline. It no longer substitutes submitted projection poses for recent
layers. Submitted poses remain source-image metadata used by the normal layer
rendering/timewarp path. No diagnostic toggle was added. The existing override
and helper remain compiled on non-macOS platforms, preserving their behaviour;
Linux CI has not been run locally. Graphics and one-projection fast paths are
unchanged.

`analyze-tracking-freshness.py` now reports `renderer_pose`, comparing the actual
left-eye begin orientation against the exact matched query, with target-time
checks, sign/normalization-safe quaternion distances and invalid-pose rejection.
This PS VR2 check relies on identity eye orientation. Re-analysing the old
low-cost B capture detects 6,769/6,992 mismatches above 0.001 degrees (p95
0.944 degrees); identical source/target orientations by themselves are not a
reliable test when the headset is stationary.

The fixed five-second, low-cost UE preflight
`/tmp/monado-fresh-target-low-cost-preflight` (UE 81854/service 82007, HEAD
`2276cfba9bbca3109e8e6ac5e4d1c12b34b0e2c9` plus saved uncommitted patch)
has **609/609 renderer/query matches**, zero target-time mismatches, zero
differences above 0.001 degrees and maximum difference 0.000002415 degrees.
All physical frames also join to presentation lifecycle. Actual source-to-target
rotation is nonzero (p95 0.051 degrees) despite very low headset motion. Both
acknowledged flushes, source health and unchanged measurement buffers pass;
normal shutdown restores the original service. Physical cadence is 118.90 Hz,
with two >=20 ms gaps and maximum GPU-end-to-output delay 32.09 ms, so the fix
does not remove every presentation stall. This is a pose-selection preflight,
not proof of moving-head smoothness or Game Mode activation.

The full macOS build passes without compiler warnings; all 36 CTests pass with
desktop access, and all 15 Python diagnostic tests pass. The sandbox-only suite
initially failed three socket/remote-layer access checks, which pass unchanged
outside the sandbox. Moving-head confirmation is pending.

The runner adds `--game-mode-protocol continuous`, recording the protocol and
avoiding any instruction to open the menu. For the next pair, keep Game Mode
active throughout and rotate similarly. Omit speech cues; the runner still
prints progress and stops UE after the measurement:

```sh
python3 scripts/macos/capture-tracking-freshness.py /tmp/monado-fresh-target-a --ue-preset standard --game-mode-protocol continuous
python3 scripts/macos/capture-tracking-freshness.py /tmp/monado-fresh-target-b --ue-preset low-cost --game-mode-protocol continuous
```

Inspect actual renderer/query agreement alongside source changes, tracking
freshness and presentation stalls. Separate the visual effect of this correction
from the independently measured acquisition/scheduling and completed-frame stalls.

### Moving-head confirmation and local Metal image reuse — 2026-10-03

The user completed `/tmp/monado-ue-workload-a2` (UE 82473/service 82617,
standard) and `b2` (UE 83028/service 83161, low-cost) with the fresh-target fix.
Feedback: "So much better", regular judder gone, occasional stalls remain,
and occasional partly black frames/black noise have been apparent for a while.
Feedback is saved in each capture. All 6,985 A and 6,760 matched B renderer/query
orientations agree within 0.001 degrees. Physical cadence is 116.40/112.62 Hz;
10/58 gaps reach >=20 ms, with maximum gaps 116.78/633.96 ms. The saved protocol
field is `menu` for both; do not assert a continuous Game Mode protocol or
specific menu timing without further confirmation. This is subjective moving
validation of the pose-selection correction, not proof all stalls are fixed.

Synchronization is active in these captures. All 8,172/8,279 Metal presentation
submissions, including warmup, record `shared_event_wait=1` and
`wait_mode=metal_shared_event`. App-release traces have 762/4,947 matching events
at each of semaphore push, wait start, ready and scheduling. Empty
`metal_release_barrier.csv` is expected when the nonblocking app-release wrapper
bypasses the old CPU barrier; it does not establish missing synchronization.
These are two distinct directions: app writes becoming ready before compositor
sampling, and finished compositor output becoming ready before Metal presentation.

Code inspection exposes a third, missing guard in the client-hosted Metal path:
preventing the app from overwriting a source image until the compositor's last
GPU read finishes. Service-owned Metal swapchains call
`comp_swapchain_gpu_reuse_enable`; local Metal-owned/direct swapchains did not.
In-process sharing still uses separate Metal and Vulkan queues. Native CPU image
use counts do not by themselves prove that an asynchronous GPU read completed.
The missing timeline wait is a real synchronization gap consistent with the
reported corruption, but the timing traces do not contain pixels and cannot
identify the black-frame cause conclusively. Large rotations can also expose
unrendered image boundaries; do not classify every black edge as this race.

The direct Metal creation path now enables the existing fine-grained GPU reuse
tracker on its native swapchain, failing creation explicitly if the guard cannot
be installed. The ordinary local Metal creation path, including direct-wrapper
fallback, does the same when compositor utilities are present. Remote proxies
are not cast as native compositor swapchains; their service-side protection is
unchanged. Existing pending-consumer claims and per-submit Vulkan timeline
signals enforce the wait at `xrWaitSwapchainImage`. No queue-idle wait, new
environment toggle, or Linux behavioural change is introduced.

Full macOS build, all 36 CTests with desktop access, all 15 Python diagnostics
and diff whitespace checks pass. Ten-second low-cost preflight
`/tmp/monado-local-metal-reuse-preflight` (UE 87576/service 87845) passes source
health, both acknowledged flushes and unchanged measurement buffers, completes
1,191 actual renderer/query matches, and presents at 118.49 Hz. This low-motion
preflight is not a visual corruption test or a matched performance comparison.

A separate logging-only check `/tmp/monado-local-metal-reuse-log-check`
(UE 88780/service 88921) adds `XRT_LOG=info` and
`XRT_COMPOSITOR_LOG_SWAPCHAIN_GPU_REUSE=1` through its saved diagnostic launcher;
the extra launch environment is recorded in `process.json`. Both native
swapchains enable the tracker and all 2,111 logged nonzero-timeline reuse waits
complete, with no reuse errors. This verifies actual guarded image returns,
not merely an environment setting. Its logging can perturb timings and should
not be used for performance claims. Both checks restore the original service.
Visual confirmation of the corruption fix is pending; the fresh-target fix is
retained and stalls remain a separate investigation.

For the next visual check, use normal buffered tracing and continuous Game Mode:

```sh
python3 scripts/macos/capture-tracking-freshness.py /tmp/monado-ue-reuse-b3 --ue-preset low-cost --game-mode-protocol continuous
```

#### User confirms image-reuse correction

The user subsequently reports "Yes that fixed it", confirming that the local
Metal GPU reuse guard removes the previously reported partial black frames and
black noise. This supersedes the visual-validation-pending status above. The
earlier moving-head report separately confirms the fresh scanout target fix
removes regular judder. Retain both corrections. The source-image reuse race is
therefore supported by code inspection, actual timeline-wait verification and
the user's visual confirmation, rather than just resemblance to old sync bugs.

No `/tmp/monado-ue-reuse-b3` capture is available at this confirmation, so no new
quantitative stall result or exact run PID is attributed to the feedback. The
tested revision remains the built working tree described above; the user did
not supply a new per-run protocol assessment. Occasional stalls remain the
outstanding issue, with both acquisition/scheduling and post-GPU-completion
mechanisms already observed. Do not reopen predictor replacement or disable
the image-reuse guard to address those stalls without new evidence.

### Inherited diagnostics cleanup — 2026-10-03

The old Apple source/target pixel samples were still copied to CPU-visible
buffers every frame with logging disabled. That work is now gated by the
existing `XRT_COMPOSITOR_LOG_APPLE_SAMPLES` option, with format/usage checks
and a correct final-layout transition. The same submitted-pose override has
also been removed from non-macOS builds. Apple alpha behavior is retained,
with upstream opaque behavior restored elsewhere. These are follow-up
correctness/cleanup changes, not a measured explanation for the residual
stalls. The macOS build and 36 CTests pass; a headset comparison and Linux
validation remain pending. See the [audit](macos-inherited-compositor-audit.md).

### Black corruption recurrence after cleanup — 2026-10-03

The user reports partly black/noisy frames again after the inherited compositor
cleanup. The local/direct Metal GPU-reuse enable calls and wait forwarding are
still present, and the rebuilt `build-wine` runtime contains the reuse
implementation. The latest ordinary UE log (`/tmp/ue-client-ca-cc.log`, UE 7357)
confirms shared-event output presentation and contains 133 app-readiness
semaphore timeout warnings (>100 ms). Such warnings retry; they do not alone
prove that an image was sampled before readiness or that GPU-reuse protection
is missing. This launch has no explicit reuse diagnostic output, so actual
per-image waits have not yet been verified for this run.

A user-run comparison is prepared at `/tmp/ue-client-ca-cc-reuse-check.zsh`,
using the original standard-cost CA/client-hosted command with
`XRT_COMPOSITOR_LOG_APPLE_SAMPLES=1`,
`XRT_COMPOSITOR_LOG_SWAPCHAIN_GPU_REUSE=1` and `XRT_LOG=info`. It records
`/tmp/ue-client-ca-cc-reuse-check.log`. The user will run it now. Re-enabling
pixel samples tests whether removal of their extra transfers/barriers exposed
a dependency; that remains a hypothesis. No rollback or further runtime fix
has been made. The previous successful image-reuse confirmation remains valid
for that earlier build/run, but no longer establishes that the current
cleanup build is visually clean.

### Readback-on result and explicit output barrier — 2026-10-03

The user ran `/tmp/ue-client-ca-cc-reuse-check.zsh` (UE 9655) and reported:
“Better - no black noise, though occasional black at edges, probably related
to rapid head rotations where ATW had nothing to fill into the edge”. This
is consistent with missing source coverage during rotation, but the edge
explanation has not been independently established.

The log records two protected swapchains, 453 nonzero GPU-reuse waits and
453 completions, plus 4,662 submit assignments. There are 43 target sample
records and zero source sample records. Thus the readback work actually
restored here is on the final compositor output, not on source images.
Readiness timeout warnings still occur (13); their presence alone does not
establish an unsafe reuse. Reuse logging also changes timing, so this is not
a strictly isolated pixel-copy A/B.

The diagnostic target path uses ALL_COMMANDS memory barriers, while the
ordinary final transition used COMPUTE_SHADER → TOP_OF_PIPE with
MEMORY_READ destination access. Khronos documents TOP_OF_PIPE in the second
synchronization scope as equivalent to ALL_COMMANDS with no destination
access ([synchronization guide](https://docs.vulkan.org/guide/latest/extensions/VK_KHR_synchronization2.html#top-of-pipe-and-bottom-of-pipe-deprecation)).
That path is not an adequate explicit memory-visibility boundary for the
external Metal consumer. The readbacks had been masking that difference.

The macOS compute path now uses the existing full GPU image-barrier helper
(SHADER_WRITE → MEMORY_READ, GENERAL → requested final layout) when samples
are off. This retains the stronger output dependency without copies or
CPU-visible sample buffers. Non-macOS transitions are unchanged. This is a
concrete barrier correction and a plausible explanation for the regression,
not yet a headset-confirmed resolution.

The warning-free build and 36 CTests pass. The next user-run comparison is
`zsh /tmp/ue-client-ca-cc-visibility-check.zsh`: samples are explicitly off,
reuse logging stays on, and output is saved to
`/tmp/ue-client-ca-cc-visibility-check.log`.

### User confirms explicit output barrier — 2026-10-03

After the output-barrier correction and the requested readback-off comparison,
the user reported “Yes that fixed it”. This confirms the black-noise regression
is resolved subjectively with diagnostic pixel copies disabled. Retain both
protections: per-image GPU-reuse waits prevent application overwrites while
the compositor samples, and the full macOS output-image memory barrier
protects visibility at the Vulkan-to-Metal presentation handoff. The remaining
rapid-rotation edge borders were reported separately; missing source coverage
is a plausible explanation, not independently verified. No new quantitative
stall improvement is claimed from this confirmation.
