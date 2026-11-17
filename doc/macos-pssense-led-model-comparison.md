<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Sense LED model comparison — 17 November 2026

**Keep the existing measured correction for this controller pair and camera
calibration.** PSVR2Toolkit's new model has higher common-timestamp median
reprojection error than our correction for both hands in all eight tested
recordings. OpenBrush and the later guided survey reproduce that result.
There is no runtime/default/installed-settings change from this investigation.

## Candidate and recordings

Toolkit commit [`66b437d344`](https://github.com/BnuuySolutions/PSVR2Toolkit/commit/66b437d344966b8330efe4e48b49000b1fe804bb),
15 November, replaces Sony's right-controller LED positions with Beyley
Cardellio's bundle-adjusted model from a large CV1 controller dataset. The code
describes adjustments along the normals to compensate for shell refraction.
The project is MIT licensed; the pinned table and licence are preserved in
[the reproduction directory](macos-pssense-led-model-results/).

An inventory found 111 readable CTD files, representing 110 unique recordings:
one OpenBrush file is duplicated. All loaded to a clean end of file. Unique
recordings total about 141 minutes and 505,965 exposures. Locations:

- `/Volumes/nick-mac-external/psvr2-datasets/sessions`: 106 files, including
  the duplicate (September 24–October 5).
- `~/Code/psvr2-datasets/sessions`: two October 8 put-down recordings.
- `build/arm64/controller-evidence`: three October 25 recordings.

Many are LED phase experiments, so their total duration is not a measure of
useful geometry coverage. The comparison selects eight recordings with both
controllers, stored camera geometry, detected blobs and IMU data:

- `20261003-234218-corr-off-grip`
- `20261003-234436-corr-on-grip`
- `20261003-235855-corr-on-grip-2`
- `20261004-000108-hard-cases`
- `20261004-000405-hard-cases`
- `20261004-000909-distance`
- `20261005-2001-openbrush-oriented-recorded`
- `20261025-controller-survey-both-mode16-v3`

Together these contain 80,089 exposures over about 22 minutes. The key October
3–4 recordings retain calibration/environment/log sidecars and raw-frame
archives; this experiment reuses their recorded blobs without re-detecting them.
No independent ground-truth poses are present.

## Controlled replay

Tracker source: `b6758b7d6b0791fe9594bc1cf15bf85ddf638a94` on
`macos-upstream-clean`. Rebuilt the existing `build/arm64` replay target in
`RelWithDebInfo`; executable SHA-256:
`1f8384869730239d8c8eebe61938a65b36946466ba368202a77b4780f04f7994`.

Every recording is replayed through `--tracker-filter`: deterministic joint
tracker plus IMU/optical EKF. Oriented bootstrap is on, prior reacquisition is
off, and other inherited `CONSTELLATION_`, `PSSENSE_` and `PSVR2_` environment
settings are cleared. Calibration, blobs, normals, IMU observations and solver
settings are held constant. The three absolute models are:

1. **Sony:** the unchanged positions in `pssense_led_model.h`.
2. **Local:** Sony positions plus `pssense_led_correction.h`.
3. **Toolkit:** the pinned new right-hand positions, mirrored in x for the left.

Toolkit payload order is Monado LED 16, then LEDs 0–15. Reorder before use,
convert mm to metres and negate y/z into the recorded CV convention. Each
offset file is **target position minus recorded position**, so recordings that
already contain the local correction are never corrected twice. LED normals,
visibility limits and the IMU transform are unchanged.

All 24 transformed models were re-exported and verified against the targets,
with a maximum coordinate error of 1 micrometre (geometry CSV precision).
Repeating the first session with each corrected model produces byte-identical
timestamped pose CSVs. Input and executable hashes and the exact commands are
preserved in the manifests/receipts.

## Results

Solve percentage is accepted optical pushes divided by **all recorded camera
exposures**, including controller startup, deliberate occlusion and out-of-range
periods. It is not application position-tracked time or visibility-normalized
coverage. Error below is the median accepted-solve RMS in pixels, pooled over
timestamps solved by **all three models**, separately for each hand. The
models can choose different correspondences even at the same timestamp.

| Recording group | Hand | Sony solve % | Local solve % | Toolkit solve % | Sony / local / Toolkit common error, px |
| --- | --- | ---: | ---: | ---: | --- |
| October 3–4 | L | 57.71 | 58.00 | 58.11 | 0.412 / 0.359 / 0.466 |
| October 3–4 | R | 68.83 | 73.53 | 72.56 | 0.455 / 0.341 / 0.456 |
| OpenBrush | L | 95.75 | 95.77 | 95.74 | 0.331 / 0.314 / 0.423 |
| OpenBrush | R | 95.58 | 96.68 | 96.68 | 0.519 / 0.276 / 0.345 |
| Survey | L | 93.51 | 95.00 | 95.00 | 0.275 / 0.306 / 0.462 |
| Survey | R | 92.35 | 92.80 | 92.78 | 0.426 / 0.438 / 0.513 |

Toolkit's common-timestamp median error is worse than local in **all 16
session/hand comparisons**, not just in these pooled groups. Its accepted
coverage is generally similar. Across both controllers and all eight sessions,
local produces 139,545 accepted solves and Toolkit 139,344. Adjacent accepted
solve gaps longer than 100 ms total 124 versus 134. These gap counts include
intentional hiding and transport/synchronization failures; they are not an
isolated recovery benchmark or a significance estimate. Toolkit does not
remove the long recorded losses.

This does **not** establish that local is universally the most accurate model.
Sony has lower median reprojection error than local in the two hard-case runs
and in the later survey. Nor is lower error proof of physical accuracy:
calibration/model compensation and correspondence selection remain possible.
What the replay does establish is that Toolkit offers no demonstrated
replacement benefit for the tested pair and rig calibration.

The self-reported static markers are insufficient for a clean jitter claim.
One marked interval has roughly 193 mm median within-second position spread
and another has large movement tails; the first marker of the second hard-case
run lasts only 0.21 s and contains no accepted poses. Their measured spreads
are retained as diagnostics, not presented as a validated noise floor. The
guided targets likewise supply no independent physical reference.

## Decision and reproduction

Retain the existing local correction where it is already selected. Do not
replace the Sony base table, stack the two corrections or promote a new default.
A hardware trial of Toolkit's table is not justified by this comparison alone.
Further geometry work would need evidence that separates camera calibration
error from apparent LED geometry, ideally with independent pose/position
references and additional controller pairs.

From the repository root, with the external dataset drive mounted:

```sh
cmake --build build/arm64 --target constellation_replay --parallel 4
python3 doc/macos-pssense-led-model-results/run.py --jobs 2
python3 doc/macos-pssense-led-model-results/validate.py
python3 doc/macos-pssense-led-model-results/summarize.py
```

Raw logs, per-pose CSVs, offset files and geometry exports remain in ignored
`doc/results/pssense-led-model-20261117/`. Small durable results are alongside
the scripts: [per-session scores](macos-pssense-led-model-results/summary.csv),
[pooled scores](macos-pssense-led-model-results/pooled.csv),
[input manifest](macos-pssense-led-model-results/manifest.json) and
[validation diagnostics](macos-pssense-led-model-results/validation.json).
The replay build and checks passed; no service was started, installed or tested
on hardware, and no Linux/runtime source was changed.
