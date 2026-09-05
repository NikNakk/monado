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


## Positional prediction diagnostics and filtered velocity A/B

The PSVR2 driver now records `monado_psvr2_<PID>_prediction.csv` at each SLAM update. It compares the position predicted from the previous SLAM relation's constant linear velocity with the newly reported SLAM position, including total and along-motion error.

A selectable filtered linear predictor is also available:

- `PSVR2_FILTERED_LINEAR_PREDICTION=0` (default): existing relation-history linear velocity.
- `PSVR2_FILTERED_LINEAR_PREDICTION=1`: use an EMA-filtered SLAM linear velocity for positional dead reckoning; the high-rate gyro angular path is unchanged.
- `PSVR2_LINEAR_VELOCITY_ALPHA=0.25` controls the EMA update coefficient (0..1).

The prediction trace contains prior relation velocity, newly estimated velocity, filtered velocity, prediction horizon between SLAM samples, 3-D error, and the error component along the previous direction of motion. This permits objective A/B comparison without changing the executable.

### CAMetalLayer drawable count

`XRT_MACOS_MAX_DRAWABLES` selects the CAMetalLayer drawable-pool depth. Valid values are `2` and `3`; the default is `3`. The effective value is logged at startup. This is intended to test whether drawable buffering contributes to the observed one-refresh presentation latency.
