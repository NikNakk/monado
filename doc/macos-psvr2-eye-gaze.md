<!-- SPDX-License-Identifier: BSL-1.0 -->

# PS VR2 eye gaze on macOS

The macOS PS VR2 driver supports `XR_EXT_eye_gaze_interaction` using the
headset's gaze interface.

Enable the gaze USB stream before starting `monado-service`:

```sh
launchctl setenv PSVR2_GAZE_STREAMS 1
```

The Sony calibration blob, when available, is read from:

```text
~/Library/Application Support/monado/psvr2/eye_calibration.bin
```

## Visual gaze test

```sh
XR_RUNTIME_JSON="$PWD/build-wine/openxr_monado-dev.json" \
./build-wine/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test --gaze
```

A yellow marker is rendered 2 m along the gaze ray returned through the standard
OpenXR eye-gaze interaction profile.

## User calibration

Run:

```sh
XR_RUNTIME_JSON="$PWD/build-wine/openxr_monado-dev.json" \
./build-wine/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test --gaze-calibrate
```

The calibration is deliberately expressed in eye/head coordinates rather than
world coordinates:

- each target is rendered 2 m away at a known yaw/pitch relative to the current
  VIEW pose;
- gaze is sampled by locating the gaze action space relative to VIEW space;
- ordinary head motion therefore cancels out of the calibration;
- nine targets cover centre, horizontal/vertical extrema and four diagonals;
- each target has a 0.9 s settling period followed by a 1.2 s capture period;
- the median measured yaw/pitch at each target is used to reject transient
  samples and saccades;
- separate least-squares linear fits solve yaw and pitch gain plus offset.

The target is blue while settling and green while samples are being captured.

The result is saved to:

```text
~/Library/Application Support/monado/psvr2/gaze_user_calibration.txt
```

Format:

```text
PSVR2_GAZE_USER_CALIBRATION_V1 <yaw_gain> <yaw_offset_deg> <pitch_gain> <pitch_offset_deg>
```

Restart `monado-service` after calibration so the driver reloads the file,
then verify with `--gaze`.

If a calibration file already exists, the calibration app fits the residual
error in the currently reported OpenXR gaze and composes that residual transform
with the saved transform. This makes repeated calibration a refinement rather
than a double application.

Temporary environment overrides remain available and take precedence over the
saved file:

```sh
PSVR2_GAZE_YAW_GAIN
PSVR2_GAZE_YAW_OFFSET_DEG
PSVR2_GAZE_PITCH_GAIN
PSVR2_GAZE_PITCH_OFFSET_DEG
```

For reliable automatic calibration, leave those overrides unset.
