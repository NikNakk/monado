<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Linux solver gate before graphics integration (2026-10-14)

The graphics candidate uncovered an inherited PS Sense synthetic pose-solver
failure on Ubuntu 24.04 x86_64. The user authorised fixing this separately before
promoting the graphics work. These changes do not enable controller optical
tracking, change camera-stream defaults, or change graphics handling.

## Diagnosis and correction

The remote integration baseline `bc9591f0313508cdb19fcb3ddf8e2c39c1e2232b`
already fails the tracking accuracy case: seed 7, trial 24, 55 matches over four
cameras, 0.296 px RMS, 1.02795 degrees of orientation error. Starting the same
observations at the exact ground truth converges to 1.02973 degrees of error.
The weakly constrained ring tilt and Gaussian pixel noise invalidate a fixed
one-degree bound on every random observation set. This is an estimation error,
not evidence that the optimiser failed to converge.

The noisy tracking test now retains its per-trial position, camera count and
pixel RMS limits, checks convergence within 0.25 degrees of the fit started at
the exact truth, and requires angular RMS below 0.5 degrees across 50 trials.
A separate 50-trial noiseless test requires position error below 10 micrometres
and orientation error below 0.01 degrees. This explicitly replaces the former
per-trial noisy angular assertion; it does not claim a guaranteed one-degree
noisy pose. Zero noise uses a scaled unit-normal distribution, avoiding a
zero-standard-deviation distribution.

The other failures have different meanings:

- **Re-acquisition, seed 19/trial 36:** a retained incorrect association produced
  2.9821 degrees of error and 0.662 px RMS. Starting at truth yielded 0.5694
  degrees and 0.264 px. Recompute associations after each optimisation step at
  the tightest gate. Coarse-gate batches remain fixed: changing them also
  accepted an incorrect hand in the macOS negative test. The original
  re-acquisition position/orientation bounds and acceptance thresholds remain.
  This is the only retained production solver change.
- **Two rings, seed 29/trial 2:** default stereo acquisition refuses the noisy
  scene after 20,000 hypotheses: the attempted refinement has 0.806 px RMS and
  0.771 coverage. Refusal is permitted by the bounded acquisition API. The test
  previously required every noisy stereo attempt to succeed, unlike the
  single-ring test's 90% success criterion. The two-ring test now exercises the
  runtime's existing stereo-first/aligned-IMU orientation recovery sequence
  (`t_constellation_tracker_joint.cpp`), requires accurate acquisition of both
  rings in all ten scenes, verifies blob ownership between them, and requires
  at least 18 of the 20 acquisitions to succeed through stereo alone. It does
  not claim guaranteed first-frame cold acquisition without IMU alignment.

Changes to bootstrap shortlist diversity, score uniqueness and shortlist size
were investigated and removed: they either failed this refusal fixture or
accepted a mirror-image ring. Increasing only the search budget also failed the
frozen refusal case. No production bootstrap code, search budget or scoring
policy is changed. Wrong-model rejection tests remain strict.

No numeric-derivative change is retained: central differences barely changed
the noisy failure and would double projection evaluations. No random seed,
noise level, acceptance RMS threshold or wrong-hand rejection test was relaxed.

## Cross-platform regressions

`tests/joint_pose_solver_fixtures.hpp` freezes the three original Linux inputs and a mirror-image negative input
as round-trippable float values, including their observations and poses.
`Joint solver reproduces frozen Linux regression inputs` exercises them on
both platforms, independently of standard-library distribution algorithms and
compiler argument evaluation order. Original random stress tests also remain.

The fixtures come solely from this repository's synthetic ring/camera tests;
no third-party tracking code or recordings were imported. New source and
fixtures use BSL-1.0. Upstream submission would require human DCO certification
under `CONTRIBUTING.md`; no sign-off or upstream submission is implied here.

## Reproduce

Ubuntu 24.04, x86_64, GCC 13.3.0, Eigen 3.4.0, Ceres 2.2.0,
CMake 3.28.3, Ninja 1.11.1, Vulkan headers 1.3.275.0 and glslang 15.1.0.
The Ubuntu container image digest was
`sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55`.
It ran under Docker x86_64 emulation on an Apple M5 MacBook Air; execution times
are validation costs, not native Linux performance measurements.

```sh
apt-get update
apt-get install -y build-essential cmake ninja-build pkgconf python3 \
  glslang-tools libceres-dev libeigen3-dev libhidapi-dev libudev-dev \
  libusb-1.0-0-dev libssl-dev libvulkan-dev
cmake -S . -B /tmp/monado-linux-check -G Ninja
cmake --build /tmp/monado-linux-check --parallel 8
ctest --test-dir /tmp/monado-linux-check --output-on-failure --parallel 4
```

No CMake feature overrides are used for Linux, matching the default Ubuntu CI
configuration. For a quick targeted check:

```sh
cmake --build /tmp/monado-linux-check --target tests_joint_pose_solver
/tmp/monado-linux-check/tests/tests_joint_pose_solver 'Joint solver reproduces*'
```

## Validation

The full Ubuntu suite passes 48/48 (331.37 s under x86_64 emulation). The complete
macOS solver executable passes 1,089 assertions in 13 cases. The full macOS
suite passes 78/78, including 21 GPU cases and three service recovery cases.
The full Linux build has no compiler warnings. The unchanged solver reproduces the re-acquisition failure in the frozen test.
The stereo fixture now validates the existing aligned-orientation recovery
contract, and the mirror fixture guards rejection. Logs are
preserved in [the evidence directory](macos-graphics-integration-results/).
The complete macOS suite passes 78/78 (48.35 s), including 21 GPU cases and
three service recovery cases. The recorded negative control fails only the
original 2.9821-degree re-acquisition check.

A preliminary native CPU comparison alternated the before/after 100-trial
re-acquisition fixture for 12 pairs, discarding the first warm-up pair. Median
test durations were 19/20 ms before/after, about 190/200 microseconds per trial.
These include fixture generation and test overhead, with millisecond reporter
precision; they are not real controller replay or Linux performance evidence.
No new blocking wait or additional optimisation iteration is introduced.

GPU/IPC macOS tests require ordinary host access to Metal devices and local
sockets. A filesystem-sandboxed test run failed those permissions; rerunning
with host access passed. This was not a source regression. No headset run or
installed runtime change was performed for this solver fix. Real controller
replay/performance validation remains separate from synthetic correctness.
