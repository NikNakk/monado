# PSVR2 positional prediction experiment — 2026-09-05

Test **full-horizon raw prediction first**, then bounded acceleration against it.
The offline acceleration improvement is modest. A separate, larger tracking
mismatch was found while checking the live path: with `accel_ff == NULL`,
`t_apply_dead_reckoning()` advances `integ_rel_ts` through gyro samples without
advancing position. It finally extrapolates position only from that advanced
timestamp to the target. The existing `horizon.csv` raw/EMA columns instead
extrapolate from the original SLAM timestamp to the target.

This is in tracking, not presentation. No compositor, display, gyro integration,
clock mapping, or generic dead-reckoning implementation is changed here.
All new live behavior is opt-in; existing raw and EMA behavior remain available.

## Evidence and model selection

The replay uses 11 local recordings, 110,661 scored queries. PID 38153 has the
original long-horizon CSV; other recordings reconstruct the same target from
`pose.csv` source/target VTS and `slam.csv` positions. Earlier recordings include
shorter median horizons (~45–47 ms versus ~60 ms), so their results are reported
per recording, not pooled as one interchangeable population.

The source is selected by exact VTS equality, **never** by the latest sample at
the target time. All model states use samples at or before the source. Ground
truth is the linear interpolation between the subsequent bracketing SLAM samples.
Only positive horizons up to 120 ms, valid positions/velocities, ten contiguous
source samples, and target brackets under 35 ms are scored. Three queries are
excluded in eligible recordings; three other recordings have fewer than ten pose
queries and are skipped. The analysis does not use IMU acceleration as position
acceleration, and does not fit parameters to future samples at each query.

Tested families:

- Raw constant velocity and velocity EMA coefficients 0.1, 0.25, 0.5, 0.75.
- Successive backward-velocity acceleration, correcting its midpoint timestamp;
  acceleration EMA 0.1/0.25/0.5/1 and gain 0.25/0.5/1.
- Linear and quadratic least-squares fits over 3/4/5/7/9 samples, anchored at the
  newest measured position, plus damped/clipped quadratic variants.
- Six alpha-beta / alpha-beta-gamma configurations, including position-state
  correction, with bounded acceleration for the gamma variants.
- Conservative acceleration clipping before filtering, damping, stationary
  fallback, and a bounded correction horizon.

The first/second halves of PID 38153 were compared with a target-time embargo at
the split. Other recordings were then inspected and clipping refined. Therefore
`development` and `validation` in the CSV describe exploratory cross-checks,
**not an untouched test set**. A new headset recording is still needed. Queries
are correlated within SLAM intervals; these are descriptive percentiles, not
110,661 independent experimental observations.

The selected model is `bounded_acceleration`: clip acceleration to 2 m/s² before
an EMA of 0.25, apply gain 0.5, retain raw velocity, and disable the correction
below 0.01 m/s. Saturate only the correction horizon at 80 ms. This was chosen
for predictable cost and conservative cross-recording tails. A four-sample damped
quadratic fit also reduced median/p95, but its p99 worsened by about 55% in PID
84285; the selected model's p99 regression there is about 2%. Stronger acceleration
models and velocity smoothing did not offer the same balance.

### Newest capture: PID 38153, 6,709 queries

All errors are Euclidean position error in millimetres, at the requested target.

| Model | Median | p95 | p99 | Maximum |
|---|---:|---:|---:|---:|
| Recorded live EMA path (shortened horizon) | 5.729 | 15.013 | 21.226 | 26.992 |
| Full-horizon raw | 0.987 | 3.837 | 5.843 | 9.808 |
| Full-horizon velocity EMA, alpha 0.25 | 1.412 | 6.484 | 10.555 | 14.082 |
| Full-horizon bounded acceleration | 0.972 | 3.470 | 5.349 | 9.801 |

Against **full-horizon raw**, acceleration reduces median error 1.5%, p95 9.6%,
and p99 8.5%. It strictly wins 43.8% of all queries (stationary fallback ties count
as non-wins). This is not a claim that acceleration solves the visible judder.

The recorded live tracker position was recovered by subtracting the rotated
`T_imu_head.position` from `pose.csv` head positions. The shortened-horizon EMA
calculation matches it within **0.00012 mm maximum**. Full-horizon EMA differs
from the actual returned tracker position by 5.567 mm median / 14.225 mm p95.
The larger improvement implied by the first two rows mixes a horizon change and
EMA-to-raw change; it must not be attributed to acceleration.

| Regime | Queries | Raw median / p95 / p99 | Acceleration median / p95 / p99 | Wins |
|---|---:|---:|---:|---:|
| Speed <0.01 m/s | 1,139 | 0.040 / 0.131 / 1.231 | 0.040 / 0.131 / 1.231 | 0.0% (ties) |
| Speed >=0.05 m/s | 5,102 | 1.178 / 4.084 / 6.137 | 1.164 / 3.755 / 5.619 | 52.3% |
| Speed >=0.2 m/s | 2,978 | 1.323 / 4.495 / 6.435 | 1.367 / 4.193 / 5.861 | 44.1% |
| Speed >=0.4 m/s | 423 | 2.913 / 6.414 / 8.273 | 2.615 / 5.939 / 7.790 | 66.0% |
| Acceleration >=2 m/s² | 578 | 2.869 / 6.448 / 7.815 | 2.673 / 5.628 / 6.883 | 52.6% |
| Reversal | 102 | 1.770 / 6.989 / 8.373 | 1.872 / 5.603 / 6.285 | 45.1% |

Speed is the source raw velocity magnitude. Acceleration stratification uses its
backward change divided by sample interval (a noisy label, not physical ground
truth). Reversal means source speed >=0.02 m/s and negative dot product between
source and target-bracket ending velocity. This retrospective label is **not**
provided to the predictor. Speed groups overlap.

Signed along-motion error is `(actual - prediction)` projected on the target
bracketing segment direction; positive means undershoot, negative overshoot.
Segments <=0.1 mm are assigned zero, matching the original horizon diagnostic.
Overall mean goes from +0.115 mm to +0.019 mm; at >=0.05 m/s, +0.061 to -0.050 mm;
at >=0.4 m/s, +0.036 to -0.348 mm. Reversal mean goes from +2.430 to +2.057 mm.
Thus some overshoot remains, and reversal median and the >=0.2 m/s median regress.

### Cross-recording results

| PID | Raw median / p95 / p99 | Acceleration median / p95 / p99 |
|---|---:|---:|
| 3250 | 0.886 / 2.259 / 3.136 | 0.852 / 2.093 / 3.075 |
| 38153 | 0.987 / 3.837 / 5.843 | 0.972 / 3.470 / 5.349 |
| 43034 | 0.778 / 2.327 / 3.657 | 0.701 / 2.141 / 3.593 |
| 4761 | 1.661 / 6.957 / 11.556 | 1.444 / 6.478 / 11.313 |
| 68732 | 0.518 / 5.209 / 9.265 | 0.519 / 5.128 / 8.980 |
| 74882 | 1.489 / 5.220 / 7.769 | 1.226 / 4.710 / 7.252 |
| 79629 | 0.854 / 4.361 / 7.350 | 0.750 / 3.907 / 6.929 |
| 84285 | 0.857 / 5.163 / 20.914 | 0.757 / 4.953 / 21.339 |
| 88726 | 1.547 / 7.773 / 13.646 | 1.233 / 7.426 / 13.214 |
| 94976 | 2.013 / 8.818 / 14.465 | 1.653 / 8.027 / 13.409 |
| 96643 | 1.971 / 6.812 / 11.388 | 1.543 / 6.356 / 10.516 |

p95 improves in 11/11 recordings; median and p99 improve in 10/11. The recordings
come from the same headset/operator and different software experiments. SLAM is
an onboard estimate, not independent optical ground truth. This metric does not
measure orientation error, scanout error, or perceived inter-frame continuity.

## Implementation

`psvr2_linear_prediction.h` is a small driver-local C helper with fixed-size
state, O(1) work per SLAM update and pose query, and no allocation. It uses the
actual unequal sample intervals. For interval-average velocities `v[i]`:

```text
a_observed = 2 * (v[i] - v[i-1]) / (dt[i] + dt[i-1])
a = EMA(clip_length(a_observed, acceleration_limit), alpha)
h = min(target_time - source_time, correction_horizon)
predicted_position = p[i] + v[i] * full_horizon
                     + gain * a * 0.5 * h * (h + dt[i])
```

The `h * dt[i]` term corrects the half-interval age of backward-difference
velocity. Linear velocity returned to callers is the derivative of this position
model; after correction saturation it is raw velocity. Position is continuous
at the horizon cap, although its derivative changes there. With defaults the
correction is bounded to 4.6 mm even at a 35 ms sample interval.

Invalid/nonfinite samples or intervals outside 5–35 ms reset acceleration state.
Two valid consecutive velocity intervals are needed before using acceleration.
Below the speed threshold, during warmup, and on unavailable state, the
acceleration mode falls back to **full-horizon raw**, not EMA or the legacy
shortened horizon. Acceleration is clipped before filtering to prevent a large
single SLAM velocity jump from persisting as an unbounded correction. This does
not make raw tracking resilient to relocalization jumps.

State updates, predictions, and horizon enqueue/scoring share the existing
`data_lock`. The gyro dead-reckoning call and all orientation output are retained.
Full-horizon position/velocity are applied after it, before the tracker-to-head
relation transform. Historical pose queries still use relation-history lookup.

Existing primitives were reviewed: `m_relation_history` supplies backward
velocity, FIFO and One Euro are generic smoothing primitives, and the tracking
Kalman code uses C++ FlexKalman / OpenCV pose-fusion machinery. None directly
implements this bounded position-only correction with raw-velocity fallback;
a driver-local helper avoids adding fusion dependencies or changing shared code.

## Runtime options and diagnostic compatibility

Options are read once at device startup. Restart the service to change them.

| Variable | Default | Meaning / accepted range |
|---|---:|---|
| `PSVR2_FULL_LINEAR_HORIZON` | 0 | Predict raw/EMA translation over the full SLAM-to-target interval |
| `PSVR2_ACCELERATION_PREDICTION` | 0 | Enable bounded acceleration; implies full horizon and overrides EMA selection |
| `PSVR2_ACCELERATION_ALPHA` | 0.25 | Acceleration EMA coefficient, 0–1 |
| `PSVR2_ACCELERATION_GAIN` | 0.5 | Fraction of acceleration correction, 0–1 |
| `PSVR2_ACCELERATION_LIMIT` | 2 | Pre-filter vector magnitude limit, 0–20 m/s² |
| `PSVR2_ACCELERATION_MIN_SPEED` | 0.01 | Raw speed below which to retain raw prediction, 0–1 m/s |
| `PSVR2_ACCELERATION_HORIZON_MS` | 80 | Cap on correction horizon only, 0–120 ms |

Finite parameters are clamped to these ranges; nonfinite parameters use defaults.
`PSVR2_FILTERED_LINEAR_PREDICTION` and `PSVR2_LINEAR_VELOCITY_ALPHA` are retained.
Both new booleans at zero preserve legacy behavior. `FULL_LINEAR_HORIZON=1`,
`FILTERED_LINEAR_PREDICTION=1`, `ACCELERATION_PREDICTION=0` tests full-horizon EMA.
Acceleration gain zero provides another full-horizon raw control.

All old CSV columns retain their order and meaning. Only `horizon.csv` appends:

- `accel_pred_x/y/z`, `accel_error_mm`, `accel_along_mm`;
- `accel_x/y/z` (clipped/filtered acceleration before gain and speed gate);
- `accel_applied` (candidate eligible, even if not selected live), `accel_enabled`;
- `accel_alpha`, `accel_gain`, `accel_limit`, `accel_min_speed`, `accel_horizon_ms`;
- `returned_pred_x/y/z`, `returned_error_mm`, `returned_along_mm`, and
  `full_linear_horizon_enabled` (effective, including acceleration implication).

The returned position is the actual tracker position before the head offset.
Raw, EMA, and acceleration candidates are scored in parallel regardless of which
is active. `filter_enabled` retains its old meaning as the EMA option value;
`accel_enabled` takes precedence. CSV consumers should select by column name and
allow appended fields. The existing 256-slot pending queue is unchanged; it can
overwrite outstanding entries under overload and only scores bracketed targets.

## Reproduction and validation

From the repository root, using NumPy in a temporary environment:

```sh
python3 -m venv /tmp/psvr2-predictor-env
/tmp/psvr2-predictor-env/bin/pip install numpy
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I src/xrt/include -I src/xrt/drivers/psvr2 \
  tests/tests_psvr2_linear_prediction.c -o /tmp/tests_psvr2_linear_prediction
/tmp/tests_psvr2_linear_prediction
/tmp/psvr2-predictor-env/bin/python scripts/psvr2_predictor_replay.py \
  --trace-dir /tmp --output /tmp/psvr2-replay \
  --c-predictor /tmp/tests_psvr2_linear_prediction
cmake --build build --target drv_psvr2 --parallel 4
cmake --build build-macos-psvr2-display --target drv_psvr2 comp_main monado-service --parallel 4
```

Both macOS builds and sanitizer tests passed. The C helper matches the independent
Python bounded model across all 110,661 scored queries within 0.000077 mm. Raw,
EMA, and interpolated truth reproduce the original horizon CSV within 0.002 mm.
Synthetic tests cover unequal sample intervals, analytic acceleration, clipping,
long horizons, stationary fallback, invalid samples, gaps, and timestamp reversal.
The standalone test is also added to the existing macOS driver CI job; remote CI
has not been run for this commit. No new headset run was performed.

`psvr2-prediction-results/manifest.json` records input paths, SHA-256 hashes,
counts, horizons, and parity checks. `summary.csv` keeps all candidates' overall
cross-checks plus all strata for raw, EMA 0.25, bounded acceleration, and recorded
live tracker. The full generated summary remains in `/tmp/psvr2-replay`. Original
traces are not committed; preserve the source files identified in the manifest
for replay. Assessment: **usable with the limitations above; hardware A/B pending**.

## Exact next headset runs

Close GAV and stop any previous Monado service. In terminal 1, from this checkout,
first run the full-horizon **raw** control:

```sh
mkdir -p /tmp/psvr2-full-raw
PSVR2_AUXILIARY_STREAMS=0 PSVR2_TIMING_TRACE=1 \
PSVR2_TIMING_TRACE_DIR=/tmp/psvr2-full-raw \
PSVR2_FILTERED_LINEAR_PREDICTION=0 PSVR2_FULL_LINEAR_HORIZON=1 \
PSVR2_ACCELERATION_PREDICTION=0 XRT_MACOS_MAX_DRAWABLES=3 \
VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
./build-macos-psvr2-display/src/xrt/targets/service/monado-service
```

In terminal 2, from this checkout:

```sh
XR_RUNTIME_JSON="$PWD/build-macos-psvr2-display/openxr_monado-dev.json" \
DYLD_LIBRARY_PATH=/tmp/OpenXR-SDK-build/src/loader \
/tmp/OpenXR-SDK-build/src/tests/hello_xr/hello_xr -g Metal -s Local -v
```

Hold stationary, then perform slow lateral sweeps, faster sweeps, deliberate
stops/reversals, and stationary again, about 5 seconds each. Exit `hello_xr` and
stop the service cleanly. Then repeat with acceleration, changing only these
tracking options and the trace directory:

```sh
mkdir -p /tmp/psvr2-acceleration
PSVR2_AUXILIARY_STREAMS=0 PSVR2_TIMING_TRACE=1 \
PSVR2_TIMING_TRACE_DIR=/tmp/psvr2-acceleration \
PSVR2_FILTERED_LINEAR_PREDICTION=0 PSVR2_FULL_LINEAR_HORIZON=1 \
PSVR2_ACCELERATION_PREDICTION=1 PSVR2_ACCELERATION_ALPHA=0.25 \
PSVR2_ACCELERATION_GAIN=0.5 PSVR2_ACCELERATION_LIMIT=2 \
PSVR2_ACCELERATION_MIN_SPEED=0.01 PSVR2_ACCELERATION_HORIZON_MS=80 \
XRT_MACOS_MAX_DRAWABLES=3 \
VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
./build-macos-psvr2-display/src/xrt/targets/service/monado-service
```

Launch the same terminal-2 command. To reproduce the old raw baseline, use
`PSVR2_FULL_LINEAR_HORIZON=0 PSVR2_ACCELERATION_PREDICTION=0
PSVR2_FILTERED_LINEAR_PREDICTION=0` with a third trace directory.
Keep all existing presentation settings identical across runs. Compare returned
error with the corresponding candidate columns before attributing any subjective
change to acceleration; the horizon correction is a separate experiment.
