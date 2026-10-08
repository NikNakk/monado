<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# Floor calibration of the managed STAGE space

Date: 2026-10-05. Branch `claude/stage-floor-calibration`, based on `619b57c84`.

## Problem

The PS VR2 driver reports poses relative to the headset's own tracking origin,
the point where its tracking started. Nothing in that origin marks the
physical floor. With `PSVR2_STAGE_SPACE` off (the default), the space overseer
manages STAGE as an identity offset from root, so STAGE and every LOCAL_FLOOR
put the floor near head height. Apps that use a floor-based space (Open Brush,
OpenComposite titles) then place the user under the floor.

The earlier workaround is `PSVR2_STAGE_SPACE=1` plus
`PSVR2_RECENTER_ON_FIRST_POSE=1` and `PSVR2_RECENTER_EYE_HEIGHT_M`. It assumes
the user is standing at that eye height when tracking starts. It measures
nothing.

Monado can already move a managed STAGE at runtime
(`set_reference_space_offset`, libmonado `mnd_root_set_reference_space_offset`).
Two gaps prevented that from working as a floor calibration:

1. With per-app local spaces (the default target builder), each app's
   LOCAL_FLOOR was created under root with the stage height copied at
   creation. A later stage change did not move running apps' floors, even
   though their REFERENCE_SPACE_CHANGE_PENDING events were sent.
2. No tool measured the floor.

## Change

- `b_space_overseer`: when STAGE is managed (an offset space directly under
  root), per-app LOCAL_FLOOR is created as a child of STAGE at its floor, so
  stage changes move it. A driver-provided STAGE keeps the previous behaviour.
  Per-app local-space creation also no longer crashes when there is no head
  (no view space).
- `monado-ctl` gains floor calibration:

```sh
# Stand upright wearing the headset; give your eye height in metres.
monado-ctl --floor-eye-height 1.75
# Or leave a controller lying still on the floor, in view of the headset.
monado-ctl --floor-device left
monado-ctl --floor-device right --floor-device-height 0.03
```

  The head pose, or the controller's grip pose, is located in root through
  existing IPC calls and averaged over 0.5 s. Untracked poses, or movement of
  more than 1 cm, are refused. Only the stage height changes. The
  controller's resting grip height (default 0.03 m) is provisional and should
  be measured against the eye-height method on hardware. On PS VR2 a
  controller lying on the floor is usually not tracked from standing height
  (see Validation); the eye-height method is the practical one.

No IPC protocol, OpenXR or driver change is involved; any app or tool can use
the same reference-space-offset calls.

## Automatic calibration from eye height

Set `XRT_FLOOR_EYE_HEIGHT_M` in the service environment to your standing eye
height. Each time the service starts, a low-rate service thread waits until
all of these hold:

- the head device reports it is worn (`XRT_INPUT_GENERIC_HEAD_DETECT`, the PS
  VR2 proximity sensor; devices without presence are assumed worn);
- its position is tracked;
- it is roughly level, with pitch within 20 degrees;
- it is steady, within 1 cm for 1 s.

It then sets the managed STAGE floor at the averaged head height minus the eye
height, using the same reference-space-offset call as `monado-ctl`. Running
apps' floors move immediately. The calibration is applied once per service
run, so taking the headset off and on does not move the floor. It is skipped
if the STAGE has already been moved, for example by `monado-ctl`, which can
still correct it at any time.

Put the headset on standing and look ahead for a second. Putting it on while
seated puts the floor too high by the difference in eye height; stand and run
`monado-ctl --floor-eye-height` to correct it.

The decision rules are in `u_floor_calibration` (aux_util) and are unit-tested
with a fake head device through the real space overseer. The service glue is
`ipc_server_floor_calibration.c`.

## Use with PS VR2

- Leave `PSVR2_STAGE_SPACE` unset. A driver-provided STAGE cannot be moved,
  and `monado-ctl` reports this.
- `PSVR2_RECENTER_ON_FIRST_POSE` is not needed.
- With `XRT_FLOOR_EYE_HEIGHT_M` set, no manual step is needed. Otherwise
  calibrate after the service has started, either before or while an app runs.
  Running apps receive reference-space-change events and their floor moves.
- The calibration is runtime state. It is lost when the service exits; with
  `IPC_EXIT_WHEN_IDLE=1` that happens a few seconds after the last client
  closes. It is also wrong after the tracking origin resets (for example a
  headset reconnect), so recalibrate then.

## Validation

- `tests_space_overseer`: shared and per-app LOCAL_FLOOR before and after a
  stage calibration, plus an app created afterwards. The per-app case fails
  without the overseer change (floor stays at 0 instead of -1.2 m).
- Simulated service from this branch with a headless OpenXR client. Setting
  STAGE y to -1.2 m mid-session delivered STAGE and LOCAL_FLOOR change events.
  Head height in both LOCAL_FLOOR and STAGE moved from about 1.6 m to about
  2.8 m without restarting the app.
- `monado-ctl` correctly refused the simulated head, whose position is not
  tracked.
- PS VR2 hardware, 2026-10-05: service and x86_64 in-process client built from
  `ba9bdd6ae`, Underture through the in-process D3DMetal bridge with the
  client compositor. Running `monado-ctl --floor-eye-height 1.75` during the
  game placed the floor correctly, as the user confirmed.
  `--floor-device left` with the controller lying on the floor was refused as
  untracked: the headset cameras do not track a Sense controller on the floor
  from standing height. The refusal is intended; the controller method needs
  the controller in view (for example crouching and looking at it), and its
  0.03 m resting height remains unmeasured. Use the eye-height method.

## Future work

- A seated mode, or detecting seated use, for the automatic calibration.
- A controller-button or in-headset trigger for recalibration.
