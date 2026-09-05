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
