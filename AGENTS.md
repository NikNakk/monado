# PS VR2 on macOS: Codex handover

This repository contains experimental work toward using a wired Sony PS VR2 as
a native OpenXR HMD on Apple Silicon macOS. Read this file and
`doc/macos-port.md` before changing the macOS, PSVR2, compositor, or OpenXR
paths.

## Objective and first acceptance test

The immediate goal is a headset-only MVP:

- PS VR2 discovered through Monado's normal hardware prober
- 6-DoF HMD pose from the headset's SLAM/IMU path
- native macOS Metal OpenXR application support
- stereo output on the directly connected PS VR2 display
- Khronos `hello_xr -g Metal` running in the headset

Sense controllers, eye tracking, passthrough, boundary support, Windows games,
and OpenXR conformance are explicitly outside the first milestone.

## Branch and upstreams

- Working fork: `NikNakk/monado`
- Working branch: `macos-psvr2-mvp`
- Branch base: `kzahel/monado:combined` at
  `9c1dbc398261fa8136dbb3fbae34ac9739132847`
- macOS PSVR2 reference: `GAVProject/gav-psvr2-player-mac`
- macOS Metal/OpenXR/IOSurface work: `kzahel/monado:combined`
- Packaging/reference wrapper: `kzahel/wivrn-macos`
- Streaming reference only: `kzahel/WiVRn:combined`
- Future Windows/OpenVR reference: `cbusillo/macos-game-patches`

GAV is the known-good hardware baseline and a reference implementation. Keep it
separate initially; do not copy its code into this repository without checking
licensing and identifying the smallest required part.

## Work completed as of 2026-09-02

The first implementation commit is
`8f591e05fbabcaf8018905a0852f2bcde3c55cb2` and the macOS CI commit is
`eb9d4d31cb7d408c28980249b2559c7d12a42386` on the remote branch.

Implemented changes:

- `CMakeLists.txt` permits the PSVR2 HMD driver on Apple platforms with libusb;
  internal HID remains required only by the separate PSSENSE driver.
- `src/xrt/drivers/psvr2/psvr2.c` defaults macOS to a conservative HMD mode
  claiming only the status and SLAM USB interfaces.
- Camera, gaze, LED-detector, relocalizer, and VD interfaces and transfers are
  disabled in that mode.
- `PSVR2_AUXILIARY_STREAMS=1` opts back into the full stream set. Other
  platforms retain full-stream behavior by default and may use `0` to test the
  minimal path.
- Eye/face callbacks, gaze support, gaze interaction profiles, and camera debug
  UI are not advertised when their streams are disabled.
- Teardown now safely handles USB/data/eye threads and locks that were never
  initialized by minimal mode.
- `doc/macos-port.md` documents the conservative HMD path.
- `.github/workflows/macos-psvr2-driver.yml` builds `drv_psvr2` on `macos-15`.

Validation already completed:

- The macOS driver-only GitHub Actions job passed:
  <https://github.com/NikNakk/monado/actions/runs/33572654233>
- A Linux driver-only compile passed locally.
- `git diff --check` passed before the commits were published.

## Key architecture decision

Do not start by replacing WiVRn's virtual HMD or by running its complete server.
That path is designed for an encoded, streamed headset and uses a bespoke target
instance that bypasses Monado's normal hardware probing.

Instead, use:

1. Monado's normal target instance and hardware prober.
2. The existing PSVR2 `xrt_device`, made safe and buildable on macOS.
3. The existing macOS Metal/OpenXR/IOSurface work in this fork.
4. A new local macOS display compositor target for the PS VR2 display.

The major missing component is the local display target. The preferred first
design is an Objective-C or Objective-C++ target under
`src/xrt/compositor/main`, likely `comp_window_macos.m`, which:

- selects the `NSScreen` named `PS VR2` or, provisionally, a 4000-pixel-wide
  screen
- creates a borderless/full-screen `NSWindow` backed by `CAMetalLayer`
- creates a Vulkan surface through `VK_EXT_metal_surface` under MoltenVK
- feeds that surface to Monado's existing Vulkan compositor so existing layer,
  distortion, and timewarp code remains in use
- later uses `CVDisplayLink` associated with that display for accurate 90/120 Hz
  pacing

GAV's per-scanline rolling-shutter correction is a later latency/quality
optimization, not a prerequisite for first light.

Khronos `hello_xr` currently creates one array-size-1 swapchain per eye. That
matches the present Metal path's constraints and is why it is the first target.

## Build and test

The authoritative driver-only macOS recipe is in
`.github/workflows/macos-psvr2-driver.yml`. In outline:

```sh
brew install cmake eigen glslang libusb ninja

cmake -S . -B build -G Ninja \
  -DBUILD_TESTING=OFF \
  -DXRT_FEATURE_OPENXR=OFF \
  -DXRT_FEATURE_SERVICE=OFF \
  -DXRT_MODULE_COMPOSITOR=OFF \
  -DXRT_MODULE_COMPOSITOR_CLIENT=OFF \
  -DXRT_MODULE_COMPOSITOR_MAIN=OFF \
  -DXRT_MODULE_COMPOSITOR_MULTI=OFF \
  -DXRT_MODULE_COMPOSITOR_NULL=OFF \
  -DXRT_MODULE_COMPOSITOR_RENDER=OFF \
  -DXRT_MODULE_COMPOSITOR_SHADERS=OFF \
  -DXRT_MODULE_COMPOSITOR_UTIL=OFF \
  -DXRT_MODULE_COMPOSITOR_MOCK=OFF \
  -DXRT_MODULE_OPENXR_STATE_TRACKER=OFF \
  -DXRT_MODULE_IPC=OFF \
  -DXRT_MODULE_MONADO_GUI=OFF \
  -DXRT_MODULE_MONADO_CLI=OFF \
  -DXRT_FEATURE_WINDOW_PEEK=OFF \
  -DXRT_FEATURE_DEBUG_GUI=OFF \
  -DXRT_FEATURE_CLIENT_DEBUG_GUI=OFF \
  -DXRT_BUILD_DRIVER_PSVR2=ON \
  -DXRT_BUILD_DRIVER_PSSENSE=OFF \
  -DXRT_BUILD_DRIVER_QWERTY=OFF \
  -DXRT_BUILD_DRIVER_SIMULATED=OFF

cmake --build build --target drv_psvr2 --parallel
```

Keep this build green while adding hardware-probe tests. The fuller runtime and
compositor build recipes and probes are recorded in `doc/macos-port.md`.

## Hardware setup and baseline

Expected topology:

- Mac USB-C/Thunderbolt DisplayPort output -> direct DP 1.4/HBR3 cable -> Sony
  PS VR2 PC adapter DisplayPort input
- adapter USB -> Mac directly, not through a hub
- Sony power supply -> adapter
- PS VR2 -> adapter

The user has ordered the Sony adapter and a UGREEN DP 1.4/HBR3 cable.

Before testing Monado with hardware, build and run GAV unchanged. Confirm:

- the headset powers and is detected
- the PS VR2 display runs at 4000x2040 and 120 Hz
- stereo output is stable
- SLAM/IMU 6-DoF tracking works

Close GAV before starting Monado. Both use libusb and cannot simultaneously
claim the same headset interfaces.

## Next milestones

Work in this order unless hardware findings force a change:

1. Run the GAV hardware baseline when the adapter arrives.
2. Add a small Monado PSVR2 discovery/pose probe on macOS and verify status plus
   SLAM USB interfaces in conservative mode.
3. Add the macOS display target and achieve static/full-screen headset output.
4. Submit native Metal OpenXR projection frames and run `hello_xr -g Metal`.
5. Improve refresh-rate selection, hotplug, display selection, frame pacing,
   prediction, and latency.
6. Treat Sense controllers as a separate project: buttons/IMU first, optical
   tracking later.
7. Explore CrossOver/OpenVR game compatibility only after native OpenXR is
   stable.

## Development guardrails

- Preserve existing Linux behavior and full PSVR2 streams by default.
- Keep macOS conservative mode the default until each extra USB interface is
  proven safe on hardware.
- Keep PSSENSE disabled for the HMD MVP.
- Prefer small, reviewable commits with one buildable milestone each.
- Add explicit unsupported responses instead of partially advertising missing
  OpenXR features.
- Do not describe the runtime as conformant; it is experimental.
- Check the remote branch before publishing. Repository writes may have been
  made through the GitHub integration, so local and remote commit hashes can
  differ even when their trees are identical.
