# PSVR2 continuity experiment — 2026-09-06

The next headset candidate transitions between bounded-acceleration predictions
in host time, with a **4 ms exponential time constant and 5 mm correction cap**.
It is opt-in. This trades a little tracking lag for substantially smaller
instantaneous changes when SLAM updates arrive. It is not yet hardware-validated.

## Offline selection

`scripts/psvr2_continuity_replay.py` evaluates 14 recordings, 136,259 eligible
queries, using the causal source joins and SLAM-interpolated truth from the
existing predictor replay. One additional query with a host timestamp before
its source SLAM receipt was excluded. No presentation path was changed.

Candidates included raw prediction, velocity EMA, bounded acceleration,
alpha-beta/gamma filters, and host-time exponential transitions:

- Raw transitions: 2, 4, 8, 12, 16 ms, uncapped and capped at 5 mm.
- Acceleration transitions: 2, 4, 8, 12 ms, uncapped and capped at 5 mm.

Raw transitions reduced jumps but worsened prediction accuracy. Longer transitions
introduced progressively more lag. The selected 4 ms acceleration transition
retains enough of acceleration's accuracy improvement to make a reasonable
experimental tradeoff. Selection was exploratory across all recordings; these
are not untouched validation recordings or independent observations.

Each query's candidate is scored against the same subsequent SLAM position.
For continuity, take the first scored query per source SLAM update and evaluate
both old and new models at the **identical target VTS**:

1. At receipt, measure the instantaneous change in prediction.
2. At receipt +8.3417 ms, measure the new prediction against the pre-update
   prediction at receipt. This measures correction delivered over one display
   period, without declaring success merely because the instant change is zero.
3. Also record the difference against the old model evaluated at +8.3417 ms,
   separating the new observation's effect from ordinary decay.

These are model continuity proxies, not measured rendered-frame trajectories.
They exclude target-time progression, orientation and head-offset motion.

| Recording | Raw error median / p95 / p99, mm | Transition error median / p95 / p99, mm |
|---|---:|---:|
| 72364, latest raw run | 2.045 / 7.443 / 11.589 | 1.750 / 6.913 / 10.863 |
| 73461, latest acceleration run | 2.360 / 9.255 / 13.833 | 2.062 / 9.249 / 13.951 |
| 38153, earlier long-horizon run | 0.987 / 3.837 / 5.843 | 0.987 / 3.646 / 5.619 |
| 71605, shorter-horizon run | 1.192 / 5.850 / 8.707 | 1.290 / 6.094 / 9.096 |

| Recording | Raw instant jump p95 | Transition instant jump p95 | Raw correction over 8.34 ms p95 | Transition correction over 8.34 ms p95 |
|---|---:|---:|---:|---:|
| 72364 | 4.744 mm | 0.123 mm | 4.744 mm | 4.182 mm |
| 73461 | 5.219 mm | 0.385 mm | 5.219 mm | 4.707 mm |
| 38153 | 2.599 mm | approximately zero | 2.599 mm | 2.674 mm |
| 71605 | 4.850 mm | 0.075 mm | 4.850 mm | 4.440 mm |

Across 14 recordings, correction-over-one-period p95 improves in 12. Overall
long-horizon p95 ranges from 7.1% better to 5.1% worse than raw; p99 ranges from
19.8% better to 4.5% worse. Median accuracy is not uniformly improved either.
These ranges describe the observed tradeoff, not a predeclared acceptance gate.

Regime caveats matter. At >=0.05 m/s in PID 73461, median improves 2.605→2.272 mm
but p95 changes 9.687→9.805 mm. Reversal p95 worsens 10.039→10.577 mm. At source
speed <0.01 m/s in PID 72364, p95 rises 0.772→1.145 mm: residual correction is
allowed to settle briefly rather than dropping instantly when speed crosses the
threshold. There is no claim that the previous stationary fallback remains an
exact tie. Large reversals still produce jumps when the correction reaches its
cap. Full regime results and source hashes are in `psvr2-continuity-results/`.

## Implementation and limits

`psvr2_continuity_prediction.h` holds a polynomial correction in position,
velocity and acceleration. At a SLAM update it advances the old prediction and
remaining correction to the new source VTS, subtracts the new prediction, and
stores that difference. At query time it adds the residual multiplied by
`exp(-(query_host_ns - source_received_host_ns) / tau)` to bounded acceleration.
The decay uses elapsed **host time**, never future requested VTS.

The residual is radially capped after decay. The correction's target-time velocity
derivative accounts for that radial cap. This is a derivative at fixed query
host time, not a guarantee that host-time output velocity is continuous. The
transition is exactly position-continuous only when unbounded and both underlying
quadratics are within the acceleration horizon. The 80 ms acceleration horizon
and 5 mm residual cap can leave a nonzero jump; replay includes both effects.

State and query work are fixed-size, allocation-free, and protected by the
existing data lock. Source intervals outside 5–35 ms, invalid data, host-time
reversal or host gaps above 100 ms reset correction state. A query timestamp
recorded just before acquiring the lock can precede source receipt; live decay
clamps that age to zero. Query evaluation never changes state, so querying eyes
or targets in a different order cannot accumulate different corrections.

All existing predictors remain selectable. Continuity implies the bounded
acceleration mode and full linear horizon; it supersedes EMA selection. Existing
gyro integration, orientation prediction, historical pose lookup, and compositor
behavior are retained. This is not a relocalization-jump repair and does not
promise perceptual smoothness.

## Options and traces

Options are read once at device creation; restart the service between tests.

| Variable | Default | Range / behavior |
|---|---:|---|
| `PSVR2_CONTINUITY_PREDICTION` | 0 | Opt in; implies acceleration and full horizon |
| `PSVR2_CONTINUITY_TAU_MS` | 4 | 0.5–20 ms decay time constant |
| `PSVR2_CONTINUITY_LIMIT_MM` | 5 | 0–20 mm residual magnitude cap; zero gives bounded acceleration |

Numeric options clamp to range, with defaults for nonfinite input. Acceleration
parameters remain tunable as previously documented. `accel_enabled` in the trace
is effective, so it is one when continuity is enabled even if the acceleration
environment option was zero.

`horizon.csv` preserves every existing column and appends:

- `continuity_pred_x/y/z`, `continuity_error_mm`, `continuity_along_mm`;
- `continuity_enabled`, `continuity_tau_ms`, `continuity_limit_mm`;
- `continuity_source_host_ns` (SLAM receipt timestamp used for decay).

`query_host_ns` already records the exact query timestamp used by the transition.
The continuity candidate is scored even when it is not active. `returned_*`
columns verify the selected live output before the tracker-to-head offset.

## Reproduction and checks

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I src/xrt/include -I src/xrt/drivers/psvr2 \
  tests/tests_psvr2_linear_prediction.c -o /tmp/tests_psvr2_linear_prediction
/tmp/tests_psvr2_linear_prediction
/tmp/psvr2-predictor-env/bin/python scripts/psvr2_continuity_replay.py \
  --pids 38153 72364 73461 71605 3250 4761 68732 74882 79629 84285 88726 94976 96643 43034 \
  --output /tmp/psvr2-continuity-final \
  --c-predictor /tmp/tests_psvr2_linear_prediction
cmake --build build --target drv_psvr2 --parallel 4
cmake --build build-macos-psvr2-display --target drv_psvr2 comp_main monado-service --parallel 4
```

The C implementation matches independent Python predictions within 0.000114 mm
across replayed queries, including the excluded negative-age query. Tests cover
continuity, decay settling, capping and its velocity derivative, clock reversal,
and the existing linear-prediction cases under address/undefined-behavior
sanitizers. Both macOS builds pass. Hardware validation remains outstanding.

## Next headset run

Close GAV and stop the old service. From this checkout:

```sh
mkdir -p /tmp/psvr2-continuity-run
PSVR2_TIMING_TRACE=1 PSVR2_TIMING_TRACE_DIR=/tmp/psvr2-continuity-run \
PSVR2_AUXILIARY_STREAMS=0 PSVR2_FILTERED_LINEAR_PREDICTION=0 \
PSVR2_FULL_LINEAR_HORIZON=1 PSVR2_ACCELERATION_PREDICTION=1 \
PSVR2_CONTINUITY_PREDICTION=1 PSVR2_CONTINUITY_TAU_MS=4 PSVR2_CONTINUITY_LIMIT_MM=5 \
XRT_MACOS_MAX_DRAWABLES=3 \
VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
./build-macos-psvr2-display/src/xrt/targets/service/monado-service
```

In another terminal, from this checkout:

```sh
XR_RUNTIME_JSON="$PWD/build-macos-psvr2-display/openxr_monado-dev.json" \
DYLD_LIBRARY_PATH=/tmp/OpenXR-SDK-build/src/loader \
/tmp/OpenXR-SDK-build/src/tests/hello_xr/hello_xr -g Metal -s Local -v
```

Repeat the previous stationary / lateral translation / stop-reversal pattern.
For a raw control explicitly set **both** `PSVR2_CONTINUITY_PREDICTION=0` and
`PSVR2_ACCELERATION_PREDICTION=0`, retaining full horizon and all display settings.
Use a separate trace directory. Assess snap-back and added lag independently.
