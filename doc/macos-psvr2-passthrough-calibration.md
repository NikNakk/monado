<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# PS VR2 passthrough ChArUco capture guide

Updated 2026-10-08. Eight user-operated ChArUco captures support the transferred
lens/stereo calibration, and the user now confirms the service-composited
camera-only image looks good. Perceived motion lag remains; blended-scene and
hosted-client validation are separate outstanding checks. The dated evidence
below preserves the capture, implementation and failed-startup history.

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

- Reuse the existing A4 board, with a matte surface and rigid flat backing;
- 7 by 5 squares and `DICT_4X4_50`; the generator uses nominal 40 mm
  squares and 30 mm markers, but an A4 print may be scaled;
- Measure the actual printed square and marker size. Cross-mode corner-ID
  comparison is independent of this physical scale; the metric solve is not.

Only if a replacement is needed, generate the original A3 design from this checkout:

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
has installed passthrough calibration. The experimental runtime integration
below now permits testing a transferred candidate; it has no hardware result yet.

## First new capture: P00-centre, 2026-10-08

The user captured `~/Code/psvr2-datasets/passthrough/20261008-charuco/P00-centre`
with the existing A4 board, source `8dae28b60aff6674eb23fa50cde49e7cf846efce`.
The user confirms the printed square size is 40 mm. Marker size has not been
separately confirmed; do not infer it from paper size. All six requested mode visits
synchronized and received frames. The decoded stereo views show a coherent board.
All 16 saved images per passthrough view detect all 24 board corners, without
stationary averaging. Mode-4 lower cameras detect 3–19 and 9–24 corners per frame;
stationary averaging recovers 24/24 lower-camera corners (20/24 in the upper pair).

The lower-camera correspondence repeats the earlier result: using median corners
from individual frames, exact doubling gives RMS 0.90/0.77 passthrough pixels and
an affine fit gives 0.49/0.29 pixels. Stationary-mean results give exact doubling
RMS 0.87/0.78 and affine fit 0.44/0.31 pixels. These are single-position fitting
results, not held-position validation. The next capture should move the board
left in the camera image, keeping both cameras' board coverage and the headset
fixed. No reason to reprint the A4 target is established by this capture.

The BC4 visits report 19/17 invalid-header candidates and 975104 prefix bytes
while framing the stream; decoded examples and detections succeed. Preserve
these counters for comparison in subsequent captures rather than treating them
as proof of corrupt saved images or silently discarding them.

Analysis artifacts are under this checkout's
`.build/passthrough-calibration-guide/P00-centre-{individual,mean}/comparison.json`.
The raw capture is unchanged. The guide deliberately keeps those two analysis
treatments separate.

## Second new capture: P01-left, 2026-10-08

The user completed `P01-left` in the same session, with the 40 mm-square A4
board. All six visits synchronized and received frames. The board has visibly
moved left relative to `P00-centre`. Passthrough view 0 detects 24 corners in
all 16 saved images; view 1 detects 22–24. The mode-4 lower pair detects 10–22
and 0–8 corners per individual frame; stationary averaging recovers all 24 in
both lower cameras and both passthrough views. This is a usable second position,
with averaging important for the darker tracking readout.

Across both positions, stationary-mean exact-doubling RMS is 0.79/0.91
passthrough pixels and affine-fit RMS is 0.43/0.51. Individual-frame-median
results give exact doubling 0.86/0.91 and affine fit 0.44/0.60. These remain
fitting results; the comparator requires a third usable position for
held-position-out checks. BC4 invalid-header candidate counts are 23/15 at
stream framing, with successful decoded images retained.

Reports are under `.build/passthrough-calibration-guide/P00-P01-{individual,mean}`.
Next capture: `P02-right`, with the board right of centre in both camera views
and a modest change in tilt, while the headset stays fixed.

## Third new capture: P02-right, 2026-10-08

All six visits synchronized and received frames. Both passthrough views detect
all 24 corners in all 16 saved images each. Averaging recovers all 24 in both
lower tracking cameras (upper cameras: 14/24). Across P00/P01/P02, the
stationary-mean affine fit RMS is 0.47/0.48 passthrough pixels. Leaving each
whole position out of fitting gives RMS ranges 0.49–0.60 and 0.33–0.69 pixels
for the matching lower-camera/view pairs. Individual-frame-median checks give
held-position ranges 0.55–0.60 and 0.42–1.00 pixels. These support correspondence
across the sampled positions rather than only within one fitted board plane.

Exact doubling gives RMS 0.83/0.87; doubling plus a half-pixel offset gives
0.48/0.50. This supports the pixel-centre hypothesis over the present coverage,
without settling full-field intrinsics or camera-to-head alignment. Three
positions remain insufficient to claim complete passthrough calibration.
BC4 invalid-header candidate counts are 9/26, with successful saved-image
detection. Reports: `.build/passthrough-calibration-guide/P00-P02-{individual,mean}`.
Next: vary vertical coverage with `P03-upper`, board centred horizontally and
raised slightly, headset fixed.

## Fourth new capture: P03-upper, 2026-10-08

All six visits synchronized and received frames; source remains `8dae28b60`
with documentation-only local updates. The elevated board is visible in the
upper image. Passthrough detections are 22–24 corners in view 0 and all 24 in
view 1, in every saved frame; stationary averaging recovers 24 corners in all
four mode-4 cameras and both passthrough views. Across four positions, mean
image affine RMS is 0.48/0.49 pixels and held-position RMS spans 0.45–0.54 and
0.33–0.68. Exact doubling RMS is 0.81/0.86, and doubling plus half a pixel is
0.48/0.51. The added upper coverage does not degrade the correspondence.
BC4 invalid-header candidate counts are 38/32; all saved views detect the board.
Report: `.build/passthrough-calibration-guide/P00-P03-mean/comparison.json`.

Next is lower image coverage (`P04-lower`). Put the board back on its original
surface and, if needed, aim the headset slightly above it so the board appears
lower in the camera image. Headset movement between separate pose roots is
permitted for this cross-mode comparison: only the headset and scene within
each root must stay fixed across its sequential mode visits. No SLAM alignment
is being solved by this survey. Prefer modest pitch changes and keep the board
fully visible to both lower cameras where possible.

## Fifth new capture: P04-lower, 2026-10-08

All six visits synchronized and received frames. Both passthrough views detect
24 corners in every saved image, and the lower tracking cameras detect 18–24
and 19–24 per individual frame. Stationary means recover all 24 in the lower
pair and both passthrough views. The upper tracking cameras have only 6/5
corners in their means and do not pass the eight-corner gate; that does not
invalidate this lower-pair passthrough capture.

This position adds substantial lower-image coverage and a larger board image:
matched passthrough corners span y=398–829 and 393–825, compared with earlier
positions ending around y=420. Across all five positions, stationary-mean
affine RMS is 0.45/0.46 pixels; held-position RMS ranges 0.36–0.53 and 0.34–0.72.
P04 itself is predicted with 0.36/0.64 pixel RMS when excluded from fitting.
Exact doubling RMS is 0.80/0.83; adding a half-pixel offset gives 0.46/0.47.
BC4 invalid-header candidate counts are 39/25; all saved passthrough images
successfully detect the board. Report:
`.build/passthrough-calibration-guide/P00-P04-mean/comparison.json`.

Next: `P05-tilted`, with the board centred and rotated about its vertical axis
by approximately 25–30 degrees (one side nearer the headset), keeping both
views' corners visible. Keep the board and headset fixed during the command.

## Sixth new capture: P05-tilted, 2026-10-08

All six visits synchronized and received frames. Both passthrough views detect
all 24 corners in every saved image. The lower tracking pair detects 18–24 and
16–24 individually and 24/24 in stationary means; the upper cameras' means
have 5/5 corners and are excluded by the eight-corner gate. This tilted view is
usable for lower-camera correspondence.

Across P00–P05, stationary-mean affine RMS is 0.42/0.44 passthrough pixels.
Held-position RMS spans 0.19–0.53 and 0.27–0.71. P05 itself, excluded from
fitting, gives 0.19/0.29 pixels. Exact doubling RMS is 0.79/0.81; doubling plus
half a pixel gives 0.43/0.44. BC4 invalid-header candidate counts are 25/30;
all decoded passthrough views detect the board. Source remains `8dae28b60`
with documentation-only local updates. Report:
`.build/passthrough-calibration-guide/P00-P05-mean/comparison.json`.

Six useful positions give strong cross-mode evidence over their sampled area;
they do not yet constitute full-field intrinsic calibration. Next capture:
`P06-opposite-tilt`, rotating the board the other way at approximately the same
distance, followed by a farther position to vary depth before reassessing
coverage and the need for further captures.

## Seventh new capture: P06-opposite-tilt, 2026-10-08

All six visits synchronized and received frames. Passthrough detections are
24 corners throughout view 0 and 22–24 in view 1. Lower tracking-camera
individual detections are 18–24 and 19–24; stationary means recover 24 corners
in both lower cameras and both passthrough views. Upper-camera means are 11/3,
with the latter excluded. Source remains `8dae28b60` with documentation-only
local updates.

Across seven positions, stationary-mean affine RMS is 0.40/0.42 pixels.
Held-position RMS ranges 0.19–0.53 and 0.26–0.71; P06 itself gives 0.23/0.28
when excluded. Exact doubling RMS is 0.78/0.81 and doubling plus a half-pixel
offset gives 0.40/0.42. The opposite tilt does not degrade correspondence.
BC4 invalid-header candidate counts are 27/27; saved passthrough images detect
the board successfully. Report:
`.build/passthrough-calibration-guide/P00-P06-mean/comparison.json`.

Next: `P07-far`, straighten the board and place it approximately 1.5–2 times
farther away than P06, centred in the stereo views. Keep everything stationary
throughout the command. After P07, reassess coverage and the offline solve
rather than automatically continuing a long capture sequence.

## Eighth new capture and capture-stage reassessment: P07-far, 2026-10-08

All six visits synchronized and received frames. Both passthrough views detect
24 corners in every saved image. Stationary-mean lower tracking detections are
24/8 corners; both meet the comparator gate, but only eight right-eye IDs are
available for cross-mode matching at this distance. Upper means are 20/12.
Source remains `8dae28b60` with documentation-only local updates. BC4
invalid-header candidate counts are 44/35, with all saved passthrough images
detecting the board.

Across all eight positions, mean-image affine RMS is 0.43/0.42 passthrough
pixels; held-position RMS ranges 0.20–0.60 and 0.26–0.71. The fitted diagonal
scales are 2.00040/1.99992 and 1.99969/1.99968. Doubling plus a half-pixel offset
has RMS 0.44/0.43. In a 3x3 diagnostic grid the matched corners occupy six cells
in view 0 (leftmost column missing) and eight in view 1 (bottom-right missing).
This is strong cross-mode evidence across multiple depths/tilts, with incomplete
full-field coverage. Report:
`.build/passthrough-calibration-guide/P00-P07-mean/comparison.json`.

Before requesting further captures, an offline check transferred the existing
`20260926-charuco-mode4-combined-head.json` lower-camera intrinsics using
`fx/fy *= 2`, `cx/cy = 2*cx/cy + 0.5`, retaining distortion and stereo transforms.
It fitted only one six-parameter board pose per capture jointly against both
cameras, using the confirmed 40 mm square size; the camera model was held fixed.
All eight fits converged. Per-position stereo reprojection RMS is 0.32, 0.34,
0.35, 0.39, 0.61, 0.70, 0.60 and 0.87 passthrough pixels. The far position has
32 matched corner observations; each other position has 48. This supports
reusing the existing lens/stereo geometry over the sampled area rather than
refitting it from these eight captures.

The scratch check and report are
`.build/passthrough-calibration-guide/check_existing_stereo.py` and
`existing-stereo-validation.json`. They preserve the original calibration and
raw capture. This is an independent check of fixed camera geometry, not a new
production calibration: the source remains `runtime_usable: false`, edge
coverage is incomplete, and this capture does not independently validate
camera-to-head alignment or latency. Stop this capture sequence here for now.
The next step is a versioned transferred-calibration candidate and calibrated
presenter UV mapping, followed by visual validation and a separate alignment/
timing assessment. Do not request arbitrary additional poses merely to meet
the guide's initial 15–25-position planning target.

## Experimental transferred runtime mapping, 2026-10-08

The presenter now accepts `XRT_MACOS_PASSTHROUGH_CALIBRATION`, an absolute path
to `psvr2-passthrough-calibration-v1` JSON. It is opt-in and PS VR2-specific.
Unset, unreadable or invalid files retain the previous approximate mapping;
a supplied invalid file produces a warning. The calibrated path logs
`Experimental calibrated passthrough` at info level.

Generate a fresh candidate (the converter refuses to overwrite files):

```sh
.venv/bin/python scripts/psvr2_passthrough_calibration_transfer.py \
  "$HOME/Code/psvr2-datasets/calibration/20260926-charuco-mode4-combined-head.json" \
  .build/passthrough-calibration-guide/passthrough-candidate-v1.json
```

That candidate has already been generated locally. The source and captures are
unchanged. The converter records the source SHA-256 and experimental status,
transfers intrinsics as above, keeps all four distortion coefficients, and
composes `head_from_camera0_xrt * camera0_from_camera_xrt` for each lower camera.
The source's serial is null, so this candidate is unbound to a serial; use it
only with the headset that supplied these captures. A non-null serial in a
candidate must match the runtime device serial.

The v1 file requires `projection: "rotation-only"` and two unique views, 0/1,
each with `width: 1024`, `height: 1016`, `model: "fisheye_equidistant4"`,
`intrinsics: {fx,fy,cx,cy}`, `distortion: {k1,k2,k3,k4}` and
`head_from_camera_xrt: {orientation: {x,y,z,w}, position: {x,y,z}}`.
All numbers must be finite; focal lengths are positive and quaternions unit.
`runtime_usable: false` is retained as evidence of experimental status; the
explicit environment setting permits testing this candidate.

The presenter recovers each optical green-channel tangent ray as before,
rotates from head to camera, converts XRT camera axes to OpenCV, and applies
the four-coefficient fisheye model. Pixel centres become normalized Metal UVs
with `(pixel + 0.5) / dimension`. This sampling half pixel is separate from
the half-pixel principal-point transfer. Rays behind the camera or outside the
image are masked. Camera/eye translations are intentionally unused at infinity;
this does not correct nearby-object parallax or camera latency. The previous
FOV/convergence settings affect only the approximate fallback. Brightness
continues to apply to both paths.

### Next headset check

Use the rebuilt `.build/native-service-check` service and matching diagnostic.
This build directory previously had PS VR2 and Sense drivers disabled; a
successful software build alone was not a headset-ready build. Configure and
check the hardware drivers before using it:

```sh
cmake -S . -B .build/native-service-check \
  -DXRT_BUILD_DRIVER_PSVR2=ON -DXRT_BUILD_DRIVER_PSSENSE=ON
cmake --build .build/native-service-check --parallel 8
rg '^XRT_BUILD_DRIVER_(PSVR2|PSSENSE):BOOL=' .build/native-service-check/CMakeCache.txt
```

Both cache values must be `ON`. The configuration and build have now been
corrected locally after the first unsuccessful test described below.
Close XR clients and GAV first; only one process may claim the headset.
Use launchd for the direct Metal XPC endpoint, rather than starting
`monado-service` in the foreground. The development helper below unloads the
current service registration and registers its rebuilt sibling service on demand.
It captures the shell's relevant runtime variables into a temporary plist;
it does not overwrite the persistent LaunchAgent plist. In the setup terminal:

```sh
cd ~/Code/monado-2
export XRT_MACOS_PASSTHROUGH_CALIBRATION="$PWD/.build/passthrough-calibration-guide/passthrough-candidate-v1.json"
PSVR2_CAMERA_STREAMS=1 PSVR2_CAMERA_MODE=16 PSVR2_AUXILIARY_STREAMS=0 \
PSVR2_SENSE_6DOF=0 \
  .build/native-service-check/src/xrt/targets/service/monado-service-xpc-control bootstrap
```

The helper registers the service without starting a headset session. The client
activates it through XPC. In the same or a second terminal, first use service
compositing:

```sh
cd ~/Code/monado-2
XR_RUNTIME_JSON="$PWD/.build/native-service-check/openxr_monado-dev.json" \
XRT_MACOS_CLIENT_COMPOSITOR=0 \
  .build/native-service-check/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
  --passthrough-only
```

Confirm the calibration info log in
`/tmp/monado-service-launchd.<uid>.err.log` before interpreting the image.
The helper prints the exact stdout/stderr paths. Check left/right
assignment, upright orientation, comfortable stereo and stationary board/room
geometry first. Then exit and run the same diagnostic with `--passthrough` to
check transparent virtual content over the cameras. For the hosted-client check,
set `XRT_MACOS_CLIENT_COMPOSITOR=1` **and** the calibration path in the client
terminal too: each process builds its own UV maps from the same file. Keep the
service's camera settings above. Record the tested commit and distinguish
stationary geometry from movement-induced lag, near-object parallax and edge
artifacts. An unset calibration path plus a restarted presenter provides the
approximate-map comparison.

After closing the diagnostic, restore the persistent registration:

```sh
.build/native-service-check/src/xrt/targets/service/monado-service-xpc-control bootout
launchctl bootstrap "gui/$(id -u)" \
  "$HOME/Library/LaunchAgents/org.freedesktop.monado.service.plist"
```

This reloads the existing persistent service path and settings. To repeat this
experiment with different service variables, run the development `bootstrap`
command again; changing only the client shell cannot update the running service.

Offline validation: macOS service/runtime/diagnostic build, schema/projection
CTest (including OpenCV fisheye reference values, coordinate signs, inverse
rotation and invalid-file handling), and Python transfer tests. No calibrated
hardware rendering result is claimed yet.


## First runtime attempt selected a simulated HMD, 2026-10-08

At `1f7186287`, the user reported black passthrough, badly distorted normal
rendering and no head tracking using the development launchd registration.
The loaded job pointed at `.build/native-service-check`, with camera mode 16
and the candidate path correctly supplied. Both hardware drivers were `OFF`
in that build's CMake cache. The service log selected `Simulated HMD`, while
the macOS presenter still selected the attached physical PS VR2 display, and
logged `PS VR2 passthrough unavailable: the head device has no camera source here`.
This run did not exercise PS VR2 tracking, optical distortion or the calibrated
camera mapping; it is not evidence against the transferred calibration.

The software-check build has now been reconfigured with both hardware drivers
`ON` and rebuilt. The native diagnostic now checks the system's reported
passthrough capability before creating a passthrough session, so a simulated
head without cameras produces an explicit error instead of a misleading black
camera view. Before repeating the hardware run, re-run the launchd bootstrap
command above and confirm the service selects `PS VR2`, not `Simulated HMD`.
A calibrated hardware result remains pending.

## Second runtime attempt exposed IPC capability reporting, 2026-10-08

With both hardware drivers enabled, the user received the diagnostic's explicit
`runtime system has no passthrough camera support` error. The service log now
selected `PS VR2 HMD`, accepted the transferred file (`Experimental calibrated
passthrough`) and reported `PS VR2 BC4 passthrough attached`. This fixes the
previous simulated-HMD selection, but does not establish visible camera output.

The service's shared-memory camera-availability flag was propagated to an IPC
head device's `set_passthrough_sinks` hook only inside local-compositor
preparation. With `XRT_MACOS_CLIENT_COMPOSITOR=0`, OpenXR's system capability
query therefore returned false despite the service's attached camera source.
The IPC head now installs that hook at device creation whenever the service
publishes camera availability. Both service and hosted compositing can report
the capability before session creation. The frame-share consumer remains lazy,
so reporting support does not itself attach sinks or start a consumer thread.
The diagnostic capability check remains active. Linux behaviour is unchanged.

The hardware-enabled macOS build and existing passthrough/frame-share/hosted-
client/session CTests pass after the fix. Re-run the same launchd bootstrap and
camera-only diagnostic commands above with the rebuilt binaries; the actual
calibrated image and motion remain unvalidated.


## Calibrated camera-only image visually confirmed, 2026-10-08

After the IPC capability correction at source commit `c0c24f1a0`, the user
repeated the service-composited `--passthrough-only` run with mode 16 and the
transferred candidate. Their report: “Calibration looks good. It feels slightly
behind lag-wise where I remember the PSVR2 on PS5 being, but otherwise fine.”
This establishes visible calibrated camera-only output and acceptable apparent
geometry for this session. It is a subjective comparison from memory, not a
measured PS5 latency comparison or a full-field/metric alignment validation.
Keep the calibration opt-in. No blended-scene or hosted-client result is implied.

The BC4 receive path currently stamps each image with host USB-callback time
(`os_monotonic_get_ns()`), rather than a calibrated exposure timestamp. The
presenter retains the latest image and uploads it at presentation, with a
static calibrated UV map; it does not rotate the camera image from its exposure
head pose to the display head pose. These are concrete timing/reprojection
limitations, but their share of the reported lag has not been measured.

Next: run `--passthrough` with the same service-composited setup to check the
virtual scene over the cameras. Then assess hosted compositing separately.
For latency work, instrument camera arrival-to-presentation age and establish
BC4 exposure timestamps/pose association before adding late rotational camera
reprojection. Arrival age alone cannot measure exposure-to-display latency;
near-object translational parallax still requires depth or a reference plane.
Do not request more board captures merely because this timing issue remains.
