<!--
Copyright 2026, Collabora, Ltd.

SPDX-License-Identifier: BSL-1.0
-->

# macOS Port Tracker

## Branch

- `kg/macos-ipc-port-spike`

## Fork

- GitHub working fork: `kzahel/monado`
- Canonical upstream remains GitLab `monado/monado`

## Initial blocker stack

1. Missing local toolchain on the Mac host:
   `pkg-config`, `ninja`, `glslangValidator`
2. SDL2 include-path mismatch in optional desktop helper targets
3. IPC client `wait` symbol collision on BSD/macOS
4. Desktop IPC server explicitly unported on Apple
5. Linux-only `epoll` dependency in server IPC threads

## Changes started in this branch

- renamed the IPC future `wait` callback implementation to avoid the POSIX
  `wait(2)` collision
- added a macOS desktop IPC mainloop source using Unix sockets plus `poll()`
- added an Apple path to IPC CMake source selection
- added a macOS `poll()` path for per-client IPC thread waiting
- ported the first remote-rendering compositor behavior from WiVRn patch `0008`
  so compute rendering can use submitted projection-layer pose data instead of
  always querying device pose data
- added the first macOS native runtime probe in `tests/tests_macos_runtime_probe.c`
- enabled MoltenVK portability-driver instance creation
- relaxed the native macOS compositor probe path so internal image allocations
  do not require Linux FD export support

## Still expected after this patch set

- more IPC/server portability work after the first `poll()` conversion
- replacing the placeholder macOS FD graphics-handle model with a real one
- compositor format advertisement that matches the macOS native path instead of
  Linux-style import/export assumptions
- client/shared-image plumbing for real OpenXR apps or a streaming bridge
- deciding whether any additional WiVRn compositor patches should be ported, or
  only mined for design ideas

## Current local status

- the no-SDL macOS build recipe in `doc/macos-port.md` completes successfully
- `monado-service`, `monado-ctl`, `monado-cli`, and `libopenxr_monado.dylib`
  all build on the current branch
- the native macOS runtime probe can now:
  - create the MoltenVK instance and compute device
  - start an in-process session against the simulated HMD
  - create a native swapchain
  - submit a non-fast-path frame
  - validate that the compute compositor consumes submitted projection-layer
    pose data after the multi-compositor render thread latches the queued frame

## Recommended workflow

1. Reconfigure with the no-SDL macOS recipe from `doc/macos-port.md`
2. Build with `ninja -k 1`
3. Use `tests/tests_macos_runtime_probe` for native runtime regression checks on
   macOS while the handle model is still in flux
4. Fix only the first blocker each round
5. Keep notes here so the blocker order stays explicit
