<!--
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

- use the PS VR2 camera intrinsics/extrinsics rather than the initial tunable
  fisheye/convergence approximation;
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
