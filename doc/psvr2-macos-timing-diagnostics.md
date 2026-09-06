<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# PS VR2 macOS timing diagnostics

This document records the tracking-side timing experiments performed while
investigating visible judder in the experimental PS VR2 macOS compositor.

The main question was whether the judder could be caused before the compositor,
for example by macOS receiving older SLAM poses than Linux or by Monado's PSVR2
prediction behaving differently on macOS.

## Test environments

Two builds of Monado were compared using the same PS VR2 headset and the same
`monado-cli pose-dump` diagnostics.

- **macOS:** native Apple Silicon macOS build from the PSVR2 macOS work.
- **Linux reference:** Ubuntu ARM64 running as a guest under **VMware Fusion on
  the same Mac**, with the PS VR2 USB device passed through to the VM. This is
  therefore a useful Linux implementation/reference comparison, but it is not a
  bare-metal Linux benchmark. USB virtualization and guest scheduling can add
  latency or jitter of their own.

The Linux VM was deliberately used only to exercise the PSVR2 USB/tracking path;
it was not expected to drive the PS VR2 display through Fusion.

## Pose prediction sweep

The first diagnostic sampled `xrt_device_get_tracked_pose()` at 200 Hz and, for
each common base query time, requested poses at prediction horizons of 0, +5,
+10, +15, and +20 ms.

The purpose was to test the boundary after the PSVR2 driver/SLAM/dead-reckoning
path but before compositor presentation timing.

### Linux reference

A 33.31 s Linux/Fusion run contained 6,656 rows.

- Median sampling interval: 5.000 ms.
- p99 sampling interval: about 5.18 ms.
- After initial tracking acquisition, all sampled poses were fully valid and all
  pose calls succeeded.
- Translational prediction scaled almost perfectly with requested horizon.
  At +20 ms the observed displacement was essentially exactly
  `|linear_velocity| * 20 ms`.
- Rotational prediction similarly matched `|angular_velocity| * horizon`, with
  correlation about 0.99998 at +20 ms.
- Only a handful of tiny non-monotonic changes between increasing prediction
  horizons were seen, mostly during acquisition or at sub-millimetre scale.

### macOS

A comparable macOS run contained 6,203 rows over about 31 s.

The macOS movements were somewhat faster, so raw predicted displacement was
larger, but after accounting for movement speed the prediction behaviour was
very similar to Linux.

- Median sampling interval was about 4.996 ms.
- macOS showed somewhat more ordinary scheduler jitter at 200 Hz than the
  Linux/Fusion run, but no corresponding tracking discontinuity.
- Translational prediction again scaled essentially exactly with velocity and
  requested horizon.
- Rotational prediction correlation with angular velocity and horizon was
  greater than 0.99997.
- Measurable backwards prediction between successive horizons was negligible;
  the largest relevant reversal was only around hundredths of a millimetre.

A further comparison of each +5 ms prediction against the subsequently observed
0 ms pose showed larger absolute corrections on macOS only because the macOS
run used faster movements. When stratified by translational speed, the median
corrections were very similar between macOS and Linux/Fusion. For example:

| Translational speed | macOS median correction | Linux/Fusion median correction |
| --- | ---: | ---: |
| 0.02-0.10 m/s | ~0.54 mm | ~0.51 mm |
| 0.10-0.20 m/s | ~1.22 mm | ~1.04 mm |
| 0.20-0.40 m/s | ~2.04 mm | ~2.06 mm |
| 0.40-0.80 m/s | ~3.34 mm | ~3.88 mm |

The prediction sweep therefore did **not** find a macOS-specific defect in
`xrt_device_get_tracked_pose()` prediction that resembles the visible backwards
judder.

## SLAM availability latency diagnostic

Prediction can still look correct even if the underlying SLAM pose is older on
one platform. A second diagnostic therefore exposed the PSVR2 driver's raw VTS
clock timing through a small public diagnostics API and sampled it at 1000 Hz.

For each sample the CLI recorded:

- latest SLAM VTS timestamp;
- that timestamp mapped into Monado's monotonic host clock;
- latest IMU VTS timestamp in the same clock domain;
- age of the latest SLAM pose at query time;
- latency when the polling CLI first observed a new SLAM timestamp.

`slam_first_seen_latency_ns` is an upper bound on true availability latency,
because the CLI discovers a newly published pose on its next poll. At 1000 Hz
the observation uncertainty is approximately 1 ms, apart from occasional host
scheduler stalls.

Startup samples were excluded from the latency summary because the VTS-to-host
clock mapping is still settling during initial timestamp calibration and can
produce impossible transient ages.

## SLAM latency results

The two platforms were essentially indistinguishable.

| Metric | Linux/Fusion | macOS |
| --- | ---: | ---: |
| Median SLAM update interval | 16.683 ms | 16.683 ms |
| p95 SLAM update interval | ~16.96 ms | ~17.00 ms |
| Median first-seen SLAM latency | ~23.97-24.00 ms | ~22.95 ms |
| p95 first-seen SLAM latency | ~28.27-28.28 ms | ~27.88-27.89 ms |
| p99 first-seen SLAM latency | ~29.23 ms | ~28.52 ms |
| Median latest-SLAM age at arbitrary query | ~31.95-32.0 ms | ~31.51 ms |
| Median IMU timestamp lead over new SLAM pose | ~23.04 ms | ~22.5-22.6 ms |

The update interval corresponds to approximately 60 Hz SLAM output on both
platforms.

The approximately 23-24 ms delay from SLAM pose timestamp to first observation
is independently supported by the latest-IMU-minus-SLAM timestamp difference,
which is also about 23 ms. The two quantities are strongly correlated. This is
consistent with Monado receiving a SLAM pose that is already tens of
milliseconds old, then dead-reckoning it forward using newer IMU samples.

The roughly 1 ms lower median first-seen latency on macOS is too small to treat
as a meaningful platform advantage, especially because the Linux comparison is
inside VMware Fusion. The important result is that there is **no evidence for a
substantial extra macOS SLAM delay**.

### 1000 Hz poller behaviour

Both platforms sustained the 1 kHz diagnostic adequately.

- Median polling interval was about 1.000 ms on both.
- macOS had somewhat more ordinary scheduling jitter in the p95/p99 range.
- Linux/Fusion had a few larger rare stalls, including a larger maximum
  interval than macOS in these particular runs.

These poller differences do not resemble the persistent visible headset judder
and do not materially change the SLAM latency conclusion.

## Current interpretation

Together, the prediction and SLAM-latency experiments substantially reduce the
likelihood that the visible macOS judder originates in the PSVR2 tracking path.

The following stages now look broadly comparable between native macOS and the
Ubuntu/Fusion reference:

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

1. macOS prediction over 0-20 ms horizons is smooth and quantitatively very
   similar to Linux/Fusion;
2. macOS does not receive materially older SLAM poses than the Linux/Fusion
   reference;
3. the PSVR2 SLAM stream is approximately 60 Hz on both;
4. newly available SLAM poses are about 23-24 ms old on both, with newer IMU
   data available for forward prediction.

This does **not** prove that every possible tracking-side issue is excluded, and
the Linux reference is virtualized rather than bare metal. It does, however,
make a large macOS-specific SLAM/prediction latency defect an unlikely
explanation for the observed backwards judder.

The remaining investigation should therefore concentrate primarily on stages
after the pose has been chosen for the frame:

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

In particular, compositor diagnostics should correlate the pose prediction time
with desired presentation time and the actual Metal/CVDisplayLink presentation
time to determine whether otherwise smooth poses are sometimes displayed on the
wrong refresh interval.

## Diagnostic implementation notes

The CLI diagnostics are based on `monado-cli pose-dump`.

Prediction testing uses common-base requests at 0/5/10/15/20 ms horizons.
SLAM timing diagnostics use a small PSVR2 diagnostics interface rather than
including the private PSVR2 driver header from the CLI; this keeps libusb and
other private driver dependencies confined to the driver target.

For high-resolution SLAM latency collection, use a 1000 Hz run such as:

```sh
./build/src/xrt/targets/cli/monado-cli pose-dump 1000 \
  > slam-latency.csv \
  2> slam-latency.log
```

For pure prediction comparison, 200 Hz is sufficient and produces substantially
smaller traces.
