# macOS PS VR2 timing diagnostics

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
- `XRT_MACOS_CVDISPLAYLINK_PACING=0` remains available as an A/B diagnostic to disable display-link feedback while keeping the trace enabled.

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

`XRT_MACOS_LATE_RENDER_LEAD_US` is retained only as the legacy predicted-display-relative diagnostic. Its default remains `0` (disabled), and the desired-relative option takes precedence if both are set.

Previous traces showed roughly 2-3 ms scheduler overshoot with a 0.5 ms spin margin, so the diagnostics-only wait now sleeps until 3 ms before its target and spins for the remainder. `monado_psvr2_<PID>_late_render.csv` retains the original columns and appends `desired_offset_us`, `wait_mode`, `target_minus_desired_ns`, and `pose_begin_minus_desired_ns` so the desired-relative and legacy modes can be distinguished without breaking column-name-based analysis. This remains an A/B diagnostic rather than the final late-latching design.

## Asynchronous Metal presentation / Vulkan-to-Metal shared event

`XRT_MACOS_ASYNC_PRESENT=1` is an opt-in diagnostic that removes the synchronous Metal `waitUntilCompleted` from the compositor thread. Source IOSurfaces are marked in-flight on acquire and are not reused until the Metal blit command buffer completes; with three target images this should normally avoid blocking, while remaining correct if the GPU falls behind. The default is `0`, preserving the prior synchronous path.

When async present is enabled, `XRT_MACOS_METAL_SHARED_EVENT_WAIT=1` (default) also requests `VK_EXT_metal_objects`, creates the render-complete Vulkan timeline semaphore as exportable to Metal, exports its underlying `MTLSharedEvent`, and encodes the timeline-value wait directly into the Metal command buffer. If the extension/event export is unavailable, presentation falls back to the existing CPU Vulkan timeline wait but still avoids the Metal completion wait. Set `XRT_MACOS_METAL_SHARED_EVENT_WAIT=0` to test that intermediate mode explicitly.

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
