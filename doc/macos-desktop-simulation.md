<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# Simulated HMD in a desktop window

Added 18 October 2026 on `claude/wsi-fragment-output`. On 19 October the user ran it
on a Mac with `psvr2-openxr-test --passthrough`: the room and scene rendered and the
trackpad controls worked (commit `00567077e`, not compositing in the client).
At `e4411508c` the close button stopped the client and service cleanly, and the
WSI presenter looked right with `XRT_COMPOSITOR_COMPUTE` both on and off. With
client compositing (`XRT_MACOS_CLIENT_COMPOSITOR=1`) the scene, room and close
button also worked, once the client had the presenter variables too (below).

`SIMULATED_DESKTOP=1` runs Monado without a headset. It uses the simulated HMD and
shows the compositor's output in an ordinary window on the Mac's built-in display
(or on the main display if there is no built-in one). The trackpad steers the head:

| Input | Effect |
| --- | --- |
| Click in the window | Capture the pointer (it is hidden and stays put) |
| One finger, pointer captured | Turn: left/right is yaw, up/down is pitch (0.15° per point, pitch limited to ±89°) |
| Two fingers | Move across the floor relative to the heading: away from you is forward, sideways strafes (2 mm per point, no inertia) |
| `R` | Back to the starting pose |
| `Esc`, or switching to another window | Release the pointer |

The two-finger direction follows the fingers whatever the natural-scrolling setting.
A mouse wheel also works, scaled from lines to points.

## Use

Set the variable in the service's environment. For client compositing
(`XRT_MACOS_CLIENT_COMPOSITOR=1`), set it in the client's environment as well, so
both size the layer the same way. Both presenters work:

```sh
# WSI compute path: the earlier flicker/head-lock case, without a headset.
SIMULATED_DESKTOP=1 XRT_MACOS_PRESENTER=wsi XRT_COMPOSITOR_COMPUTE=1 monado-service
```

With the experiment runner, pass these through `--service-env`. The variable also
turns on the simulated builder, so `SIMULATED_ENABLE` is not needed. The prober
takes the first builder in its list that is certain of a head (it does not compare
priorities), and the qwerty and simulated builders come before the PS VR2's. So
they are chosen even with a headset attached, and the headset is left unclaimed.

The client needs the service through launchd, which serves the Metal XPC
endpoint, so a hand-started `monado-service` does not work. The development
LaunchAgent copies `XRT_*` and similar variables from the shell but not
`SIMULATED_*`, so set that one in launchd's environment:

```sh
launchctl setenv SIMULATED_DESKTOP 1
XRT_MACOS_PRESENTER=wsi XRT_MACOS_WSI_PASSTHROUGH=1 \
  ./build/arm64/src/xrt/targets/service/monado-service-xpc-control bootstrap
XR_RUNTIME_JSON="$PWD/build/arm64/openxr_monado-dev.json" XRT_MACOS_CLIENT_COMPOSITOR=0 \
  ./build/arm64/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test --passthrough
```

Afterwards, `launchctl unsetenv SIMULATED_DESKTOP`, run
`monado-service-xpc-control bootout`, and bootstrap the persistent LaunchAgent
again.

## Qwerty controllers

Add `QWERTY_ENABLE=1` to the service's environment (through `launchctl setenv`,
like `SIMULATED_DESKTOP`) to use upstream's qwerty devices instead of the simulated
HMD: a 1280x720 HMD and two controllers with simple-controller bindings. They take
their input from the same window, not from qwerty's SDL debug window.

| Input | Effect |
| --- | --- |
| Click in the window | Capture the pointer. Later clicks are the trigger |
| Secondary click (two-finger click) | Squeeze |
| Pointer, once captured | Turn the focused device |
| Two fingers | Move the focused device relative to its orientation (2 mm per point) |
| W A S D, Q E | Move forward, left, back, right; down, up |
| Arrow keys | Turn |
| Hold left Shift | Sprint |
| Hold left Control / left Option | Focus the left / right controller instead of the head |
| N, B | Menu, system |
| T F G H, V | Thumbstick up, left, down, right; click |
| I J K L, M | Trackpad up, left, down, right; click |
| C | Controllers follow the head, or stop following |
| R | Reset the controllers (the focused one, or both) |
| `-` / `=`, or a mouse wheel | Slower / faster movement for the focused device |
| `Esc`, or switching window | Release the pointer; switching also releases every key |

Controller-only input with the head focused goes to the right controller. The
keys are positional (US layout positions on any keyboard). Qwerty moves "per
frame": each key-held step happens on every pose query, so its speed depends on
how often the compositor and application ask. Trackpad moves and pointer turns
are applied once and do not have this problem.

## What it covers

- The window keeps the simulated HMD's panel aspect (16:9) at up to 90% of the
  visible screen. Its size in pixels becomes the compositor target, so nothing is
  stretched. It is fixed-size. Its close button stops the service: each connected
  application is asked to exit its session (as if it had called
  `xrRequestExitSession`), and the service stops once they have disconnected, or
  after 3 seconds if one does not respond.
- Presentation, pacing (from the built-in display's link), the compute/graphics
  compositors, timewarp and the WSI fragment output pass all run as with the
  headset. The simulated HMD has no lens distortion.
- Refresh rates: the built-in display offers only its current rate, and Monado never
  changes its mode. App refresh requests for other rates fail.
- Not covered: anything that needs PS VR2 hardware, including camera passthrough,
  eye gaze and the PS VR2 distortion and panel timing. The window is
  WindowServer-composited, so direct-scanout questions still need the headset.

## Simulated passthrough room

With passthrough, the WSI camera pass draws a simple room behind the
application instead of PS VR2 camera images. The room is 5 × 2.7 × 5 m with the
floor at 0 and a 0.5 m grid. Each face has its own colour: blue ahead (-Z), red
to the left, purple to the right, yellow behind, a green floor and a grey ceiling.

It is drawn at exactly the eye poses the compositor timewarped that image to, so
the room and the application's content should never slip against each other.
If they do, while the same scene is steady without passthrough, the fault is in
composition or presentation rather than in camera handling. A later
"camera-like" mode could draw the room at an older pose and correct it like the
real cameras, to test that correction too.

```sh
SIMULATED_DESKTOP=1 XRT_MACOS_PRESENTER=wsi XRT_MACOS_WSI_PASSTHROUGH=1 monado-service
psvr2-openxr-test --passthrough   # or --passthrough-only, or any XR_FB_passthrough app
```

- The simulated HMD advertises `XR_FB_passthrough` (a stub camera hook) only with
  `SIMULATED_DESKTOP`. The service also flags passthrough to clients, which draw
  their own room when compositing in-process. Such a client needs
  `SIMULATED_DESKTOP=1 XRT_MACOS_PRESENTER=wsi XRT_MACOS_WSI_PASSTHROUGH=1` in its
  own environment; without the presenter variables it uses the native presenter,
  warns, and shows the scene with no room.
- The WSI presenter only, which uses compute when passthrough is on. The native
  presenter declines with a warning; matching it in Metal, as the visual control,
  is a later step.
- Rays come from the HMD's lens model and field of view, through per-eye maps
  interpolated in the shader. For a headset without lenses (the simulated HMD),
  the maps are built straight from the field of view with no device queries.

## Code

- `auxiliary/util/u_desktop_pose.c`: the pose state (yaw, pitch, floor position)
  behind a mutex; tested by `tests/tests_desktop_pose.cpp`.
- `auxiliary/util/u_desktop_input.c`: a bridge for the window's key, pointer and
  trackpad events (keys as USB HID usages). A driver registers to receive them;
  when none has, the window steers `u_desktop_pose`.
- `drivers/qwerty`: `qwerty_input.c` maps those events to qwerty actions. The SDL
  path (`qwerty_sdl.c`) now converts SDL events and uses the same mapping. In
  desktop mode the qwerty system registers with the bridge, and its HMD
  advertises the simulated-room passthrough. Tested by
  `tests/tests_qwerty_desktop_input.cpp`.
- `drivers/simulated`: `SIMULATED_MOVEMENT_DESKTOP` reads that pose relative to
  the HMD's centre (1.6 m eye height from the simulated builder).
- `compositor/main/comp_macos_frontend.m`: display choice, the window and the
  `MonadoDesktopView` input handlers. The service's main loop already pumps AppKit
  events, so input arrives on the main thread.
- `compositor/main/comp_macos_display.m`: built-in panels report one rate and
  refuse mode changes.
- `compositor/main/comp_sim_room_passthrough.c`: the room source (ray maps and
  per-frame eye poses). `render/shaders/camera.frag` draws the room in
  `RENDER_CAMERA_MODE_ROOM`; `comp_renderer.c` records each image's eye poses
  beside the existing display orientation.

## Validation

- Linux build has no warnings and `tests_desktop_pose` passes (directions, pitch
  limit, reset).
- Room: Lavapipe GPU tests compare the production shader against a CPU reference
  of the room for three poses (ahead, turned left, looking down), without, under
  and behind application content, through standalone and private-output
  sources. All pixels match exactly, with Khronos and synchronization validation
  clean. A deliberate one-colour change in the shader makes the test fail. The
  macOS-only room source and adapter pass a Linux syntax check with the
  compositor's real flags; they are not otherwise built here.
- The Objective-C window and input code is not compiled on Linux. It needs the macOS
  build, then a manual check that the directions feel right.
