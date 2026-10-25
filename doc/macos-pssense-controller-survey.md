<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Guided controller survey in psvr2-openxr-test

`--controller-survey NEW-DIRECTORY` adds a headset-guided capture mode to the
existing native OpenXR/Metal test app. It automatically enables generic
controller actions. The normal diagnostic scene and its automatic refresh-rate
cycling/stress delays are excluded from this mode; the current display rate is
retained. No calibration is installed or fitted by the survey.

## What the wearer does

By default the app presents 14 trials for the left hand, then mirrors them for the right.
`--controller-survey-hand left|right` selects one hand for a shorter capture or
to complete the other hand later, using a fresh output directory. Placements are:
front, chest, low, high, forward reach, side, cross-body, near face, palm up,
palm down, wrist roll, beside shoulder, behind shoulder and a head-turn trial.
The other hand stays comfortably in camera view to make left/right ambiguity
less likely. Targets are comfortable nominal placements, not reach requirements;
any trial can be skipped.

Each trial waits for a **trigger press on either hand**. It then gives three
seconds to move, five seconds to hold or perform the requested rotation, and
five seconds to return to the front. The next trial waits for another press.
The full sequence has about six minutes of timed capture plus preparation time.

A solid gold target box with a thicker surrounding outline shows the requested grip position. It turns green when the
reported position is within 12 cm, which is feedback about the current estimate,
not proof of accurate tracking. Captions, countdown and tracking status follow
the head. A blue cross fixed two metres ahead indicates where to face. The
survey frame (origin, height and horizontal forward direction) is fixed on the
first usable ready frame, so previews stay still when you turn your head.
During readiness, glance over to find an outside/shoulder target, then face the
blue cross and press trigger. Starting requires head alignment within 15 degrees
and a fresh trigger press. The LOOK LEFT/RIGHT/UP/DOWN/AHEAD cue points back to
the cross; HAND TARGET directions help find the hand target. These use head
orientation, without enabling eye tracking.

A top-down inset also shows the target relative to the starting frame (white
centre, gold target, front upwards). Except for the head-turn trial, face the
cross during the timed portions. Head deviations during move/hold/recovery are
recorded rather than pausing the trial or discarding failures. Targets use the
initial survey head frame until a reference-space reset; remain near that
starting position. This is not an anthropometric shoulder calibration.

- Trigger: start a trial; after completion, finish the app.
- Primary face button (the generic controller's `FACE 1`): pause/resume.
- Secondary face button (`FACE 2`): skip the current trial, including while paused.
- Either hand's buttons work without a tracked controller position.
- Hold right Sense Options for one second: the service recenters (and re-measures the floor using the saved eye height, when configured), and the survey resets its forward/target reference at the current trial. This resets spatial alignment, not optical camera/LED calibration files.
- Ctrl-C stops early and closes the files normally.

Timers do not wait for a controller to become valid or arrive at the target:
that would discard the failures we need to study. Loss of focus/head tracking
suspends timing; long rendering gaps do not consume whole phases. A reference-space change records an event and resets the current trial directly
to readiness without clearing earlier data. A fresh trigger press restarts it.

## Prepare a matched sensor capture

Build the test app in the existing build directory:

```sh
cmake --build build/arm64 --target psvr2-openxr-test tests_psvr2_controller_visuals --parallel 4
```

The preparation helper creates a new session directory and an isolated service
plist, copies the camera calibration, saves executable hashes/source status and
service settings, and **prints** the launch/cleanup commands. It starts no
hardware. Use a fresh directory for every session:

```sh
python3 scripts/macos/prepare-pssense-6dof-runtime.py \
  ~/Code/psvr2-datasets/calibration/20260926-charuco-mode4-combined-head.json \
  --controller-survey build/arm64/controller-evidence/controller-survey-01
```

Run the printed commands after unloading the other Monado service, as instructed
by the helper. Restore the previous service afterwards. The isolated service
uses the normal opt-in 6DoF configuration, enables the one-second Options recenter
and copies the saved local profile's floor height/alignment (explicit shell
settings override those values), enables `PSSENSE_POSE_DIAG=1` and
`CONSTELLATION_TRACKER_DATA_RECORDER_OUTPUT`, and disables synthetic positioning.
The initial capture uses `PSSENSE_IMU_FALLBACK_MS=0` (ordinary 300 ms validity);
prepare a separate session with `--imu-fallback-ms 600` for a matched fallback
comparison. Each new survey uses its own launchd label, XPC endpoint and runtime directory,
so an older loaded agent cannot silently retain another capture's configuration.
Only one service may claim the physical headset at a time. Re-preparing into an existing directory is refused.

To capture only the right hand with camera passthrough behind the survey, add
`--controller-survey-hand right --passthrough` to the preparation command. The
helper enables the camera source in the service, forwards both options to the
client, and records the selected hand/passthrough setting in `identity.json`.
Passthrough stays optional; the app uses its existing `XR_FB_passthrough` layer
and transparent scene clear. An unsupported runtime reports that explicitly.
Passthrough may change image/processing conditions, so use separate sessions
when comparing captures with and without it.

For application-pose-only capture against an already configured service:

```sh
build/arm64/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
  --controller-survey build/arm64/controller-evidence/controller-survey-poses-01
```

The same `--controller-survey-hand right` and `--passthrough` switches can be
added directly to the app command. The service must have its camera source and
passthrough capability enabled before launch.

That command does not enable sensor recording in an already running service.
Use the runtime/endpoint environment for that service if it is isolated.

## What is saved and what it establishes

The helper's session directory contains:

- `constellation.ctd`: existing replayable camera/blob, device-model, IMU and
  tracking evidence from the service; this is not raw camera video.
- `calibration.json`, `identity.json`, `service.plist` and `service.log`: the
  calibration snapshot, binary hashes/configuration and tracking diagnostics. `client.log` also records the launch's passthrough lifecycle and application output.
- `guided/plan.csv`: all requested placements, mirrored coordinates in metres
  relative to the initial head/facing frame, instructions and nominal durations.
- `guided/poses.csv`: every rendered frame's left/right grip/aim results, action
  activity, location/velocity flags, poses, velocities, head pose, requested
  target and controls. Version 2 also appends `look_x,look_y,look_z,look_error_deg`:
  the world-space fixation point and the angular error of head forward toward it.
  Invalid components are NaN, not fabricated zero poses.
- `guided/events.csv`: phase boundaries, starts, pauses, skips, reference-space
  changes and orderly closure, using host uptime and OpenXR timestamps.
- `guided/summary.csv`: selected-hand hold-frame counts: sampled, active,
  position valid and position tracked, including lost-position frames.
- `guided/README.txt`: protocol/schema interpretation.

Hand 0 is left, hand 1 right; kind 0 is grip, kind 1 aim. Single-hand captures
retain the original trial IDs (0–13 left, 14–27 right); unused summary rows have
zero frames. The README records the selected hands and passthrough setting. Poses use OpenXR LOCAL
space at predicted display time. Host timestamps use `CLOCK_UPTIME_RAW`, which
matches service/CTD host timestamps; join by time, accounting for the sample's
predicted display time versus sensor exposure time. CTD optical-world coordinates
and OpenXR LOCAL coordinates must be transformed before comparing positions.

The CSV samples are rendered frames, not an independent sensor stream. Paused,
unfocused and invalid samples are labelled; service CTD evidence covers camera/
IMU behaviour during application rendering gaps. Summary counts are not
wall-clock coverage percentages, and skipped/unfinished trials can have zero
hold frames. Inspect events before treating a trial as completed. Buffered pose
rows flush at phase/control events and on orderly exit; a hard kill can lose the
last buffered rows. Failed writes are reported, rather than showing success.

This supports comparing loss/recovery by placement, wrist orientation and head
motion, separating optical gaps from HID/IMU failures, and replaying identical
observations against calibration or solver candidates. It supplies no independent
physical ground truth: following a virtual target with the same tracker cannot
prove its absolute accuracy or justify fitting camera/controller geometry to
those target coordinates. Candidate calibration changes still need held-out
replay checks and user-operated validation.

## First wearer run and version 2 follow-up (25 October 2026)

The user completed the left-hand portion but ran out of time for the right.
The preserved capture in
`build/arm64/controller-evidence/20261025-controller-survey-ready/guided/`
has 6,292 left-hand hold frames and no right-hand hold frames; its last transition
is trial 14, right/front/ready, followed by orderly closure. These are protocol
completion observations, not a tracking-accuracy result. The user reported
missing forward-looking guidance and difficulty seeing the outside target.

Version 1 recomputed the ready preview from the current head pose every frame,
so looking aside moved the target. Version 2 fixes the initial survey reference,
adds the blue forward cross and directional guidance, permits preview glances,
records head-facing error and gates only the start on forward alignment. It also
adds explicit hand selection and exposes optional passthrough in the preparation
helper. No existing capture is modified or resumed in place.

The native app and test target build. All ten controller/survey test cases pass,
including fixed preview through a glance, the fresh-press alignment gate,
single-hand trial bounds, all captions fitting the instance buffer, invalid
samples and output-file protection. The preparation helper and CLI options were
checked without starting hardware. Version 2 cue legibility and its passthrough
combination still need the next wearer-operated check. No service was started
or installed as part of these changes.

## Version 3: mode-4 passthrough and Options reset

The wearer reports no camera image despite `Passthrough requested: 1`, missing
hand target boxes, and asks for the service's Options-hold reset. The new capture
folder had no service log; the old `controller-survey-ready/service.log` was
still receiving service output. Unique survey endpoints now prevent this
registration reuse and client output is captured through `tee`.

The camera failure has a separate concrete cause: Sense 6DoF selects mode 4,
while `img_xfer_cb` previously forwarded passthrough frames only from modes 16
(BC4) and 1 (L8). Attaching a passthrough sink succeeded but delivered no mode-4
images. The new driver path forwards camera-set 4 (the lower stereo pair) to
passthrough as locally encoded BC4 in the existing 1024×1016 transport. Mode 4
and all four optical camera streams remain active. No additional USB interface
or camera-mode switch is requested. Without an attached passthrough consumer,
there is no conversion work. Mode-16 delivery is unchanged.

Each original 512×508 image is doubled in each dimension; its four transport
padding columns are zeroed. Local BC4 endpoints preserve image contrast with
bounded quantisation error. This is a lower-resolution camera view than native
mode-16 passthrough. The [existing geometric evidence](macos-psvr2-passthrough-calibration.md#fourth-new-capture-p03-upper-2026-10-08)
supports using the lower pair with doubled coordinates and no reflection; no
calibration is fitted or installed here. Full-field image alignment, image
quality and simultaneous tracking/passthrough remain hardware validation gates.

Target rendering now includes a visible 4.5 cm solid box plus a thicker outline,
independently of whether the selected controller has a valid pose. Holding
Options resets the service alignment/floor through the existing worker. Its
reference-space event also invalidates the survey anchor, resets the current
trial to readiness and rebuilds the cross/targets from the current head pose.
Earlier samples remain, with the change marked in `events.csv`.

The service and native test app build. Five focused CTest suites pass (driver,
controller visuals, floor calibration, optical filter and clock), including all
12 controller/survey cases and new mode-4 tests for stereo delivery, timestamps,
BC4 pixel decoding/padding/bounds and in-flight detach. A 120-pair synthetic
high-contrast conversion benchmark measured 0.548 ms per stereo pair on this
host. This is a short CPU-only measurement, not a headset latency or loaded
tracking regression result. Visual confirmation remains pending. The installed
LaunchAgent and previous capture folders are unchanged.
