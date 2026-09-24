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

1. **Wide scan.** A 2.1 ms pulse is stepped in 1 ms steps across the whole camera period (17 steps). Each
   step waits 8 exposures to settle, measures 8, then allows 4 of grace for late reports. That is 20
   exposures, about 0.33 s per step.
2. The best step (circularly smoothed) must score at least 1.0 and beat the median step by 0.75. The score
   is the sum over cameras of the fraction of frames with at least 3 blobs. Otherwise the scan fails and
   the controller idles for 60 exposures.
3. **Narrow scan.** A 450 µs pulse is stepped in 250 µs steps from 1.5 ms before to 1.5 ms after the best
   wide pulse (21 steps). The lit run is the contiguous set of steps at or above half the peak score.
4. **Lock.** The pulse is centred in the lit run, 1.0 ms wide by default (`PSSENSE_LED_BOOTSTRAP_LOCK_PERIOD_ID`,
   50 µs per id, default 20). If no camera sees at least 3 blobs for 300 exposures (~5 s), it rescans.

A full bootstrap takes about 13 s. Only one controller scans at a time, because blob counts cannot tell
the controllers apart. The other controller holds its LEDs off, and its own state is frozen, until the scan
finishes. The fixed macOS `PSSENSE_TIMING_FUDGE_100US` (3.6 ms) is still added, but it doesn't matter
because the scan covers the whole period. With the variable unset, behaviour is unchanged.

Log lines use the form `LED_BOOTSTRAP side=L event=...`. The events are `scan_start`, `step`,
`wide_result`, `locked`, `locked_status` (every 300 exposures), `scan_failed` and `lost`. The
`psvr2-constellation` CSV gains the columns `led_bootstrap_state` (0 idle, 1 wide, 2 narrow, 3 locked),
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
