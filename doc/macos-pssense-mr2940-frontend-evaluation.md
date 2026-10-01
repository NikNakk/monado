# PS Sense: upstream's optical front end (MR 2940) against M1, and recording for evaluation

Branch `claude/pssense-mr2940-evaluation`, on top of `codex/pssense-upstream-fusion-evaluation`. The upstream side is
on `claude/pssense-upstream-frontend-replay`, which branches from `macos-upstream-sync-2026-10`. That branch is upstream
`main` with MR 2940 merged into this fork.

This note covers the optical front end. The fusion back ends are in `doc/macos-pssense-upstream-fusion-evaluation.md`.

## Status (1 October 2026)

- **The tooling is built and checked end to end on synthetic recordings with exact ground truth.** The upstream
  tracker runs on a recording in its own tree. Its poses are scored beside M1's by one evaluator.
- **No real recording has been run through it yet.** The recordings are on the Mac. The commands are under
  "Running it on the Mac recordings". Until those results are in, the earlier conclusion stands: our front end has
  similar accuracy at much lower cost. On synthetic data it also has better accuracy, but this has not been measured
  on real data.
- **The recording format now carries what this evaluation lacked** (packet 5 extension records, below). New
  recordings need no extra steps beyond the static markers, which are optional.

## Synthetic results

The recordings were made by `constellation_synth`:

- two Sense rings, four 508×508 KB4 cameras at 60 Hz;
- 0.2 px blob noise, 5% blob drops, merged blobs, three background lights;
- 3 s still periods every 10 s;
- IMU at 66 Hz.

Results are given as left / right controller.

`base` is seed 1, 20 s, speed 1.0:

| | M1 | upstream (MR 2940) |
|---|---|---|
| solved, % of exposures | 83.8 / 79.5 | 100 / 100 |
| position error vs truth, mm, p50 / p95 | 0.62 / 1.62, 0.61 / 1.59 | 1.79 / 5.42, 1.66 / 4.34 |
| orientation error vs truth, deg, p50 / p95 | 0.38 / 0.75, 0.36 / 0.80 | 0.97 / 2.50, 0.93 / 2.23 |
| wrong poses (> 30 mm or > 10°) | 0 / 0 | 0 / 2 (37° max) |
| evaluator reprojection, px, p50 | 0.264 / 0.263 | 0.334 / 0.318 |
| rotation vs gyro, deg, p50 / p95 | 0.26 / 0.71, 0.25 / 0.71 | 0.94 / 3.37, 0.84 / 2.65 |
| step while still (gyro), mm RMS | 1.12 / 1.32 | 4.80 / 3.18 |
| spread in annotated still intervals, mm | 0.75 / 1.27 | 2.94 / 2.14 |
| cost per exposure, µs, mean / p95 | 776 / 1774 | 7272 / 27080 |
| exposures costing more than the 16.7 ms interval | 0% | 17.3% |

`fast` is seed 2, 30 s, speed 1.5:

| | M1 | upstream (MR 2940) |
|---|---|---|
| solved, % of exposures | 85.0 / 84.8 | 99.9 / 99.8 |
| position error, mm, p50 / p95 | 0.59 / 1.58, 0.65 / 1.70 | 1.53 / 4.59, 1.67 / 4.85 |
| orientation error, deg, p50 / p95 | 0.36 / 0.73, 0.38 / 0.77 | 0.88 / 2.28, 0.89 / 2.24 |
| wrong poses | 1 (180 mm, 34°) / 0 | 2 (366 mm, 45°) / 0 |
| cost per exposure, µs, mean / p95 | 721 / 1664 | 9317 / 32654 |
| exposures costing more than 16.7 ms | 0% | 20.8% |

Costs are single-threaded on a 2.1 GHz Xeon. Both trees were built RelWithDebInfo against the same Ceres.

What this shows, on synthetic data only:

- **Upstream's poses are 2–3× less accurate.** Its position and orientation errors are larger, its rotation agrees
  less with the gyro, and it moves more while the controller is still. It solves each camera on its own, and the
  record keeps the camera with the most matched blobs. M1 solves all cameras jointly. It also uses the IMU orientation
  as a prior when tracking, which upstream's tracker does not. Upstream's own RMS (0.19 px) is lower than M1's (0.26
  px), but the common evaluator puts it at 0.32–0.33 px against M1's 0.26 px. One camera's fit is tighter than the
  pose really is.
- **Upstream solves more exposures.** M1 rejects 15–20% of exposures in which the pose is in fact correct. The cause
  is its coverage gate: at least 80% of the LEDs it predicts as visible must be matched. The synthetic merges and drops
  push those exposures to 0.70–0.79 coverage. With drops off, M1 still misses about 10%, from merges alone. Real
  recordings behave differently: after the coverage-margin fix, M1 solved about every lit exposure of `234059` (see
  `doc/pssense-optical-tracking.md`). How often merged blobs occur in real data is something the Mac recordings must
  show. If the gap holds there, the fix is to relax the coverage gate where the RMS is low. Upstream's approach is not
  needed for that.
- **Both front ends make rare wrong poses.** The consistency metrics did not flag M1's single wrong pose in `fast`
  (0 rotation jumps), but the evaluator's "unsupported" count did. Ground truth found it directly.
- **Upstream costs about 10× more on average and 20× more at p95.** In deterministic mode, 17–21% of exposures take
  longer than the frame interval. Live, upstream runs the slow correspondence search on a separate thread and skips
  frames while it is busy. Its live coverage would therefore be lower than the replay's, and its CPU use about the
  same.

The synthetic recordings model blob noise, drops, merges and background light. They do not model:

- LED timing or brightness;
- self-occlusion by the ring;
- the hand;
- calibration error;
- SLAM error.

They test the pipeline and give a first comparison. They are not a substitute for the recordings.

## How the comparison works

### Upstream side: `constellation_upstream_replay`

It runs in upstream's tree, because the two trees' constellation code shares symbol and type names and cannot be
linked together. It feeds a recording's camera frames through upstream's `t_constellation_tracker` in timestamp order,
and writes a records CSV (`# constellation frontend records v1`):

- `R` rows: every pose pushed, with its camera, matched blob count and RMS (OpenCV convention);
- `M` rows: the blob-to-LED matches behind each pose;
- `F` rows: the time spent on each camera frame.

Choices, made to give upstream every advantage the live tracker would have:

- **Deterministic, single-threaded mode.** Every frame gets the fast path and, if that fails, the slow search, so
  coverage is an upper bound.
- **Blob labels are carried between frames by blob id**, as `t_rift_blobwatch` does live. Recorded blob ids come from
  the same blobwatch, and recorded labels are stripped.
- **The tracking source of each controller is the history of upstream's own pushed poses**, as the Sense driver's
  constellation pose is. Upstream's tracker ignores IMU samples, so none are fed to it.
- **The Sense match parameters are upstream's** (`DEFAULT_MATCH_PARAMETERS`, 7 LEDs without a prior, 5 with one).
  `--min-leds-without-prior` and `--min-leds-with-prior` override them.

### Our side: `constellation_replay --compare-frontend`

`--compare-frontend NAME=RECORDS.csv` imports a records file. It may be given more than once. M1 always runs. Each
exposure keeps the pose of the camera with the most matched blobs, then the lowest RMS. Its correspondences are the
matches of every camera that produced a pose.

Every pose is then scored by the same evaluator, which uses nothing from either front end:

- **Coverage:** exposures solved, as a share of all exposures and of those any front end solved. With ground truth,
  also as a share of those where the true pose puts at least 4 LEDs on blobs.
- **Reprojection:** the device's visible LEDs are projected into every camera at the pose and paired one-to-one with
  the nearest blob within 3 px. The report gives inliers, their RMS, and the found/predicted ratio. A pose is
  "unsupported" when it has fewer than 4 inliers or under half the predicted LEDs are found.
- **Gyro consistency:** the rotation angle between consecutive poses (at most 60 ms apart) against the integrated gyro
  angle. The angle does not depend on the IMU-to-model or world alignment, so neither needs estimating. A residual
  above 10° counts as a rotation jump.
- **Position jumps:** misses of more than 30 mm against a constant-velocity prediction from the two previous poses.
- **Stillness:** pose-to-pose steps while the gyro reads under 0.05 rad/s for 50 ms either side. Also the spread of
  poses inside annotated static intervals, skipping 0.5 s at each end.
- **Ground truth**, when recorded: position and orientation error, and the count of poses more than 30 mm or 10° off.
- **Agreement:** the difference between front ends where both solved, and where only one did.
- **Cost:** front-end time per exposure, all devices, and the share of exposures over the exposure interval.

`--compare-out OUT.csv` writes one row per scored pose. `--fusion-compare --fusion-frontend NAME` runs the EKF and
sliding-window fusion on an imported front end's poses instead of M1's. This is checked on the synthetic recordings,
where both paths accept upstream's records. Fusion results on synthetic data are not reported here.

## Running it on the Mac recordings

Build both trees. Upstream's tracker needs Ceres (`brew install ceres-solver`).

```sh
git fetch origin claude/pssense-upstream-frontend-replay claude/pssense-mr2940-evaluation
git worktree add ../monado-upreplay origin/claude/pssense-upstream-frontend-replay
cmake -S ../monado-upreplay -B ../monado-upreplay/build-rel -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build ../monado-upreplay/build-rel --target constellation_upstream_replay
cmake --build build-sense-rel --target constellation_replay constellation_synth
```

Then, for each recording:

```sh
UP=../monado-upreplay/build-rel/src/xrt/tracking/constellation/constellation_upstream_replay
REPLAY=build-sense-rel/src/xrt/tracking/constellation/constellation_replay
OUT=~/Code/psvr2-datasets/experiments/20261001-mr2940-frontend
mkdir -p $OUT
for S in ~/Code/psvr2-datasets/sessions/{20260926-010135-imu-capture,20260926-014505-filter-live}; do
  N=$(basename $S)
  $UP $S/constellation.ctd $OUT/$N-upstream.csv > $OUT/$N-upstream.log 2>&1
  $REPLAY $S/constellation.ctd --compare-frontend upstream=$OUT/$N-upstream.csv \
    --compare-out $OUT/$N-compare.csv > $OUT/$N-compare.log
done
```

Any recording with a `constellation.ctd` works. Those without IMU packets have no gyro metrics. The upstream replay
has no `--calibration` override, so leave it off `$REPLAY` too. Both front ends must see the same camera poses.

To check the pipeline on the Mac first:

```sh
build-sense-rel/src/xrt/tracking/constellation/constellation_synth $OUT/synth.ctd --duration 20
```

Then run the two commands above on `$OUT/synth.ctd`. The results should be close to the `base` table.

**What would change the conclusion:**

- Upstream's poses on real data as accurate as M1's (the evaluator's RMS and the gyro residual are the
  ground-truth-free measures).
- M1 missing many exposures that upstream solves with supported poses ("only upstream" in the agreement line, with
  upstream's unsupported share low).

The first would mean the synthetic accuracy gap does not carry over. The second would point to M1's coverage gate.
Neither would justify upstream's cost.

## The dataset format

### Distortion model codes

The dataset wrote `t_camera_distortion_model` as its raw value. Upstream commit `9e06aa432` inserted
`T_DISTORTION_PINHOLE` at the front of that enum. As a result, a KB4 recording made here read as `RADTAN_14` in
upstream's tree. Upstream's tracker then ran about 20× slower and found under half the poses. The format now stores fixed codes in the
enum's original order:

| code | model |
|---|---|
| 0 | RADTAN_5 |
| 1 | RADTAN_8 |
| 2 | RADTAN_14 |
| 3 | KB4 |
| 4 | WMR |
| 5 | PINHOLE |
| 6 | RIFT_CV1 |

Every existing recording reads unchanged, and both trees now agree. Files written by upstream `main`'s own recorder
still use upstream's shifted values, and would read wrongly here. The same fix belongs upstream.

### Extension records (packet 5)

Packet 5 is `{u32 kind, u32 length, payload}`. A reader skips kinds it does not know, and ignores bytes past the end
of the fields it knows. Fields can therefore be appended to a kind without breaking older readers. Readers from before
packet 5 stop at the first one, after reading everything before it.

| kind | record | written by |
|---|---|---|
| 1 | session info: JSON with tool, git tag, calibration, duration, world frame, capture directory, notes, device ids, and every `PSVR2_`/`PSSENSE_`/`CONSTELLATION_` variable | `psvr2-constellation` at start |
| 2 | sync event `{device, host_ns, kind, value[3]}`: clock snap, gyro bias, LED lock, lost, scan, phase move | Sense driver |
| 3 | IMU timing `{device, host_ns, device_ns, clock_offset_ns, applied_gyro_bias[3]}`, beside each IMU sample | Sense driver |
| 4 | head pose `{timestamp_ns, flags, T_world_head (XR), source_ns, interpolated / exact}` at each exposure | `psvr2-constellation`, world frame only |
| 5 | ground truth `{device, timestamp_ns, T_world_device (XR), sigmas, flags}` | `constellation_synth`; a future fixture or mocap |
| 6 | annotation `{device, host_ns, text}` | `psvr2-constellation` (static markers) |

`constellation_replay DATASET.ctd` summarises all of these: the session JSON, counts per kind, head-pose age against
the newest SLAM pose, and the annotations. `tests_constellation_dataset` covers the round trip, unknown kinds and
truncation.

This replaces the "smallest compatible extension" proposed in the fusion evaluation. The sync events replace parsing
`run.log`. The IMU timing records hold the device clock and the bias the driver subtracted. The head poses record
their age.

## Recording for future evaluations

Use `scripts/psvr2_sense_session.sh` as before. For recordings meant for evaluation:

- **Record in world frame** (`PSVR2_CONSTELLATION_WORLD=1`). Head poses and their age are recorded only then.
- **Mark still intervals.** Press a controller's Create button when it is resting on something fixed, and again
  before picking it up. Each press toggles `static_begin` / `static_end` for that controller, and the CLI echoes it.
  The replay then scores spread and drift against a pose that is known to be constant. This is the closest thing to
  ground truth without a fixture. A few 5–10 s rests, near and far, at different orientations, are enough.
- **Describe the session.** The script's NOTE argument now goes into the recording's session record too (through
  `PSVR2_CONSTELLATION_NOTES`), so a `.ctd` copied elsewhere still says what was done.
- **Include the hard cases on purpose:**
  - both controllers close together, which causes merged blobs;
  - fast rotations;
  - controllers far from the headset;
  - a background light in view.

  These are where the front ends differ.
- **Keep runs short and separate.** Several 30–60 s sessions are easier to compare than one long one.

## Commits

On `claude/pssense-mr2940-evaluation`:

- `t/constellation: extension records for timing, head pose and ground truth`
- `t/constellation: store distortion models as fixed dataset codes`
- `t/constellation: synthetic dataset generator with ground truth`
- `t/constellation: score optical front ends side by side in the replay`

On `claude/pssense-upstream-frontend-replay`:

- `t/constellation: replay datasets through the tracker for front-end comparison`. It also brings over the dataset
  format, with the extension records and the fixed codes.
