# PS Sense optical (6DoF) tracking on macOS

Working branch: `macos-pssense-6dof`, based on `macos-psvr2-camera-calibration`.
Background and earlier results: `doc/psvr2-camera-calibration.md` and `doc/pssense-led-blink-waveform.md`.

## Where things stand (2026-09-24)

- Camera plumbing, the visible-mode fisheye rig and the direct mode-4 ChArUco solve are described in the
  calibration doc. Its numbers stand, but **most of their inputs and outputs are lost**. macOS purges `/tmp`,
  and every capture, log and calibration JSON from 8–15 September was recorded there. That includes
  `char-mode-12`, `sense-validation`, the force-IR runs, the phase sweep, the reviewed rig and the aligned
  ChArUco candidate. Partial copies were recovered to `~/Code/psvr2-datasets/old`:
  - `psvr2-mode4-tracking-calibration-provisional.json`: the pre-ChArUco provisional calibration. The CLI
    accepts it. It is fine for LED illumination work, but it is the calibration that could not bootstrap
    poses.
  - `psvr2-mode4-forced-joint-rig.{json,log}` and `psvr2-native-model-comparison.json`: diagnostics only.
- The original 21-pose mode-4 ChArUco capture (`char-mode-12`) was later recovered from a zip. It is unpacked
  at `~/Code/psvr2-datasets/calibration/20260913-char-mode-12/`, and the direct solve was re-run there on
  2026-09-24 (`psvr2-mode4-charuco-direct.json`, `direct-solve.log`). Fisheye RMS was 0.44 / 0.45 / 0.45 /
  0.35 px, and the upper cameras' leave-one-pose-out RMS was 0.47 / 0.38 px. The joint rig reached 0.50 px
  RMS (median 0.30, p95 0.93). The lower baseline is 81.0 mm and the lower-to-upper baselines are 75.3 and
  74.9 mm.
- The reviewed visible-mode rig is still lost, so the candidate can't be aligned into visible camera0 as on
  13 September. `scripts/psvr2_tracking_charuco_native_origin.py` builds a `psvr2-constellation` calibration
  with **native mode-4 camera0 as the tracking origin** instead:
  `psvr2-mode4-charuco-native-origin-candidate.json`. The geometry between cameras, and so every
  camera-relative controller pose, matches the aligned candidate; only the origin's placement relative to
  the headset differs. That placement was never trusted anyway. Sense held-out validation must be repeated,
  because the `sense-validation` captures were lost. Use this candidate, not the provisional file, for pose
  work. Record everything under `~/Code/psvr2-datasets`, never `/tmp`.
- The immediate blocker for live tracking is illumination. The controller LEDs are usually dark when the
  cameras expose (see "Static LED phase sweep" in the calibration doc).

## Plan

1. **Illumination (this branch).** Lock the LED phase from raw blob counts, then check that it holds
   (below).
2. **Recapture the mode-4 calibration.** Run the direct ChArUco capture and solve again, save it under
   `psvr2-datasets/calibration/`, and validate it on held-out Sense poses. Explain or eliminate the 15.5°
   lower-camera disagreement with the visible rig.
3. **Solve jointly across cameras.** Replace per-camera PnP followed by averaging with one pose optimised
   against every camera's blobs in the same exposure. Bootstrap by triangulating the lower and upper stereo
   pairs, with an IMU gravity prior.
4. **Filter.** Use an EKF/UKF with IMU propagation, taking the optical pose as a measurement, in place of the
   jump, stale and agreement gates.
5. **Integrate.** Expose 6DoF through OpenXR only once recorded sessions pass the acceptance numbers.

Every live run should be recorded and scored with the session tools, so runs can be compared.

## LED phase bootstrap (`PSSENSE_LED_BOOTSTRAP=1`)

The previous search was the pose-driven `t_led_sync_refinement`. It only counts a frame as lit once a pose
is solved, which now means a multi-camera fused pose. Its search range on macOS (programmed pulse centre
3.8–11.8 ms after the projected exposure) also misses the lit window seen in the static sweep, at about
2.5–3.0 ms.

`t_led_phase_bootstrap` (`src/xrt/tracking/constellation/`) instead scores each commanded phase by the raw
blob count every camera reports. The constellation tracker reports blob counts for every frame through the
optional `push_camera_blob_count` device callback.

0. **Dark baseline.** The LEDs are held off for one step (20 exposures) and each camera's highest blob count
   is recorded. A frame counts as lit only with at least 3 blobs above its camera's baseline.
1. **Wide scan.** A 2.1 ms pulse is stepped in 1 ms steps across the whole camera period (17 steps). Each
   step waits 8 exposures to settle, measures 8, then allows 4 of grace for late reports. That is 20
   exposures, about 0.33 s per step.
2. The best step (circularly smoothed) must score at least 1.0 and beat the median step by 0.75. The score
   is the sum over cameras of the fraction of lit frames. Otherwise the scan fails and
   the controller idles for 60 exposures.
3. **Narrow scan.** A 450 µs pulse is stepped in 250 µs steps from 1.5 ms before to 1.5 ms after the best
   wide pulse (21 steps). The lit run is the contiguous set of steps at or above half the peak score.
4. **Lock.** The pulse is centred in the lit run, 1.0 ms wide by default (`PSSENSE_LED_BOOTSTRAP_LOCK_PERIOD_ID`,
   50 µs per id, default 20). If no camera sees at least 3 blobs for 300 exposures (~5 s), it rescans.

A full bootstrap takes about 13.5 s. Only one controller scans at a time, because blob counts cannot tell
the controllers apart. The other controller holds its LEDs off, and its own state is frozen, until the scan
finishes. The fixed macOS `PSSENSE_TIMING_FUDGE_100US` (3.6 ms) is still added, but it doesn't matter
because the scan covers the whole period. With the variable unset, behaviour is unchanged.

Log lines use the form `LED_BOOTSTRAP side=L event=...`. The events are `baseline`, `scan_start`, `step`,
`wide_result`, `locked`, `locked_status` (every 300 exposures), `scan_failed` and `lost`. The
`psvr2-constellation` CSV gains the columns `led_bootstrap_state` (0 idle, 1 wide, 2 narrow, 3 locked, 4 baseline),
`led_bootstrap_fudge_us`, `led_bootstrap_pulse_us`, `led_bootstrap_scans` and `led_bootstrap_locks`.

Unit tests: `tests/tests_led_phase_bootstrap.cpp` simulates a controller with unknown latency, including
across the period wrap, partial camera visibility, a constantly lit background, loss and relock.

### Hardware test

Wake only the left controller for the first run. Hold it still, ring facing the headset, where all four
cameras can see it:

```sh
cmake --build build-sense --target cli
scripts/psvr2_sense_session.sh bootstrap-static-left \
  ~/Code/psvr2-datasets/calibration/20260913-char-mode-12/psvr2-mode4-charuco-native-origin-candidate.json 45 \
  "left only, static, ring facing headset"
```

Then repeat with slow movement between held positions, and then with both controllers awake.

Acceptance, judged from `score.txt`. The LED bootstrap does not use the calibration at all; pose numbers are
a bonus at this stage:

- the first lock arrives within about 15 s;
- the wide scan shows one clear peak, and the narrow scan's lit window is roughly 0.5–1.5 ms wide;
- the locked lit fraction in `locked_status`, and the captured-frame lit fraction while locked, stay above
  90% for a stationary controller in view;
- the lock position (`lock_fudge_us`) is consistent across runs and restarts. If it moves between runs, the
  clock mapping is the next thing to look at.

If the wide scan never finds a peak, check `PSSENSE_FORCE_IR=1` at the same placement. If that doesn't
show a lit ring either, the problem is framing, not timing.

### Hardware results

**2026-09-24, `sessions/20260924-174601-bootstrap-static-left`** (left only, static, ring facing headset,
45 s, commit `0a2cd3902`). Failed acceptance, but the cause is the clock mapping, not the bootstrap.

- First lock at 12.9 s. The wide scan's best step was at 0–1 ms, and the narrow scan found a lit window at
  2250–2500 µs (lock fudge 2100 µs). The first 5 s of lock were lit 808/1196 camera reports (68%), then
  dropped to 0 lit and were declared lost at 22.2 s. The rescan locked at fudge 15725 µs (about 3 ms
  earlier), lit only 117/1200, and was lost again at 42.9 s. Median locked lit fraction was 0.10. All four
  cameras saw the ring whenever the pulse was in phase (captured frames: 39–41 of 174 lit per camera while
  locked). Placement was fine.
- Cause: the scheduling clock offset (controller minus host, `controller_now - now` in `LED_SCHEDULE`)
  rose linearly by 5460 µs over the first 32.7 s (about 165 µs/s), then went flat. Every per-frame
  step during the ramp is an exact multiple of 2.5 µs. That's the `±2.5 µs per sample` slew clamp in
  `pssense_add_clock_offset_sample` catching up after the first HID report arrived about 5.4 ms late,
  with about 66 input reports/s. It isn't real clock drift.
- The lit position is constant in *fudge + offset* coordinates: 2375 + 1830 = 4205 µs at 11 s and
  15750 + 5125 − 16683 = 4192 µs at 31 s. So the bootstrap measured the right phase, the ramp carried the
  lock away, and the second narrow scan ran during the tail of the ramp. That makes it ragged and biased.
- Fix: `PSSENSE_CLOCK_OFFSET_SNAP_US` (default 0, off) lets the smoothed offset jump to the max-tracked
  offset when the gap exceeds the threshold, logging `CLOCK_OFFSET side=L event=snap`.
  `psvr2_sense_session.sh` sets it to 250. `score.txt` now reports `clock offset: creep`, the settle time
  and the snap count. A creep of more than ~100 µs after the first lock invalidates the run.
- Also seen: occasional partially lit steps away from the main window (for example narrow 3000–3250 µs at
  12 s, and 14000 µs at 29 s). These may be ramp artefacts; recheck them once the offset is stable.

**2026-09-24, `sessions/20260924-175254-bootstrap-static-left`** (same placement, commit `af05d7ed2`,
`PSSENSE_CLOCK_OFFSET_SNAP_US=250`). The snap works: two snaps at start-up (507 µs and 380 µs), and the offset
then stays within ±150 µs. Close, but not yet a pass.

- The wide scan had one clean peak at 12–14 ms (8/8, 8/8, 7/8). The narrow lit window was 14500–14750 µs
  (700 µs), with the lock at fudge 14350 µs at 12.9 s. **It held for the whole run with no rescans.**
  Lit reports per 5 s window were 78, 70, 99, 74, 80 and 99%, with a median of 0.79. Captured frames while
  locked were about 85% lit (274/321 on camera 0). Position was tracked 77.9% of the time, against 26.4%
  before; median pose age was 39 ms. There were 1743 two-camera fused poses and static jitter was 0.85 mm
  median.
- The dark frames are shared by all four cameras and come in episodes, including a 1 s blackout at 19 s.
  They follow the **host-time exposure timestamps**, not the controller clock. `raw_exposure` in
  `LED_SCHEDULE` has a residual of std 0.9 ms against the camera grid, with excursions of +1 to +5 ms, and
  its per-second median wanders by ±600 µs. The ~100% lit stretches (23–26 s, 38–44 s) are exactly the
  seconds where that residual is tight. The captured VTS exposure times sit on a 16683.03 µs grid to
  0.4 µs, so the noise is entirely in the VTS→host mapping (`hw2mono_vts`, the exponential
  `m_clock_offset_a2b` fed by delayed libusb IMU observations).
- `PSVR2_ROBUST_CLOCK=1`, the existing minimum-delay `hw2mono_vts` filter already used for calibration
  capture, targets exactly this. `psvr2_sense_session.sh` now sets it by default. `score.txt` gains an
  `exposure timestamp residual` line: p5/median/p95 were −1081/−466/3077 µs for the first run and
  −718/−230/1532 µs for this one.

**2026-09-24, `sessions/20260924-175831-bootstrap-static-left`** (same placement, commit `adb03d70c`, snap 250 µs
plus `PSVR2_ROBUST_CLOCK=1`). **Passes the static acceptance criteria.**

- First lock at 12.9 s, with no losses and no rescans. The wide scan had one clean peak at 13–15 ms (3.5, 4.0,
  4.0; every other step 0.0 except 16000 at 0.5). The narrow lit window was 15250–16000 µs (1200 µs), locked
  at fudge 15350 µs. The narrow scan still has weak partial steps at its edges (14750 µs at 1.75,
  16500 µs at 2.07).
- Locked lit reports per 5 s window were 93, 99, 100, 100, 100 and 100%, median 1.00. Captured frames while
  locked were 319/320 lit on every camera.
- The exposure timestamp residual (p5/median/p95) fell from −718/−230/1532 µs to −348/−22/416 µs. Three
  controller offset snaps at start-up (3536, 304 and 293 µs); the offset then drifted −289 µs over the run
  without losing light.
- Poses (bonus): position tracked 79.2%, median pose age 38 ms, static jitter 0.52 mm median (0.78 p95).
  There were 2047 fused poses, all two-camera, because `PSSENSE_CONSTELLATION_LIVE_RECOVERY` was unset.
  107 slow and 25 fast tracker sample drops.
- The lock fudge was 14350 µs in the previous run and 15350 µs here. Robust clock changes the absolute
  `hw2mono_vts` mapping (minimum-delay instead of exponential), so a shift between these two configurations
  is expected. Repeatability still has to be judged across restarts with the same configuration.

**2026-09-24, `sessions/20260924-180054-bootstrap-slow-left`** (60 s: still for 15 s, then slow moves between
held positions; commit `6f62ebf11`). The lock held (1 scan, 1 lock, 0 lost), but it was **placed about 1 ms
off-centre**, because a background source fooled the scoring.

- Cameras 0 and 2 could see bright window panes, which gave 3–7 compact blobs with the LEDs dark. Camera 2
  was "lit" in 56/56 wide-scan frames and in the idle frames. Every wide and narrow step therefore scored
  at least 2.0, and the narrow "half the peak" rule accepted the whole floor: lit window 12500–817 µs
  (5450 µs, `narrow_edge_unbounded`), lock at 14725 µs. The real plateau was 15750–16250 µs.
- Locked lit reports per 5 s window were 76, 85, 87, 79, 80, 51, 71, 91 and 78% (median 0.79). This is
  inflated by the two background cameras. Captured lit frames while locked: camera 1 at 355/469 and
  camera 3 at 195/468. The movement took the controller out of some views.
- The tracker dropped 3219 slow samples (about 100 in the static runs). Correspondence search can't keep
  up with a moving controller; this has to be addressed with the pose-solver work.
- Fix (bootstrap, still opt-in): every scan now starts with a **dark baseline step**. The LEDs are held off
  for one 20-exposure step (state 4, `baseline` in the CSV), and each camera's highest blob count is
  logged as `event=baseline blobs=...`. From then on a frame is lit only when it has at least 3 blobs above
  that camera's baseline. This applies to scan scoring, the locked lit fraction and loss detection alike.
  A scan now takes about 13.5 s. The baseline is fixed for the scan and the lock that follows; if the
  headset turns so the background changes, it is only re-measured on the next rescan.

**2026-09-24, `sessions/20260924-180557-bootstrap-slow-left`** (same placement with the windows in view, slow
moves, commit `8a87ece6b`). **The dark baseline works.** Illumination under movement is close to passing;
pose tracking is not.

- Baseline `1,0,8,0`. The wide scan had one clean peak (13000: 1.0, 14000 and 15000: 4.0, 16000: 3.5, all
  other steps ≤ 0.12). Narrow lit window 15750–16500 µs (1200 µs), centred at 16350 µs, locked at fudge
  15850 µs. That is where the plateau really was in the previous run. 1 scan, 1 lock, 0 lost.
- Locked lit reports per 5 s window were 69, 83, 90, 84, 64, 58, 77, 72 and 60% (median 0.72). The
  image-based per-second lit counts, with camera 2 measured against its baseline, show two causes:
  - Visibility: the controller left camera 3 for 12–18 s, 31–40 s and 53–57 s, and camera 2 for 21–26 s.
  - Timing (37–49 s): lit fell to 5–8/10 on every camera at once. The exposure timestamp residual
    swung by −0.7 to +1.5 ms within single seconds, and 749 schedules saw exposure ages over 30 ms
    (normally ~24 ms). The machine was loaded (below). `PSVR2_ROBUST_CLOCK` limits upward offset movement
    to 2.5 µs per IMU sample, but at 2 kHz that is 5 ms/s, so sustained USB delays still leak into the
    mapping. Fix: `PSVR2_ROBUST_CLOCK_MAX_PPM` (default 0, unchanged) caps upward movement at a clock-drift
    rate instead. The two clocks differ by ~20 ppm here (host fit 16683.42 µs vs VTS 16683.03 µs per
    frame). `psvr2_sense_session.sh` sets 200 ppm.
- **Pose tracking collapsed:** 26% of samples position-tracked, median pose age 4.5 s, 5240 slow-sample
  drops. From 31 s to the end, cameras 0–2 were lit in 5–10 of 10 frames per second, but the tracker
  produced almost no candidates (a handful per second at most) and no fused poses. Meanwhile the slow
  correspondence thread dropped 20–35 samples per second in each of its five slots. The same pattern, in
  a shorter form, appeared at 16–17 s and recovered at 18 s. Once the fast path loses the controller, the
  slow search can't keep up with a moving target and never re-acquires. This is the tracker, not
  illumination, and belongs with plan item 3 (joint multi-camera solve seeded by the IMU).

## Session tools

- `scripts/psvr2_sense_session.sh NAME CALIBRATION [DURATION] [NOTE]` records into
  `$PSVR2_DATASETS/sessions/<timestamp>-NAME/` (default `~/Code/psvr2-datasets`). Each session gets `run.log`,
  `poses.csv`, frames captured every sixth sequence, `env.txt`, `git.txt` and a copy of the calibration.
  The script scores the session and appends checksums to `SHA256SUMS`.
- `scripts/psvr2_sense_session_score.py SESSION [--json OUT]` reports:
  - the bootstrap timeline, with bar charts of the last wide and narrow scans;
  - locked lit fraction;
  - per-camera lit-frame fraction from captured images, split by bootstrap state;
  - candidates per camera, and fused poses by camera count;
  - reacquisitions, slow/fast sample drops, and optical-vs-aligned-IMU residuals;
  - position-tracked fraction, pose age, and static jitter.

It is tested by `tests/test_psvr2_sense_session_score.py`.

A full offline replay, feeding recorded frames, IMU and HMD poses back through the tracker, is not built
yet. It should follow once the calibration is recaptured, so pose-solver changes can be scored on identical
input.
