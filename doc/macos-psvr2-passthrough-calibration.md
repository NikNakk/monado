<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# PS VR2 passthrough ChArUco capture guide

Updated 2026-10-08. This guide separates the capture we can do now from the
remaining stereo solve, pose alignment and runtime integration. No hardware
capture was performed while preparing it.

## Tooling audit and existing evidence

The core board generator, mode-3 recorder, calibration solver, assessment,
mode-4 capture/coverage and alignment tools in `monado-2` are byte-identical to
`~/Code/monado` on `claude/pssense-mr2940-evaluation` at `e54478a16`.
The source working tree additionally has uncommitted camera-survey updates and
`psvr2_camera_mode_charuco_compare.py`. These were missing from the integration
and have now been copied with their existing tests. They add explicit BC4
layout decoding, lossless compact captures and native-coordinate comparison.
The source checkout was not modified; its unrelated LED/protocol experiments
were not imported.

The source working-tree note `doc/pssense-stationary-protocol-experiment.md`
records October 4 ChArUco evidence from three usable positions (1/3/4):

- Mode 0x10 is a 1024x2032 BC4 raster split into two 1024x1016 views. Use the
  survey's explicit `--bc4-layout stacked` setting.
- Mode-4 lower cameras 0/1 correspond to passthrough views 0/1, approximately
  doubled in both coordinates, with no reflection over the sampled coverage.
- With separately labelled stationary-frame averaging, affine fit RMS is
  0.77/0.82 passthrough pixels. Held-position-out RMS spans 0.61–1.09 and
  0.70–1.01 pixels. Exact coordinate doubling gives RMS 1.11/1.24 pixels.
- One position lacked useful lower-camera detections; averaging recovered
  another. Those failures are retained rather than counted as successes.

This is useful evidence for reusing the lower tracking-camera geometry. It
is not full-field validation, an exact pixel-centre convention, absolute
camera-to-head alignment, or an installed passthrough calibration. The earlier
advice to verify the mapping remains valid, but we are not starting from zero.

## What a live session can do now

A powered, connected headset can capture a board pose, decode both readout
modes, produce contact sheets and receive an offline quality report immediately
after each capture. We can guide successive placements based on those results.

The survey claims USB interface 6 directly. Monado, GAV and other headset owners
must be closed. It does not attach to a running OpenXR application. Keep
DisplayPort connected and active. The survey turns camera streaming off on
exit, so the next runtime session must start its normal camera initialization.

There is no continuous ChArUco overlay/coverage UI in this tool. Capturing while
an OpenXR application runs would require an asynchronous recorder fed from the
service's existing camera sinks, with synchronized exposure/pose metadata. Do
not open a second USB owner to achieve that. The existing CLI calibration
recorder owns its own device and records four mode-4 cameras; it is not a
passthrough-mode or live-service recorder.

## Prepare the board and environment

Use the same physical target as Sense calibration:

- A3 landscape, matte print, rigid flat backing;
- 7 by 5 squares, nominal square size 40 mm, marker size 30 mm;
- `DICT_4X4_50`; the patterned rectangle is nominally 280 by 200 mm.

Generate it from this checkout:

```sh
cd ~/Code/monado-2
.venv/bin/python scripts/psvr2_charuco_calibrate.py board \
  .build/passthrough-calibration-guide/psvr2-charuco-a3.png
```

Print the whole image onto A3 landscape, preserving aspect ratio. The generated
PNG is an A3-sized raster but has no guaranteed printer DPI metadata: check the
printed squares with a ruler rather than trusting the print-dialog scale. Check
several adjacent squares in both directions. Record the measured size; do not
stretch the image independently in X/Y. Avoid glossy lamination, bowed paper
and glare. Even room illumination is preferable to a bright reflection.

The capture/comparison environment needs NumPy, Pillow, PyUSB, SciPy and OpenCV
with `cv2.aruco.CharucoDetector`. This checkout's `.venv` has been checked and
its missing SciPy dependency installed. The imported offline tests and help
commands do not claim the headset:

```sh
.venv/bin/python -m unittest discover -s tests \
  -p 'test_psvr2_camera_mode*.py' -v
.venv/bin/python scripts/psvr2_camera_mode_survey.py --help
.venv/bin/python scripts/psvr2_camera_mode_charuco_compare.py --help
```

## Capture one position

Set up a persistent session directory. The survey rejects `/tmp` and reused
nonempty pose directories. Record the source commit, date, measured square
size and lighting in a session note.

```sh
session="$HOME/Code/psvr2-datasets/passthrough/20261008-charuco"
mkdir -p "$session"
git rev-parse HEAD > "$session/source-commit.txt"
```

Rest the headset on a firm support, cameras unobstructed. Start with the board
centred roughly 0.6–1.0 m in front of the lower camera pair; this is a starting
placement, not a calibrated distance. Keep the board and headset completely
still throughout each command. A support for the board is better than holding
it by hand. Do not wear and move the headset for this cross-mode comparison:
the modes are captured sequentially, not simultaneously.

```sh
caffeinate -dims .venv/bin/python scripts/psvr2_camera_mode_survey.py \
  "$session/P00-centre" \
  --sequence 4,0x10,4 --repeat 2 \
  --settle 1 --sample 2 --examples 8 --save-every 8 \
  --bc4-layout stacked --compact-examples
```

Each position takes tens of seconds, depending on USB and decoding. It saves
raw `.bin.gz` packets, unamplified L8 PNG images, packet CSVs, mode-command
metadata, `survey.json` and a contact sheet when generation succeeds. Raw
packets remain authoritative. `--raw-if8` is available for LED-detector protocol
investigation but is unnecessary for board calibration and is omitted here.

Check both decoded passthrough views show a coherent board; at least eight
ChArUco corners per view is the comparator's stationary-mean acceptance gate.
The two visits back to mode 4 help reveal movement or unstable exposure.
Do not interpret reduced packet receipt while saving examples as proof of a
slower camera hardware rate.

## Build coverage, one pose at a time

Move only the board between commands, then support it and wait for it to stop
moving. Repeat the capture command with a new directory name, for example
`P01-left`, `P02-right`, `P03-upper`, `P04-lower`, `P05-tilt-left`,
`P06-tilt-right`, `P07-tilt-up`, `P08-near`, `P09-far`.

Aim initially for 15–25 distinct, usable positions as a capture target, not an
automatic pass threshold. Cover the centre, edges and corners of each camera's
image, with several depths and moderate board tilts around both axes. Keep
shared board corners visible to both lower cameras for stereo constraints;
supplement with partial views for lens coverage. Avoid collecting many nearly
identical centre views. OpenCV's
[ChArUco calibration guidance](https://docs.opencv.org/4.12.0/da/d13/tutorial_aruco_calibration.html)
likewise calls for varied viewpoints and allows partial board views.

Run the report after the first position and rerun after each small batch:

```sh
.venv/bin/python scripts/psvr2_camera_mode_charuco_compare.py \
  "$session"/P* --out "$session/comparison-01"

.venv/bin/python scripts/psvr2_camera_mode_charuco_compare.py \
  "$session"/P* --mean-frames --out "$session/comparison-01-mean"
```

Use new output names on subsequent reports (`comparison-02`, etc.). Individual
frames and stationary averaging are separate treatments; do not average frames
from a moving board. With three usable positions, inspect `held_pose_out` in
`comparison.json`. Inspect image detections and coverage as well as fit error.
Missing or incorrect cross-camera mappings should worsen on held-out poses.
Do not accept a low training residual from one planar board placement as a
complete calibration.

For an assisted session, start with only `P00-centre`: inspect it, correct board
visibility/lighting, then expand the capture rather than collecting a whole
weak dataset in one go.

## Solve and install: remaining implementation

The imported comparator fits an image-coordinate relationship; it does not
produce a runtime-ready stereo passthrough calibration. The existing general
`psvr2_charuco_calibrate.py solve` consumes four-camera mode-3 datasets, and
`psvr2_tracking_charuco_direct.py` consumes mode-4 survey PGMs. Neither accepts
these two-view BC4 PNG surveys as a complete passthrough solve.

After sufficient coverage, the next implementation should:

1. Export per-image ChArUco observations from these surveys and fit/validate a
   two-camera fisheye model and stereo transform. Compare against the existing
   lower-camera calibration transferred through the measured pixel mapping.
   Keep some whole poses out of fitting. Preserve measured board scale.
2. Record a separate synchronized camera/SLAM dataset with the board fixed in
   the room and the headset moving through varied orientations and translations.
   That constrains camera-to-head alignment and timing. The fixed-head/moving-
   board cross-mode capture above cannot solve this alignment. The current
   mode-3 recorder has exposure/SLAM association, but extending it or a service
   recorder to mode 0x10 remains work.
3. Add a versioned calibration loader and replace the presenter's approximate
   FOV/convergence UV map with calibrated per-eye projection. Keep the existing
   Sense calibration separate until the geometry and coordinate conventions
   have been validated.
4. Check camera-only output, then transparent virtual content over it, through
   both service and client-hosted compositing. Assess stationary geometry and
   moving-head latency separately. Calibration does not remove camera-to-eye
   parallax for every scene depth; positional reprojection requires depth or a
   declared reference plane.

Do not overwrite the existing Sense JSON or claim the generic `solve` command
has installed passthrough calibration. Passthrough's approximate renderer still
needs the runtime integration above.
