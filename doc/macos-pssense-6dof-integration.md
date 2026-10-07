<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# PS Sense 6DoF integration

Integration date: 2026-10-04. **The opt-in runtime path has user-confirmed
OpenBrush 3D painting and game sessions lasting 10–20 minutes.** On 2026-10-08,
the user confirmed that the combined quick-lock, headset-detector and reconnect
defaults have already been exercised in games. This supersedes the pending
combined-game check at the end of the October 5 record. Exact game names,
commits, settings and instrumented loss statistics were not recorded for this
confirmation; it establishes practical sustained use, not complete coverage of
all reconnect, occlusion or teardown cases.

The Sony-like LED profile and oriented reacquisition improved the earlier
left/right imbalance and avoided lockouts in the recorded successful sessions.
Earlier LED faults remain historical evidence rather than an established fault
in the current profile. Broad reliability validation and the latest Linux CI
remain outstanding. The dated investigation below preserves the evidence and
implementation history.

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
and unload it after the app closes. With `--generic-controller`, the app now draws
6 cm grip cubes and 40 cm aim rays: cyan left, orange right. Both are located in
the app reference space at predicted display time. Grey means both pose
components are valid but at least one is untracked; missing position/orientation
validity or an inactive action hides the marker. Terminal logs report flags,
tracking status and positions, periodically and on flag changes. This is actual
OpenXR grip/aim placement; the diagnostic does not fabricate positions. Close and
relaunch the test app to pick up a rebuilt binary; the tracking service need not
restart for this client-only change.

 The agent uses `RunAtLoad=false`: loading
it registers the Mach endpoint; the client's XPC activation starts the service
on demand. **Preparation does not start hardware.**
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
the file does not change launchd's already loaded environment. The subsequent
hardware run starts successfully; controller observations are recorded below.

The isolated agent also now uses `RunAtLoad=false`, matching the normal direct
XPC helper. The original test generator eagerly started it at bootstrap. After
reloading the corrected plist, it stays stopped until a client requests the
registered Mach service. No-stdin remains required once launchd starts it.

### Controller visualization checks (2026-10-04)

The optimised diagnostic app rebuild passes without compiler warnings. Three
mocked OpenXR tests (21 assertions) verify left/right grip placement, rotated
local -Z aim rays, the reference space and display timestamp, invalid/inactive
and failed queries, valid-but-untracked colouring, and keeping controller/gaze
action sets active together. The visual result on the headset remains for the
user to validate. The earlier `--generic-controller` implementation logged
only action state and did not locate/render controller poses.

### First controller visualization hardware report (2026-10-04)

The user reports that the right works well and the left never starts tracking
visibly. The running service identifies itself as `7b0ebe154`; the controller
visualization client was rebuilt at `5ae61987d`. The corrected LaunchAgent starts
and serves the client successfully.

The service log shows that the left acquired an LED timing lock and accepted
many optical poses earlier in this service lifetime. It subsequently lost the
lock after 301 exposures without detected illumination, exhausted four hinted
retries, and repeatedly failed full scans with `wide_peak_below_minimum`.
During client 3, the left accepted only six optical poses in two short bursts,
while the right continued accepting poses. The left's subsequent wide scan
reported zero lit camera samples at every phase, despite the right tracking.
No `stuck_lit` event was logged. These observations point to optical/illumination
reacquisition; they do not establish its cause or exclude a separate client pose
problem. Position validity expires after 300 ms without accepted optical data,
so sustained loss should hide the diagnostic cube.

A snapshot of the log, loaded plist source and count summary is retained in
`build-macos-sense-rel/diagnostics/20261004-left-reacquisition/`. This is an ignored
local evidence directory. Controlled left-ring visibility and button/pose-state
observations remain necessary before changing illumination or blob thresholds.

The user subsequently confirmed that holding the left ring 30–50 cm in front
of the headset does not make it appear, while the right starts almost
immediately. Button response could not conveniently be checked from terminal
output while wearing the headset. The diagnostic now adds head-relative LEFT
and RIGHT panels below the fixation cross, independent of controller pose:
FACE 1/2 (Square/Triangle on left, Cross/Circle on right), TRIGGER, GRIP
(L1/R1) and STICK show green on input, grey when released and red when their
OpenXR action is inactive. Bars show trigger/grip level and stick deflection.
The footer reports TRACKED, POSE ONLY or NO POSE separately from input state.
Close and relaunch the same `--generic-controller` command after rebuilding;
this client-only addition does not require restarting the service. It does not
yet resolve the left optical acquisition failure.

The optimised client rebuild succeeds without compiler warnings. The controller
visual checks pass all 29 assertions in four cases, including visible input
panels with missing controller poses, head-relative placement, active input and
inactive-action colours. Headset readability remains to be confirmed.

### Left input loss and HID output failure (2026-10-04)

The user confirmed that left buttons also fail to respond in the visual panel,
while right input works. This broadens the investigation beyond optical
acquisition. The original service log contains a single
`Failed to send output report: -1` before the visualization clients connected
(snapshot line 35852). That message did not identify the hand. The driver's
loop exits on any failed output write, stopping input reads, IMU propagation
and future LED writes together; no automatic restart exists for that thread.
Left gyro-bias updates cease before this error, while right updates continue.
The evidence makes an exited left I/O thread a likely cause, but hand attribution
and the native IOKit failure reason are not yet proven.

On macOS, failed output writes now leave input reads running and retry fresh
settings after 100 ms. Warnings identify the hand and are rate-limited; a
successful write logs recovery. Genuine input errors still terminate the loop
and now identify the hand explicitly. Non-macOS write-failure behavior is
preserved. The IOKit backend also logs the native failing IOReturn, rate-limited,
instead of reducing the only diagnostic evidence to `-1`.

The hardware log before this fix is saved in
`build-macos-sense-rel/diagnostics/20261004-left-no-input/`. The service must be
restarted by the user to replace the already exited thread and load the rebuilt
binary. No hardware restart was performed during investigation. This correction
does not establish whether the underlying write failure was transient or a
controller disconnect; the next run's hand-labelled errors and input behavior
will distinguish them.

The optimised service rebuild and Sense tracking tests pass. A hardware-free
mock exercises the actual controller thread: input reaches `update_inputs`
while three output writes fail, a later write recovers, and a subsequent input
disconnect stops further output. Hardware confirmation remains pending.

When rebuilding after a commit, build the whole `build-macos-sense-rel` tree
before restarting the agent. The IPC handshake checks the embedded Git version
of both `monado-service` and `libopenxr_monado.dylib`. Building only the service
can leave the runtime library behind; rebuilding both while a service is running
can leave the process behind. The user's subsequent conflict had matching
`4a03bc0fb` binaries on disk but the latest service startup still identified
`f326247b5`. Close clients, finish the full build, boot out/bootstrap the isolated
agent, then reopen clients. Do not bypass this check with `IPC_IGNORE_VERSION`.

### Local default preparation and remaining LED fault (2026-10-04)

The user confirms the corrected setup works well and requests native and Wine /
OpenVR use as the local default. `scripts/macos/rebuild-both` now explicitly
configures the existing native `build-wine` and x86-64 client build with
RelWithDebInfo and generic-controller interaction enabled. Keep their existing
paths and other feature selections; build both clients and service together.
The script's final kickstart is a user hardware action.

`scripts/macos/prepare-local-sense-default.py CALIBRATION.json` prepares a
persistent normal-endpoint LaunchAgent from the existing normal plist, retaining
its tuning while enabling Sense with the supplied calibration. It prints the
user steps to unload both old registrations, copy the prepared plist into
`~/Library/LaunchAgents`, and bootstrap the normal agent. It does not start or
stop hardware. Generated native-client environment instructions select
`build-wine/openxr_monado-dev.json`, client-hosted compositing, and the normal
endpoint. Existing Wine launchers must retain their x86-64 manifest override.
The macOS SDK loader falls back to `/usr/local/share/openxr/1`, currently pointing
at Meta XR Simulator on this machine, so service registration alone does not
make unconfigured native applications select Monado. Use the generated client
environment for the current shell. The preparation tool also writes a companion
client-environment LaunchAgent that sets those two variables with `launchctl
setenv` at login for subsequently launched GUI apps. It does not start hardware.
Restart already-open terminals/apps to inherit that environment. Wine launchers
must continue overriding XR_RUNTIME_JSON with the x86-64 manifest.

The user still observes the IR-ring-always-on/status-LED-off fault. This is
distinct from the exited I/O thread: Claude's historical fault runs had healthy
input and output traffic, and LED_ALL_OFF could not clear it. All seven located
historical onsets followed period-42 wide pulses; the current opt-in runtime
already defaults to period 32. Thus shorter pulses are a mitigation, not a
proven cure. No command-based recovery is established; power cycling remains
the only demonstrated recovery.

The next useful controlled run should start from power-cycled controllers,
verify period 32 on the wire with `PSSENSE_TIMING_DIAG=1`, and enable
`PSSENSE_INPUT_DIAG=1` to capture ignored status/counter bytes at onset. Compare
against `PSSENSE_LED_BOOTSTRAP_TRACK=0` to remove phase-adjustment probes while
retaining initial acquisition and loss-triggered rescans. Keep rings in view;
record whether the onset follows a full scan, probe or write failure. This is
an A/B investigation, not a new default. Scan steps already have eight settling
and eight measurement frames, so simply increasing dwell is not the first
untried fix. If content transitions are implicated, compare command application
and transition semantics with Sony's PRESCAN/BROAD/BG/STABLE state sequence.
Those phases have different cycle-position semantics; merely switching the
phase enum is not a correct implementation. Any Toolkit-derived work must
first check licensing. See the original
[fault ledger](pssense-optical-tracking.md#the-always-lit-fault-what-the-logs-and-psvr2toolkit-say-26-sep).

The latest `4a03bc0fb` hardware log confirms `pulse_us=1600.0` for full scans,
with no HID output failures in that service lifetime. Despite the user's visual
fault report, no `event=stuck_lit` was logged; detection is not yet demonstrated
for this occurrence. This remains an unresolved LED-control fault, not proof of
an output-transport failure. Historical recovery attempts included LED_ALL_OFF,
INIT/ALL_ON/PRESCAN/DEBUG sequences, status-LED toggles and calibration rereads;
none cleared the stuck ring, while vibration still worked.

Local-default preparation completed: the ARM `build-wine` service, ARM OpenXR
runtime and x86-64 native Wine runtime build successfully without compiler
warnings and all identify `v25.1.0-2090-g1e9421909`. Effective solver, driver and
compositor optimization was checked by the preparation tool. The persistent
normal-endpoint plist and native environment snippet are in
`build-wine/local-sense-default/`. Registration, client selection and the new
Wine/native hardware trial remain user steps; the existing live service was
not restarted or unloaded during preparation.

### Both controllers lock out after clients disconnect (2026-10-04)

The user reports that leaving the service running with no clients eventually
caused both controllers to enter the IR-always-on/status-LED-off state. An active
XR application is therefore not necessary for the observed fault. This does not
establish the exact onset time or a timer-based cause: the service continues
camera tracking and LED scans independently of client presence.

In the latest `4a03bc0fb` service log, after client 1 disconnected at line 71589,
each controller performed nine further full scans, all failed, with no accepted
optical poses. No `stuck_lit` event or HID output failure identifies the onset.
The snapshot is retained in
`build-macos-sense-rel/diagnostics/20261004-both-stuck-after-idle/`.

The prepared normal local default already has `IPC_EXIT_WHEN_IDLE=1` and a
5000 ms delay. The isolated test agent omitted these settings, allowing endless
idle scanning. Its preparation script and generated plist now include the same
idle-exit policy. Reload is required to change the loaded environment; no live
service was stopped or restarted here. The next client activates the service
on demand. This limits idle exposure to the fault-triggering command stream; it
does not cure the controller firmware state or clear a ring already stuck on.

### Controlled lockout investigation prepared (2026-10-04)

The next step is to locate the onset in the command stream, rather than infer it
from the final stuck state. `PSSENSE_TIMING_DIAG=1` now also logs each actual HID
write as `PSSENSE_OUTPUT`: full report bytes, start/end monotonic timestamps,
result/expected byte count, latest input timestamp, estimated device time,
phase, LED sequence, pulse period, cycle position/length and flags/status-LED
field. This is opt-in; it does not change the transmitted report. Input diagnostic
summaries now include current ignored bytes as well as change counts, so busy
counters are still inspectable after the first 20 individual changes.

`scripts/macos/prepare-sense-lockout-trials.py --output NEW_DIRECTORY` prepares
three normal-endpoint service profiles without operating launchd or hardware:

| Trial | Phase probes | Forced full scans | Condition |
| --- | --- | --- | --- |
| A-steady | Off | Off | Both rings continuously visible; steady-lock control. |
| B-probes | On | Off | Same visibility; isolates phase-adjustment probes. |
| C-rescans | Off | Every 20 s after lock | Same visibility; isolates full-scan transitions. |

The initial profiles and detailed user procedure are in
`build-wine/diagnostics/20261004-lockout-abc/`. Each has a separate service log and
constellation recording. Power-cycle both controllers before each run, keep the
XR client open, hold rings unobstructed 30–50 cm away, and use the same optimized
build/calibration/period-32 pulse throughout. Observe for 180 seconds or until a
fault; record hand, approximate elapsed time, status LED, IR state and whether
buttons still respond. A spontaneous loss/rescan contaminates a steady-lock
control and must be noted. Repeats require a fresh evidence directory; a short
fault-free run does not establish prevention.

If A fails before any scan/probe, audit the steady command stream and clock
mapping first. Repeated faults confined to B implicate probes; faults confined
to C implicate scan transitions. Candidate prevention then follows the evidence:
disable implicated probes, replace the repeated full-scan fallback, or correct
command/state semantics. Do not blindly switch to BROAD/BG/STABLE: position
semantics differ from PRESCAN. Confirm any mitigation with verbose diagnostics
off afterwards; logging itself adds overhead. Idle exit remains enabled as an
exposure reduction, not a firmware fix.

A mocked real driver-loop test passes with tracing enabled, exercising failed
and recovered writes plus input disconnect. The captured hex reports have the
expected byte counts. No hardware was used for this validation.

The forced-rescan path was also found to retain the previous lock's phase hint,
contrary to the historical "full scan" description. It now clears that hint
only for the explicitly requested stress rescan, so trial C actually exercises
wide-to-narrow scanning. Ordinary acquisition and loss-driven rescans retain
their hinted behavior.

### Trial A: right lockout during steady timing lock (2026-10-04)

The user reports that the right controller locked out during A. The captured
service identifies `v25.1.0-2095-g6c76552ed`; the A plist disables phase probes
and forced rescans. Evidence is retained in
`build-wine/diagnostics/20261004-lockout-abc/A-steady/` (`service.log`,
`constellation.ctd`, `service.plist`). The log includes an initial short service
lifetime with a display-selection error, followed by the actual recorded run.
The recorded HID-write span is about 146 seconds, shorter than the requested
180 seconds. The user reports no fault during approximately the first minute, while the
headset was off and the controllers stationary; the fault was noticed only
after removing the headset at the end. Exact onset, post-fault buttons and
pre-run power-cycle confirmation remain pending.

Both controllers perform one narrow acquisition and reach timing lock. Neither
logs a subsequent loss, scan, scan failure or stuck-lit detection. The right
continues yielding accepted optical poses through the final camera frames.
Consequently, the reported fault is not confined to rescanning or phase probes;
the historical scan-only pattern must not be treated as a general exclusion of
steady-state triggers. LED visibility and accepted poses do not prove that the
controller is still obeying the requested blink schedule.

All 9,627 right and 9,626 left traced writes return their expected 78 bytes.
Ignored input fields remain unchanged except the previously active counter at
byte 14 and Bluetooth header at byte 23. There is no new clock snap after the
initial acquisition. These checks do not establish firmware acceptance of the
LED command or button behavior after the observed fault.

A significant transport delay occurs around 72.7 seconds from the first traced
write: right write at log line 26462 takes 806.945 ms; left at line 26463 takes
811.474 ms. Both report success. Their estimated cycle centers are about
63/68 ms ahead at write entry, and about 744 ms behind at return, assuming the
clock mapping remains applicable. Input servicing also stalls for about
0.82 seconds. This is a candidate scheduling hazard, not an identified onset:
SetReport return time does not reveal when the controller received the report,
and only the right is reported to lock out. Camera scheduling and optical
accepts continue across the stall. Negative right scheduling lead at the very
end of the log occurs during teardown and must not be mistaken for the
user-observed onset.

The 73-second delay falls within the user's possible onset window, but does
not establish causation. Repeat A with both controllers freshly power-cycled and an
explicit elapsed-time marker when the fault is noticed. Audit steady PRESCAN
re-latching, the actual HID transport cadence and late report behavior before
changing phase semantics. B/C are no longer necessary to establish that scans
or probes are not required, though they can still compare fault rates. No
prevention or recovery is validated by this run.

### Linux CI warning fixes and Rift isolation audit (2026-10-04)

[Linux run 37195947341](https://github.com/NikNakk/monado/actions/runs/37195947341)
built successfully, then failed its compiler-warning gate. GCC reported an
omitted `joint_camera_count` initializer, conditionally supported `offsetof`
on the replay's non-standard-layout fake device, and partially initialized
pose-metrics test aggregates. These are corrected with an explicit zero camera
count, a typed tracking-source owner link, and zero initialization followed by
field assignments. The warning gate remains unchanged.

[Contribution run 37195947368](https://github.com/NikNakk/monado/actions/runs/37195947368)
failed formatting on imported/edited files. Those files now pass the pinned
clang-format 23.1.1 check. Local REUSE also exposed missing metadata in five
imported notes and two calibration scripts; matching fork copyright/BSL-1.0
headers were added. Full contribution-style and REUSE checks now pass.

The audit compares the integration against the pre-Sense tree (`80481ea41^`):
Rift driver/builder, correspondence search, original Ceres optimizer and LED
refinement are unchanged. The legacy pose matcher body is identical, ignoring
whitespace and its renamed wrapper. Rift's calloc-initialized model keeps
`unique_blob_matches=false`, and its legacy callback does not edit the sample.
The new acceptance callback, filter, LED bootstrap and joint worker belong to
the Sense opt-in path. Optional HID timestamps leave existing Rift reads alone;
the unchanged Linux hidraw backend zero-initializes the added function pointer.

One process-wide boundary was strengthened: `CONSTELLATION_TRACKER_JOINT=1`
now additionally requires `T_CONSTELLATION_TRACKER_FLAGS_ALLOW_JOINT` from the
caller. Only Sense runtime and PS VR2 diagnostic/replay callers set it; Rift's
existing flags remain unchanged. Thus even an inherited Sense joint option
cannot select the new worker for Rift. Legacy callbacks retain the original
tracker pose-cache behavior, and LED shape counting is skipped unless a device
actually requests the new callback.

Regression tests verify legacy blob assignment/degeneracy, legacy sample
callback routing, opt-in rejection, and real tracker construction with the joint
environment option enabled: legacy stays per-camera while the explicit Sense
caller creates the joint worker. The optimized macOS full build has no compiler
warnings. All 44 CTest suites pass (three IPC/remote-layer suites needed a
sandbox-free rerun to create OS resources). Rift hardware has not been tested.
Linux CI still needs to run on the corrected revision; Docker is installed but
its local daemon is not running, so a Linux build was not claimed locally.

### First non-test application: OpenBrush (2026-10-04)

The user confirms that OpenBrush works and they could paint in 3D. This is the
first reported non-test application exercising the integrated Sense 6DoF path,
following the controller test application. The left controller still dropped
tracking more often than desired. This validates practical application use of
controller poses, not sustained reliability, quantitative alignment/latency or
a fix for the persistent IR LED lockout.

The normal LaunchAgent was verified loaded with `PSVR2_SENSE_6DOF=1` and the
optimized `build-wine` native service, starting on demand. Its latest logged
service version is `v25.1.0-2095-g6c76552ed`; association of that specific service
lifetime with OpenBrush is inferred from the local setup rather than an
application-name marker. Repository HEAD at report time is `27f5123a3`; do not
label the hardware run as validating its subsequent Rift/CI changes.

Next reliability work should distinguish left-controller optical dropouts
(visibility, illumination timing, correspondence and reacquisition) from the
persistent LED-command fault. Record both hands' tracking quality during real
app movement alongside any observable LED fault onset; successful painting alone
does not establish which cause dominates the left dropouts.

### Left/right asymmetry: offline analysis of existing runs (2026-10-04)

No new hardware run. Sources: the last service lifetime in
`/tmp/monado-service-launchd.501.err.log` (`6c76552ed`, 111 s of real use,
inferred to be OpenBrush), and `build-macos-sense-rel` `constellation_replay
--tracker-filter` over the 3–4 Oct CLI sessions. Device 0 is the left.

| Evidence | Left | Right |
| --- | --- | --- |
| Service: accepted poses per 5 s window | 0–300; lost 15–20 s and 45–60 s, ~300 after 65 s | 252–300 throughout |
| Service: filter reinitialisations / reacquisitions | 13 / 16 | 2 / 5 |
| Service: reacquisition gaps | mostly 1.1–5.9 s, hand moved 70–435 mm | mostly 317 ms, 5–114 mm |
| Service: matched p50, reprojection p50 when accepted | 27, 0.298 px | 25, 0.271 px |
| `000405-hard-cases` replay (both resting, symmetric) | 66.7% solved | 65.3% solved |
| `235855-grip-2` replay, after both locked (≥ 15 s) | ≈ 82% | ≈ 87% |

- **When the left is tracked, it tracks as well as the right**, and when both rest
  symmetrically they solve equally. A left-specific LED model, calibration or
  solver defect is therefore unlikely. Blob sizes against range also match.
- **The deficit is long losses plus slow reacquisition.** In the service run,
  the left's phase probes during losses mostly read `blob_too_dim` with about
  1–2 LED-shaped blobs at reference, early and late offsets: the ring was
  not seen at all, which looks like out of view or covered, not off-phase.
  Without frames this cannot separate posture (OpenBrush's palette hand)
  from a tracker failure.
- **Some phase disturbance exists on the left only.** Left probes moved the
  lock −200 µs and later +200 µs. A 378 µs left clock snap at 83.2 s precedes a
  reacquisition and a probe that found early dark and late lit. At 33.4 s a
  probe read 0.25/2.66/4.69 blobs (late brighter) but was discarded as
  `blob_too_dim`. Right probes were centred throughout.
- **CLI sessions start the left late.** Bootstrap scans are serialized and the
  left scanned second (first lock 11.6 s vs 5.1 s in `grip-2`), inflating
  whole-session left deficits.
- Unrelated anomaly: right `GYRO_BIAS event=still` reports about 20 deg/s
  (6, −18, 5) repeatedly while the left reports about 1.6 deg/s.

The joint worker's ordering does not disadvantage the left: tracked devices
refine in device order (left first) and bootstrap is a best-fit contest.

Next steps, in order:

1. **Classify each loss.** Implemented as `JOINT_LOSS`; see the next section.
2. **Mirrored hardware protocol.** Using the CLI session tool with frame capture,
   run identical mirrored movements with both hands, then swap which hand does
   the "painting" and which holds still. Also scan the left first. If the
   deficit follows the role, it is posture/visibility; if it stays with the
   left controller, it is the controller or its timing.
3. **Fix by category.** For out-of-view losses, make reacquisition faster (for
   example, IMU-predicted re-entry). For dark losses, act on probe imbalance
   and clock snaps. For lit losses, examine the bootstrap contest.

### Loss classification log: `JOINT_LOSS` (2026-10-04)

The joint worker now logs one `JOINT_LOSS` warning per device loss of at
least 250 ms, when the track is confirmed again. It is always on in the joint
path, rate-limited by losses, and changes no solve: replaying `grip-2` pushes
the identical 2837/3382 poses. In the Sense runtime, device 0 is the left.

Each exposure between the last confirmed solve and the next is put in one
class. LED-shaped blobs no device claimed count against a per-camera background
measured while every device is solved. A camera is "lit" with at least three
such blobs above background.

| Field | Meaning |
| --- | --- |
| `acquiring` | Tentative bootstrap/track, not yet confirmed (three solves) |
| `lit_multi` | Lit in two or more cameras while all other devices are solved: visible but not re-acquired |
| `lit_ambiguous` | As above, but another device is also unsolved, so the light may be its ring |
| `lit_single` | Lit in one camera only, which stereo bootstrap cannot use |
| `dark_in_view` | Nothing lit, predicted position inside some camera's image: suspect LED timing |
| `dark_out_of_view` | Nothing lit, predicted position outside every image |
| `dark_unpredicted` | Nothing lit after the filter's position expired (about 300 ms); location unknown |

`start_cameras`, `start_in_view` and `start_margin_px` describe the last
confirmed solve: cameras used, cameras it projects into and its best distance to
an image edge. A small margin suggests leaving the field of view.

Replay of 3–4 Oct sessions (`--tracker-filter`): in `234436-corr-on-grip`,
where the left's LED lock was known to sit at its window edge, left losses are
predominantly dark (186 `dark_in_view`, 1126 `dark_unpredicted` of 1846
exposures), consistent with that illumination fault. `000405-hard-cases`
shows 120 left `lit_multi` exposures, a real reacquisition delay with the ring
visible. `lit_single` is common in all three sessions. These replays are offline
checks of the classifier, not new hardware evidence.

Validation: full `build-macos-sense-rel` RelWithDebInfo build without
warnings; all 44 CTest suites pass; pinned clang-format 23.1.1 clean. No
synthetic test covers the classification itself.

### Phase tracking A/B in OpenBrush, and a minimum lit window (2026-10-05)

Three user OpenBrush sessions with `JOINT_LOSS` logging (`27f5123a3` plus the
uncommitted loss diagnostic), all with the same calibration:

| Session | Phase tracking | Left lit fraction / filter resets | Right lit fraction / filter resets |
| --- | --- | --- | --- |
| 4 Oct, normal profile | on (coverage + blob fallback) | 0.61 / 13 in 111 s | 0.85 / 2 |
| 4 Oct, A-steady profile | off | **0.86 / 3 in 160 s** | 0.68 / 11 |
| 5 Oct, normal profile | on | **0.32 / 8 in 72 s** | 0.85 / 2 |

The A-steady and normal profiles differ only in phase tracking, diagnostics
(`PSSENSE_TIMING_DIAG`, `PSSENSE_INPUT_DIAG`, recording) and the wide-scan
period, which no full scan used. The user reports the left better under A.

- **Right under A: a one-step lock.** Its hinted narrow scan saw the ring in a
  single 250 µs step and locked on it, about 1 ms from the left's centre. With
  tracking off it was never corrected: lit 0.68, collapsing to 0.07–0.29 from
  118 s. Its host/device clock estimate stayed within ±60 µs of trend until
  155 s, so clock drift did not cause this.
- **Left with tracking on: steered while untracked.** Its scans were healthy
  (1700 µs windows). When its reference probe window is not tracked, the probe
  falls back to blob counts. In the 5 Oct run these moved the lock −200, +200
  and −200 µs on counts like 0.50/5.88/1.41 (reference/early/late), the
  signature of a moving hand rather than a slid window. Long left losses
  followed (up to 16 s), with many unconfirmed bootstraps (`acquiring` up to
  117 exposures per loss) and `lit_single`, consistent with a ring at the edge
  of its lit window. The right is mostly tracked during its probes, so it
  steers on pose coverage and stays `centred`.
- Working hypothesis: blob-fallback steering on an untracked, moving controller
  moves a good lock off-centre. The hand that is more often untracked (the
  palette hand) suffers most. This is three sessions with uncontrolled motion,
  not a controlled A/B.

**Change: minimum lit window.** The narrow scan now treats a lit run (after
bridging) shorter than `narrow_min_lit_steps` (default 3 steps, 750 µs) as a
weak scan, retried like a low peak. Healthy hardware windows are 6–11 steps.
The simulator test reproduces the one-step lock with the rule disabled and a
centred lock after rejection with it enabled. The simulator's delayed reports
now persist across `run()` calls. All 21 bootstrap tests and the Sense tests
pass; no hardware validation yet.

Next A/B: the normal profile with the minimum-window build, then the same with
`PSSENSE_LED_BOOTSTRAP_TRACK=0`, comparing per-hand lit fraction and
`JOINT_LOSS` classes. If tracking-off wins, restrict steering to tracked probes
(no blob fallback), re-centring by a hinted rescan after long losses instead.

### Tracking on/off A/B: clock-offset snaps precede the losses (2026-10-05)

Two further OpenBrush sessions with the minimum-window build: the normal profile
(tracking on, 75 s), then the same with `PSSENSE_LED_BOOTSTRAP_TRACK=0`
(70 s). No narrow scan was rejected by the new rule in either.

| Session | Left accepted poses/s, lit | Right accepted poses/s, lit |
| --- | --- | --- |
| Tracking on | 38.0, 0.61 | 56.9, 0.80 |
| Tracking off | 37.4, 0.64 | 37.8, 0.64 |

The left is no better with tracking off, so the blob-fallback hypothesis above
is **not supported**. The user saw both hands lose tracking in turn with
tracking off.

What the sessions share is a mid-session `CLOCK_OFFSET event=snap` shortly
before each long loss, on either hand:

| Session | Snap | Effect |
| --- | --- | --- |
| 5 Oct normal, old build | L +293 µs at 15.7 s | L losses from 16.3 s, lit 0.13–0.37 for the rest of the run |
| Tracking on | L +351 µs at 48.1 s | L lit 0.79 → 0.50 → 0.15, an 11.5 s loss |
| Tracking off | R +980 µs at 12.4 s | R lit 0.70 → 0.06 → 0.03; lock lost and rescanned at 26.7 s |
| Tracking off | L +252 µs at 43.2 s | L lit 0.75 → 0.55 → 0.32 → 0.11; losses from 51.7 s |
| 4 Oct A-steady | L +291 µs at 45.2 s | Followed a ~150 µs sag in the estimate and a lit dip to 0.63; restored to 0.92–1.00 |

The LED lock is measured relative to the host/device clock mapping at scan
time, so a later step in that mapping moves the pulse against the exposure by
the same amount. Lock tolerance is about ±350 µs. The mapping keeps the largest
recent `device − arrival` offset, drags it down at a fixed 50 ppm, and jumps
(snaps) when the gap exceeds 250 µs. Real controller drift measured 9–28 ppm.
A device clock does not step, so a step in the estimate is almost certainly a
Bluetooth latency change (for example a connection-event anchor shift). The
estimator applies it to the LED schedule all the same. Gradual sags (50 ppm
leak against true drift during slow-report stretches) and snaps are two
outcomes of the same design. This also explains why the affected hand varies
between sessions.

Next: replace the leak/snap mapping, while locked, with a fitted offset and
drift rate on the lower latency envelope. Treat step changes as latency
artefacts unless they persist, and slew at no more than physical drift rates.
Keep the startup snap, which fixes a genuinely late first report. Develop it
offline: next sessions should set `PSSENSE_TIMING_DIAG=1` so the mapping is
logged around snaps. Check whether the A-steady CTD recording's IMU/sync
records allow offline estimator replay.

### Opt-in steady clock mapping (2026-10-05)

The Sense clock mapping moved into `drivers/pssense/pssense_clock.{c,h}`. By
default it reproduces the previous max-envelope/leak/snap mapping exactly; a
test compares them sample by sample through latency steps.

`PSSENSE_CLOCK_STEADY=1` (opt-in, not in the runtime defaults) holds the
offset once the controller's LED bootstrap has locked. From then on it advances
only at a fitted drift rate: the median of differences between per-second
best-latency samples 10 s apart over 90 s (4 s apart from 8 s of data, so the
hold starts about 8 s in). A latency step contaminates only the differences
straddling it. The rate is clamped to ±200 ppm. The hold lasts the controller's
lifetime, so rescans calibrate against the held mapping. It logs `CLOCK_OFFSET
event=hold` with the rate and its gap from the envelope.

Simulation: 20 ppm drift, report latency floor plus 0–10 ms, floor stepping
−1 ms at 40 s and +2 ms at 70 s, hold requested at 10 s. The held offset stays
within 90 µs of truth (after the constant bias at hold), and the fitted rate
ends within 0.6 ppm. The previous mapping moves by up to 1.1 ms. Not yet run on
hardware.

`PSSENSE_TIMING_DIAG=1` now also logs `PSSENSE_CLOCK` (best sample per
100 ms window) so recorded sessions can replay clock mappings offline.

Next hardware A/B: the normal profile with `PSSENSE_TIMING_DIAG=1`, then
adding `PSSENSE_CLOCK_STEADY=1`. Compare per-hand lit fraction, `JOINT_LOSS`
classes and, in the first run, whether snaps precede losses. Long sessions
should also check for slow residual drift, which phase tracking is expected to
absorb.

### Best OpenBrush run, and a timing-trace correlation (2026-10-05)

The user reports the best session so far: 145 s, normal profile plus
`PSSENSE_TIMING_DIAG=1`, the minimum-window build (before the clock-module
commit, so default clock mapping). Left accepted 54.8 poses/s, lit 0.88, 4
filter resets; right 54.6/s, lit 0.81, 6 resets, both near 300 accepts per 5 s
until the right fell off in the last ~15 s. Both startup locks were healthy
(1450 µs windows).

It had **12 mid-session clock snaps of 250–840 µs without losses**, so snaps
alone do not explain the losses recorded above.

Across six OpenBrush sessions, the two with `PSSENSE_TIMING_DIAG=1` (A-steady
and this one) had a good left (lit 0.86 and 0.88). The four without it had a
poor left (0.32–0.64). The flag only adds a log line after each HID output
write on the controller I/O thread, so this may be chance with uncontrolled
hand use. With n=6, 2 of 6, it is a correlation to test, not a cause.

In both traced runs, HID output writes block 8.6–8.8 ms median, 19 ms p99 (up
to 111 ms), so 10% of output intervals are 21 ms instead of 10.7 ms. Input
age at write is 5.5–5.7 ms median. Both hands are alike. There is no trace of
a bad run to compare.

Next: rebuild `build-wine` (steady option and `PSSENSE_CLOCK` logging), repeat
with `PSSENSE_TIMING_DIAG=1`, then once with it removed, before the
`PSSENSE_CLOCK_STEADY` A/B.

### Sony's LED phase sequence on the wire (analysis of existing capture, 2026-10-05)

Source: `~/Code/psvr2-datasets/experiments/20261004-sony-wire-preparation/wire-reports.jsonl`,
CRC-validated Bluetooth A2/31 output reports from Sony's Windows driver
(PSVR2Toolkit fork oracle capture, both controllers, 185 s, about 77 reports/s
per controller). Wire observations only; no Sony implementation was used. The
38-byte settings block starts at report byte 2. Phase, sequence and period are
at block offsets 19–21; cycle position and length (LE u32) at 22 and 26;
`led_blink` at 30.

- **Sony latches rarely.** A new LED sequence (a latch) occurs about once per
  second in PRESCAN: 94 latches in about 70 s of PRESCAN on one controller.
  Successive PRESCAN anchors are 60 nominal frames apart (position advances by
  about 3,003,000 ticks, 1,001,001 µs), and their phase moves smoothly by tens
  of µs between latches. Our driver latches a new PRESCAN anchor with every
  output report (every 10.7–21 ms), so each report carries that instant's
  host/device mapping error.
- **`cycle_length` is constant at 50,050,050** (thirds of a ns) = 16.68335 ms,
  one nominal 59.94 Hz frame, in every report and every phase. Sony never
  trims it. Rate mismatch between controller and camera is handled by
  re-anchoring. (An earlier session note called it three frames; that was
  wrong.)
- **Phase sequence:** OFF (phase 5) for 5 s, PRESCAN (period 32 then 40) until
  acquisition (about 49 s in this capture), then alternating **BROAD (phase 2,
  period 42) for 10.0 s** and **PRESCAN for about 3 s** (three latches 1 s
  apart, occasionally 5–9 s). BROAD starts 75 ms after the last PRESCAN latch
  with `cycle_position = 0`. The controller keeps the PRESCAN anchor and
  free-runs on `cycle_length`. During BROAD Sony re-latches only to change
  `led_blink[0]` (ff, then 0a/0c/0d/0e about once per second, or not at all).
- **BG (3) and STABLE (4) never occur** on either controller in this or the
  second capture, including quiet holds.

Implications, untested here:

- BROAD looks like a bounded free-run: anchor once, run 10 s on the
  controller's own oscillator with a wider pulse (2.1 ms) for drift tolerance,
  then re-anchor. Over 10 s, 10–30 ppm of relative drift is 100–300 µs.
- Our per-report latching couples the LED phase to every host/device mapping
  wobble, and it is far more latch churn than Sony ever produces. Content
  transitions have been associated with the always-lit fault; latch rate is a
  candidate factor alongside period 42.

### Opt-in Sony-like latching and an experimental BROAD cycle (2026-10-05)

Two opt-in scheduler options follow the wire observations above. Both leave
scans, probes, LED-off and the unlocked state latching every exposure as
before.

- `PSSENSE_LED_LATCH_INTERVAL_MS=1000` holds the PRESCAN anchor (same sequence
  number, so the controller keeps it and free-runs on `cycle_length`) and
  re-latches once per interval, or immediately on new bootstrap or
  sync-refinement output, phase or period.
- `PSSENSE_LED_BROAD_S=N`, once locked and not probing: PRESCAN anchors 1 s
  apart; after the third, BROAD with `cycle_position = 0` and `led_blink`
  `ffffffff` 75 ms later, held N s, then a fresh PRESCAN anchor and repeat.
  Losing the lock, LEDs going dark or any content change aborts BROAD. The
  bootstrap's probes are only granted outside BROAD. The BROAD pulse defaults
  to the lock period (period 20, 1.0 ms); `PSSENSE_LED_BROAD_PERIOD_ID=42`
  tries Sony's 2.1 ms separately.

Not covered by unit tests (the driver I/O loop has no harness); built without
warnings, Sense tests pass. Hardware plan, each a separate session with
`PSSENSE_TIMING_DIAG=1` so `PSSENSE_OUTPUT` shows the latch rate:

1. `PSSENSE_LED_LATCH_INTERVAL_MS=1000` in normal use: sequence numbers should
   change about once per second when steady. Compare lit fraction and
   `JOINT_LOSS` with the previous runs, and watch for the always-lit fault.
2. Still test with both rings resting in view: `PSSENSE_LED_BROAD_S=10`, then
   30 and 60. Measure lit fraction against time within each BROAD window
   (drift with no anchors) and confirm clean PRESCAN re-anchors.
3. Only then `PSSENSE_LED_BROAD_PERIOD_ID=42`, watching for the always-lit
   fault.

BG and STABLE remain untouched: Sony never used them in either capture.

### Stuck-dark trap and two bootstrap fixes (2026-10-05)

In the morning's last OpenBrush session (left lit 0.33), the left locked
normally at 8 s. Untracked probes then moved its lock +200 µs at 17.8 s and
21.2 s by blob-count fallback. From 26 s it was lit only at the centre (±300 µs
probes dark: 5.9/0.2/0.2 blobs). After an 896 µs clock snap at 41 s, probes saw
nothing, so they stopped steering. Stray lit frames kept `frames_since_lit`
below 300, so it never rescanned: lit 22–44 of 1200 reports per 5 s until the
end. Moving the controller did not help (user report). The centre-only signature
appears only on the left and only in that morning's two sessions (2/7 and 3/5
lit probes), never in earlier runs. The user reports both controllers were
fully charged overnight. Backgrounds were low and similar in all sessions.

Fixes (Sense runtime defaults; environment restores the old behaviour):

- Blob-fallback steering of untracked probes is now opt-in
  (`PSSENSE_LED_BOOTSTRAP_BLOB_FALLBACK=1`). Untracked probes log and hold.
- New `lost_lit_fraction` (driver default 10%,
  `PSSENSE_LED_BOOTSTRAP_LOST_LIT_PERCENT`): a locked controller with fewer
  lit camera reports than that over a 300-exposure window rescans (hinted
  ±1.5 ms, then the existing retries). Logs `event=lost reason=dim`. A
  simulator test reproduces the trap: a 2 ms phase step with one stray lit
  frame in 20 stays locked with the rule off and relocks centred with it on.

Out-of-view controllers with stray frames can now rescan sooner than before;
hinted retries back off for 15 s before any full scan, as for the existing
loss rule. Hardware validation is pending, with `PSSENSE_CLOCK_STEADY=1`
already set in the normal profile.

### The phase probes black out the left ring (2026-10-05)

Recorded OpenBrush session `~/Code/psvr2-datasets/sessions/20261005-0810-openbrush-left-recorded/`
(build `9fc55e368`, steady clock, lost-lit 25%, blinds closed with daylight at
the edges). The user saw the left alternate almost regularly between tracking
and not. Replay (`constellation_replay --tracker-filter`):

- Left 60% of exposures solved, right 91%. The left ring sits at a median 0.41 m
  from the cameras against 0.24 m for the right, so its blobs are smaller (median
  area 20 px against 48). Area × range² is similar (3.6 against 3.0), so the left
  LEDs are not intrinsically weaker.
- Light leaking round the blinds forms 4–11 static bright patches per camera, most
  in camera 2. The left ring was never within 60 px of one, tracked or at a loss.
  It is not the cause here.
- Of 34 left losses (at least 30 unsolved exposures), most started from 20–33
  matched LEDs in 3–4 cameras. **The next exposure had no blobs at all near the
  ring**, in every camera, for at least 0.5 s. That is the LEDs leaving the
  exposure, not occlusion.
- From 26 to 105 s **every left loss began 0.5–1.1 s before a phase-probe result
  was logged** (onsets 35.3, 40.2, 44.9 … 105.2 s; probe results 35.9, 40.7,
  45.5 … 105.7 s). The median onset interval is 3.4–4.9 s, matching the probe
  cadence.

The probes shift the 1.0 ms locked pulse ±300 µs. With the exposure about 1 ms
(1450 µs narrow windows less the 450 µs narrow pulse), pulse and exposure are
the same length. Any offset reduces the overlap, and ±300 µs costs about 30% of
the light. The right, close and bright, survives (probe edge/centre blob ratio
0.99, 1% of probes below 0.3). The left, further away, drops below the blob
threshold (15% of its centred probes below 0.3; in this session nearly every
probe read 0.00 at both offsets). Each probe therefore blacks out the left for
its early and late windows, so the tracker loses it every few seconds. The
A-steady run, with tracking off, had the best left lit fraction (0.86).

Candidate fixes, in order of cost:

1. A lock pulse longer than the exposure (`PSSENSE_LED_BOOTSTRAP_LOCK_PERIOD_ID=32`,
   1.6 ms, within Sony's range). This gives a plateau of about ±300 µs where
   the overlap stays complete, so probes and small drifts cost no light.
2. Probe amplitude scaled to what the ring tolerates: halve the offset after a
   probe that darkens a ring that was lit at the reference, and end a probe
   window early when the ring vanishes.
3. Probe less often once stable.

### 1.6 ms lock pulse and lower blob thresholds: the left holds (2026-10-05)

OpenBrush, build `9fc55e368`, normal profile plus `PSSENSE_CLOCK_STEADY=1`,
`PSSENSE_LED_BOOTSTRAP_LOST_LIT_PERCENT=25`, `PSSENSE_TIMING_DIAG=1`,
**`PSSENSE_LED_BOOTSTRAP_LOCK_PERIOD_ID=32`** (1.6 ms) and
**`PSVR2_BLOB_PIXEL_THRESHOLD=50`, `PSVR2_BLOB_REQUIRED_THRESHOLD=120`**
(both changed together). The user reports the left took a while to settle,
then was good for a long time.

- 208 s: left 48.8 accepted poses/s, lit mean 0.80; right 58.9/s, 0.81. From
  62 s the left ran at about 300 per 5 s for about 140 s, matching the right.
- Every left probe in that stretch was `centred` with equal light at the
  reference and both ±300 µs offsets (for example 8.12/8.66/7.34 blobs). The
  previous session's blackout signature (0.00 at both offsets) is gone, as
  expected when the pulse outlasts the ~1 ms exposure.
- Settling, 30–62 s: the first lock (8.6 s) held until about 31 s, then went
  dark. Dim rescans at 37.0 s and 57.3 s, one weak hinted scan, relocks at
  50.9 s and 62.2 s. Successive lock centres moved by about +500 µs (15550 →
  15425 → 16050 µs fudge), consistent with early drift against the held clock
  mapping (fitted −13.8 ppm 8 s in) or scans taken while the hand moved. The
  dim-rescan rule recovered both times, as intended.

Not yet separated: how much comes from the longer pulse and how much from the
thresholds. Neither is a runtime default yet.

**Correction (same day): the long good stretch was the always-lit fault.** The
user reports the left locked out, with its ring permanently lit. The left's
third narrow scan in 25 s (57.9–62.2 s, after dim rescans at 37.0 s and
57.3 s) shows the onset. From step 6 (59.9 s) to the last step (62.2 s),
cameras 0 and 1 saw the ring lit at every remaining phase (8/8), so the "lock"
had an unbounded 2200 µs window. `own_ring_lit_across_narrow_scan` did not fire:
the first five steps were dark, and it needs nearly every step lit. From then
on, equal light at all probe offsets is the stuck ring, not evidence for the
longer pulse. The 62–208 s results above are void.

What remains valid is 8–31 s, with the 1.6 ms lock and before the fault: probes
were lit at the reference and at ±300 µs (8.12/8.66/7.34, 6.56/6.12/5.62,
7.94/7.47/5.50 blobs) and the left tracked about 270–300 per 5 s. That is short
supporting evidence that a pulse longer than the exposure removes the probe
blackout.

Consequences:

- The dim-rescan rule at 25% caused three scans in 25 s, and scans have preceded
  the fault before. Rescans need a rate limit, and the threshold should go back
  to the 10% default.
- Stuck-lit detection should also catch a narrow run lit to the end of the scan
  with a window well above the expected 1.2–1.95 ms.
- Power-cycle the left controller to clear the fault.

### Stuck check before unbounded locks, and a dim-rescan cooldown (2026-10-05)

Two bootstrap changes after the lockout above:

- `stuck_check_unbounded_steps` (Sense driver: 8 steps, 2200 µs): a narrow lit
  run that reaches either end of the scan and is at least that long is not
  locked at once. The LEDs are held dark for one baseline. A ring still matched
  while commanded off enters `stuck_lit`
  (`own_ring_lit_after_unbounded_scan`); otherwise the pending lock is applied
  (`event=stuck_check result=dark`). Healthy windows seen so far span at most
  7 steps (1950 µs), so this costs one dark baseline only on suspicious scans.
- `dim_rescan_cooldown_frames` (Sense driver: 1800 exposures, 30 s): at most
  one dim rescan per 30 s.

Simulator tests: a ring that becomes stuck lit at the fourth narrow step locks
without the check and enters `stuck_lit` with it; a healthy ring whose long
exposure lights a run to the end of the scan passes the dark check and locks.

### Still BROAD sessions (2026-10-05)

CLI sessions with both rings resting, build `6a07fc2b2`, steady clock, 1.6 ms
lock pulse (`PSSENSE_LED_BOOTSTRAP_LOCK_PERIOD_ID=32`), runtime bootstrap
options, no blob-threshold overrides.

- `20261005-082835-broad10-still` (user): `PSSENSE_LED_BROAD_S=10`. Two BROAD
  windows per controller, each 10.01 s. **Every 10 Hz sample in every BROAD
  window had fused optical poses for both hands, in the first, middle and last
  thirds.** So 10 s of free-running with no new PRESCAN anchor lost nothing
  with a 1.6 ms pulse. Left probes centred at all offsets (6/6), locked lit
  median 1.00. Startup was slow: the right, seen by only two cameras, needed
  five hinted scans and first locked at 40 s.
- `20261005-083028-broad10-p42-still` (run by Claude): `PSSENSE_LED_BROAD_PERIOD_ID=42`.
  **BROAD never started.** All five of the right's hinted scans lit only 1–2
  steps and were rejected by `narrow_min_lit_steps`. It fell back to the full
  scan (1.6 ms wide pulses, period 32), which failed once and found the ring
  on the second attempt. The following narrow scan then found the ring lit at
  every step: `stuck_lit own_ring_lit_across_narrow_scan`. The fault again
  followed full scans, without any period 42. The right's reported battery fell
  95 → 75 → 65% during the stuck state.
- The 30 s and 60 s runs were not made: the CLI caps sessions at 120 s (the
  first attempts at 150/180 s exited immediately, leaving the empty folders
  `20261005-083028-broad30-still` and `-broad60-still`), and the queue then
  stopped at the fault. They need a power-cycled right controller.

Consequence: rejecting narrow windows escalates to the fault-associated full
scan when a controller is only weakly visible. A weak but correctly placed
narrow run should retry or lock (letting probes refine it), not fall back to a
full scan; full-scan fallback should be reserved for scans that found no light
near the hint.

**Change:** a hinted scan rejected only for `narrow_window_below_minimum` (the
ring is lit at the hinted place, but for too few steps) no longer falls back to
the full scan once its retries are exhausted. It locks on that run
(`event=narrow_window_accepted`) and leaves refinement to the tracking probes.
Low peaks, and scans that found nothing, still fall back as before. A simulator
test with a ring lit at a single hinted step confirms the lock arrives after
the retries with no wide scan.

**Further still sessions (build `90f8345ef`, run by Claude, controllers
power-cycled by the user beforehand):**

- `20261005-083844-broad30-still`: `PSSENSE_LED_BROAD_S=30`, 120 s. Two 30.0 s
  BROAD windows per controller. **Every 10 Hz sample in every window had fused
  poses for both hands, in all thirds.** No fault.
- `20261005-084050-broad60-still`: the right's first hinted scan lit one step
  (`narrow_window_below_minimum`); the next three were `narrow_peak_weak`, which
  still falls back to the full scan. The dark baselines before each hinted scan
  were clean (`own=0/32`). The full scan's wide stage then found the ring lit at
  every phase (`stuck_lit own_ring_lit_at_every_phase`), so the fault began on
  entering the full scan. The queue stopped; the 60 s and period-42 BROAD runs
  remain undone.

All three lockouts today followed a full scan or a burst of scans, on a right
ring weakly visible from this resting place (two cameras). The runtime always
has a hint (`PSSENSE_LED_BOOTSTRAP_HINT_US=16350`), and lock centres have stayed
within its ±1.5 ms. The next change to consider is never falling back to the
full scan while a hint exists, retrying hinted scans with backoff instead.

**Change:** with a hint (always, in the Sense runtime), the bootstrap no longer
falls back to the full scan. Failed hinted scans retry with doubling pauses
capped at 10 s, indefinitely. A weak run at the hint is still locked once the
retries are spent. New option `full_scan_fallback` (library default on); the
Sense driver turns it off unless `PSSENSE_LED_BOOTSTRAP_FULL_SCAN_FALLBACK=1`.
Trade-off: if the true phase ever left the hint's ±1.5 ms, the controller would
never lock. That has not been seen, and the hint follows each lock's centre. A
simulator test covers a long out-of-view spell: a wide scan with the fallback,
none without it, and a centred lock once back in view.

**Still sessions after the no-full-scan change (build `2ca7cc39a`, run by Claude,
right power-cycled and repositioned by the user):**

- `20261005-084947-broad60-still`: **the left ran one 60.0 s BROAD window with
  fused poses in every 10 Hz sample, all thirds.** With the 1.6 ms pulse, BROAD
  windows of 10, 30 and 60 s have each held fully in the still tests. The right's
  hinted scans failed five times (weak peak / one lit step), retried without any
  full scan, then locked on a one-step run (`narrow_window_accepted`). It was lit
  in 40–62% of reports, then became stuck lit while locked. The onset coincided
  with a probe (blobs 2.38/5.88/5.88, then equal at every offset and 1200/1200
  lit). No scan was involved.
- `20261005-085152-broad10-p42-still`: the right was already stuck at the first
  baseline (`own_ring_lit_while_commanded_off`, 24/32); the queue stopped.
  Period 42 in BROAD remains untested.
- **Right battery:** 95% this morning, 75% and 65% during the earlier stuck
  episode, 65% at the start of this session, 25% at its end, then 15%. The left
  stayed at 85%. Every lockout today was on the right except the left's at
  57.9–62.2 s in the OpenBrush session. Whether a sagging or failing battery
  contributes to the fault, or the stuck ring simply drains it, is open. Repeat
  the period-42 and right-hand tests only with the right fully charged.

### The always-lit fault as a command-stream problem, and a Sony-like profile (2026-10-05)

The user reports that a stuck-lit controller becomes warm, which explains the
battery drain, and that the fault has never occurred with a PS5. That points to
this driver's command stream rather than the hardware. Onsets on 4–5 Oct
occurred in every context: a steady lock with no scans or probes (A-steady),
during narrow scans, on entering a full scan, and during a probe. So the trigger
is probably something constant in our stream rather than one scan step.

Differences from Sony's stream in the wire capture:

| | Sony | This driver |
| --- | --- | --- |
| New LED latches | ~1/s in PRESCAN; in BROAD only to change `led_blink[0]` | every camera exposure (~60/s) |
| `cycle_length` | constant 50,050,050 | measured average, changing almost every latch |
| Pulse widths | 1.6–2.1 ms (periods 32/40/42) | 0.45 ms scan steps, 1.0 ms lock, 1.6 ms wide |
| LED off (phase 5) | ~5 s at start only | baselines, yields, dark checks |

New opt-in `PSSENSE_LED_NOMINAL_CYCLE=1` sends the constant value. Test
profile for long OpenBrush sessions, counting lockouts against the earlier rate
(roughly one per session today):

```
PSSENSE_CLOCK_STEADY=1
PSSENSE_LED_BOOTSTRAP_LOCK_PERIOD_ID=32
PSSENSE_LED_BROAD_S=10
PSSENSE_LED_LATCH_INTERVAL_MS=1000
PSSENSE_LED_NOMINAL_CYCLE=1
```

**Sony-like profile, first CLI series (build `2102f4e0f`, charged controllers,
run by Claude):** `sonylike-broad10-still-1` to `-3` (`20261005-1223xx`–`1228xx`)
ran BROAD 10 s with `LATCH_INTERVAL_MS=1000` and `NOMINAL_CYCLE=1`. Each sent
one `cycle_length` value, against 150–200 before. But **latches still averaged
12–33 per second per controller**. Steady PRESCAN latched about 1/s and BROAD
not at all, as intended. Scans, probes and LED-off (yield) periods still
latched on every exposure (~50/s), because the BROAD branch held latches only
when steady. The right's long hinted-scan sequence (60 s) was all
per-exposure latching. **The right became stuck lit in run 3 at about 84–88 s**,
just after BROAD window 1 ended (83.1 s), during the probe that followed
(31–45 latches/s); it was stuck at the next session's first baseline. The
earlier `broad60` right lockout also began during a probe.

**Change:** in BROAD mode, latches are held whenever the schedule content,
phase and period are unchanged, in every state. A scan step or probe stage
latches once when it changes, and LED-off periods do not re-latch.
`20261005-123344-latchhold-check` was meant to verify the rate on hardware, but
both controllers had powered off, so it did not run. Hardware verification is
pending.

**Sony-like profile with latch hold (build `7cdff048a`, run by Claude, right
power-cycled, controllers charged):**

| Session | Latches/s L / R | BROAD windows L / R | Tracked L / R | Lockout |
| --- | --- | --- | --- | --- |
| `20261005-135030-latchhold-check` (60 s) | 1.11 / 1.11 | 2 / 2 (+1 cut by session end) | 83.6% / 94.3% | none |
| `20261005-135147-sonylike2-broad10-still-1` | 1.05 / 0.97 | 4 / 5 | 91.7% / 96.9% | none |
| `…-135353-…-still-2` | 1.05 / 0.97 | 4 / 5 | 91.9% / 97.5% | none |
| `…-135559-…-still-3` | 1.05 / 0.97 | 4 / 5 | 91.8% / 97.2% | none |
| `…-135805-…-still-4` | 1.07 / 1.11 (first 55 s) | 2 / 4 | 38% / 49% | none |

- Every BROAD window held tracking fully (100% of 10 Hz samples with fused poses
  in sessions 1–3). One `cycle_length` value per session.
- Session 4: at about 55 s both controllers stopped together (`pssense_handle_read`
  errors, zero blobs on all cameras for both rings). That was idle power-off
  after about 15 minutes resting, not the fault. Sessions `…-140009-…-still-5`
  and `…-p42-still` found no controllers and are empty. Period 42 in BROAD is
  still untested.
- The right battery held at 95% throughout. On the morning's stuck episodes it
  had fallen to 15%.

About 7.5 minutes of Sony-like operation without a lockout, against roughly one
lockout per two sessions earlier today with per-exposure latching. That is
supporting evidence, not proof, that per-exposure latch bursts trigger the
always-lit fault. Next: the same profile in longer OpenBrush sessions.

### `led_blink` sweep diagnostic (2026-10-05)

Purpose: resolve what the four `led_blink` bytes do, using the headset cameras
as the light sensor. The earlier single-bit test held a bit and saw dark and lit
frames alternate, which fits 32 frame-slots (one bit per camera cycle) better
than 32 slots within a frame. Under that reading Sony's BROAD values darken a few
of the first eight frames in each 32 (`0a`: bits 1 and 3 set). Those earlier
tests re-latched constantly; the latch-hold scheduler now lets one value run
undisturbed.

`PSSENSE_LED_BLINK_SWEEP` steps through the listed values once the lock is held
(see the toggles document), with one latch per step and probes suspended.
`scripts/pssense_blink_sweep_analyze.py SESSION` replays the session's
`constellation.ctd`, marks each frame lit or dark against each camera's
LED-off baseline, and prints per value: lit fraction, strongest repeat period
(autocorrelation over 1–40 frames), and frames folded modulo 32 beside the
value's bits in both bit orders. Run with one controller awake. Smoke-tested on
an existing recording; not yet run on a sweep.

### Recording the headset's LED detector stream (2026-10-05)

The headset sends its own LED-spot detections on USB interface 8 (endpoint
0x89), 36,944 bytes at 60 Hz: a 64-byte header (`LD`, length, device µs
timestamp at offset 8, frame counter at offset 20), then four camera sections,
each a u32 count and up to 256 36-byte records. Sony's matched LED indices
point into these records, so Sony's driver appears to track from these
detections. Monado's PS VR2 driver previously opened the stream only with
`PSVR2_AUXILIARY_STREAMS` (which also selects camera mode 10) and dumped it at
trace level.

`PSVR2_LED_DETECTOR_RECORD=<file>` now opens interface 8 alone (camera mode 4
and the other auxiliary interfaces unchanged) and records each packet compactly:
host receipt time, header, and only the populated records.
`scripts/psvr2_led_detector_dump.py` summarises the file and exports records
to CSV. The session script records it into `led_detector.bin` with
`PSVR2_SENSE_RECORD_LED_DETECTOR=1`.

Reader validation: the 4 Oct mode-4/mode-16 survey's raw packets
(`20261004-lower-wearer-left-covered`) converted to this format read back as 906
packets at 60.1/s, counter steps of 1 (one 35,711 jump at the start), device
time 16,683 µs per frame and 3,866 records; 226 KB against 33.5 MB raw. That
survey shows the stream populated in mode 4. The first records suggest the four
u16 fields are a bounding box (`x0,x1,y0,y1` = 344,348,165,168), not two points.
The driver writer has not yet run on hardware (headset and controllers were
off); full macOS build without warnings, all 45 tests pass.

Next: correlate records with our blobs frame by frame (coordinate mapping, size
and brightness fields), then compare detection rates on dim and far rings.

**First sweeps (build `327be507f`, run by Claude, right controller only, resting):**
`20261005-174245-blinksweep-prescan` (PRESCAN, 4 s per value, latched only on
change) and `20261005-174418-blinksweep-broad` (the same list inside one BROAD
window). Wire check: each value was sent ~265 times in the intended phase.

- **No value changed which frames show the ring, or how many spots.** Our blobs
  per camera were constant at 4/5/5/5–6 (minimum equal to mean, so no dark
  frames), and the headset's own detections were constant at 5–6 per camera,
  for every value including `01000000`, `80000000`, `00ffffff` and `000000ff`.
  So in PRESCAN and BROAD, at camera-frame level, `led_blink` is neither a
  32-frame on/off code nor a spatial LED selector.
- Spot brightness (headset `m00`) drifted monotonically over each sweep (PRESCAN
  up to +20%, BROAD down to −15%), consistent with phase drift between
  re-anchors, not value effects. In BROAD only `01000000` stood out (−10 to −20%
  against its neighbours, box area 36 against 45–54): weak and unconfirmed.
- The earlier finding that a held single bit alternates dark and lit frames came
  from the FORCE-IR diagnostic path, not PRESCAN/BROAD. The bits may only apply
  there, or act below camera-frame resolution. Sony's varying `led_blink[0]` in
  BROAD remains unexplained, but sending `ff` costs us nothing visible.

The analysis script's background estimate was corrected twice for sessions in
which the ring is lit almost throughout. It now uses the lowest per-camera count
seen in at least 0.3% of frames. With no dark frames at all, as here, use the
headset's per-section detection counts or the raw counts.

**LED detector records decoded (from `20261005-174149-ld-record-check`, right
only):** sections are Monado's cameras 0–3 in the same mode-4 pixel grid. Bytes
4–11 are the bounding box `xmin, xmax, ymin, ymax`. u32 at 12 is the summed
intensity `m00`, and u32 at 16 and 20 are the intensity-weighted x and y sums
measured from `xmin`, `ymin`: `xmin + m10/m00` matches Monado's blob centroid to
0.011 px median. The u32 at 24, 28 and 32 scale with spot size (correlation with
box area 0.87–0.96), probably second moments; the obvious assignment gives
negative variances 18% of the time, so it is unconfirmed. The headset reports
about one more spot per camera than Monado (5.9 against 4.0 on camera 0);
roughly 10% of its records have no Monado blob within ~7 px. The u16 at 2 has
low byte 0xff and an unknown high byte.

### OpenBrush with the Sony-like profile: no lockouts, left on par (2026-10-05)

Build `4342d178e`, normal profile with `PSSENSE_CLOCK_STEADY=1`,
`PSSENSE_LED_BOOTSTRAP_LOCK_PERIOD_ID=32`, `PSSENSE_LED_BROAD_S=10`,
`PSSENSE_LED_LATCH_INTERVAL_MS=1000`, `PSSENSE_LED_NOMINAL_CYCLE=1`,
`PSSENSE_TIMING_DIAG=1` and blob thresholds 50/120. 388 s session; the user
reports no lockouts, with some tracking loss.

- No `stuck_lit` event. Latches 0.99/s (left) and 0.59/s (right); 16 left and 25
  right BROAD windows. Batteries normal (left 75%, right 95 → 85%).
- Left 43.4 accepted poses/s, lit 0.62, 56 losses (96 s total); right 46.7/s,
  0.75, 55 losses (71 s). The earlier left/right gap has largely closed.
- `JOINT_LOSS` exposures (left / right): `dark_unpredicted` 2393 / 1562 (no light
  after the prediction expired: mostly out of view or covered), `lit_single`
  1534 / 903 (ring lit in one camera only; stereo bootstrap cannot use it),
  `lit_multi` 796 / 598 and `lit_ambiguous` 587 / 562 (lit in two or more cameras
  but not re-acquired), `acquiring` 170 / 212.

The remaining losses are re-acquisition and visibility, not the LED faults.
Candidates: single-camera re-acquisition using the IMU orientation as a prior
(for `lit_single`), and recorded replays of `lit_multi` intervals to find why
stereo bootstrap fails with the ring visible.

### Opt-in oriented re-acquisition (2026-10-05)

`CONSTELLATION_TRACKER_ORIENTED_BOOTSTRAP=1` (default off) adds a fallback to
the joint tracker's bootstrap contest. When stereo bootstrap fails for a device
with an IMU orientation and an alignment from earlier solves, the orientation
is taken as known. Two blob–LED pairs in one camera then fix the position
linearly. Hypotheses are scored by projection, and the best few are refined
against every camera with the orientation as a 4° prior. The result enters the
same best-fit contest and needs the usual three confirming solves. The unit
tests re-acquire a ring seen by only one camera (60 of 60, 40 of 40 with 5° of
orientation error) and never accept the other hand's mirror-image ring (0 of
60).

Instrumenting the replay showed what had blocked re-acquisition. Most fits
that failed were correct poses: 12–21 matches over three or four cameras at
0.2–0.3 px, rejected only by the 0.8 coverage rule because part of the ring was
occluded. Phase-1 tracking applies the same rule, so it dropped these poses too.
Coverage exists to catch a pose slipped round the ring. The orientation prior
already rules that out, since adjacent LEDs are about 20° apart and the prior's
sigma is 3–4°. The opt-in therefore also lowers the coverage limit to 0.5 for
any solve anchored to the IMU orientation. The mirror-ring test still accepts
nothing.

Replay of `20261005-0810-openbrush-left-recorded` (pre-Sony-like profile; at
`2eb074e96` plus this change), off / on:

- Left: 5201 / 5294 poses pushed (+1.8%). `lit_multi` exposures 222 / 83,
  `lit_single` 193 / 160, total loss time 50.5 / 46.5 s. RMS p50 0.265 /
  0.266 px, p95 0.419 / 0.441 px.
- Right: 8272 / 8274 poses pushed, loss time 10.0 / 10.5 s (noise level).
- Cost: mean solve 59 / 73 µs, worst 0.74 / 1.25 ms per exposure.
- Most of this session's left loss is `dark_unpredicted` (2156 exposures), which
  no solver change can recover.
- The Sony-like still sessions (`sonylike*-still-*`, `135030-latchhold-check`)
  are unchanged, because their losses are dark rings.

The single-camera path accounts for little of the gain here; the lower
coverage accounts for most of it. A moving session recorded with the Sony-like
profile, which had 1534 left `lit_single` exposures live, is the next test.
Known limitation: a ring seen in one camera, but predicted clearly visible in
others where it is hidden, still fails coverage below 0.5.

### OpenBrush with oriented re-acquisition, recorded (2026-10-05)

Build `caae3a05a`, the Sony-like profile of the previous OpenBrush run plus
`CONSTELLATION_TRACKER_ORIENTED_BOOTSTRAP=1`, recording blobs and the LED
detector stream (session `20261005-2001-openbrush-oriented-recorded`, 458 s).
The user reports a few seconds before the left was first tracked, then no
losses apart from deliberate occlusion, with quick recovery.

- LED schedule: one hinted narrow scan per controller, locked once, never lost
  or rescanned, no `stuck_lit`. The left's delayed start is the bootstrap order:
  the right scans first and the left begins after it locks. Batteries: left 100%,
  right 85 → 75%.
- Live `JOINT_LOSS` (left / right): 8 / 17 losses, 6.6 / 9.8 s in total, the
  longest 1.4 / 1.6 s; 5 / 5 oriented re-acquisitions.
- Replay of the recording, off / on (the same blobs both times, so a
  counterfactual for the tracker only):
  - Left: 25906 / 26275 poses pushed (94.3% / 95.7% of exposures), 18 / 7
    losses, 13.2 / 6.4 s lost, `lit_multi` 350 / 22, `lit_single` 85 / 65.
  - Right: 25591 / 25643 poses, 31 / 30 losses, 15.8 / 15.0 s lost.
  - RMS p95 0.450 / 0.453 px (left), 0.449 / 0.453 px (right). Mean solve 87 /
    82 µs, worst 0.2 ms.
  - The replay with the option on matches the live log (7 left losses against 8
    live, 6.4 s against 6.6 s).
- LED detector stream: 27,461 packets at 59.9/s over the whole session (a few
  counter gaps), 12–17 records per camera per packet.

On this session the option halves the left's lost time, mainly by keeping and
recovering partly occluded rings (`lit_multi`). The remaining loss for both
hands is mostly `dark_unpredicted`, which here is deliberate hiding.

### Sony-like profile and oriented re-acquisition become the 6DoF defaults (2026-10-05)

`PSVR2_SENSE_6DOF=1` now also sets `PSSENSE_CLOCK_STEADY=1`,
`PSSENSE_LED_BOOTSTRAP_LOCK_PERIOD_ID=32`, `PSSENSE_LED_BROAD_S=10`,
`PSSENSE_LED_LATCH_INTERVAL_MS=1000`, `PSSENSE_LED_NOMINAL_CYCLE=1`,
`PSVR2_BLOB_PIXEL_THRESHOLD=50`, `PSVR2_BLOB_REQUIRED_THRESHOLD=120` and
`CONSTELLATION_TRACKER_ORIENTED_BOOTSTRAP=1`, each only when not already set,
like the helper's other defaults. Evidence: the `sonylike2-*` CLI series and the
two OpenBrush runs above, none with a lockout or `stuck_lit`. The driver and
tracker defaults without the helper are unchanged, as are Linux and the
non-Sense macOS paths. The service plist no longer needs these entries.

### The headset's LED detections against ours (2026-10-05)

Comparison of the LED detector stream with Monado's blobs over the whole of
`20261005-2001-openbrush-oriented-recorded` (blob thresholds 50/120), pairing
each packet with an exposure by centroid matching:

- Timing: packets arrive a steady 8.7 ms after the exposure timestamp (p5 8.5,
  p95 9.7 ms), host clock, 136 of 138 sampled packets aligned.
- Detections are nearly the same set: 15.0 headset spots against 15.2 of ours
  per camera frame while both controllers are tracked. About 0.6 per frame on
  each side have no counterpart within 2 px. The headset-only spots are faint
  (summed intensity deciles 177–792, against a median of 4078 for matched
  spots), plus a few very large bright ones. Ours-only blobs are large (box area
  deciles 30–780 px²). With the lower thresholds we no longer miss spots that
  the headset finds; the earlier one-spot deficit was at the old thresholds.
- During losses the headset sees no more than we do: frames with the left lost
  have 8.8 headset spots against 9.1 of ours, and frames where it has three or
  more extra spots are 0.2–0.8% throughout. The remaining losses are rings that
  were not visible, not detection failures.

So the stream offers no recovery gain as a blob source. Its value would be
latency and load (below).

### Camera-path latency against the LED detector stream (2026-10-05)

`PSVR2_LATENCY_DIAG=1` (CLI session `20261005-201940-latency-diag`, 45 s,
nine 5 s windows, controllers off; values in ms after the exposure timestamp,
stable to ±0.1 ms across windows):

| Stage | p5 | p50 | p95 |
| --- | --- | --- | --- |
| LED detector packet arrives | 7.9 | 8.2 | 8.6 |
| Camera transfer, cameras 0–1 (set 4) arrives | 25.1 | 25.2 | 25.7 |
| Camera transfer, cameras 2–3 (set 5) arrives | 28.0 | 28.2 | 28.7 |
| Joint solve finished (`pose_age_ms`) | — | 28.6 | 29.1 |

The live 8.2 ms agrees with the offline alignment (8.7 ms, measured against
recorded host times), which confirms that the stream's device time is on the
camera VTS clock. The camera path's latency is almost all transport. The
second transfer of each exposure lands 28 ms after it, and blob detection plus
the joint solve then take about 0.4 ms. Tracking from the detector stream would
make optical poses about 20 ms fresher. The IMU covers that gap between
optical updates, so the gain is mainly in how quickly position drift is
corrected after fast motion, and in re-acquisition.

Load: the camera path moves 2 × (header + 2 × 512 × 508 bytes) per exposure,
about 62 MB/s of USB bulk traffic, plus four 258 KB frame copies and blob
detection. The detector stream is 37 KB per packet, 2.2 MB/s, and only ~2 KB
of that is populated. The whole CLI process (camera path, IMU, SLAM, LED
output, controllers untracked) used 11–17% of one core. A profile splitting
out blob detection was not taken: the controllers went to sleep.

With `PSVR2_CAMERA_STREAMS=0` the stream still arrived at 60 Hz with populated
records (`20261005-202046-if8-no-cameras`). The CLI exits without mode-4
cameras, so that run covered only 14 packets. The headset may therefore detect
without our camera streams, but this is unconfirmed over a real session and in
the camera mode the headset then runs. Moving to the detector stream would
also mean feeding the LED bootstrap's lit counts from it, and losing the
thresholds we tune ourselves.

### Tracking from the headset's LED detections (2026-10-05)

`PSVR2_LED_DETECTOR_BLOBS=1` (opt-in, in the 6DoF helper and the CLI) decodes
each detector packet into four blob observations. Each blob's centre is
`xmin + m10/m00`, `ymin + m01/m00`, its box is the record's bounds and its
brightness is 1.0, since the records carry no peak. The observations go to the
tracker's per-camera blob sinks in place of image blob detection, so the joint
solvers, LED bootstrap and scheduling are unchanged. The exposure time is the
packet's device time mapped on the camera VTS clock. Its offset from the
camera VTS of the same exposure was 0 in every frame, so detector observations
and camera exposure events share one timestamp. Without camera frames (for
200 ms), the driver announces exposures from the detector stream instead, so
the Sense LED schedule keeps its reference with `PSVR2_CAMERA_STREAMS=0`.

CLI sessions (headset resting, both controllers resting in view, Sony-like
profile, 60 s each, run by Claude):

| Session | Blobs | Cameras | Tracked L / R | First lock L / R | Pose ready after exposure (p50 / p95) | Mean solve |
| --- | --- | --- | --- | --- | --- | --- |
| `202915-ldblobs-cams-on` | detector | on | 83.7 / 94.8% | 11.6 / 5.1 s | 9.1 / 9.8 ms | 290 µs |
| `203025-ldblobs-baseline` | ours | on | 83.9 / 95.0% | 11.6 / 5.1 s | 29.8 / 30.9 ms | 232 µs |
| `203133-ldblobs-cams-off` | detector | off | 83.8 / 94.8% | 11.6 / 5.1 s | 8.8 / 9.6 ms | 271 µs |

The LED bootstrap scans and locks the same way from detector counts as from
our own: one narrow scan each, with lit steps at the same phases. With the
cameras off, the pose is ready about 21 ms earlier. USB traffic for tracking
falls from about 62 MB/s to 2.2 MB/s, and blob detection on the camera images
no longer runs. The tracked fraction is set by the left's late first lock; see
below. Still to do: a moving session (OpenBrush) and a comparison of pose
jitter and RMS against our own blobs.

### The left's late first lock, and opt-in quick lock (2026-10-05)

In the three detector-blob sessions above, the bootstrap ran the same way
every time:

| Time | Event |
| --- | --- |
| 1.0 s | Right starts a 13-step hinted scan |
| 5.3 s | Right locks |
| 5.3–7.4 s | Fixed 1.5 s handoff, then the left's dark baseline |
| 7.4 s | Left starts the same scan |
| 11.7 s | Left locks |

Lock centres have stayed within about 1 ms of each other and of the default
hint (15.4–16.5 ms), well within the reach of the 1.6 ms lock pulse.

`PSSENSE_LED_BOOTSTRAP_QUICK_LOCK=1` (opt-in) changes three things:

- After the dark baseline, a single quick-check step applies the lock pulse
  exactly where a lock at the hint would put it. If the ring scores at least 2
  (two cameras' worth of lit frames), it locks there and phase tracking centres
  it. Otherwise the usual hinted scan follows. This takes about 0.3 s instead
  of 4.3 s.
- A controller that has never locked takes the most recent lock of either
  controller as its hint.
- The scan handoff ends as soon as the controller that released the scan token
  has an optical pose accepted, which means the joint tracker has claimed its
  ring. Before, the handoff waited a fixed 1.5 s.

Unit tests: a hint within ±0.3 ms locks on the quick check and stays lit; a
hint 1.2 ms off falls through to the scan and locks at the true centre.

Hardware (build `e82dce351`, CLI, headset and both controllers resting in
view, Sony-like profile, 60 s, run by Claude), against the earlier sessions
under the same conditions:

| Session | Blobs | First lock L / R | Tracked L / R |
| --- | --- | --- | --- |
| `202915-ldblobs-cams-on` (no quick lock) | detector | 11.6 / 5.1 s | 83.7 / 94.8% |
| `203025-ldblobs-baseline` (no quick lock) | ours | 11.6 / 5.1 s | 83.9 / 95.0% |
| `204030-quicklock-ldblobs` | detector | 2.1 / 1.1 s | 96.8 / 98.5% |
| `204147-quicklock-ourblobs` | ours | 2.1 / 1.1 s | 96.1 / 98.5% |

Every quick check passed. The right locked 0.33 s after its baseline and the
left 0.6 s after the right. Scores were 2.0–2.75: from where each ring rested,
two cameras saw it, and both were lit in all eight frames. Phase tracking
found every lock centred (imbalance 0). Each lit fraction (about 50% of
reports, its two cameras out of four) held for the whole session, with no
`stuck_lit` or lost lock. A ring seen by only one camera fails the check and
gets the usual scan.

### Quick lock when the rings start out of view (2026-10-05)

An OpenBrush session at `a80293ef0` locked late: the left at 90 s and the right
at 96 s. The service was loaded before the plist gained the quick-lock and
detector-blob entries, so both were still off (a LaunchAgent's environment is
read only at `launchctl bootstrap`). The log also shows the rings mostly out of
view for the first ~85 s:

- The right's five hinted scans (1–45 s) were dark in all 103 steps on every
  camera.
- The left waited behind it until `first_wait_timeout` at 45 s. Its scans then
  saw the ring in camera 0 alone, too weak to lock.
- Each failure doubled the pause before the next scan, up to 10 s. So a ring
  that came into view could wait 5–15 s for a lock.
- The right's clock drifted at about 190 ppm for its first minute (a controller
  just woken). The mapping followed it.

Two additions to `PSSENSE_LED_BOOTSTRAP_QUICK_LOCK`:

- After a failed scan, a controller that has never locked retries with a
  quick check alone after 30 idle exposures, about 1.5 s a cycle with the dark
  baseline. A full hinted scan runs only every fourth attempt, in case the
  window has moved away from the hint. The quick check uses only the lock
  pulse: about two LED setting changes per cycle, close to Sony's ~1 latch/s.
- If the controller named by `PSSENSE_LED_BOOTSTRAP_FIRST` fails a scan, the
  other may scan without waiting for it.

Unit test: a ring that is out of view at start locks 76 exposures (1.3 s)
after coming into view, against 136 or more with doubling pauses. Not yet run
on hardware.

### Controllers that connect late or reconnect (2026-10-05)

Monado creates the Sense controllers once, when the system starts. Before this
change, a controller that was off at start never appeared, and one that
dropped out (asleep, powered off) stopped for good, because its read thread
ended on the first read error.

`PSSENSE_RECONNECT=1` (opt-in, macOS) changes this:

- The PS VR2 builder creates a missing controller anyway with
  `pssense_create_disconnected`.
- The controller's thread looks for its Bluetooth HID by vendor and product ID
  every 0.5 s (`os_hid_open_iokit_bluetooth`, independent of the prober's
  IOHIDManager). It attaches the controller when it appears: calibration, PC
  polling rate, then the usual read loop. It logs `CONNECTION event=connected`.
- On a read error the controller is closed and the thread waits again
  (`CONNECTION event=disconnected`). This applies to controllers present at
  start too.

Each new connection resets the per-connection state, because the controller's
clock restarts:

- the clock mapping and skew tracker, and the tick unwrapping;
- the IMU and optical relation histories, the 3DoF fusion and the optical
  filter;
- the LED bootstrap. Its last lock is kept as the hint, so quick lock
  re-acquires it.

While disconnected, the controller's inputs are inactive (OpenXR reports its
actions as inactive), it has no pose, and it stays out of the LED scan
rotation, because it has no clock mapping. A side named by
`PSSENSE_LED_BOOTSTRAP_FIRST` that is not connected no longer makes the other
side wait.

Builds without warnings; all 45 tests pass.

First hardware run (`222439-reconnect-cycle`, 120 s): no ring was lit in any
camera for the whole session, on Monado's blobs or the headset's own. The
rings were evidently out of view, and the left was not power-cycled (no
`CONNECTION` event). The run did expose a fairness bug in the quick retries.
Both unlocked controllers become ready together, and the left took the scan
token on every turn for 110 s while the right never scanned again.

The fix gives fair turns when quick lock is on. A controller ready to scan, or
unlocked and kept from retrying, marks itself waiting. The controller that
held the token last defers to a waiting one. The waiting mark is cleared on
disconnect, so a controller that goes away is never waited for. Check
(`222804-fairness-check`, 40 s, both resting in view): quick locks at 1.1 s (right,
score 4) and 2.1 s (left, score 3); 95.0% and 94.5% tracked.

Power-cycle test (`222947-reconnect-cycle-2`, build `eaa10e1aa`, 120 s, both
controllers said to be in view, the user power-cycling the left):

- Reconnect works. The left locked by quick lock at 8.1 s. At 28.6 s a read
  error ended its connection (`CONNECTION event=disconnected`). At 37.8 s it
  was attached again (`event=connected`), the calibration read succeeded, and
  its output reports resumed (result 78).
- The new clock mapping was healthy: device time restarted from 1.9 s, and the
  mapping was holding at −7 ppm within about 8 s, as before the power cycle.
- The left never relocked. Its baselines and scans after the reconnect were
  dark in every camera for 80 s, and its quick checks scored 0. The right was
  lit in only one camera for most of the session (scores 1.0–1.9) and locked
  only at 109 s.
- A fresh 30 s session right after it (`223255-after-reconnect-check`) also failed to
  lock the left. Its baseline then caught it lit while commanded off
  (`own=16/32`, `stuck_lit`). Its ring was solved in three cameras at 5
  matches, 445 times.

Not settled: whether the left was out of view after the power cycle, or did
not accept the LED schedule after power-on and later fell into the always-lit
state. The next test needs both rings clearly in view, at least two cameras
each, and a note of where the left is put down after it is switched back on.

Repeat test (`224123-reconnect-cycle-3`, build `eaa10e1aa`, 120 s, headset
moved further back so both rings were in all four cameras, the user
power-cycling the left):

| Time | Event |
| --- | --- |
| 1.3 s / 2.3 s | Right / left quick lock, score 4.0 |
| 34.9 s | Left read error, `CONNECTION event=disconnected` |
| 48.1 s | `CONNECTION event=connected`, calibration read |
| 48.7 s | Dark baseline |
| 49.0 s | Quick lock, score 4.0, 0.9 s after reconnecting |

- Accepted left poses per 10 s: 599–600 before the power cycle, 301 and 68 in
  the windows containing it, and 598–600 from 50 s to the end.
- The right was unaffected throughout (590–600 per 10 s).
- Tracked over the whole session: left 86.8% (it was off for 13 s), right 99.2%.

The previous run's failures came from the headset's position. A fresh
40 s session (`224019-headset-moved-check`) locked both at 1.1 and 2.1 s with score 4.
The earlier `stuck_lit` on the left was most likely a false positive: with the
headset close and the left barely in view, the left's model matched the locked
right ring weakly (5 matches, 3 cameras). The dark baseline then counted that
as the left ring lit while commanded off. The stuck state only stops scanning
and probing, but this false positive is possible when rings are poorly
visible.

### Quick lock, detector blobs and reconnect become 6DoF defaults (2026-10-05)

`PSVR2_SENSE_6DOF=1` now also sets `PSSENSE_LED_BOOTSTRAP_QUICK_LOCK=1`,
`PSVR2_LED_DETECTOR_BLOBS=1` and `PSSENSE_RECONNECT=1`, each only when unset,
like the helper's other defaults. Evidence (CLI, this document):

- quick lock: first locks at 1.1 s and 2.1 s instead of 5.1 s and 11.6 s;
- detector blobs: same tracking, pose ready 9 ms after exposure instead of
  29 ms;
- reconnect: a power-cycled left relocked 0.9 s after reconnecting.

The camera streams stay on, so passthrough is unaffected. Setting any of the
three to 0 restores the earlier behaviour. Combined defaults subsequently
exercised in games for 10–20 minutes, as confirmed by the user on 2026-10-08;
see the current status above.

### Steady clock hold kept after a lost lock, 2026-10-07

**Field failure.** A SteamVR session with the CrossOver rig ran for about 30 minutes
(`steamvr-crossover/run-mwxr-20261007-224508`). When Half-Life: Alyx exited
(22:56:17), both controllers were put down and lost their optical lock
(last fused pose 22:56:19). They never relocked: every hinted scan found
`lit=0/8` at every step and retried with backoff, so the hands stayed in
3DoF until the service restarted.

**Cause.** The driver requested the steady clock hold whenever a lock had *ever*
been acquired (`locks_acquired > 0`), so the hold continued after the lock was lost.
While held, the mapping only extrapolates its fitted rate. The fit swung from
+0.2 to −5.9 ppm, and the held offset drifted 10.8 ms (left) and 9.5 ms (right)
from the measured one. Against the 16.7 ms LED cycle, that put every hinted
scan's ±1.5 ms window on the wrong part of the cycle. This was not the
always-lit fault: the LEDs ran normally, but the scans looked in the wrong place.

**Change.** Hold only while the bootstrap is `LOCKED`. When the lock is lost,
the mapping returns to the measured (default) offset, as at start-up. The
lock, and so the hint, was measured against that offset. Recovery uses the same
quick hinted scan as start-up, and the full-scan policy is unchanged.
`CLOCK_OFFSET event=release` logs each release. A new clock test reproduces
the drift: a drift change during a hold leaves the held mapping 12.2 ms off,
and two seconds after the release it is back at the 4.3 ms latency floor. The hold
was introduced to stop latency snaps moving the pulse while locked, and that
case is unchanged. Hardware validation is pending: the next session should
log `event=release` and then a quick hinted relock after the controllers have been put
down and picked up again.

