<!--
Copyright 2026, Collabora, Ltd.

SPDX-License-Identifier: BSL-1.0
-->

# macOS Port Notes

This document tracks the experimental macOS build and runtime work needed to get
Monado closer to a usable desktop runtime on Apple Silicon.

## Scope of this branch

This branch is intentionally narrow:

- get a desktop macOS configure/build as far as possible
- remove obvious BSD/macOS portability errors in IPC and service code
- document the blocker stack in repo-local notes

It is not yet trying to deliver:

- a complete macOS compositor backend
- OpenXR conformance
- display direct mode
- a packaged end-user build

## Current local build recipe

The most useful local probe so far is:

```sh
cmake -S . -B /tmp/monado-macos -G Ninja \
  -DXRT_HAVE_OPENGLES=OFF \
  -DXRT_HAVE_SDL2=OFF \
  -DXRT_MODULE_MONADO_GUI=OFF \
  -DXRT_FEATURE_WINDOW_PEEK=OFF \
  -DXRT_FEATURE_DEBUG_GUI=OFF \
  -DXRT_FEATURE_CLIENT_DEBUG_GUI=OFF \
  -DXRT_BUILD_DRIVER_QWERTY=OFF

ninja -C /tmp/monado-macos -k 1
```

These options strip out SDL2-driven desktop helper targets so the build reaches
the macOS-relevant IPC/runtime blockers faster.

## Current findings

After installing a basic Homebrew toolchain floor (`glslang`, `pkgconf`,
`ninja`, `eigen`, `vulkan-headers`, `vulkan-loader`, `molten-vk`), the build on
macOS gets well into the compositor, Vulkan, and OpenXR state tracker code.

The first meaningful platform blockers are in IPC:

- a `wait` symbol collision with POSIX `wait(2)`
- Linux-only desktop IPC server guards in `ipc_server.h`
- Linux-only `epoll` usage in desktop IPC server code

Those are the first blockers addressed by this branch.

## Likely next blocker classes

Once IPC compiles further, expected next blockers are:

- additional Linux-only event loop assumptions in server/service code
- desktop compositor assumptions around display/window targets
- Vulkan portability gaps specific to MoltenVK or macOS surface handling
- missing macOS-native process/service integration

## Strategic note

For a Mac-to-Quest streaming runtime, direct mode is not the first problem to
solve. A headless or offscreen compositor path plus working IPC/runtime control
would already be enough to support a remote-streaming experiment.
