<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Sense tracking loss in native Alyx, 25 October 2026

The user reports intermittent complete right-controller loss and difficulty
putting objects over either shoulder. The implementation below improves diagnosis
and prepares a bounded inertial fallback; hardware improvement is **not yet
validated**. No service was started, installed or switched during this work.

## Recorded evidence

Source: `~/Code/alyx-native/runs/`, specifically the `service.log` and
`identity.json` in these directories. These are native Alyx runs, separate from
the earlier Wine clock-slew investigation. Their identities select the isolated
`alyx-native/build/monado-claude-gaze-fix` build. The 20:27 identity records service
SHA256 `b2ce2204959dcaee0296467c1a33e04cb8ab31ef431d5ab9f239c7b14aed642a`;
it does not supply a source commit, so do not substitute this checkout's HEAD.

| Run (24 October, UTC name) | Device 0 (left) recovered gaps / total / longest | Device 1 (right) recovered gaps / total / longest |
| --- | --- | --- |
| `headset-native-msaa-wsi-direct-20261024T202748Z-monado` | 39 / 46.81 s / 6.14 s | 93 / 197.10 s / 76.61 s |
| `headset-native-msaa-wsi-direct-20261024T213852Z-monado` | 160 / 171.24 s / 59.39 s | 169 / 169.15 s / 68.50 s |

These are `JOINT_LOSS` summaries, not application-pose availability: they exclude
gaps shorter than 250 ms and losses still open at shutdown, include tentative
reacquisition, and cannot distinguish deliberate hiding/put-down from accidental
loss. Hand assignment in these logs is supported by the interleaved per-hand
`CONSTELLATION_FUSED_ACCEPT` records at recovery. New `POSE_STATE` records include
both side and tracker device ID to make this explicit.

The 20:27 run has **51 failed right HID output writes**, no failed left writes,
and two right disconnect/reconnect cycles. Lines 41914–44299 show IOKit error
`0xe00002bc`, 50 consecutive output failures and a disconnect. Lines 46547 and
46550 show reconnect and output recovery. A second failure/disconnect at
62275–62289 recovers at 64808–64811. The right optical filter starts afresh at
80096, immediately before the 76.61 s recovered-loss report at 80098. The
largest observed input-timestamp gap at output writes is 10.51 s right versus
0.32 s left. This is transport/reconnect evidence, not just optical occlusion;
it does not identify whether a disconnect was spontaneous or user initiated.
An IMU fallback cannot bridge periods without IMU input.

In contrast, all 141,553 output writes in the 21:38 run succeed. Its largest
observed input gaps are 111/120 ms (left/right), despite very long optical losses.
That run needs a separate optical/visibility explanation; successful writes do
not establish that the programmed LED pulses intersect camera exposure.

Most long-loss exposures are `dark_unpredicted` (20:27: 1,729 left / 8,536
right; 21:38: 6,342 / 6,347). This means insufficient free LED-shaped blobs above
the learned background and an expired position prediction. It **does not prove
out-of-view, physically dark LEDs, or a clock fault**. The 20:27 right also has
967 `lit_single`, 821 `lit_multi`, and 1,100 `acquiring` exposures: some recovery
opportunities remain with light visible. These categories are evidence for
investigating visibility, correspondence and confirmation separately; they do
not justify loosening optical acceptance gates indiscriminately.

The [earlier host-clock diagnosis](macos-pssense-6dof-integration.md#the-shared-clock-shift-is-the-host-clock-macos-slews-clock_monotonic-2026-10-14)
remains relevant history, not an established cause of these native sessions.

Reproduce the summary without hardware:

```sh
python3 scripts/macos/summarize-sense-losses.py \
  ~/Code/alyx-native/runs/headset-native-msaa-wsi-direct-20261024T202748Z-monado/service.log \
  ~/Code/alyx-native/runs/headset-native-msaa-wsi-direct-20261024T213852Z-monado/service.log
```

Local machine-readable results are saved in
`build/arm64/controller-evidence/20261025-alyx-occlusion/summary.json`.
Use one service lifetime per input log. Active-loss snapshots are cumulative and
are kept separate from recovered totals. Observed HID gaps are sampled at output
writes; pose-state log counts are not time fractions.

## Changes prepared in this checkout

- `JOINT_LOSS_ACTIVE` starts at 250 ms missing and repeats every five seconds,
  even if the controller never returns. It carries an exposure timestamp, loss
  classes, initial camera coverage/edge margin and current prediction visibility.
  The final `JOINT_LOSS` retains its existing meaning and gains a timestamp.
- `PSSENSE_POSE_DIAG=1` logs `POSE_STATE` on application relation-flag changes
  and once per second while queried, with host/query times, side/device ID,
  accepted-optical and IMU age, filter counts, last solved camera count and LED
  model position. This is the relation before grip/aim transforms and any explicit
  synthetic-position options, not a full trace of the final OpenXR space graph.
  Filter rejection samples (first and every 30th) and timestamped filter status
  make visible-optical versus accepted-filter disagreements diagnosable.
- The existing EKF integrates accelerometer/gyro data during optical loss. It
  already allows position for 300 ms, reporting tracked position for 250 ms.
  `PSSENSE_IMU_FALLBACK_MS=600` with `PSSENSE_FILTER=1` extends **application
  position validity only** to 600 ms since the last accepted optical exposure.
  Runtime input is clamped to 0–1000 ms; 0 retains the default. This never extends
  tracked status or the optical solver's prior. The same filter state is used,
  and reacquired optics use the existing filter update/reinitialisation policy.
- Both normal and fallback relations now stop claiming live tracking/velocity
  when IMU age exceeds the filter's 100 ms forward-prediction limit. A held
  orientation can remain valid. Previously stale IMU output retained tracked
  orientation indefinitely. Reconnect resets the filter and diagnostics state.

A 600 ms fallback is an experiment for short shoulder excursions. IMU integration
has no absolute position reference and can drift significantly; it cannot make
seconds of hidden movement accurate. Applications may also choose to hide a
valid but untracked position. Do not claim shoulder interaction is fixed before
a user-operated test. Optical solver thresholds, Linux camera-stream defaults
and normal position horizons remain unchanged.

## Origin jump after loss (25 October 2026)

The mode-16 controller survey shows each optical loss, including 28 gaps of
about 0.1 s, ending position validity 300 ms after the last accepted pose. The
driver then reports orientation only, and OpenXR receives the tracking origin,
about 6 m away, until optics return (up to 10 s behind the shoulder; 19–37 cm
from the last tracked position after long holds). `PSSENSE_HOLD_POSITION=1`
(opt-in) holds the last valid position instead, untracked, while orientation
continues. A headset check with `--generic-controller` is prepared in
`20261025-depth-hold-v4`.

## Offline coast and blob evidence (25 October 2026)

Dataset: `20261025-controller-survey-both-mode16-v3/constellation.ctd` (518 s,
both controllers, detector blobs, 66 Hz IMU). Ground truth is the offline joint
tracker (`constellation_replay --tracker --tracker-csv`, 93% of exposures, RMS
0.3–0.4 px). `--filter-eval` now takes tool-only `FILTER_GAP_MS`,
`FILTER_GAP_PERIOD_MS`, `FILTER_DAMP_MS` and `FILTER_STILL_GATE=1`, and writes gap
age, hold and damped/gated estimates per hidden exposure. Errors by time since
the last delivered optical pose, 3 s gaps every 6 s (`moving`: truth more than
5 cm from the hold position; p50 / p95 mm):

| Since optical | Hold | EKF coast | Still-gated coast |
| --- | --- | --- | --- |
| 0.15–0.3 s, all / moving | 2 / 21, 82 / 146 | 2 / 8, 5 / 15 | 2 / 10, 7 / 12 |
| 0.5–1 s | 5 / 46, 138 / 304 | 13 / 50, 22 / 66 | 6 / 32, 26 / 59 |
| 1–2 s | 9 / 168, 157 / 457 | 51 / 196, 56 / 140 | 9 / 81, 46 / 124 |
| 2–3 s | 13 / 245, 173 / 468 | 169 / 545, 165 / 395 | 14 / 164, 74 / 218 |

The EKF coast is far better than holding while the hand moves, even beyond 1 s,
but drifts while it rests. Integrating the filter velocity only while the IMU
shows motion (|gyro| > 0.3 rad/s or |accel| more than 0.5 m/s² from 1 g) keeps
both benefits. Exponential velocity damping (100–400 ms) was worse than either.
Runs of metres seen at first were real tracker losses coasted unboundedly, not
IMU faults; the live 300 ms validity limit already prevents those.

Blobs during real losses (the offline tracker's gaps over 250 ms, projected
from the nearer of the last/next solved pose; projection checked at 6–7 px on
tracked exposures): behind-shoulder and shoulder losses have essentially no
unowned blobs near the predicted position. In-view losses (low, side, palm-up,
cross-body) often have 4–9 unowned blobs there, frequently in both lower
cameras: the controller is visible but not solved. These could constrain
position with the IMU orientation (partial-blob updates), which needs tracker
changes and is not attempted here. The offline tracker loses some poses the live
run kept (trial 16 low: 92% live), so these counts are indicative.

### Driver coast (opt-in)

`PSSENSE_COAST_POSITION=1` implements the still-gated coast in the driver, per
IMU sample under the controller lock, from the moment the filter's normal
position validity (300 ms) expires; earlier gaps already use the filter's own
coast. It adds limits the offline test did not have: 3 s, 0.8 m from the coast
start and 4 m/s, then holds. Unit tests cover tracking, gating, the limits and
the application relation. It is enabled in `20261025-depth-hold-v4` with the
hold; the headset check is pending.

## Next user-operated capture

Build the service used by the Alyx launcher from these sources. Its recorded
identity currently points to another build directory; rebuilding `build/arm64`
alone does not update that isolated service. Pass these environment variables
to the **service**, not just Alyx:

```sh
PSSENSE_POSE_DIAG=1
PSSENSE_IMU_FALLBACK_MS=0  # baseline: current 300 ms horizon
# Second capture: change the value to 600.
# The integrated 6DoF setup must retain PSSENSE_FILTER=1.
```

The isolated native-test preparation helper also accepts `--pose-diag` and
`--imu-fallback-ms 600`. Preparing its plist does not start hardware. Existing
`PSSENSE_TIMING_DIAG=1` can add detailed HID/clock evidence but emits much larger
logs. Preserve the launcher identity and service log with each capture.

Start with both rings visible, then test left/right separately: slow shoulder
reach, quick shoulder reach, hold behind shoulder for 0.2/0.5/1 s, return to view.
Record which side and approximate elapsed time. Follow with sustained normal
play to separate a transport disconnect from optical occlusion. Compare time to
position expiry, visible-light failures, filter rejection/reinitialisation,
return-to-view recovery and subjective shoulder interaction. Longer fallback
must not delay optical reacquisition or leave large position jumps.

## Validation

Prepared against `0daaffc4a` plus the working-tree changes. Regression tests cover
66 Hz synthetic occlusion, normal-prior expiry while the application fallback
remains valid, fallback expiry/cap, optical recovery, and stale-IMU flags. The
summary tests cover cumulative active snapshots and independent HID failures.
Full `build/arm64` build passes. All 91 native CTests pass with host IPC/GPU
access, including the seven focused tracking/filter/clock suites; the two new
Python summary tests pass. The first sandboxed full run could not initialise
20 IPC/GPU fixtures (one optional test skipped); rerunning with host access
passes all 91. Logs are saved beside `summary.json` as `build-final.log` and
`ctest-host.log`. `git diff --check` passes. Hardware acceptance and Linux CI
remain pending.
