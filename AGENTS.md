<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# Monado on macOS with PS VR2: agent handover

This fork runs Monado natively on Apple Silicon macOS, with a wired Sony PS VR2
as the primary headset. It is experimental: not upstream-supported, not
packaged, and not conformant.

## Read first

- `doc/macos-port.md`: the authoritative status and roadmap. Start here.
- `doc/macos-port-tracker.md`: short branch-oriented companion.
- The detailed note for the area you are changing, linked from those two.
  In particular:
  - Presentation and pacing: `doc/macos-psvr2-timing-diagnostics.md` (current
    defaults at the top) and `doc/macos-psvr2-judder-evidence.md`.
  - Game Mode and in-process compositing:
    `doc/macos-client-compositor-design.md` and
    `doc/macos-remote-layer-hosting.md`.
  - Environment variables: `doc/macos-env-toggles.md`.

Keep these documents current when a change or a hardware run alters what they
say. The evidence ledgers exist so that settled questions are not reopened:
check them before re-testing a hypothesis.

## Branches

- `macos-wine-openvr-legacy-unity`: the integration branch, despite its name.
  It carries the native runtime, PS VR2, Metal/IOSurface sharing,
  launchd/XPC, depth, passthrough, eye gaze, foveation and Wine/OpenVR work.
- `claude/game-mode-priority-issue-xkx6m7`: client-side compositing for Game
  Mode, built on the integration branch and not merged into it yet.
- `macos-pssense-6dof`: PS Sense optical tracking, deliberately separate until
  it is reliable.
- `standards/*`: smaller review units for upstreaming, already merged into the
  integration branch.
- `main` tracks upstream Monado. Most other `macos-*` branches are history.

Check the remote branch before publishing. Commits may have been made through
the GitHub integration, so local and remote hashes can differ even when the
trees match.

## Architecture in brief

- Monado's normal target instance and hardware prober find the PS VR2 over
  libusb. The headset's own SLAM and IMU give 6DoF head tracking.
- A Vulkan (MoltenVK) compositor does distortion and timewarp.
  `comp_window_macos.m` presents the result through a `CAMetalLayer` on the
  PS VR2 display, paced from CVDisplayLink.
- Clients share swapchains with the service as IOSurfaces or Metal objects. XPC
  handles launchd activation and Metal handle transfer; the Monado protocol
  stays on the Unix socket.
- Under Game Mode, macOS throttles `monado-service` from outside the process,
  and no XPC importance or `ProcessType` setting undoes that. The fix is to
  composite in the client's process (`XRT_MACOS_CLIENT_COMPOSITOR=1`). The
  service then hosts the client's layer through `CALayerHost`, and tracking
  stays in the service.

## Build and test

The macOS CI recipe (`.github/workflows/macos-build.yml`) builds everything:

```sh
brew install cmake eigen glslang jpeg-turbo libusb molten-vk ninja \
  pkgconf sdl2-compat vulkan-headers vulkan-loader
cmake -S . -B build -G Ninja
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Linux CI (`linux-build.yml`) must stay green too. The driver-only and
standards-slice jobs are in the other workflows. On-headset regression
testing uses `psvr2-openxr-test`; see `doc/macos-port.md` for its modes.

Several existing `build*` directories belong to other checkouts or to the
installed LaunchAgent. Check `CMAKE_HOME_DIRECTORY` in a build directory's
`CMakeCache.txt` before building into it.

## Hardware

- Mac USB-C DisplayPort -> DP 1.4/HBR3 cable -> Sony PS VR2 PC adapter, with
  the adapter's USB connected directly to the Mac (no hub) and the Sony power
  supply attached.
- The headset runs at 4000x2040, 120 Hz (90 Hz is also available).
- Only one process can claim the headset's USB interfaces. Close GAV or any
  other PS VR2 tool before starting Monado.
- Hardware runs are done by the user. Record results, with the commit tested,
  in the relevant document.

## Guardrails

- Preserve Linux behaviour, and full PS VR2 streams by default on Linux.
- On macOS, keep the conservative USB set as the default. Camera streams
  (`PSVR2_CAMERA_STREAMS=1`) stay opt-in until proven safe; gaze activates
  lazily when a feature needs it.
- Keep new OpenXR features opt-in until validated on hardware, and return
  explicit unsupported results rather than advertising partial features.
- Prefer small commits, one buildable step each. Remove failed experiments
  rather than leaving them behind environment variables.
- GAV (`gav-psvr2-player-mac`) is a reference only. Check the licence before
  copying any of its code.
