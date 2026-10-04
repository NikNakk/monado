# PS Sense: evaluating Monado's upstream constellation fusion (MR 2940, MR 3015)

Branch `codex/pssense-upstream-fusion-evaluation`, based on local `macos-pssense-6dof` at `c1ca162de`. That tip is
37 commits ahead of `origin/macos-pssense-6dof` (`0eb4ee373`) and includes the IMU recording and the EKF this evaluation
needs. Upstream references:

- MR 2940 (merged, `33df3989d`): Ceres pose refinement and a custom RANSAC/P3P. It is in upstream `main` but not on this
  branch.
- MR 3015 (open, head `cd7174257` when read on 2026-09-28): rig calibration, BROAD/BG LED sync for the Sense, IMU
  preintegration and sliding-window sensor fusion.

## Summary and recommendation

**Recommendation: adopt selected components only. Keep M3 and the existing EKF as the live path, and do not integrate the
upstream-derived sliding-window fusion.**

An offline adaptation of MR 3015's fusion (`fusion/`) was run against the frontend's M1/M2 poses, raw, and the
branch's EKF (`t_imu_optical_filter`). All three paths got identical input: two recordings with Sense IMU data, two
controllers each, and three input scenarios. On the recorded data:

- **Availability is the same.** The sliding-window fusion (SWF) and the EKF both lift valid-pose coverage well above
  the raw optical (e.g. 69.8% → 86.0/86.1%), and differ from each other by at most 0.7 percentage points. Both are
  limited by the same optical dropouts.
- **The SWF is modestly better in several consistency metrics.** Its reprojection p95 stays close to the optical (0.66
  against the EKF's 0.71 px; 0.95 against 1.66 px on the sparse right controller). It has 16–40% fewer frame-to-frame
  jumps. It tracks through 300 ms optical gaps with a lower position p95 on three of four controllers (12.0 against
  19.1 mm, 15.4 against 23.8, 22.7 against 37.1), but slightly worse on the fourth (23.9 against 22.8). Orientation
  through gaps is no better: better on one controller, slightly worse on three.
- **Neither is better at static jitter.** It is set by the optical input, within about 0.4 mm across paths.
- **It costs about 300× more** (1.0 ms p50, 2.3–2.6 ms p95 per controller-exposure, against 3 µs), and it adds Ceres as
  a dependency. The EKF is already fast enough to run inline.
- **As written upstream, the fusion does not work on this hardware without changes.** With the IMU-to-model rotation
  solved for, as upstream does, the Sense ring's weakly observed tilt let the solution drift 20–30° from the optical
  while its reprojection stayed at 0.3–0.9 px. Anchoring the bias to its latest estimate let a runaway bias persist
  across resets. Its only outlier protection, a Huber loss, would have fed wrong locks into the marginalisation
  prior. The gains above come from the adapted version, and part of its robustness comes from gating added here.
- **The dominant failures in the data are frontend ones.** Unsolved exposures (23–62%) and wrong optical locks, which
  are self-consistent poses 0.1–0.9 m or 30–170° from where the gyro puts the ring, affect every path equally. Neither
  backend fixes them. Both correctly reject short wrong-lock streaks, and both re-initialise onto a wrong lock that
  persists for five exposures.

The components worth taking are listed under "Recommendation in detail" below.

## 1. Architecture audit

### This branch

- **Frontend.** Mode-4 blobs from four PS VR2 cameras. **M2** (`stereo_bootstrap`) triangulates stereo pairs and
  registers them to the ring with RANSAC. **M1** (`joint_pose_solver`) is a robust Gauss–Newton over every camera's
  correspondences. It uses a 3° orientation prior from the driver's IMU orientation, aligned into the optical world.
  **M3** (`t_constellation_tracker_joint.cpp`, `CONSTELLATION_TRACKER_JOINT=1`) runs M1 then M2 per exposure in the
  live tracker. A new track stays tentative until it has solved 3 times.
- **Backend.** `t_imu_optical_filter` (`PSSENSE_FILTER=1`) is an error-state EKF (15 states). It takes the bias-corrected
  Sense IMU and each M3 pose as a 6-DoF measurement (2 mm / 0.46°), gates at Mahalanobis² 30, and re-initialises after
  0.5 s or 5 rejections.
- **Replay format** (`constellation.ctd`, `t_constellation_tracker_dataset.cpp`) has these packets:
  - 0 (camera sample): blobs, `Txr_world_cam`, and per-device priors and found poses;
  - 1 (device info): the LED model, in the OpenCV convention;
  - 3 (device tracking): the driver's tracking-source relation at each sample;
  - 4 (IMU sample): host time, factory- and online-bias-corrected, IMU frame, XR convention.

  An unknown packet type stops the reader, because packets carry no length.

### MR 2940 (`33df3989d`)

`optimizer/pose_optimize.cpp` and `ransac.cpp` replace the OpenCV per-camera PnP and refinement with Ceres. Each camera
solves on its own (P3P RANSAC, then a Ceres refinement with a whitened pose covariance). The MR also makes Ceres a hard
dependency of the constellation module (`XRT_MODULE_CONSTELLATION_TRACKING DEPENDS XRT_HAVE_BIGCERES`).

### MR 3015 (`cd7174257`)

| upstream file | what it is | reusable here? |
|---|---|---|
| `optimizer/imu_preintegration.hpp`, preintegration half of `internal_math.cpp` | Forster-style preintegration with Jet-derived bias Jacobians, covariance propagation, IMU and bias factors | **Yes, nearly unchanged.** Only the noise constants are CV1-specific |
| `optimizer/internal_math.{hpp,cpp}` (rest) | parameter blocks, right-multiplying quaternion manifold, OKVIS-style `marginalize()` | **Yes, unchanged.** The LED residual needed our KB4 projection |
| `optimizer/sensor_fusion.{hpp,cpp}` | 3-keyframe fusion thread with an IMU ring, FEJ wrapper, marginalisation prior, relation history | **Factors and prior machinery: yes.** Threading, ring and keyframe bookkeeping are tied to upstream's `CameraSample`/`DeviceState` and a per-camera PnP seed |
| `optimizer/rig_calibration*` | online calibration of static outside-in sensors (pitch/roll, gravity) | **No.** It assumes stationary CV1 sensors. Our rig comes from ChArUco and moves with the head |
| `led_sync/pssense_gray_code.h`, `t_led_sync_refinement.c` | Sony-style BROAD/BG blink codes | **Not now.** It conflicts with our PRESCAN-only `t_led_phase_bootstrap` and heavily modified `t_led_sync_refinement` |
| `m_quatexpmap{,_ceres_internal,_bigceres}.hpp` (upstream `main`) | Jet-safe SO(3) exp/log | **Yes, copied verbatim.** The branch's `m_quatexpmap.cpp` only has file-local half-angle versions, so this is not a duplicate |

**API conflicts with this branch.** Taking MR 3015 wholesale would break all of the following:

- `t_blob.center` is split into `center_distorted`/`center_undistorted`/`center_homogenized`. Every M1/M2 routine and
  the dataset format read `center`, and upstream undistorts in blobwatch while M1 fits KB4-distorted pixels.
- `t_blob` and `CameraSample` gain a `sequence_id`, which changes the dataset format.
- `t_constellation_tracker_sample.pose` becomes `world_pose`, with a per-LED observation array. The Sense driver's
  joint-sample path (`joint_camera_count`) depends on the current layout.
- `DeviceLastPose.Txr_world_device` becomes non-optional, and `tryDevicePose` takes a relation instead of a pose.
- MR 2940 removes `pose_optimize.{h,cpp}`, which the per-camera path still uses.
- `pssense_driver.c` has diverged by about 3000 lines on this branch against 166 upstream.

Selective adaptation avoids all of these.

**`DeviceObservation` and moving cameras.** Upstream stores `Tcv_world_cam` in each observation. Its camera factor
(`StaticCameraObservationCostFunctor`) treats that pose as a constant of the factor, so "static" only means the camera
pose is not optimised. A time-varying `T_world_camera(t) = T_world_HMD(t) · T_HMD_camera` can be supplied per observation
without changing the factor. That is what this adaptation does. World-frame recordings (`PSVR2_CONSTELLATION_WORLD=1`)
already store the composed pose per sample. The HMD/SLAM pose is taken as exact: **its uncertainty and latency are not
modelled.**

### Coordinate systems

| frame | convention | where it appears | mapping |
|---|---|---|---|
| Sense IMU | XR axes of the IMU | driver, dataset packet 4 | to CV: `C·v`, where `C = diag(1, −1, −1)` |
| LED model | CV (dataset, M1) or XR (driver) | LED positions and normals, poses of the "device" | `p_imu = Q_imu_model · p_model`, `Q_imu_model = R_x(+50.27°)` (`pssense_imu_angle`); commutes with `C`, so it is the same quaternion in both |
| camera c | CV | blobs (KB4, distorted px) | `T_world_cam_c(t) = T_world_HMD(t) · head_from_camera0 · T_cam0_camc` |
| HMD | XR | SLAM pose, `head_from_camera0_xrt` in the calibration | recoverable from a recording: `T_world_HMD = T_world_cam0 · head_from_camera0⁻¹` |
| optical/SLAM world | XR (y up) or CV (y down) | poses in and out | pose XR↔CV is conjugation by `C` (a 180° rotation about x, its own inverse); gravity is +y in CV, −y in XR |

The fusion works in CV throughout, with the IMU body as its state frame. Its output is the LED model pose,
`q_world_imu · Q_imu_model`. The IMU-to-LED lever arm (`T_led_imu`, 1–2.5 cm) is ignored, as it is upstream and in the
EKF. The tests check the composition, the conversions and the mounting rotation. Replay confirms the IMU convention: gyro
rotation over 100 ms matches the optical to 0.46° p50, and 0.57° above 120°/s. The wrong sign of the mounting angle gives
18.7°, and no XR→CV conversion gives 23.3°.

### Clock domains

| stream | source clock | to host monotonic |
|---|---|---|
| Sense IMU samples | controller device ticks, one sample per Bluetooth report (~66 Hz) | `pssense_device_ts_to_host`: clock offset from input-report receive times (max-tracker, snaps with `PSSENSE_CLOCK_OFFSET_SNAP_US`) |
| Sense LED schedule | controller clock | the same offset, from projected exposures (`LED_SCHEDULE now=` in `run.log`) |
| PS VR2 camera exposures | VTS | `hw2mono_vts` (minimum-delay robust filter with `PSVR2_ROBUST_CLOCK=1`) |
| PS VR2 SLAM poses | headset timestamps | the same VTS mapping; world mode samples the SLAM history at the exposure (interpolated, or predicted a few ms past) |

The two mappings are independent. A diagnostic in the replay finds the IMU time shift that best matches the optical
rotation. On both recordings it is 0 to +3 ms, so they agree to within a few milliseconds there. No path estimates a
time offset.

### Are the recordings sufficient?

**Yes, for the initial offline experiment**, on the two recordings with IMU packets: `20260926-010135-imu-capture` and
`20260926-014505-filter-live`, both world frame, 90 s and two controllers. Each has four cameras' blobs every exposure,
every sample's camera world pose (HMD pose composed), and every IMU sample. Earlier recordings have no IMU and cannot be
used for fusion. Their M1/M3 results are unchanged (below). Several things are missing:

1. **LED-sync, clock-offset and gyro-bias events** exist only in `run.log`, without timestamps. The replay places each
   one at the preceding `LED_SCHEDULE now=`, which is accurate to about one frame.
2. **Raw IMU and the bias the driver subtracted.** The samples already have the online gyro bias removed, which steps
   when the driver detects stillness. The replay resets at those steps.
3. **The HMD pose, its age, and whether it was interpolated or predicted.** Stale HMD poses cannot be detected offline.
4. **The IMU sample's device timestamp.** Without it the clock mapping cannot be recomputed offline.

**Smallest compatible extension.** This was proposed here and has since been implemented, in a different form, as
packet 5 extension records. See `doc/macos-pssense-mr2940-frontend-evaluation.md`. The original proposal was:

- packet 5, device sync event: `{device, host_ns, kind (clock_snap | led_lock | led_lost | led_scan | phase_move |
  gyro_bias), value[3]}`;
- packet 6, IMU timing: `{device, device_ts_us, wrap_count, clock_offset_ns, applied_gyro_bias[3]}` beside each packet 4;
- packet 7, HMD pose: `{sample_id, T_world_HMD (XR), flags (interpolated | predicted), prediction_ns}`.

Current readers stop at the first unknown packet. New readers read old files unchanged. A length-prefixed extension
packet would let future readers skip what they do not know.

## 2. The offline experiment

### What was adapted, and from where

| file (this branch) | from | status |
|---|---|---|
| `aux/math/m_quatexpmap{,_ceres_internal,_bigceres}.hpp` | upstream `main` | verbatim, Apache-2.0 header kept |
| `constellation/fusion/fusion_math.{hpp,cpp}` | MR 3015 `optimizer/internal_math.{hpp,cpp}` | parameter blocks, manifolds and `marginalize()` unchanged; LED residual projects with this branch's templated KB4 model in distorted pixels |
| `constellation/fusion/imu_preintegration.{hpp,cpp}` | MR 3015 `optimizer/imu_preintegration.hpp`, `internal_math.cpp` | integration, covariance, bias Jacobians and factors unchanged; noise model is a runtime struct (upstream CV1 values kept as `ImuNoiseModel::upstreamCv1()`); an interval with no IMU sample inside it is integrated with the next sample |
| `constellation/fusion/sliding_window_fusion.{hpp,cpp}` | MR 3015 `optimizer/sensor_fusion.cpp` | camera factor, `FirstEstimateJacobianCostFunction`, `MarginalizationPriorCostFunction` and the eviction procedure adapted; window bookkeeping rewritten (synchronous, one device, keyframes in a deque, prior blocks named by keyframe id) |
| `constellation/fusion/psvr2_fusion_frames.hpp` | new | XR/CV, IMU mounting, HMD-to-camera composition |
| `constellation/tools/fusion_compare.{hpp,cpp}`, `replay_records.hpp` | new | the comparison harness |

Upstream's copyright and SPDX lines are kept on every adapted file, with Beyley Cardellio as author.

The library (`constellation_fusion`) is built only when CMake finds Ceres ≥ 2.1. The live tracker, the drivers and
`monado-cli` never link it. There is no live connection, compiled in or otherwise.

### Material deviations from upstream

1. **Moving cameras.** Each observation carries `T_world_HMD(t) · T_HMD_camera`. One keyframe holds every camera of an
   exposure, and it is seeded from the M1/M2 joint pose, not from one camera's PnP.
2. **IMU-to-model extrinsic fixed** at the mounting angle by default; upstream solves for it. The ring's tilt is weakly
   observed: 13° moves the blobs about 0.5 px at 40 cm. With the extrinsic free, the solve traded the two against each
   other. It drifted 20–30° from the optical during fast turns while fitting at 0.3–0.9 px, and its left-controller
   orientation p95 through gaps went from 2.6° to 8.8°.
3. **Bias anchor at the calibrated bias** (zero, since the driver has already removed its bias), not upstream's latest
   estimate. Resets re-seed from it. On `014505` a bias that reached its bound (22.9°/s) otherwise held itself there
   across resets.
4. **Noise.** Gyro 0.02 rad/s/√Hz and accelerometer 0.3 m/s²/√Hz, the EKF's tuned values, which absorb 66 Hz
   zero-order hold and timestamp jitter. Blob σ is 0.5 px (CV1: 0.16). Upstream's CV1 values are worse (see
   sensitivity). Random walks, anchors and bounds are upstream's.
5. **Gating and resets (additions).** Before admission, the seed position must lie within 40 mm + ½·40 m/s²·dt² of the
   IMU prediction. The observations must reproject within 4 px RMS at the seed position with the predicted
   orientation, a test that passes a tilt disagreement and fails a wrong lock. A 45° + 90°/s·dt angle gate is the
   backstop. After the solve, a new keyframe above 2 px RMS is dropped and the window and prior are restored. At
   eviction, camera factors above 2 px are left out of the prior. The fusion resets after 1 s without an accepted
   keyframe, after 5 rejections, when IMU is missing, or on an epoch change.
6. **Solver.** Dense QR, one thread, 15 iterations. Upstream uses sparse normal Cholesky across all cores. The problem
   is tiny, and replay needs determinism.
7. **Output.** A causal pose at any time: the newest keyframe, carried forward by the IMU. Position counts as valid for
   300 ms after the last accepted keyframe, the same rule as the EKF.

An optional seed-orientation prior (`seed_orientation_sigma_deg`, 3° in the `swf_op` path) mirrors M1's own IMU prior.
With the extrinsic fixed it makes no material difference, so it is off by default.

### MR 3015's known failure modes

| failure mode | handling here |
|---|---|
| bad optical solves contaminating the marginalisation prior | pre-admission gate plus post-solve check with window and prior restored; outlier camera factors excluded at eviction (`factors kept out of the prior` counter: 0–1 per run) |
| no reset when Sense LED sync changes | epochs from `run.log` (`led_locked`, `led_lost`, `led_scan_start`, `stuck_lit`, `clock_snap`, `gyro_bias`) reset window and prior (3–4 per controller per run). Phase-tracking moves are not epochs: they move the LED pulse, not any timestamp |
| controller-on-controller misidentification | diagnostic: frontend solves within 60 mm of the other device's (0 in both recordings); wrong locks are caught by the gate, not by identity |
| fast-motion loss | IMU prediction for 300 ms and reset after 1 s; see dropout results |
| missing or wrong BG blink headers | not applicable: this branch drives the LEDs in PRESCAN with its own phase bootstrap; the `stuck_lit` epoch covers the always-lit fault |
| stale camera or HMD poses | not detectable offline (no HMD pose age recorded; see the extension above); the HMD pose is taken as exact |
| unbounded queues or replay memory | IMU buffer trimmed to the newest keyframe − 100 ms and capped (peak 156–587 samples); published states capped at 512; fixed window; no queue (synchronous) |

**Not solved by either backend:** a wrong lock that persists for five or more exposures. Both backends re-initialise
onto it, the EKF after 5 rejections and the SWF after 5. Live M3 guards against it with 3 tentative solves, which the
open-loop replay frontend does not have. Requiring the rejected streak to be IMU-consistent before re-initialising would
not have helped here, because the wrong M1 tracks are self-consistent.

### Tests

`tests/tests_pssense_fusion.cpp` (9 cases, 614 assertions; synthetic scene of a moving headset with four KB4 cameras
and a 17-LED ring):

- HMD-to-camera composition, and recovering the HMD from camera 0;
- XR/CV conversion (conjugation by C, involution, right-handedness), the rest accelerometer against CV gravity, and the
  mounting quaternion against the EKF path's `imu_to_led`;
- camera timestamps between IMU samples (exact for constant rates), and an interval with no sample inside it;
- preintegration composing across a keyframe boundary that falls between samples;
- a moving controller seen by moving cameras (0.29 mm RMS, max 0.8 mm / 0.21°), and the same run with frozen camera
  poses failing (more than 20 rejections);
- a 300 ms dropout bridged (max 1.8 mm / 0.24° synthetic) and reacquired;
- two self-consistent wrong locks (90°, 10 cm) rejected with no effect on the output;
- an epoch change resetting the state;
- a bounded IMU buffer.

## 3. Comparison on recorded data

### Method

`constellation_replay SESSION/constellation.ctd --fusion-compare --run-log SESSION/run.log` runs the M1/M2 frontend
(`replay_m1`) once, open loop. Its accepted solves, with their correspondences and per-sample camera poses, go to four
paths:

- `optical`: the solves as they are;
- `ekf`: `t_imu_optical_filter` with defaults, as `PSSENSE_FILTER=1` feeds it (IMU rotated into the LED frame, pose
  σ 2 mm / 0.46° scaled by RMS);
- `swf`: the adapted sliding-window fusion;
- `swf_op`: `swf` plus the 3° seed orientation prior.

Every path is queried at every exposure, with IMU up to 20 ms past it and optical input up to and including it.

The scenarios alter the shared input identically for every path:

- `nominal`: the input as recorded.
- `dropout`: optical withheld for the last 300 ms of every 2 s. Withheld exposures are scored against the withheld
  solve; exposures a path has no output for are scored by holding its last output.
- `corrupt`: three consecutive solves every 2 s (at +1.0 s) replaced with a self-consistent wrong lock. The pose turns
  60° and moves 8 cm, and the blobs are re-rendered from it. Scored by whether the output follows it (within 20 mm),
  the maximum excursion from the path's nominal output, and the time after the burst until the output is back within
  5 mm / 2° of nominal.

Metric definitions, shared by all paths:

- **Interval:** from the device's first frontend solve to the end.
- **Valid:** position valid at the exposure.
- **Dropouts:** runs of invalid exposures after the first valid one. Their duration, last valid to next valid, is the
  reacquisition time.
- **Reprojection:** per-LED RMS of the output pose against the uncorrupted correspondences, where the path was given
  them.
- **Static jitter:** RMS spread within 1 s windows where the unaltered optical path solved at least 20 times and moved
  less than 20 mm. The windows are the same for every path.
- **|Δv|, |Δω|:** change in finite-difference velocity between consecutive frames.
- **Jumps:** consecutive outputs more than 30 mm or 10° apart.
- **IMU-inconsistent:** an output more than 20° from the path's previous output (≤ 0.5 s back) carried forward by the
  gyro. This means an adopted or abandoned wrong lock.
- **Gyro vs output:** angle between the output's rotation over 100 ms and the gyro's. It is partly circular for paths
  that use the IMU.
- **Cost:** backend time per optical input; for `optical`, the frontend's own solve time.

**Acquisition** is not tabulated. Every path outputs from the first frontend solve, so the backends add 0 ms. That solve
came 2.3–3.0 s into each recording, the same for every path.

Machine: Apple M5, macOS 26.6.2, `RelWithDebInfo`, one thread.

### Results

<!-- Generated from ~/Code/psvr2-datasets/experiments/20260928-upstream-fusion-eval/*.log -->

**`20260926-010135-imu-capture`, left controller**

| path | valid | dropouts (>100 ms) | reproj px p50/p95 | static jitter mm p50/p95 | static jitter ° p50/p95 | \|Δv\| p95 m/s | \|Δω\| p95 °/s | jumps | IMU-inconsistent | gyro vs output 100 ms ° p50/p95 | µs per input p50/p95 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| optical | 69.8% | 174 (32) | 0.405 / 0.632 | 2.23 / 4.89 | 0.705 / 2.175 | 0.282 | 73.3 | 2 | 14 / 3624 | 0.46 / 1.74 | 59.5 / 106.1 |
| ekf | 86.1% | 21 (16) | 0.428 / 0.712 | 2.33 / 4.80 | 0.554 / 1.832 | 0.169 | 40.2 | 57 | 9 / 4481 | 0.31 / 1.16 | 3.1 / 3.8 |
| swf | 86.0% | 19 (15) | 0.409 / 0.655 | 2.37 / 4.75 | 0.433 / 1.534 | 0.142 | 39.5 | 34 | 7 / 4473 | 0.24 / 0.89 | 1023.5 / 2336.6 |
| swf_op | 86.0% | 19 (15) | 0.409 / 0.655 | 2.36 / 4.75 | 0.469 / 2.031 | 0.142 | 38.7 | 34 | 7 / 4473 | 0.24 / 0.88 | 1171.5 / 2493.6 |

| path | dropout: hidden exposures with output | vs withheld solve mm p50/p95 | ° p50/p95 | hold-last for the rest mm p50/p95 | corrupt: followed wrong lock | max excursion mm | recovery after burst ms p50/max |
|---|---|---|---|---|---|---|---|
| optical | 0.0% | – | – | 7.7 / 174.8 (n=513) | 92 / 92 | 80.0 | 17 / 83 |
| ekf | 93.8% | 3.2 / 19.1 | 0.52 / 2.55 | 13.6 / 427.1 (n=32) | 0 / 92 | 10.0 | 17 / 17 |
| swf | 93.8% | 2.9 / 12.0 | 0.50 / 2.61 | 11.7 / 279.1 (n=32) | 0 / 92 | 10.4 | 17 / 17 |
| swf_op | 93.8% | 2.8 / 12.0 | 0.50 / 2.37 | 11.7 / 279.1 (n=32) | 0 / 92 | 10.4 | 17 / 17 |

**`20260926-010135-imu-capture`, right controller**

| path | valid | dropouts (>100 ms) | reproj px p50/p95 | static jitter mm p50/p95 | static jitter ° p50/p95 | \|Δv\| p95 m/s | \|Δω\| p95 °/s | jumps | IMU-inconsistent | gyro vs output 100 ms ° p50/p95 | µs per input p50/p95 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| optical | 38.4% | 325 (161) | 0.529 / 0.848 | 1.35 / 1.35 | 0.884 / 0.884 | 0.122 | 71.3 | 0 | 18 / 1989 | 0.44 / 5.86 | 59.6 / 311.9 |
| ekf | 76.2% | 48 (41) | 0.579 / 1.655 | 1.54 / 1.54 | 0.697 / 0.697 | 0.215 | 51.0 | 70 | 15 / 3985 | 0.27 / 1.81 | 3.1 / 3.8 |
| swf | 75.5% | 48 (41) | 0.533 / 0.949 | 1.21 / 1.21 | 0.633 / 0.633 | 0.187 | 42.4 | 54 | 11 / 3945 | 0.22 / 1.25 | 1248.7 / 2419.4 |
| swf_op | 75.5% | 48 (41) | 0.533 / 0.948 | 1.19 / 1.19 | 0.527 / 0.527 | 0.189 | 42.7 | 54 | 11 / 3945 | 0.22 / 1.25 | 1344.9 / 2590.2 |

| path | dropout: hidden exposures with output | vs withheld solve mm p50/p95 | ° p50/p95 | hold-last for the rest mm p50/p95 | corrupt: followed wrong lock | max excursion mm | recovery after burst ms p50/max |
|---|---|---|---|---|---|---|---|
| optical | 0.0% | – | – | 53.0 / 293.9 (n=297) | 50 / 50 | 80.0 | 17 / 384 |
| ekf | 86.5% | 4.0 / 23.8 | 0.48 / 1.58 | 40.6 / 915.8 (n=40) | 4 / 50 | 225.8 | 17 / 567 |
| swf | 86.5% | 4.0 / 15.4 | 0.40 / 1.09 | 24.3 / 918.9 (n=40) | 1 / 50 | 92.1 | 17 / 851 |
| swf_op | 86.5% | 4.0 / 15.4 | 0.40 / 1.09 | 24.3 / 918.9 (n=40) | 1 / 50 | 92.7 | 17 / 851 |

**`20260926-014505-filter-live`, left controller.** The live EKF collapsed in this run; the replay frontend solved 38%.

| path | valid | dropouts (>100 ms) | reproj px p50/p95 | static jitter mm p50/p95 | static jitter ° p50/p95 | \|Δv\| p95 m/s | \|Δω\| p95 °/s | jumps | IMU-inconsistent | gyro vs output 100 ms ° p50/p95 | µs per input p50/p95 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| optical | 38.1% | 116 (24) | 0.386 / 0.833 | 2.85 / 9.29 | 1.367 / 2.980 | 0.242 | 89.2 | 0 | 0 / 1972 | 0.41 / 3.99 | 60.1 / 103.3 |
| ekf | 49.3% | 16 (16) | 0.415 / 1.057 | 2.44 / 10.59 | 1.391 / 2.979 | 0.199 | 54.3 | 45 | 2 / 2559 | 0.30 / 2.11 | 3.1 / 3.8 |
| swf | 49.0% | 15 (15) | 0.391 / 0.903 | 2.84 / 10.59 | 1.428 / 2.977 | 0.184 | 49.9 | 38 | 1 / 2543 | 0.22 / 1.84 | 1035.5 / 2406.5 |
| swf_op | 49.0% | 15 (15) | 0.391 / 0.902 | 2.78 / 10.59 | 1.422 / 2.499 | 0.181 | 49.3 | 38 | 1 / 2543 | 0.22 / 1.76 | 1133.5 / 2586.2 |

| path | dropout: hidden exposures with output | vs withheld solve mm p50/p95 | ° p50/p95 | hold-last for the rest mm p50/p95 | corrupt: followed wrong lock | max excursion mm | recovery after burst ms p50/max |
|---|---|---|---|---|---|---|---|
| optical | 0.0% | – | – | 13.5 / 223.9 (n=285) | 57 / 57 | 80.0 | 17 / 1000 |
| ekf | 90.2% | 4.4 / 22.8 | 0.58 / 3.82 | 27.9 / 631.1 (n=28) | 4 / 57 | 218.2 | 17 / 1000 |
| swf | 90.2% | 3.0 / 23.9 | 0.51 / 4.51 | 32.2 / 634.9 (n=28) | 1 / 57 | 222.5 | 17 / 1000 |
| swf_op | 90.2% | 3.0 / 25.2 | 0.51 / 3.60 | 32.5 / 634.1 (n=28) | 1 / 57 | 222.5 | 17 / 1000 |

**`20260926-014505-filter-live`, right controller.** Both controllers' IMU streams stop for 2 × 420 ms (a host stall). The
SWF resets for the 46 exposures without IMU and passes the optical through.

| path | valid | dropouts (>100 ms) | reproj px p50/p95 | static jitter mm p50/p95 | static jitter ° p50/p95 | \|Δv\| p95 m/s | \|Δω\| p95 °/s | jumps | IMU-inconsistent | gyro vs output 100 ms ° p50/p95 | µs per input p50/p95 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| optical | 77.0% | 124 (48) | 0.470 / 0.759 | 2.26 / 7.75 | 1.254 / 6.027 | 0.139 | 74.8 | 10 | 9 / 4009 | 0.43 / 1.41 | 63.0 / 119.9 |
| ekf | 91.9% | 18 (13) | 0.500 / 1.276 | 2.26 / 7.74 | 1.217 / 6.311 | 0.160 | 57.3 | 58 | 4 / 4788 | 0.30 / 1.16 | 3.1 / 3.8 |
| swf | 91.6% | 18 (13) | 0.474 / 0.778 | 2.29 / 7.76 | 1.218 / 6.309 | 0.151 | 55.5 | 47 | 3 / 4772 | 0.28 / 1.16 | 868.8 / 2293.6 |
| swf_op | 91.6% | 18 (13) | 0.474 / 0.778 | 2.29 / 7.76 | 1.218 / 6.303 | 0.151 | 55.5 | 47 | 3 / 4772 | 0.28 / 1.18 | 982.5 / 2472.6 |

| path | dropout: hidden exposures with output | vs withheld solve mm p50/p95 | ° p50/p95 | hold-last for the rest mm p50/p95 | corrupt: followed wrong lock | max excursion mm | recovery after burst ms p50/max |
|---|---|---|---|---|---|---|---|
| optical | 0.0% | – | – | 31.7 / 230.7 (n=585) | 107 / 107 | 80.0 | 17 / 1000 |
| ekf | 89.1% | 3.8 / 37.1 | 0.45 / 1.65 | 39.9 / 454.3 (n=64) | 3 / 107 | 109.8 | 17 / 100 |
| swf | 89.1% | 3.9 / 22.7 | 0.46 / 1.70 | 36.8 / 484.4 (n=64) | 0 / 107 | 33.6 | 17 / 33 |
| swf_op | 89.1% | 3.8 / 23.3 | 0.46 / 1.69 | 36.8 / 481.5 (n=64) | 0 / 107 | 33.6 | 17 / 33 |

The imu-capture right controller has only one static window, so its jitter figure is one sample.

**SWF counters, nominal:**

| recording, controller | accepted | gate rejections (reprojection / position) | resets: gap / rejections / epoch / no IMU | marginalisations |
|---|---|---|---|---|
| imu-capture, left | 3580 | 33 / 3 (+3 post-solve) | 6 / 8 / 4 / 0 | 3550 |
| imu-capture, right | 1930 | 48 / 15 | 8 / 15 / 3 / 0 | 1884 |
| filter-live, left | 1957 | 14 / 0 | 11 / 2 / 4 / 0 | 1939 |
| filter-live, right | 3942 | 13 / 6 | 5 / 4 / 3 / 46 | 3918 |

EKF over the same four runs: 67, 80, 58 and 33 rejections; 26, 48, 24 and 16 re-initialisations.

### Sensitivity (imu-capture; left / right)

| variant | hidden-gap mm p95 | hidden-gap ° p95 | gyro vs output ° p95 | µs p50 / p95 |
|---|---|---|---|---|
| defaults (`swf`) | 12.0 / 15.4 | 2.61 / 1.09 | 0.89 / 1.25 | 1024 / 2337 |
| upstream CV1 noise and 0.16 px blobs (`FUSION_UPSTREAM_NOISE=1`) | 16.2 / 36.0 | 2.15 / 2.41 | 0.96 / 1.52 | 1410 / 2431 |
| window 6 instead of 3 (`FUSION_WINDOW=6`) | 12.1 / 14.9 | 2.62 / 1.07 | 0.91 / 1.25 | 1733 / 8084 |
| solve the IMU extrinsic, as upstream (`FUSION_OPTIMISE_EXTRINSICS=1`) | 28.8 / 17.9 | 8.75 / 1.89 | 1.54 / 1.75 | 1138 / 2656 |

Before the extrinsic was fixed and the gate became reprojection-based, the left controller's `swf` output was 21.7° from
the optical at p99, and the output-versus-gyro residual was 5.0° at p95. With the final defaults they are 5.0° and
0.9°.

### What the data show

- **Both fused paths win clearly over raw optical**, and the gain is about equal between them. Coverage rises by 11–38
  percentage points. Through 300 ms gaps the IMU keeps 86–94% of exposures valid, at 3–4 mm p50, against holding the
  last pose (8–53 mm p50). Wrong locks stop reaching the output.
- **SWF over EKF.** Reprojection stays close to the optical (the EKF's p95 grows to 1.3–1.7 px on the right controller).
  Jumps and |Δv| p95 are lower. Position p95 through gaps is lower on three of four controllers. A wrong lock reached
  the output on at most one exposure per run (EKF: up to four), and the excursion from nominal was lower where the EKF
  followed one (33.6 against 109.8 mm; 92 against 226 mm). These differences are modest. Some come from the gate added
  here, and in principle the EKF could use a reprojection-based gate too.
- **Where the SWF is not better:** static jitter (equal), orientation through gaps (no better), availability (equal to
  0.7 points worse), and cost (about 300×).
- **Wrong optical locks from the open-loop frontend** (e.g. 620 mm and 104° from the gyro-propagated pose, then back
  within 100 ms) cause most rejections in both backends. Streaks of five or more produce re-initialisations onto the
  wrong pose in both. This is a frontend problem (M2 registration), which live M3's tentative confirmation mitigates.
- **Timing.** The best IMU-to-camera shift is 0–3 ms. The fused paths' gyro-versus-output residual (0.2–0.3° p50 over
  100 ms) is below the optical's (0.4–0.5°), which is partly circular. The optical's own p95 of 1.4–6° is mostly
  frames where M1 orientation is weakly constrained.

## Reproduction

```sh
cmake --build build-sense-rel --target constellation_replay tests_pssense_fusion
./build-sense-rel/tests/tests_pssense_fusion
S=~/Code/psvr2-datasets/sessions/20260926-010135-imu-capture   # or 20260926-014505-filter-live
OUT=~/Code/psvr2-datasets/experiments/20260928-upstream-fusion-eval
./build-sense-rel/src/xrt/tracking/constellation/constellation_replay $S/constellation.ctd \
  --fusion-compare --run-log $S/run.log --fusion-out $OUT/$(basename $S) > $OUT/$(basename $S).log
```

`build-sense-rel` needs Ceres ≥ 2.1 (`brew install ceres-solver`; the CMake cache shows
`XRT_HAVE_CONSTELLATION_FUSION_EVAL=ON`). The per-exposure outputs of every path are in `$OUT/<session>-<scenario>.csv`.

Tool-only environment overrides:

- `FUSION_UPSTREAM_NOISE=1`
- `FUSION_BLOB_SIGMA`
- `FUSION_GYRO_NOISE`
- `FUSION_ACCEL_NOISE`
- `FUSION_WINDOW`
- `FUSION_OPTIMISE_EXTRINSICS=1`
- `FUSION_GATE_PX`
- `FUSION_GYRO_BIAS_ANCHOR`
- `FUSION_MAX_GYRO_BIAS`
- `FUSION_NO_GATE=1`
- `FUSION_TRACE=PATH` (every exposure's update result)

A full run of all scenarios takes about 38 s per recording.

**Regression checks:**

- `--m1 --csv` (excluding the timing column) and `--tracker-csv` are byte-identical to the base build (`c1ca162de`) on
  `233900`, `002110`, `010135` and `014505`.
- `tests_imu_optical_filter`, `tests_joint_pose_solver`, `tests_led_phase_bootstrap` and `tests_pose_metrics` pass.
- The AGENTS.md driver-only `drv_psvr2` recipe builds.
- A configuration without Ceres builds, and its `--fusion-compare` exits with an explanation.

## Known limitations

- **No ground truth.** Every metric measures consistency: with the frontend's blobs, with the withheld solves (which
  can be wrong themselves), or with the gyro (partly circular for fused paths).
- **The frontend is open loop.** It is `replay_m1`: no fused prior and no tentative confirmation, unlike live M3. On
  `010135`, M1 made 3638 / 2020 solves and the M3 tracker replay pushed 3605 / 1693. It is the same for every path,
  which is what makes the comparison valid, but absolute numbers differ from live M3.
- **The HMD/SLAM pose is taken as exact.** Its uncertainty and latency are not modelled, and stale poses cannot be
  detected in these recordings.
- **Only two recordings have IMU data** (90 s each). The right controller in `010135`, and the left in `014505`, are
  mostly unsolved (38%).
- **The IMU runs at 66 Hz**, with inflated noise densities, and the lever arm is ignored (the EKF has the same
  limitations).
- **Epoch times are approximate:** they are bracketed from `run.log` to about one frame.
- **Costs are single-threaded offline times on an Apple M5.**
- **MR 2940's per-camera solver was not run here.** It now runs in upstream's own tree
  (`constellation_upstream_replay`), and its poses are compared with M1's by `--compare-frontend`, or fed to these
  fusion paths by `--fusion-frontend`. See `doc/macos-pssense-mr2940-frontend-evaluation.md`.

## Recommendation in detail

**Adopt selected components only:**

1. **Keep** M3 and `t_imu_optical_filter` as the live path. On this data the SWF gives no availability gain, gives
   mixed accuracy gains, and costs about 300× more plus a Ceres dependency.
2. **Take** upstream's SO(3) exp/log headers and IMU preintegration (with bias Jacobians), now on this branch. They are
   the natural base for any IMU-aided frontend prior or a future smoother.
3. **Try in the EKF next.** These are the two SWF advantages that do not need a smoother:
   - a reprojection-based gate: score the IMU-predicted orientation against the blobs, not against the M1 pose;
   - optionally, a reprojection measurement update from M1's correspondences, in place of the 6-DoF pose measurement.
     This is where the tighter reprojection tail and the fewer wrong-lock excursions came from.
4. **Fix at the frontend what neither backend can:**
   - wrong M2 registrations that M1 then tracks;
   - re-initialisation onto a wrong lock after five rejections, e.g. by requiring agreement with the IMU-propagated
     orientation across the streak, or M3-style confirmation in the replay.
5. **Revisit the SWF only if** a recording format with HMD pose age and raw IMU timing (above) shows timing or HMD-pose
   error dominating, or the Sense IMU can be read faster than one sample per report.

MR 3015's rig calibration and BG blink-code LED sync are not applicable to this hardware setup as it stands. Neither
was evaluated.
