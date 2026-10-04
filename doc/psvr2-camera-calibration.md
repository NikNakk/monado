# PS VR2 mode-4 camera calibration

This branch adds the first stages of a repeatable four-camera calibration workflow for the PS VR2 controller-tracking cameras.

## Physical target

Use a flat A3 ChArUco target with:

- 7 x 5 squares
- nominal 40 mm square length
- nominal 30 mm marker length
- `DICT_4X4_50`

Generate the agreed A3 landscape target with:

```sh
python3 scripts/psvr2_charuco_calibrate.py board /tmp/psvr2-charuco-a3.png
```

Print at 100% / actual size with fit-to-page disabled. Mount the print completely flat and measure the actual printed square length before solving metric calibration. The marker length scales with the same print factor.

## Recording a dataset

The recorder consumes the four existing mode-4 L8 frame sinks. Frames are paired by the PSVR2 hardware `source_sequence`; camera IDs 0/1 originate from camera set 4 and 2/3 from camera set 5. A set is written only when all four cameras for the same sequence are present.

Disk I/O is performed on a separate writer thread. The camera sink only retains frame references, groups synchronized frames, samples the HMD pose, and queues a completed set. The queue is bounded; writer and incomplete-set drops are reported at the end of the run rather than allowing calibration recording to stall USB processing.

Build and run:

```sh
git switch macos-psvr2-camera-calibration
cmake --build build-sense --target cli

PSVR2_CAMERA_STREAMS=1 \
PSVR2_CAMERA_MODE=4 \
PSVR2_ROBUST_CLOCK=1 \
  ./build-sense/src/xrt/targets/cli/monado-cli \
  psvr2-calibration-record /tmp/psvr2-calibration 30 6
```

Arguments are:

```text
psvr2-calibration-record <output-dir> [duration-seconds] [sequence-stride]
```

Defaults are 30 seconds and a sequence stride of 6. At a nominal 60 Hz camera sequence rate that samples about 10 synchronized sets per second, reducing disk traffic while retaining substantially more views than a calibration solve normally needs.

The dataset contains:

```text
dataset.json
manifest.csv
frames/set-000000-camera0.pgm
frames/set-000000-camera1.pgm
frames/set-000000-camera2.pgm
frames/set-000000-camera3.pgm
...
```

`manifest.csv` records:

- dataset set index
- PSVR2 hardware camera sequence ID
- camera exposure timestamp in Monado monotonic time
- raw exposure timestamp in PSVR2 VTS time
- whether the HMD pose query was valid
- relation flags
- HMD position and quaternion at the requested camera exposure timestamp
- all four image paths

The raw VTS timestamp is retained explicitly so later calibration work is not forced to rely on the host clock mapping.

## Inspecting ChArUco visibility

After recording a real board, run:

```sh
python3 scripts/psvr2_charuco_calibrate.py inspect /tmp/psvr2-calibration
```

The tool validates the dataset, detects ChArUco corners in each camera image, prints per-camera detection coverage, and writes:

```text
/tmp/psvr2-calibration/charuco-detections.csv
```

The observations contain synchronized set ID, hardware sequence ID, camera ID, ChArUco corner ID and sub-pixel image coordinates. The inspector returns non-zero for a weak dataset by default if fewer than 20 synchronized sets have at least eight ChArUco corners in at least two cameras.

## Planned solve stages

The next implementation steps are deliberately offline so they can be tested repeatedly without the headset connected:

1. Solve fisheye intrinsics independently for each camera from ChArUco corner correspondences and report per-view/per-camera reprojection errors.
2. Use synchronized views and common ChArUco IDs to solve pairwise fisheye stereo extrinsics for camera pairs with useful overlap, then form a consistent four-camera rig.
3. Estimate board-to-camera poses and combine them with the recorded HMD SLAM poses using hand-eye calibration to solve camera-rig-to-head alignment.
4. Reject poorly conditioned datasets and write a versioned, per-headset calibration JSON containing intrinsics, distortion, camera-to-rig transforms, rig-to-head transform, fit statistics and target dimensions.
5. Add an opt-in Monado runtime loader that attaches the calibrated four-camera streams to the constellation tracker. Invalid or absent calibration must leave the existing 3-DoF controller fallback unchanged.

Do not tune controller constellation pose solving against uncalibrated camera geometry. The calibration JSON is the boundary between the acquisition/calibration work and runtime 6-DoF integration.

## Direct native mode-4 ChArUco alignment (2026-09-13)

The 21 static mode-4 captures under `/tmp/char-mode-12` use eight frames per
camera per pose. Cameras 0/1 are set 4 planes 0/1; cameras 2/3 are set 5
planes 0/1. Columns 508–511 of each 512 x 508 transport frame are padding.
Greyscale averaging and stretching aid detection only; the 508 x 508 native
pixel coordinates are unchanged. Reproduce the solve and validation with:

```sh
.venv/bin/python scripts/psvr2_tracking_charuco_direct.py \
  /tmp/char-mode-12 --output /tmp/psvr2-mode4-charuco-direct.json
.venv/bin/python scripts/psvr2_tracking_charuco_align.py \
  /tmp/psvr2-mode4-charuco-direct.json \
  /private/tmp/psvr2-camera-calibration-reviewed.json \
  /tmp/psvr2-mode4-tracking-calibration-provisional.json \
  --output /tmp/psvr2-mode4-charuco-aligned-candidate.json
.venv/bin/python scripts/psvr2_tracking_charuco_sense_validate.py \
  /tmp/psvr2-mode4-charuco-aligned-candidate.json /tmp/sense-validation \
  --source /tmp/psvr2-mode4-tracking-calibration-provisional.json \
  --output /tmp/psvr2-mode4-charuco-sense-validation.json \
  --validated-output /tmp/psvr2-mode4-charuco-validated-opt-in.json
```

The direct solver estimates `T_camera0_camera`, mapping each OpenCV camera
frame into native camera0. The reviewed calibration stores `T_rig_camera` in
visible camera0. Both are camera-to-rig transforms. OpenCV camera axes are +x
right, +y down, +z forward. The standard candidate converts OpenCV transforms
to XRT poses by `C T C`, with `C = diag(1,-1,-1,1)`, for +x right, +y up,
-z forward. Its runtime image size is 512 x 508; K/D retain the active native
508 x 508 coordinate system.

The direct lower baseline is 80.585 mm versus 78.579 mm in the reviewed
calibration. Before alignment their translation vectors differ by 23.081 mm
and relative rotations by 30.997°. No single rigid transform can make both
native camera axes equal the visible axes. The alignment estimates one common
rotation from both lower orientations, then a least-squares translation from
both camera centres, and applies it to all four direct poses. Each lower camera
retains a 15.499° axis difference and 1.270 mm centre difference. The candidate
records those residuals. This is a plausible frame placement for an opt-in
test, not proof of absolute HMD-frame accuracy. The reviewed calibration's
SLAM hand-eye rotation is marked trusted but its translation is not. Its
diagnostic `T_tracker_rig` is preserved in candidate provenance but is not
applied; the candidate uses the existing visible-camera0 tracking origin.

On six independent left-Sense `V*` poses, native cam0/1 stereo reconstructed
30 points. The sparse V04/V05 poses used three-point lower anchors and are
flagged in the validation JSON. At 5, 10, and 20 px thresholds, cam2 matched
all 28 possible blobs at 1.303 px RMS and cam3 matched all 30 at 1.637 px RMS.
Combined upper-camera RMS was 1.485 px. V03 cam2 matched 5/5 at 0.890 px RMS.
The Sense data did not change ChArUco K/D or rig poses. This validates
multi-camera geometric consistency offline; right-controller performance,
dynamic behavior, and absolute HMD-frame accuracy remain unverified. Both
candidate files retain `runtime_usable: false`. The validated copy is for an
explicit opt-in `monado-cli psvr2-constellation` live test only.

### Opt-in live diagnostics

The second continuous-IR run (`PSSENSE_FORCE_IR=1`) is retained at
`/tmp/psvr2-charuco-force-ir-2.log` (SHA-256
`061db76c3b08230457cbefa1a65c7a253efc628c544cfae1982d60cb7942977f`)
with frames in `/tmp/psvr2-charuco-force-ir-capture-2`. It recorded 1,290
candidates by camera `[371,373,292,254]`, 366 fused poses, 42 disagreements,
and one jump rejection over 15 seconds. Position tracking appeared in 148/150
CLI samples. All reported fused poses used only two cameras: the current
device callback emits a group as soon as two candidates agree and marks it
emitted, so later synchronized cameras cannot join that pose. Independently
grouping the candidate log by timestamps within 1 ms found 181 groups with
all four cameras; 146 had every pair within the current 80 mm / 35° fusion
limits. Across all 181 groups, the median worst-pair difference was 4.96 mm
and 1.71°. Thus the live data support four-camera *candidate* consistency,
but the runtime has not yet demonstrated four-camera fused output.

The normal-pulse follow-up, without `PSSENSE_FORCE_IR`, is
`/tmp/psvr2-charuco-force-ir-3.log` (SHA-256
`89e426bf653ed789e835a413b30936bcc403e9ae28c51909cd15a1cdb1121968`)
with frames in `/tmp/psvr2-charuco-force-ir-capture-3`. It recorded 236
candidates `[79,76,76,5]`, four fused poses in the first half-second, one
disagreement, and 72 jump rejections. Normal illumination can therefore
produce candidates and initial fusion at the improved position. The later
cam0/1/2 candidate groups around seconds 3–4 agreed within a median worst-pair
1.95 mm / 1.01° but were about 181 mm from the stale first fused pose, beyond
the current 150 mm jump gate. The search stayed in phase 1; sampled later
frames show the ring unlit despite remaining in view. This is a timing and
reacquisition problem, not evidence to refit ChArUco intrinsics or per-camera
extrinsics. The normal-pulse run's final CLI `PASS` means its minimum two-pose
criterion was met; it does not mean tracking persisted through the 15 seconds.
The calibration remains `runtime_usable: false`.

### Opt-in live recovery experiment

`PSSENSE_CONSTELLATION_LIVE_RECOVERY=1` enables three narrow changes to the
Sense constellation callback for a live A/B test. Once two synchronized camera
candidates agree, the callback waits about 4 ms for candidates from the same
exposure before fusing; it can now fuse three or four cameras. An agreeing
multi-camera optical sample informs LED pulse timing even if the fresh-pose
jump gate rejects it. When the last accepted optical pose is more than 250 ms
old, the next agreeing multi-camera group can reacquire at a new position
without the 150 mm / 60° fresh-pose jump limits. Samples with non-increasing
timestamps remain rejected. The existing matched-blob, reprojection, camera
agreement, and fresh-pose jump limits remain in force. With the variable unset,
the prior callback behavior is unchanged.

The CLI CSV appends `reacquisition_count`, `optical_seen_count`, and cumulative
three-/four-camera fusion counts. Its original `PASS` criterion is still only a
minimum smoke test; inspect position flags, pose age, timing phase transitions,
and fusion counts throughout the run. For the next normal-pulse test, keep the
headset fixed, hold the controller in the common camera view for about five
seconds, then move it between a few positions and hold each for several
seconds. Do not set `PSSENSE_FORCE_IR` for this run:

```sh
PSVR2_CAMERA_STREAMS=1 PSVR2_CAMERA_MODE=4 PSSENSE_FORCE_IR=0 \
PSSENSE_TIMING_DIAG=1 PSSENSE_CONSTELLATION_LIVE_RECOVERY=1 \
PSVR2_CONSTELLATION_CAPTURE_STRIDE=6 \
build-sense/src/xrt/targets/cli/monado-cli psvr2-constellation \
  /tmp/psvr2-mode4-charuco-validated-opt-in.json 30 \
  /tmp/psvr2-charuco-recovery-capture \
  2>&1 | tee /tmp/psvr2-charuco-recovery.log
```

This experiment changes only the opt-in live tracking path. It does not
change the ChArUco K/D, aligned rig poses, calibration JSON, or default
runtime. Offline geometry validation remains the independent calibration
evidence; sustained normal-pulse tracking and absolute HMD-frame accuracy
still require hardware verification.

The first 30-second recovery run is `/tmp/psvr2-charuco-recovery.log`
(SHA-256 `17fea608c19533bbbdc1ac8c413a11042db0149ff3de7f825f8e0025936ef212`)
with capture `/tmp/psvr2-charuco-recovery-capture`. It yielded 3,605 mode-4
frames, zero camera candidates, zero fused poses, zero optical-seen events,
and zero reacquisitions. The controller output used normal 450 us pulses
(`force_ir=0`), and search stayed in phase 1. The scheduled blink offset
cycled through its approximately 3.825–11.813 ms search range twice and
partway through a third time. Each camera wrote 300 sampled PGM frames with
no capture failures. A simple diagnostic count of threshold-80 connected
components with peak intensity at least 180 and width/height at most 50
found no frame with five such components in any camera. Sampled frames show
no illuminated Sense ring. This run never exercised collection or stale-pose
reacquisition; it cannot judge whether the recovery patch works. A
continuous-IR positive control at the *same physical placement* should
separate poor shared-camera visibility from normal-pulse timing failure.

### Static LED phase sweep

The static scan is deliberately independent of pose fusion. With
`PSSENSE_LED_PHASE_SWEEP=1` and an attached constellation tracker, the Sense
driver holds LEDs off for 120 camera sequences (about two seconds), then
commands all LEDs through 67 phases at 250 us increments from 0 through
16.5 ms. Each phase lasts 30 camera sequences (about 0.5 seconds). It repeats
the full scan at 450 us and 2.1 ms pulse widths, then returns to LEDs off.
The normal optical phase search and 3.6 ms macOS phase correction are disabled
only for this diagnostic. The sequence, nominal phase, pulse width, and off
state are logged as `LED_PHASE_SWEEP`; `PSSENSE_TIMING_DIAG=1` also records
programmed and estimated controller-clock timing. Camera CSVs preserve the
hardware sequence and exposure timestamp so image brightness can be grouped
by each commanded phase. This is a commanded-phase scan; the phase of actual
light in each image must be inferred from those images rather than assumed.

First confirm framing with a short continuous-IR control. Keep only the left
controller awake, place it where the second continuous-IR live run tracked
well, and keep the headset and controller rigidly stationary through both
commands. The first command must show several clearly lit controller LEDs in
the saved images; pose candidates are not required for a brightness scan. If
the ring is absent, reposition before starting the sweep.

```sh
PSVR2_CAMERA_STREAMS=1 PSVR2_CAMERA_MODE=4 PSSENSE_FORCE_IR=1 \
PSSENSE_TIMING_DIAG=1 PSVR2_CONSTELLATION_CAPTURE_STRIDE=6 \
build-sense/src/xrt/targets/cli/monado-cli psvr2-constellation \
  /tmp/psvr2-mode4-charuco-validated-opt-in.json 8 \
  /tmp/psvr2-static-ir-control-capture \
  2>&1 | tee /tmp/psvr2-static-ir-control.log
```

After confirming LED visibility without moving either device, run the full sweep:

```sh
PSVR2_CAMERA_STREAMS=1 PSVR2_CAMERA_MODE=4 PSSENSE_FORCE_IR=0 \
PSSENSE_FUTURE_LED_SCHEDULE=1 \
PSSENSE_LED_PHASE_SWEEP=1 PSSENSE_TIMING_DIAG=1 \
PSVR2_CONSTELLATION_CAPTURE_STRIDE=6 \
build-sense/src/xrt/targets/cli/monado-cli psvr2-constellation \
  /tmp/psvr2-mode4-charuco-validated-opt-in.json 75 \
  /tmp/psvr2-static-phase-sweep-capture \
  2>&1 | tee /tmp/psvr2-static-phase-sweep.log
```

The CLI may report `INCOMPLETE` during this diagnostic because many commanded
phases deliberately leave the LEDs dark. Its outcome is the per-phase image
brightness and logged controller schedule, not sustained pose tracking. The
ChArUco calibration and default LED schedule are unchanged.

The first static continuous-IR control is
`/tmp/psvr2-static-ir-control.log` (SHA-256
`06c328c6c07132a9a647baf1ee2bddfbeaf43144c42988b7b5f54c85a36b6691`)
with capture `/tmp/psvr2-static-ir-control-capture`. It recorded 479 processed
frames and 80 saved images per camera, but zero pose candidates. The images
do show the Sense LEDs in all four cameras around seconds 2–3 and 6–7.
Compared with the successful `/tmp/psvr2-charuco-force-ir-capture-2`, this
placement exposes only a shallow, partly hidden arc: roughly four to five
controller LED points per camera in a representative bright frame, compared
with a broad six-to-eight-point arc in the successful capture. The lower
views are nearly edge-on. Therefore the failed positive control does not yet
isolate a pulse-timing fault; repeat it with the controller ring facing the
headset and visibly forming a broad arc before committing to the 75-second
static phase sweep. The sampled illumination is intermittent even under the
force-IR override, as it was in the earlier successful capture; this also
deserves separate timing analysis once the framing control passes.

The second static control (`/tmp/psvr2-static-ir-control-2.log`, SHA-256
`a0f921f42ba7158f348f15e55b6fd569c77acc95687dc54302538f41226f7e4c`)
wrote 80 frames per camera under `/tmp/psvr2-static-ir-control-capture-2`.
All four cameras again see the illuminated LEDs, but the cluster is smaller,
dimmer, and nearly edge-on compared with the successful continuous-IR `-2`
capture. It yielded just one accepted camera-local candidate (cam0, five
matches, 0.127 px reprojection error) near the end and no fused pose. The
tracker logged 329 `Dropping slow sample` warnings in eight seconds, versus
16 in the 15-second successful `-2` run. That warning comes from
`Camera::deferSampleToSlowThread`: a new deferred image replaces an older one
while the slow correspondence search has not caught up. This loses possible
pose candidates but does not mean the raw camera capture failed (480 processed
and 80 saved frames per camera, zero write failures). Sparse/flattened ring
geometry makes bootstrap harder; slow-search overload can further reduce its
opportunities. Neither this control nor the previous one establishes whether
normal-pulse phase assumptions are correct. Because multiple LEDs are visible
in all four cameras, the current placement is sufficient for an image
brightness phase sweep independent of candidate/fusion counts. It is not a
good pose-solver control until the ring presents a broader arc.

The subsequent 75-second sweep used a closer headset/controller placement, so
it is not a controlled same-pose comparison with the earlier static controls.
Its log is `/tmp/psvr2-static-phase-sweep.log` (SHA-256
`d5c3c7bd6d08c66051429746c9d46725f8abaa3adcb9e459f314261c62fcbb7e`),
its captured frames are in `/tmp/psvr2-static-phase-sweep-capture`, and the
reproducible image analysis is `/tmp/psvr2-static-phase-sweep-analysis.json`.
Each camera wrote 749–750 stride-six images with zero failures. The sweep
logged every intended stage: initial off, 67 phases with 450 us pulses, 67
phases with 2.1 ms pulses, and final off. Analysis uses only the active
508×508 pixels, compares each image with the median initial-off image, ignores
the first six camera sequences after each stage transition, and counts pixels
at least 80 grey levels above that baseline within the LED cluster ROI. Its
command and selected native-pixel ROIs are:

```sh
.venv/bin/python scripts/psvr2_tracking_led_phase_analyze.py \
  /tmp/psvr2-static-phase-sweep-capture /tmp/psvr2-static-phase-sweep.log \
  --roi 265 130 345 175 --roi 135 130 215 175 \
  --roi 360 225 425 290 --roi 55 220 130 265 \
  --output /tmp/psvr2-static-phase-sweep-analysis.json
```

All four cameras show the same sustained short-pulse window at commanded
phases 2.25, 2.50, and 2.75 ms. Across those three stages, 11 of 12 sampled
images per camera have at least ten bright ROI pixels; the peak median count
is 63–79 pixels depending on camera. In the 3–11 ms part of the 450 us scan,
only one of 132 sampled images per camera reaches that threshold. The wide
2.1 ms pulse lights all four cameras over phases 15.75–16.5 ms and 0–2.0 ms,
wrapping the approximately 16.683 ms camera period: 49 of 52 images per
camera have at least ten bright pixels in that interval. The initial off
stage is dark in all sampled ROIs. There was a separate two-frame flash at
13.0 ms, and one at 13.25 ms, in every camera during the wide-pulse pass;
adjacent stages were dark. Preserve these frame-level outliers for clock/
scheduling diagnosis, but do not interpret them as a stable second window.

The driver programs the controller's pulse **center** at commanded phase plus
half the pulse width. The short-pulse sustained phases therefore correspond
to programmed center offsets 2.475–2.975 ms after the projected exposure.
The failed normal-pulse recovery run searched programmed center offsets
3.825–11.8125 ms, missing this observed window at the sweep placement. This
is direct evidence that the present bootstrap phase range can miss the light;
it does not prove a universal 2.7 ms setting because placement, device clock
mapping, or scheduling may differ across runs. The wider pulse's broader
window is consistent after accounting for its 1.05 ms center offset and
period wrap. The sweep recorded 958 pose candidates by camera
`[328, 0, 328, 302]` and 327 two-camera fused poses, almost entirely during
the wide-pulse pass. Cam1's raw LED pixels are clearly visible despite zero
accepted cam1 pose candidates, so pose-solver yield and slow-thread drops must
not be used as the brightness measure. The CLI `PASS` is only its minimum
pose-count smoke test, not evidence of sustained narrow-pulse tracking.

The next timing experiment should keep the native ChArUco calibration fixed
and try an opt-in bootstrap scan that covers the whole camera period or starts
with a longer pulse, then checks whether a narrow-pulse lock persists. The
static scan does not justify changing default runtime timing or declaring the
calibration runtime usable.
