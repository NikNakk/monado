<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# PS Sense 6DoF integration

Integration date: 2026-10-04. **The runtime port is implemented and locally
validated, ready for an opt-in OpenXR hardware trial.** Hardware validation and
Linux CI remain pending. The review and original implementation plan below
explain the selection; the completed checks and launch procedure follow.

## Sources reviewed

- Destination: `/Users/nickkennedy/Code/monado-2`, `macos-upstream-clean`,
  `c16019c55` (`docs: record integration CI validation for compositor fixes`).
- Source: `/Users/nickkennedy/Code/monado`,
  `claude/pssense-mr2940-evaluation`, `e54478a16`.
- Common ancestor: `401a34931f3376c3d7bd81957af6ed5ceeaae189`.
- The source has **uncommitted runtime integration**: seven modified tracked
  files and the new `target_psvr2_sense_tracking.{c,h}`. Importing only the branch
  tip would miss this hookup. Those files were snapshotted separately for the port; Claude's checkout was
  left untouched.
- A Git merge preview finds conflicts throughout the tracker, Sense driver,
  headset, prober, build files and status documentation. It is not a mechanical
  branch merge: much of the source's original upstream-port work is already
  present here in a newer form.

The imported evidence is:

- [Optical tracking and illumination history](pssense-optical-tracking.md).
- [Camera calibration](psvr2-camera-calibration.md).
- [LED command waveform](pssense-led-blink-waveform.md).
- [Fusion evaluation](macos-pssense-upstream-fusion-evaluation.md).
- [MR 2940 front-end evaluation and latest hardware results](macos-pssense-mr2940-frontend-evaluation.md).

These documents preserve their dated source-branch history, including old build
commands. Their opening status sections are historical; use this review and the
latest dated hardware sections for the integration decision.

## What the recent work establishes

The live path to carry over is **M3 joint multi-camera optical tracking plus
the existing IMU/optical EKF**. M1 refines one controller pose against all cameras;
M2 bootstraps from stereo geometry; M3 assembles exposures and manages tracking,
three-solve confirmation and reacquisition. Its worker keeps bounded pending
work rather than accumulating a camera backlog. The EKF rewinds delayed optical
measurements and propagates with controller IMU samples. Position validity
expires after 300 ms without an accepted optical pose; preserving that limit
avoids the documented failed-reacquisition feedback loop.

Do not adopt the sliding-window fusion as the runtime backend. The source's
comparison recommends keeping it offline: it costs about 300 times as much as
the EKF and the dominant observed failures are optical/illumination failures.
Keep the offline evaluation and tests available for reproducibility.

Recent changes and their evidence:

| Change | Source commits | Evidence / limit |
| --- | --- | --- |
| Measured LED-model correction | `5a17bc3f4`, `3185a8a4e` and associated evaluation notes | Right tracked fraction 82% → 92% in the 3 Oct A/B; held-out replay also improves. Fitted on one controller pair and the combined rig calibration. |
| Narrow LED scan bridges one weak step | `1402b9185` | Follow-up grip run: left 80%, right 95% tracked. Corrects the left's poor illumination lock in the earlier A/B. |
| Hard-case hardware evidence | `98ed30fbe` | Rings resting 122 mm apart remain distinct; with 67 cm headset motion, world position holds within 2.5–4.3 mm RMS; fast swings tracked 93% / 100%. These are diagnostic sessions, not OpenXR runtime tests. |
| More hinted retries and 1.6 ms wide pulse | `b4c54372b` | Simulator/offline tested only. The earlier 2.1 ms full scan precedes the recurring always-lit fault, which still needs a controller power cycle. |
| Right static-marker button | `461a66dcb` | Options, rather than nonexistent Create, marks right-controller intervals. |
| Distance threshold investigation | `e54478a16` | Blob detection limits arm's-length coverage. Lower thresholds recover blobs offline; retain 80/180 defaults pending hardware A/B. |

The concrete calibration exists at
`~/Code/psvr2-datasets/calibration/20260926-charuco-mode4-combined-head.json`.
It includes the fitted `head_from_camera0_xrt`. It remains an experimental
`runtime_usable: false` artifact: deliberate 6DoF opt-in may use it with an
explicit diagnostic warning, but normal runtime startup must not select it
automatically or describe it as validated production calibration.

## Compatibility decisions

1. **Keep this branch's upstream tracker and optimizer.** MR 2940 is already
   integrated here. The source uses the older per-camera OpenCV PnP interface;
   replacing its entire constellation directory would undo current Ceres
   refinement, covariance/whitening data and match-parameter support. Port the
   joint solver additions onto the current classes and keep the existing
   per-camera path when joint tracking is off. Preserve current camera-model,
   quaternion and optional-pose APIs.
2. **Extend the callbacks deliberately.** The source changes pose delivery from
   a void/mutable callback to a bool/mutable callback, adds raw/LED-shaped blob
   counts and joint-camera metrics, and records device IMU. Audit and adapt all
   consumers (including Rift), rather than copying a public header over them.
   Rejected/tentative joint poses must not become driver priors. Empty camera
   frames must still inform LED illumination and exposure assembly.
3. **Preserve current controller behaviour when opt-in is absent.** Keep
   generic-controller bindings, `XRT_BUS_TYPE_ANY` discovery and the existing
   synthetic position/arm options. Actual optical attachment takes precedence
   over synthetic positioning; failed setup retains the pre-existing fallback
   and must not falsely claim optical position tracking.
4. **Adapt the builder.** Source uses `t_builder_roles_helper`; destination uses
   `t_builder_options`. Carry over camera-exposure timing connections for both
   controllers, set opt-in defaults before device construction, and start the
   optical pipeline after both devices exist but before space setup. Controllers
   must share the HMD tracking origin so no second origin offset is applied.
5. **Preserve headset/compositor work.** Apply only the camera/timing APIs and
   lifetime changes needed by Sense. Retain shared tracking, current SLAM
   prediction, gaze/passthrough and all validated presentation/image-reuse
   corrections. Do not overwrite the source-independent macOS status documents.
6. **Own shutdown explicitly.** The prototype registers a headset teardown hook
   and drains camera sink pushes outside the data lock. Verify actual device
   and frame-context destruction ordering here; stop acquisition, detach both
   controllers, stop/drain tracker and queues, then free state while referenced
   devices still exist. Cover partial construction and hook-registration
   failures. A sink replacement must not free an object still used by USB or
   queued work. Avoid a controller-lock/tracker-lock inversion during detach.
7. **Keep Linux defaults.** The new runtime pipeline is opt-in; Linux full USB
   streams and default upstream per-camera tracking remain intact. Do not
   replace OS HID/prober code with older copies or add macOS defaults globally.

## Buildable implementation steps

1. **Optimised build and evidence baseline.** Use a fresh build directory owned
   by this checkout with `CMAKE_BUILD_TYPE=RelWithDebInfo`. Build the service,
   OpenXR runtime, CLI and diagnostics. Check actual compilation flags, not only
   the cache. Record the configuration used for later comparisons.
2. **Joint tracker and recording.** Add the joint solver, stereo bootstrap,
   exposure worker, LED-shape counts and dataset extension records to the
   current tracker. Retain the existing fallback optimizer. Import the solver,
   dataset, pose-metric tests and replay/synthetic tools. Fixed distortion codes
   must explicitly support this branch's PINHOLE/RIFT_CV1 enums and old source
   recordings; do not silently reinterpret raw enum values from upstream
   recordings. Keep optional Ceres sliding-window evaluation separate from
   runtime linkage.
3. **Sense filter and LED scheduling.** Port the EKF, online gyro bias,
   delayed-IMU capture, LED bootstrap/ownership, strict retries, narrow-gap fix,
   measured correction and safe LED shutdown onto the current Sense driver.
   Keep current bindings and synthetic fallbacks. Import their C++ tests and
   persistent session/scoring scripts plus Python tests.
4. **Runtime hookup.** Adapt the uncommitted common helper and builder changes,
   share the calibration loader with the CLI, and implement safe camera-sink
   teardown. Add calibration/lifecycle/failure-path verification. The intended
   service options are `PSVR2_SENSE_6DOF=1` and
   `PSVR2_SENSE_6DOF_CALIBRATION=/absolute/path/to/calibration.json`. Existing
   individual settings remain overridable. Do not set these in the client only:
   the service owns tracking even with client-hosted compositing.
5. **Regression checks and user hardware run.** Build/test macOS default and
   feature-enabled configurations, and Linux with the opt-in absent. Replay
   identical stored sessions through the source and ported joint+filter paths;
   compare acceptance, jitter, reprojection, reacquisition and CPU cost. Then
   give the user a concrete service/runtime launch command using the optimised
   binaries and a controller-visible OpenXR application. Record the integration
   commit, calibration, build type and all settings with the hardware result.

Import the calibration/session scripts and tests alongside their CLI commands,
preserving executable modes. Include the supporting geometry, ChArUco and
head-from-camera estimator modules rather than leaving broken cross-references
or Python imports. Historical diagnostic tools may follow the live path in a
separate buildable step, but the evidence documents should remain together.

## Optimisation and display pacing

The 25 Sep M1 replay in the source notes measures 4388 µs p50 at the empty-build-
type configuration against **48 µs p50 at `RelWithDebInfo`** (approximately
90×). This supports the user's observation of a roughly 70× slowdown and makes
optimisation a requirement for meaningful 6DoF testing. Do not silently fall
back to an unoptimised `build-sense` binary in the imported session runner.

Several local build directories have empty `CMAKE_BUILD_TYPE`, and some point
to `/Users/nickkennedy/Code/monado` despite being visible here. `build-release`
belongs to this checkout and is configured Release, but its existing artifacts
are not evidence that the current integration was built with those flags.
Use a new named directory and verify `CMAKE_HOME_DIRECTORY` and compile commands.

Optimise the **whole runtime**, including the compositor, rather than applying
`-O2` just to one solver file. Keep Debug builds available and avoid changing
Linux/default CMake policy as part of this experiment. Dependencies now include
Ceres for this branch's upstream tracker and OpenCV for the imported recording/
calibration and legacy diagnostic tools; account for OpenCV 5's geometry module.

Any display-pacing improvement is still a hypothesis. First compare optimised
against the prior build with Sense/cameras disabled and the same UE workload,
Game Mode, pacing options and passive buffered traces. Then assess the extra
6DoF camera/tracker load. Track physical presentation gaps, completion-to-output
delay, pose-query cost and sensor freshness; do not attribute an improvement
from the optimisation-only comparison to controller tracking.

## First hardware acceptance

- Actual left/right grip and aim positions reach OpenXR in the HMD's world
  space, including through the service IPC and client-hosted compositor path.
- Controllers resting on a fixed surface stay fixed while the headset moves;
  controllers close together remain distinct, and normal-grip swings track.
- Hiding and returning a controller reacquires without unbounded IMU drift or
  synthetic-position contamination; valid/tracked flags expire as documented.
- Startup/shutdown/restart works with either controller, both, missing/bad
  calibration and opt-in disabled; no stale sinks or teardown callbacks remain.
- Head tracking and display pacing are compared to the optimised baseline.

Do not reopen the rejected full sliding-window runtime fusion, legacy
camera-pose averaging or speculative LED-model changes without new evidence.
The latest source pulse/retry mitigation and reduced blob thresholds still
need their own hardware validation; keep the thresholds unchanged initially.

## Completed port and local validation (2026-10-04)

Runtime commit: `80481ea41` (`tracking: integrate opt-in PS Sense joint 6DoF runtime`).
Source evidence import: `48f5dfb89`.

The port includes the source tip and its uncommitted runtime hookup. The
four-camera joint worker, stereo bootstrap, EKF, gyro-bias estimation, LED
bootstrap/retry mitigation, measured LED correction, timestamped native HID
reports, recording/calibration CLI tools, replay/synthetic/fusion tools and
session/calibration Python tools are integrated. Sliding-window fusion remains
an offline library; the service uses the joint tracker and EKF.

Compatibility adaptations preserve the upstream Ceres optimizer, covariance,
whitening, minimum-LED and degeneracy gates, Rift's legacy callback, current
Sense bindings/arm options, HMD prediction and compositor. The source
matcher correction is selected only by the experimental Sense model: blobs
match LEDs one to one, preventing duplicate correspondence counts. Other
devices retain upstream matching; both policies have regression coverage.
Default Linux USB streams and macOS camera opt-in remain unchanged. Future LED
scheduling and the experimental controller clock path stay off by default;
6DoF opt-in selects them before device construction.

The runtime shares the HMD tracking origin and composes camera-0 extrinsics with
the HMD pose at exposure. Teardown drains outstanding camera pushes, detaches
controllers without a tracker/controller lock inversion, stops frame queues and
the tracker, then frees its state before the HMD/controllers are destroyed.
Sink/hook collisions are rejected; failed startup only releases sinks it owns.
LED-off commands are sent before stopping the controller output thread, including
guards for partial startup. Tests cover calibration rejection, head transform
composition, sink replacement/draining, hook collision, legacy/acceptance
callbacks and timestamp-less HID backends. Actual hardware shutdown still needs
the acceptance run below.

New recordings use tagged magic `0x066A4C51` with fixed model codes, including
PINHOLE and RIFT_CV1. The reader also accepts old `0x066A4C50` source recordings
with their fixed codes. Old upstream recordings used raw enum values with the
same untagged magic: pass `--upstream-distortion-codes` when replaying those.
That override is rejected for tagged recordings. All seven models, legacy
read modes, invalid codes, extension records and truncation are tested.

Validation on this Apple Silicon Mac:

- `build-macos-sense-rel`: full service, OpenXR runtime, CLI and diagnostic app
  built with `RelWithDebInfo`; effective commands contain `-O2 -g -DNDEBUG`
  for the solver, driver and compositor. No compiler warnings.
- CTest: **43/43 passed**, including the new tracker/filter/fusion/dataset and
  runtime wiring tests and the existing IPC, controller, foveation and remote
  layer tests. Unix socket/shared-memory/remote-layer tests need normal OS
  access; their initial sandbox-only failures disappeared outside the sandbox.
- Python: **36/36 passed** (`test_psvr2_*.py`), with NumPy/SciPy available in
  the source checkout's existing venv. No environment packages were changed.
- `build-macos-sense-all`: full build with all optional OpenXR foveation,
  depth, generic-controller and timing features enabled; **43/43 tests pass**,
  no compiler warnings.
- `build-macos-sense-minimal`: optimised full build with constellation tracking
  and OpenCV disabled succeeds, checking optional CLI/build dependency gates.
- `build-macos-sense-noopencv`: full tracking-enabled optimised build without
  OpenCV succeeds with no compiler warnings; **43/43 tests pass**. This covers
  the dependency combination used by Linux CI, but runs on macOS.
- The supplied combined-head calibration was loaded through the runtime
  parser without hardware: all four cameras and the head extrinsic validate.
  The expected `runtime_usable=false` warning is emitted. The tagged synthetic
  recorder/replay round trip also succeeds.
- Linux was not run locally: Docker Desktop's daemon/socket is unavailable.
  `.github/workflows/linux-build.yml` remains the required default-options gate.

Identical recorded inputs were replayed using `--tracker-filter --tracker-csv`
in source `build-sense-rel` and the port. Both **entire pose CSVs are byte
identical**, with matching acceptance/camera/RMS distributions:

| Session | Exposures | Left accepted | Right accepted | Left RMS px p50/p95 | Right RMS px p50/p95 |
| --- | ---: | ---: | ---: | --- | --- |
| `20261003-235855-corr-on-grip-2` | 3597 | 2837 | 3382 | 0.317 / 0.633 | 0.278 / 0.656 |
| `20261004-000405-hard-cases` | 3596 | 2415 | 2647 | 0.468 / 0.775 | 0.469 / 0.603 |

These are replay checks, not a new hardware run or controlled CPU benchmark.
The earlier speed evidence establishes the optimisation requirement; display
pacing remains an independent measurement.

## Build and run the opt-in OpenXR trial

From this checkout:

```sh
cmake --preset macos-sense-relwithdebinfo
cmake --build --preset macos-sense-relwithdebinfo --parallel
ctest --preset macos-sense-relwithdebinfo --output-on-failure
python3 scripts/macos/prepare-pssense-6dof-runtime.py \
  "$HOME/Code/psvr2-datasets/calibration/20260926-charuco-mode4-combined-head.json"
```

The preset builds the whole runtime with optimisation; it uses Homebrew Python
because the older system Python cannot generate this branch's runtime manifest.
The preparation command validates the checkout, cache and effective solver,
driver and compositor optimisation flags. It writes an isolated LaunchAgent
at `build-macos-sense-rel/pssense-6dof.plist` and prints three commands to load
it, run `psvr2-openxr-test --generic-controller` with client-hosted compositing,
and unload it after the app closes. **Preparation does not start hardware.**
The agent and client share an isolated Unix socket directory and Mach endpoint;
the installed runtime is not overwritten. The agent sets `XRT_NO_STDIN=1`
so launchd's closed stdin cannot terminate the service. Close other headset tools and unload
the existing hardware Monado agent first because USB ownership is exclusive;
restore that registration after the trial. Logs go to
`build-macos-sense-rel/sense-service.log`.

The service receives `PSVR2_SENSE_6DOF=1` and an absolute
`PSVR2_SENSE_6DOF_CALIBRATION`. Setting them only on an IPC client does not enable
tracking. Existing low-level environment overrides are preserved. The initial
80/180 blob thresholds are unchanged; do the separate arm's-length threshold
A/B only after the initial run. The experimental calibration emits a warning
because its `runtime_usable` field is false.

For recording rather than OpenXR, use `scripts/psvr2_sense_session.sh NAME
CALIBRATION.json [SECONDS] [NOTE]`. It now requires an optimised CLI build,
records the cache's build type/source, and disables the runtime hookup so its
own diagnostic tracker does not collide with it. New Python calibration/scoring
tools need NumPy/SciPy and ChArUco capture tools additionally need OpenCV Python.

Record the final integration commit, calibration checksum, launch environment,
log and headset observations against the acceptance checklist above. Test with
Sense disabled using the same optimised build before attributing any display
pacing change to tracking or optimisation.

### LaunchAgent startup correction (2026-10-04)

The first user trial with `7b0ebe154` started the experimental tracker for both
controllers and initialised the PS VR2 compositor, then exited with code 0
before the client could obtain shared memory. The generated isolated agent
omitted `XRT_NO_STDIN=1`: the macOS main loop monitors stdin by default and
interprets launchd's closed stdin as a shutdown request. The resulting missing
Unix socket/`recvmsg: no data` messages are startup/lifecycle symptoms, not solver
failures. The generator now forces no-stdin, matching the normal XPC bootstrap
helper. Reload the agent after regenerating or correcting its plist; changing
the file does not change launchd's already loaded environment. The corrected
hardware run remains pending.
