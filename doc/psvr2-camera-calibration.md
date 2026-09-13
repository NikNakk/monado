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
