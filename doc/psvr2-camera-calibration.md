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
