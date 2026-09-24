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

0. **Dark baseline.** The LEDs are held off for 24 exposures to let any controller that was just lit go dark,
   then each camera's median blob count over 8 exposures is recorded. A frame counts as lit only with at least 3 blobs above its camera's baseline.
1. **Wide scan.** A 2.1 ms pulse is stepped in 1 ms steps across the whole camera period (17 steps). Each
   step waits 8 exposures to settle, measures 8, then allows 4 of grace for late reports. That is 20
   exposures, about 0.33 s per step.
2. The best step (circularly smoothed) must score at least 1.0 and beat the median step by 0.75. The score
   is the sum over cameras of the fraction of lit frames. Otherwise the scan fails and
   the controller idles for 60 exposures, doubling per consecutive failure up to 600 (10 s).
3. **Narrow scan.** A 450 µs pulse is stepped in 250 µs steps from 1.5 ms before to 1.5 ms after the best
   wide pulse (21 steps). The lit run is the contiguous set of steps at or above half the peak score.
4. **Lock.** The pulse is centred in the lit run, 1.0 ms wide by default (`PSSENSE_LED_BOOTSTRAP_LOCK_PERIOD_ID`,
   50 µs per id, default 20). If no camera sees at least 3 blobs for 300 exposures (~5 s), it rescans.

A full bootstrap takes about 14 s. Only one controller scans at a time, because blob counts cannot tell
the controllers apart. The other controller holds its LEDs off, and its own state is frozen, until the scan
finishes. The fixed macOS `PSSENSE_TIMING_FUDGE_100US` (3.6 ms) is still added, but it doesn't matter
because the scan covers the whole period. With the variable unset, behaviour is unchanged.

Log lines use the form `LED_BOOTSTRAP side=L event=...`. The events are `baseline`, `scan_start`, `step`,
`wide_result`, `locked`, `locked_status` (every 300 exposures), `scan_failed` and `lost`. The
`PSSENSE_LED_BOOTSTRAP_TRACK=1` (experimental) adds closed-loop phase tracking while locked. Every 120
exposures (`PSSENSE_LED_BOOTSTRAP_TRACK_FRAMES`) the controller takes the scan token, measures the mean blob count
at its lock (12 exposures), then with the pulse moved earlier and later by 60% of half the measured lit span (each
settling 24 exposures, then measuring 8). The normalised imbalance (late − early) / ring size at lock time moves
the lock towards the brighter side (gain 1.0, at most 400 µs, deadband 0.1). Blob counts rather than the lit test
are compared, so another controller's steady light cancels out. A cycle takes about 3.4 s. Log lines are
`event=track result=moved|centred|ring_too_small`. The probe offset is capped at 300 µs, the gain is 1.5 and the
dead band is 0.2 (see "M3" below for why), and `score.txt` summarises probes, moves and the net shift. In
the unit simulator, with the latency drifting 60 µs/s for 50 s (3 ms in all, 1.5× the worst drift seen on hardware),
tracking keeps 91–92% of frames lit against 46–47% open loop. With no drift it stays put at 100% lit.

`PSSENSE_LED_BOOTSTRAP_FIRST=L|R` (diagnostic) lets only the named side start the first scan; the other waits
until it has locked (or 1200 exposures, logging `event=first_wait_timeout`).

`PSSENSE_LED_BOOTSTRAP_KEEP_LOCK=1` (experimental) keeps a locked controller lit while another scans, instead of
yielding. Its steady light is absorbed into the scanning controller's dark baseline. A controller without a
lock still stays dark. (An earlier empty-blink-mask yield did not darken a lit controller; see the hardware
results.) The
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

> **Load caveat (reported by the user):** a large compilation was running in the background during every session
> from the evening of 24 September, i.e. `20260924-224609-bootstrap-slow-left` onwards (22:46–23:19: the drift-capped
> slow-left run, all two-controller runs and the right-only run). Their host timing figures (exposure timestamp
> residuals and ages, controller clock creep and snaps, slow-sample drops) include that load. That probably
> includes the 1 ms/s clock excursion at 47–60 s in `231910`. The illumination passes in those runs held despite
> it. Whether the afternoon sessions (17:46–18:05) had background load is unknown.
>
> The logs can't separate the two cases. Late exposures (schedule age over 30 ms) track the tracker's own slow-sample
> drops, not the background build: `224609` (build running, 1504 drops) had 0.1% late with p99 26 ms, while
> `180557` (afternoon, 5240 drops) had 22.5% late with p99 110 ms and `231910` (build, 9756 drops) 9.5% with p99
> 47 ms. **The tracker's own CPU use is the largest measured timing disturbance**, so bounding its cost (plan item
> 3) is also the main timing-robustness fix. Record future runs without background builds
> unless the run is a deliberate stress test, and note the load in the session note.
>
> `20260924-175814-bootstrap-static-left` is a failed start (no Sense controller connected, exit status 1, no data);
> `175831` is the rerun.

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

**2026-09-24 22:46, `sessions/20260924-224609-bootstrap-slow-left`** (slow moves, commit `dc266c4ee`,
`PSVR2_ROBUST_CLOCK_MAX_PPM=200`). **Illumination under slow movement passes.** Not a controlled comparison
with the 18:05 run: it was evening (baseline `3,1,5,1`, window panes dimmer) and the moves differed.

- 1 scan, 1 lock, 0 lost, first lock at 13.2 s. The wide scan had one peak (13000–15000 µs; every other
  step 0 except 16000 at 0.38). Narrow lit window 15000–16250 µs (reported 1700 µs; camera 3 couldn't see
  the controller during the narrow scan, which flattens the plateau at 3.0), locked at fudge 15350 µs.
- Locked lit reports per 5 s window were 85, 98, 89, 95, 90, 89, 88, 99 and 92% (median 0.90). Captured
  frames lit while locked: 466/467 on cameras 0–2, and 384/467 on camera 3, which is visibility.
- Exposure timestamp residual std fell to 190 µs, with p99 +159 µs (18:05 run: std 372 µs, p99
  +1159 µs); p5/median/p95 were −376/89/138 µs. Schedules projected 5 periods forward, apart from 3 frames
  at 6 periods (the 18:05 run needed up to 12). The remaining negative tail (p1 −755 µs) is the minimum
  filter stepping down.
- Poses: position tracked 83.6% (18:05 run: 26.4%), median pose age 39 ms, 2778 two-camera fused poses,
  231 disagreements, 12 jumps, 1504 slow-sample drops (18:05 run: 5240). Some candidates still flip about
  80° (for example camera 0 at `imu_aligned_delta_deg=78`) and get rejected by the pair-agreement gate. This
  is a correspondence ambiguity for the joint solver to settle.
- Lock positions so far with the robust clock (fudge µs): 15350 (static), 14725 (background-biased,
  discard), 15850 and 15350. That is within ±250 µs of 15600 across four restarts, well inside the 1 ms
  lock pulse. The two-controller and dedicated restart runs are still to come.

**2026-09-24 22:48, `sessions/20260924-224851-bootstrap-both`** (both controllers, 30 s still, then slow
moves; commit `320ed19be`). **Left passes; right never locked**, and its retries starved the left.

- Left: baseline `3,1,5,1`, one clean wide peak (14000–16000 µs at 3.0), narrow window 15750–66 µs
  (1450 µs, across the period wrap), locked at fudge 15975 µs at 13.2 s. Locked lit 1192/1196 whenever it
  was allowed to be lit. The exposure timestamp residual was p5/p95 −30/+31 µs, the tightest so far.
- Right: 9 scans, 0 locks (7 `wide_peak_below_minimum`, 1 `wide_peak_not_distinct`). Its baselines were
  `11,10,11,1`, then 17–21 blobs per camera on the next scans. The right controller really was lit during
  its scans (170–237 candidates/s at 35–49 s), but nothing could reach baseline + 3.
- Cause: the left controller yields correctly. It produced 0 candidates in every second where it was only
  sent `LED_ALL_OFF`. But its light persists **90–256 ms** after the first off command (latest left
  candidate exposure after the off was sent, per handover), from the ~58 ms look-ahead LED schedule plus
  Bluetooth latency. The baseline settled for only 8 exposures (133 ms) and took the per-camera maximum, so
  one leaked frame poisoned it. The 1 s failed-scan backoff then let the right retry every ~7 s, keeping the
  left dark ~85% of the time (left position tracked 16.2%).
- Fixes (bootstrap, opt-in):
  - The baseline now settles for 24 exposures (400 ms, `baseline_settle_frames`) and takes the per-camera
    **median**.
  - After consecutive failed scans the backoff doubles (60, 120, 240, 480, then capped at 600 exposures,
    10 s) and resets on a lock.
  - A scan now takes about 14 s.
- The same 90–256 ms off-latency probably exceeds the 8-exposure settle between *scan steps* too, and may
  explain the weak partial steps at the edges of some narrow scans. Not changed yet; check the step pattern
  before lengthening the scan.

**2026-09-24 22:55, `sessions/20260924-225515-bootstrap-both`** (both; full ring held still, then normal grip
and slow moves after ~30 s; commit `76601136a`). **Invalid as a bootstrap test: the right controller ignored
its LED commands.**

- The right Sense emitted continuously from t=0 to 47.75 s (190–240 candidates/s, i.e. every frame) even
  though it was sent `LED_ALL_OFF` (phase 5) most of that time. Its light was in every baseline: left
  `11,9,11,8`, right `10,9,10,8` and so on, against the `3,1,5,1` window-only background. The left still
  locked (fudge 15975 µs, the same as the previous run), but its locked lit fraction was 0.74. The right
  failed 3 wide scans and then locked on a bogus flat window (12500–15500 µs, 3450 µs).
- The state began in the *previous* run (`224851`) at 34.0 s, on the right controller's first wide-scan
  command after a baseline (phase 1, `period_id` 42, fudge 0). It persisted through all later commands and
  across the monado restart. It ended at 47.75 s here, on a narrow-scan command (phase 1, `period_id` 9,
  fudge 13750 µs). Its output reports were delivered normally (about 3,900 per run, no `SetReport` failures,
  sequence number advancing). The left controller received the same command types and always obeyed
  (0 candidates while off).
- The user noticed the right controller's **visible status LED was off**. The driver never sets `flag2`,
  `STATUS_LED_SET_ENABLE` or `status_led_enable`, so this is the controller's own state, probably the same
  abnormal firmware mode. Cause unknown.
- After ~48 s neither controller produced many candidates while commanded lit (slow-sample drops 6776).
  That's the same tracker saturation seen in the slow-movement run, made worse by the grip change.
- The scorer now reports `candidates while commanded off > 400 ms` per side and flags more than 20:
  `224851` R 665 in 11 s, `225515` R 4731 in 25 s, every left run 0–1. A flagged run's baselines and scans
  must not be used.

**2026-09-24 23:00, `sessions/20260924-230002-bootstrap-both-static`** (both still, full ring; right controller
power-cycled first; commit `3c1e722ac`). **The right-controller fault reproduced.**

- Left: baseline `1,1,4,1`, locked at 13.4 s at fudge 15475 µs (window 1450 µs), 1.00 lit while allowed.
  Lock positions so far: 15350, 15850, 15350, 15975, 15975, 15475 µs.
- Right: its first baseline was clean (`2,1,3,1`). Wide steps 1–2 (fudge 0 and 1000 µs) were dark as
  expected. **From step 3 (fudge 2000 µs, about 14.6 s) it was lit in every frame (about 38 blobs across four
  cameras) at every phase and under `LED_ALL_OFF`.** The scorer flagged 620 candidates while commanded off.
  Its later baselines were `10,9,9,10` and similar, so every scan failed.
- The user saw the right controller's **status LED go off at about 20–30 s**.
- The link stayed healthy: the right controller's input reports kept arriving every ~17 ms (clock sample age
  ≤ 31 ms throughout, the same as the left), and its output reports went out normally. Nothing in the
  commands changed at step 3 (phase 1, `period_id` 42, schedule 50–70 ms ahead, sequence number
  advancing). The left controller receives the same command types and has never done this.
- Working hypothesis: a controller-side fallback or fault mode (LEDs always on, status LED off). The
  trigger is unknown. The next run isolates the right controller alone.

**2026-09-24 23:02, `sessions/20260924-230238-bootstrap-static-right`** (right only, power-cycled, full ring,
still; commit `1ce3c7fff`). **The right controller passes on its own.**

- Baseline `1,0,3,1`. One peak (14000–16000 µs, wrapping into the 0 step at 2.88). Narrow window 1450 µs,
  locked at fudge 15725 µs at 13.5 s: the same as the left controller's locks. Locked lit reports median
  0.87 (the tracker's reports). Captured frames lit while locked: 314/315 on every camera. **0 candidates
  while commanded off**, and the user saw the status LED stay on throughout.
- So the always-on / status-LED-off fault needs both controllers running. It isn't a faulty right
  controller.
- Pose tracking failed despite good light: 77 candidates (76 from camera 2), 0 fused poses, 4860 slow-sample
  drops. Frames show the ring clearly (6 compact blobs on camera 0, 8+ on camera 2), but also two bright
  ceiling lamps in view (blob areas ~100 and ~1150 px). A tracker problem for plan item 3; the lamps may be
  feeding the correspondence search.

Illumination status after these runs: a single controller passes, static and under slow movement (left
static, left slow twice, right static). Lock positions across runs and controllers: 15350–15975 µs. With
both controllers, the handover sequence triggers a controller-side fault. That is the open illumination
issue.

**2026-09-24 23:07, `sessions/20260924-230720-bootstrap-both-yieldmask`** (both power-cycled, still, full ring,
`PSSENSE_LED_BOOTSTRAP_YIELD_MASK=1`, commit `87af35d88`). **An empty blink mask is not an off command.**

- Left scanned first. Baseline `5,3,7,2`. The wide scan had an unexpected floor (1.0–2.5 across 5000–13000 µs;
  previous both-static run: all 0), but it still locked at 15350 µs, with a noisy narrow window (4700 µs).
- While yielding with `masks=00000000` during the right controller's scan (13–26 s), the **left kept
  emitting** at full rate (~900 candidates per 5 s at x ≈ −0.03 m; the scorer flagged 2311 in 14 s).
  Positions confirm these were the left ring, not a mirror-image fit to the right ring (right at x ≈ +0.17 m).
- The right controller, with the left steadily lit, still **scanned cleanly**: baseline `8,9,10,2` (window
  background plus the left ring), wide peak 13000–15000 µs, narrow window 1200 µs, lock at 15600 µs, and 0
  candidates while off. The steady light of another controller is absorbed by the dark baseline.
- Unexplained: after the right controller locked (26.6 s), its locked lit fraction was only 0.12, with 8–14
  candidates per 5 s until 35 s and 138–160 afterwards. The two locks were 250 µs apart, so both controllers
  were pulsing in the same exposures.
- Both status LEDs stayed on (user). So far the always-lit/status-LED-off fault has appeared only in
  two-controller runs that yielded with `LED_ALL_OFF` (`224851`, `225515`, `230002`); it didn't appear in the
  right-only run or in this one.
- The empty-mask option is removed. It is replaced by `PSSENSE_LED_BOOTSTRAP_KEEP_LOCK=1`: a locked
  controller stays lit (and keeps tracking) while the other scans; a controller without a lock stays
  dark. This avoids switching a lit controller to `LED_ALL_OFF` and back.

**2026-09-24 23:11, `sessions/20260924-231119-bootstrap-both-keeplock`** (both power-cycled, still, full ring,
`PSSENSE_LED_BOOTSTRAP_KEEP_LOCK=1`, commit `ebf28f129`). **The handover works; the lock drifts.**

- Both controllers locked once with clean scans and **0 candidates while off** on either side. Left: baseline
  `2,2,3,1`, lock at 16408 µs. Right: baseline `5,8,8,2` (includes the left ring, which stayed lit), clean
  wide peak 14000–16000 µs, narrow window 1450 µs, lock at 16225 µs. No always-lit fault.
- Locked lit per 5 s window: left 81, 68 and 45%; right 42, 37 and 7%. With both lit, the right controller's
  figure is confounded (the left ring is in its baseline, so any dip in the left counts against the right),
  but the left's own decline is real.
- Cause of the decline: slow wander in the timing chain, not jitter. The exposure timestamp residual is steady
  within each second, but its per-second median went +636 µs (13 s) → +49 µs (33 s) → −448 µs (36 s).
  The controllers' clock offsets moved −253 µs (left) and +524 µs (right) over the run. Together that
  exceeds the ~±400 µs margin of a 1 ms lock pulse around a ~0.5 ms lit window.
- Pose tracking: almost no left candidates after 14 s despite the ring being lit (81% in its first
  locked window), with 50–200 slow-sample drops per second. Tracker saturation again, now with two rings.
- Conclusion: an open-loop lock (a fixed fudge after one scan) isn't robust to the millisecond-scale wander
  of the host clock mappings. The lock needs closed-loop phase tracking from brightness, and the tracker
  needs the plan item 3 work.

**2026-09-24 23:19, `sessions/20260924-231910-bootstrap-both-track`** (both power-cycled, still, full ring, 60 s,
`PSSENSE_LED_BOOTSTRAP_KEEP_LOCK=1 PSSENSE_LED_BOOTSTRAP_TRACK=1`, commit `35cc29382`). **Two-controller
illumination passes.** Both status LEDs stayed on.

- Left: baseline `0,0,3,0`, lock at 15600 µs (13.4 s). Locked lit reports per 5 s window: 1192/1196,
  1200/1200, 1196/1200 and 1200/1200 (median 1.00). **Captured frames lit while locked: 339/339 on every
  camera.** Tracking: 7 probes, 4 moves (+230, +88, −400, +400 µs), final lock 15918 µs.
- Right: baseline `12,5,11,3` (includes the left ring), clean wide peak and narrow window (1450 µs), lock at
  15725 µs (26.7 s). Tracking: 6 probes, 4 moves, net +146 µs. Its lit-test fraction (79, 74, 68, 62%) is
  confounded by the left ring in its baseline. Its probe reference windows averaged 11–13 blobs per camera,
  about both rings (ring sizes 4.6 and 5.7) plus background, so the ring was present most of the time.
- 0 candidates while commanded off on either side, and no always-lit fault.
- Host timing under load: the tracker dropped over 1000 slow samples per 5 s from 15 s, and exposure ages
  peaked at 94 ms. From 47 s **both controllers' clock offsets rose together by ~1 ms/s** (10.4 ms by the end,
  66 snaps of +250–670 µs). Receive times are stamped in the IOKit callback, so the samples are genuine: the
  controllers' device time ran fast relative to the host. The likely mechanism is the controllers
  disciplining their clocks from the host timestamps in our output reports, which became irregular under
  load; unconfirmed. The snap followed it, and tracking made the ±400 µs corrections at 45–60 s. The
  left's lit fraction didn't drop.
- Latent bug spotted: `device_ticks - device_ticks_last` is computed in `uint32_t` before widening, so the
  "went backwards" branch can't fire. An out-of-order report would add ~1431 s to device time. Not seen in
  this data.
- Pose tracking remains the blocker: 353 left and 116 right candidates in 60 s, with the slow thread saturated.

**Illumination status:** passes for one controller static and under slow movement, and for two controllers
static with `KEEP_LOCK` + `TRACK`. Still to confirm: two controllers moving, and both options' behaviour
across restarts.

**2026-09-24 23:36, `sessions/20260924-233615-replay-both-static`** (both still, full ring, keep-lock + tracking,
no background load, commit `7a5a963d1`, first `.ctd` replay recording with both controllers). **The right-controller
fault recurred without any yield.**

- Left: 1.00 lit, lock at 15975 µs, 1983 fused poses, exposure residual ±40 µs. Tracking made 3 moves (net −257 µs).
- Right: baseline `11,10,10,1` (includes the lit left ring). The wide scan found the correct peak (14000–16000 µs,
  mean blobs 62–65 against ~33 elsewhere). **From narrow step 9 (fudge 15500 µs, 450 µs pulse, 22.5 s) every step
  had ~60–65 mean blobs**, so the right controller was lit continuously. It then locked on a meaningless 3.45 ms
  window (fudge 42 µs), and the tracker found it only 3 times. The user saw its status LED go off about halfway
  through the run.
- So the fault doesn't need the `LED_ALL_OFF` yield: with keep-lock the right controller was never switched off
  after lighting. Every occurrence so far (`224851`, `225515`, `230002`, `233615`) is on the right controller, while
  it was scanning (settings changing every 20 exposures), with the left controller also connected. It hasn't occurred in
  the right-only run or on the left controller. The triggering commands differ (wide fudge 0, wide 2000 µs, narrow
  15500 µs).
- Open question: role (the second controller to scan) or device (this right controller). Test: make the right
  controller scan first.

**2026-09-24 23:39, `sessions/20260924-233900-both-static-right-first`** (both still, full ring, keep-lock +
tracking, `PSSENSE_LED_BOOTSTRAP_FIRST=R`, no background load, commit `4ace64d83`). **No fault either side.**

- Right (first): baseline `2,1,3,1`, clean scans, lock at 16475 µs, 0.97 lit, 0 candidates while off.
- Left (second): baseline `11,6,11,7` (includes the right ring), plateau 2–3 (lower because of the baseline),
  lock at 15350 µs, 0 candidates while off. Narrow steps outside the window stayed at 0.00, so it wasn't stuck
  on. Its 0.64 lit fraction is confounded by the right ring.
- Neither controller faulted with the right controller scanning first (both status LEDs stayed on, per the user). That fits a right-scans-second trigger,
  but the fault has been intermittent, so this isn't conclusive. Replay recordings use `FIRST=R`.
- **Tracker collapse with two lit rings:** 0 fused poses in 45 s (left 8 candidates, right 108), 5854 slow-sample
  drops, while 186/188 captured frames were lit. At the same placement in `233615` the left alone had 1983
  fused poses. The headline replay case for M1/M2.

## Joint multi-camera solve (plan item 3)

Why the current tracker fails even with good light: each camera solves on its own (a fast path from
last-frame blob labels or the predicted pose, then a slow 2D–3D correspondence search per camera). The driver
accepts a pose only when two per-camera candidates agree within 80 mm / 35°, and then averages them. After
optical loss only an IMU orientation prior remains. The per-camera slow searches then saturate: each keeps
only its newest sample, and thousands of samples are dropped per run. They rarely re-acquire a moving
controller, and they load the host enough to disturb USB/HID timing.

Milestones:

- **M0 – offline replay.** `psvr2_sense_session.sh` records the tracker input as `constellation.ctd`:
  every camera's blobs, the camera world poses, each device's prior, and (packet type 3) each device's
  tracking-source relation at every sample, including orientation-only ones. A replay tool regroups the
  samples into exposures and runs a solver on identical input. It reports solve rate, cameras and blobs used,
  reprojection error, static jitter, jumps and CPU time per exposure.
- **M1 – joint tracking solve.** From a prior (last solution propagated with the IMU), project the LEDs into
  all four cameras, associate blobs with gating, and run one robust Gauss–Newton over 6DoF on every camera's
  correspondences together, re-associating between iterations. This replaces per-camera PnP, the pairwise
  agreement gate and averaging. Target cost well under 1 ms per exposure.
- **M2 – bootstrap.** Match blobs across the lower (0/1) and upper (2/3) stereo pairs by epipolar
  distance, triangulate them to 3D, and register against the ring model with the IMU gravity prior (roughly one
  free rotation) using a small RANSAC over 3D–3D correspondences, respecting LED normals. Then refine with M1.
  Runs under a fixed time budget per exposure.
- **M3 – integration.** An opt-in exposure-level path in the tracker: collect the four synchronised camera
  samples, try M1, and fall back to M2. Push one joint pose per exposure, which the driver accepts without the
  candidate-fusion gates. Exposures that miss the budget are skipped, not queued.

### M1 replay results and an unoptimised build (2026-09-25)

**Every build directory in this checkout, including `build-sense`, has an empty `CMAKE_BUILD_TYPE`,** so the whole
tracking stack (correspondence search, blobwatch, PnP, the PSVR2 and Sense drivers) has been built without
optimisation. Only 32 of 422 translation units carry any `-O` flag. Every live session so far used that
`monado-cli`. `build-sense-rel` is configured with the same options and `CMAKE_BUILD_TYPE=RelWithDebInfo`
(`-O2 -g -DNDEBUG`). The same M1 replay runs 90× faster there, so the live tracker's slow-thread saturation (thousands of dropped
samples per run), and some of the CPU-load timing disturbance, were probably inflated by `-O0` code.

M1 on `20260924-233615-replay-both-static`, left controller (`constellation_replay --m1`):

| | `build-sense` (-O0) | `build-sense-rel` (-O2) |
|---|---|---|
| exposures solved | 1950 / 2698 (the rest are before the first LED lock at 13.4 s) | same |
| cameras per solve | 3 (camera 3 barely sees it) | same |
| RMS px p50 / p95 | 0.299 / 0.349 | same |
| LED coverage p50 / p05 | 0.95 / 0.85 | same |
| static jitter (1 s windows) p50 / p95 | 1.11 / 4.32 mm | same |
| **CPU per solve p50 / p95 / max** | 4388 / 4544 / 4898 µs | **48 / 52 / 76 µs** |

Live, the same run fused 1983 two-camera poses with 1.37 mm static jitter p50. M1 uses all three cameras that see the
ring every frame. Its real cross-camera RMS (0.30 px) is close to the synthetic noise floor, so the rig calibration
is consistent across cameras. M1 differs from the live per-camera candidates by 1.9 mm / 3.3° p50 (13 mm / 23° p95).
Those candidates include the rejected single-camera solves, whose tilt is weakly constrained.

**M1 coverage fix and two-controller seeds.** Coverage now counts only LEDs at least 20° inside their visibility
cone. Edge-on LEDs, and LEDs the ring hides from itself (the Sense model's occlusion callback can't be recorded), had
rejected correct poses at 0.73–0.79 coverage with 0.5 px RMS. On `234059` (left, slow moves) M1 then solves 2896 of
3596 exposures, about every lit one, with 4 cameras in 2282 of them. It needed 5 recorded seeds, down from 24.
RMS was 0.48 px p50, static jitter 0.99 mm p50 and CPU 58 µs p50. On `233900` (two rings, live collapse) there are no seeds:
the recorder only writes fast-path per-camera poses, and every live candidate there came from the slow search.

**The recorded tracking-source orientation is not a clean IMU signal.** The optical-from-IMU alignment contains
27–54° of tilt that varies between sessions and with orientation. Fitting a fixed IMU-to-model body rotation B
(`q_opt = A q_imu B`) still leaves the world alignment varying by 26° p50. When optical tracking is active the driver
returns its fused, optically corrected pose (11524 of 14061 packets in `234059` were full poses). So M2 is purely
geometric for now: stereo triangulation plus 3-point rigid registration, no gravity prior. A raw IMU orientation
should be recorded separately before gravity is used.

### M2 stereo bootstrap: replay results (2026-09-25)

`stereo_bootstrap()` (`stereo_bootstrap.{hpp,cpp}`) finds a device with no prior. It pairs blob rays across cameras
that pass within 4 mm, merges points seen by several pairs, and registers them against the LED model with a
three-point RANSAC on pairwise distances (Kabsch). The explaining LED must face a camera that saw the point. The best
hypotheses are refined with M1, with a stricter 0.8 px RMS limit. Synthetic tests: 30/30 found from no prior, two
mirror-image rings separated, and 0/30 mirror-image acceptances (one appeared at 0.96 px before the stricter limit).

`constellation_replay --m1` now bootstraps with M2 whenever a device isn't tracked (`--seed-recorded` keeps the old
recorded-pose seeding). Build: `build-sense-rel`.

| session | device | live (-O0 tracker) | M1 + M2 replay |
|---|---|---|---|
| `233900` two rings, right first | left | 8 candidates, 0 fused | 1297 solved, 3 bootstraps, 3 cameras, 0.30 px, jitter 1.0 mm |
| | right | 108 candidates, 0 fused | 1571 solved, 25 bootstraps, 4 cameras, 0.54 px, jitter 1.4 mm |
| `233615` two rings, right faulted always-on | left | 1983 fused (2 cameras) | 2048 solved, 3 cameras, 0.30 px, jitter 1.1 mm |
| | right | 3 candidates | 1411 solved, 4 cameras, 0.62 px, jitter 1.5 mm |
| `234059` left, slow moves | left | 2841 fused (2 cameras) | 2896 solved, 5 bootstraps, 4 cameras in 2284, 0.48 px, jitter 0.96 mm |

Cost per exposure and device: tracking 49–58 µs p50 (≤ 173 µs max). A bootstrap attempt takes about 1 µs when
nothing is visible, and at most 0.67 ms.

Identity check: on `233615` M1's left pose matches the live left candidates to 2 mm (XR −0.086, −0.051, −0.266
against −0.084, −0.052, −0.265), and the right ring is 25 cm to the right. On `233900` the live tracker's 8 "left"
candidates were at the **right** ring's position (x +0.19 m): it had fitted the left model to the mirror-image
ring. M2 places the left ring at x −0.06 and the right at x +0.19.

Still missing: M3 (live integration), and the remaining recordings (two rings in the normal grip while moving; left,
faster with occlusion) to test tracking through motion and occlusion.

### M3: joint path in the live tracker (2026-09-25)

`CONSTELLATION_TRACKER_JOINT=1` (opt-in, `t_constellation_tracker_joint.cpp`) replaces per-camera fast/slow
processing. Camera threads deposit every frame, empty ones included, into an exposure assembler. One worker, keeping
only the newest exposure, runs two phases:

1. Tracked devices refine with M1 from the driver's predicted pose (3° orientation prior) and claim their blobs.
2. Everything else enters a **bootstrap contest**: each candidate model bootstraps against the same free blobs, the
   best fit (more matches, then lower RMS) wins and claims its blobs, and the rest retry.

A freshly bootstrapped track is **tentative** until 3 consecutive solves, which adds 33 ms on re-acquisition. It claims
blobs but pushes nothing. The Sense driver accepts `joint_camera_count > 0` samples directly: no per-camera grouping,
agreement gate, averaging or fresh-pose jump gate. They share the fused acceptance tail
(`pssense_commit_optical_pose_locked`: IMU alignment, LED sync, relation history), and the log gets `joint=1` on
`CONSTELLATION_FUSED_ACCEPT`. In joint mode the recorder writes every camera sample, closing the gaps where slow
cameras dropped out of recordings.

`constellation_replay DATASET --tracker[-csv OUT]` runs a recording through the real `ConstellationTracker`
(deterministic, fake origin and device sources), exercising assembly, the worker and pushes. Poses pushed, against the
live per-camera fusion of the same session:

| session | build (live) | live fused L / R | joint path L / R | µs per exposure |
|---|---|---|---|---|
| `233615` both still (right faulted) | -O0 | 1983 / 0 | 2042 / 1407 | 71 |
| `233900` both still, right first | -O0 | 0 / 0 | 1290 / 1530 | 61 |
| `234059` left slow | -O0 | 2841 | 2886 | 50 |
| `234425` both grip (no FIRST=R, right faulted) | -O0 | 611 / 1598 | 604 / 3549 | 107 |
| `000029` both grip, right first | -O2 | 2723 / 799 | 2582 / 1788 | 105 |
| `234623` left fast | -O0 | 1186 | ~1340 | 29 |
| `000212` left fast (right on, off-screen) | -O2 | 1649 / 174 | 2584 / 0 | 71 |
| `000423` left fast `-2` (left only) | -O2 | 1772 | 2548 | 51 |

Joint poses use 3–4 cameras. RMS p50 is 0.30–0.50 px for the left and 0.54–0.66 px for the right; the right's rig
residual is consistently higher, worth a calibration look.

Findings on the way:

- **Mirror false positive.** On `000212` the right model bootstrapped the left ring (0.80 px) while the left's
  tracking had lapsed, and tracked it for one more frame (0.93 px). The contest alone didn't stop it because the
  left's own bootstrap failed in that exposure. Tentative confirmation removes it (right: 0 poses). It costs 0.3–2%
  of poses on most runs, and 9% on the grip run, which needed 126 re-acquisitions.
- **Phase-tracking probes darkened the ring.** On `000423` the narrow scan measured an inflated 1950 µs window, so
  probes stepped ±690 µs past the real edges. The ring went dark for each 0.6 s probe side: joint-path gaps of
  650–670 ms every ~3.4 s. Blob counts of a moving ring also change on their own between probe windows, so the lock
  wandered ±300–400 µs. Now the offset is capped at 300 µs, the dead band is 0.2 and the gain 1.5. Simulator: 92%
  lit under 60 µs/s drift (the gate is 90%), 100% with no drift, 92.6% with another controller lit. The proper fix is
  to drive phase tracking from each joint pose's matched/visible LED ratio. That is per ring, and independent of
  motion and of the other controller's light.
- **Optimised build, live.** Slow-sample drops fell from 6514 (-O0, both grip) to 649 (-O2), and on the
  left-only fast run from 370 to 19.
- **Right-controller fault.** It recurred in `234425`, which scanned without `FIRST=R`: 3657 candidates while off.
  That makes four occurrences, all with the right controller scanning second; none in three right-first runs.

**First live M3 run, `sessions/20260925-001238-joint-both-grip`** (both, right first, 30 s still then grip and moves,
`CONSTELLATION_TRACKER_JOINT=1`, -O2, commit `553ea0159`). **It failed: 0 left poses and 21 right poses**, while
488/488 captured frames were lit. `JOINT_STATUS` showed `tracked=0` throughout and 1935 bootstraps, all
unconfirmed. Tentative tracks push nothing, so the driver had no optical history, and its prediction was the
orientation-only, *unaligned* IMU pose. M1 was anchored to that orientation with a 3° prior, failed, and the next
bootstrap reset the confirmation count. The replay's fake device returned nothing before its first push, so it
missed this. It now returns the recorded orientation-only relation as the driver does, which reproduces the
failure offline exactly (0 / 21 poses, 852 µs per exposure). Fix: the joint path uses the device's prediction only when it
includes a position (so it rests on optical history and is aligned); otherwise it tracks from its own last pose
without an orientation prior. The same recording then replays to 2998 / 2410 poses (3–4 cameras, 0.40 / 0.62 px,
110 µs per exposure), and the other recordings are unchanged.

**Second live M3 run, `sessions/20260925-001537-joint-both-grip-2`** (commit `065b31fde`). Better but still low:
177 left and 39 right joint poses, 176 of the left's logged as re-acquisitions. `JOINT_STATUS`: tracked=400,
bootstrapped=2791. The recorded predictions show why: **the Sense driver's predicted position is right (p50 3.8 mm) but
its predicted orientation is 50–110° off.** `pssense_get_constellation_pose` takes orientation from
`pssense_get_corrected_imu_pose`, which applies only the fixed `T_led_imu` body correction and never the optical-world
alignment. `optical_from_imu_orientation` is computed on every fused pose but used only in diagnostic logs. So the
driver's predicted pose, and probably the orientation it reports to applications, mixes an optical-frame position with
an IMU-world orientation. **This must be fixed before 6DoF goes to OpenXR (plan item 5)**; it also degraded the
per-camera path's priors.

Joint-path fix: keep a per-device alignment from every solve (`align = q_solved · q_predicted⁻¹`) and predict
orientation as `align · q_predicted(t)`. That carries the IMU's rotation between exposures into the optical world,
whatever world the driver uses. The replay's fake device now behaves like the driver (IMU-world orientation from the
recording, optical position while fresh). With the previous code it reproduces this run (400 / 78 poses, against
`tracked=400` live), and with the fix gives 2812 / 559. Other recordings: unchanged within 3%.

This run also had host-timing trouble: exposure timestamp residual p5/p95 ±7 ms, and both controllers' clock offsets
crept ~15 ms with 33–35 snaps, the same pattern as the 1 ms/s excursion in `231910`. The worker averaged ≤ 0.5 ms per
exposure (3% of a core), so it is unlikely to be the cause; keep watching.

**Driver-side fix, `PSSENSE_ALIGN_IMU_ORIENTATION=1`** (opt-in, default off). When set, and once an optical pose
has been committed, `pssense_get_constellation_pose` turns the corrected IMU orientation into the optical world as
`optical_from_imu_orientation · q_imu(t)`, and rotates the angular velocity the same way. That function is used
both for the tracker's prediction (the constellation tracking source) and for `pssense_get_tracked_pose`, the grip
and aim poses OpenXR apps get. So with the option on, both report optical position with aligned orientation.
The alignment itself is still computed from the unaligned corrected IMU pose, so it doesn't feed back on itself.
The per-camera candidate diagnostics are unchanged. Before the first optical commit, and with the option off, the
orientation stays unaligned as before. Not yet tested on hardware.

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
