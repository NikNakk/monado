<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# PS VR2 passthrough on macOS

The macOS PS VR2 port has an experimental camera-backed implementation of
`XR_FB_passthrough`.

This path is designed for a stock PS VR2 headset. It does **not** require the
headset jailbreak used by some PSVR2Toolkit features such as headset vibration.

## Data path

```text
PS VR2 front cameras
  interface 6 / endpoint 0x87
  mode 0x10
  "VI" + 256 byte header
  two 1024x1016 BC4 planes
          |
          v
PS VR2 driver
  XRT_FORMAT_BC4 frames
  left/right xrt_frame_sink
          |
          v
macOS main compositor
  latest camera frames
  BC4 MTLTextures
          |
          v
XR_FB_passthrough layer state
  OpenXR -> IPC -> multi compositor -> native compositor
          |
          v
final Metal presentation pass
  camera background
  optional application compositor image over camera
          |
          v
CAMetalLayer / PS VR2
```

The camera transport and initial projection model follow the stock-headset path
validated by [GAV PSVR2 Player for macOS](https://github.com/NikNakk/gav-psvr2-player-mac).

## Enabling the camera stream

The camera stream is opt-in because two 1024x1016 BC4 images at about 60 Hz are
not needed for ordinary VR rendering:

```sh
export PSVR2_CAMERA_STREAMS=1
```

When `monado-service` is started on demand by launchd, set the variable in the
launchd environment before starting the service:

```sh
launchctl setenv PSVR2_CAMERA_STREAMS 1
```

Unset it later with:

```sh
launchctl unsetenv PSVR2_CAMERA_STREAMS
```

## Diagnostic application

Camera only:

```sh
PSVR2_CAMERA_STREAMS=1 \
XR_RUNTIME_JSON="$PWD/build-macos-psvr2-display/openxr_monado-dev.json" \
./build-macos-psvr2-display/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
  --passthrough-only
```

Passthrough behind the normal diagnostic scene:

```sh
PSVR2_CAMERA_STREAMS=1 \
XR_RUNTIME_JSON="$PWD/build-macos-psvr2-display/openxr_monado-dev.json" \
./build-macos-psvr2-display/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
  --passthrough
```

Both modes use the real `XR_FB_passthrough` API. They are not private
compositor debug switches.

## Experimental calibrated projection (2026-10-08)

`XRT_MACOS_PASSTHROUGH_CALIBRATION` selects a transferred, versioned calibration
file. The presenter applies calibrated lens distortion and camera-to-head
rotation to its optical rays. This is a rotation-only mapping at infinity;
the user visually confirmed camera-only and blended-scene service compositing
on 2026-10-08.
Perceived lag remains; measured head alignment, edge coverage and latency need
further assessment.
See the [candidate and headset commands](macos-psvr2-passthrough-calibration.md#experimental-transferred-runtime-mapping-2026-10-08).
Set the path in the service and, for client compositing, the client environment.
Invalid files warn and use the approximate mapping. Unset keeps the default.

## Initial projection model

The first implementation deliberately uses the simple model already proven by
GAV rather than blocking on complete PS VR2 camera calibration.

The compositor generates a per-eye UV map once from the HMD driver's existing
`compute_distortion()` callback. It recovers the output ray from the green
channel and maps that ray into the camera image with an equidistant fisheye
model.

Defaults:

- camera FOV: 150 degrees;
- stereo convergence: 0.100 image widths;
- brightness multiplier: 1.60.

They can be adjusted without rebuilding:

```sh
export XRT_MACOS_PASSTHROUGH_FOV_DEG=150
export XRT_MACOS_PASSTHROUGH_CONVERGENCE_MILLI=100
export XRT_MACOS_PASSTHROUGH_BRIGHTNESS_PERCENT=160
```

## What is implemented

- stock-headset BC4 camera acquisition;
- left/right frame delivery without CPU decompression;
- `XR_FB_passthrough` extension exposure on Apple builds;
- passthrough create/start/layer/resume lifecycle;
- passthrough layer state through IPC and the multi compositor;
- final Metal camera rendering;
- camera-only and camera-behind-application modes;
- capability reporting only on devices with a passthrough camera hook.

## What is not yet implemented

The current result is an experimental room-view implementation, not yet a
geometrically calibrated MR camera system.

Remaining work includes:

- validate hosted compositing with the transferred mapping (camera-only and
  blended-scene service compositing are visually confirmed);
- associate camera frames with hardware timestamps and head poses;
- camera reprojection / late correction;
- robust stream restart if camera delivery stalls;
- passthrough style/color-map controls;
- projected passthrough and depth-aware MR occlusion;
- calibrated stereo reconstruction where appropriate;
- application/shell UX such as double-pressing the HMD function button.

GAV's double-press behaviour is application logic: two function-button clicks
within its timeout toggle the camera view. Once the runtime path is validated,
SwiftXRShell can implement the same interaction while keeping applications on
standard OpenXR.

## Validation and calibration plan, 2026-10-08

The API is exposed to applications through `XR_FB_passthrough`, but a visible
PS VR2 camera-plus-scene hardware result was still pending when this plan
was written; it is now visually confirmed below. Extension enumeration and successful lifecycle calls alone
are insufficient. PS VR2 currently advertises only the `Opaque` environment
blend mode (`psvr2.c`); selecting `AlphaBlend` in hello_xr is therefore not the
passthrough test. FB passthrough submits a camera layer beneath a projection
layer whose background is transparent. The native diagnostic's `--passthrough`
mode exercises this path; `--passthrough-only` isolates camera delivery.

First run those two modes with cameras enabled in the service environment,
then repeat with client-hosted compositing. Record the exact build, session
lifecycle, visible camera image, virtual-object opacity, head motion and
teardown. Audit unsupported purposes/style requests before describing the
implementation as a complete FB passthrough implementation. Automatic room
view for arbitrary opaque applications is not implemented.

Recommended calibration sequence:

1. Record the actual mode-0x10 BC4 stereo images used for passthrough, including
   native dimensions, crop/orientation, frame sequence, hardware timestamps and
   HMD poses. Do not assume the mode-4 512x508 tracking calibration can be scaled
   into the 1024x1016 passthrough images without validating their correspondence.
2. Reuse the existing printed ChArUco target and offline tooling, adapting the
   dataset reader to this stereo mode. Measure the printed square size. Capture
   synchronized pairs across the image, at varied distances and board tilts.
3. Fit each fisheye camera's intrinsics/distortion, then stereo extrinsics;
   check withheld views and epipolar residuals, not only fitting error. OpenCV's
   [fisheye model and stereo calibration](https://docs.opencv.org/4.13.0/db/d58/group__calib3d__fisheye.html)
   provide the underlying operations already used by the calibration workflow.
4. Solve and verify camera-to-head alignment against timestamped SLAM poses,
   with explicit coordinate conventions. Replace the FOV/convergence UV map
   with calibrated camera rays projected from each eye. Camera and eye centres
   differ: calibration alone cannot remove near-field parallax at every depth.
5. Measure camera age and time alignment, then add rotational late correction.
   Positional reprojection and near-field occlusion need scene depth or a
   declared reference plane; they are later work.

Keep calibration per headset and versioned, and retain the current approximate
path as an explicitly experimental fallback until the calibrated path passes
stationary and moving-head checks.

The [step-by-step ChArUco capture guide](macos-psvr2-passthrough-calibration.md)
includes the tooling audit, existing mode-4/0x10 correspondence evidence, live
USB-session commands, and the remaining solve/runtime work (2026-10-08).


## Calibrated camera-only result, 2026-10-08

The user reports the calibrated image looks good with `--passthrough-only`,
service compositing and source commit `c0c24f1a0`. They perceive slightly more
lag than they remember on PS5; this is subjective, not a measured comparison.
The BC4 path currently timestamps USB receipt and uses a static UV map without
camera-pose late reprojection. Measure timing and establish exposure-pose
association next; retain the geometry result and keep blended-scene/hosted
validation separate. See the [detailed record](macos-psvr2-passthrough-calibration.md#calibrated-camera-only-image-visually-confirmed-2026-10-08).


## Calibrated blended-scene result, 2026-10-08

The follow-up `--passthrough` run with service compositing also received the
user's “Works well” confirmation. Both native diagnostic camera modes are now
visually confirmed. Hosted compositing and latency work remain separate; see
[the detailed record](macos-psvr2-passthrough-calibration.md#calibrated-blended-scene-visually-confirmed-2026-10-08).
