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
- stopped suppressing native macOS swapchain formats just because the current
  placeholder handle model cannot export Linux FD handles
- changed the macOS graphics-buffer abstraction from the placeholder FD model to
  `IOSurfaceRef`
- wired the first real macOS native-image path with `VK_EXT_metal_objects`
  so compositor swapchains can export/import `IOSurfaceRef` handles

## Still expected after this patch set

- more IPC/server portability work after the first `poll()` conversion
- client/shared-image plumbing for real OpenXR apps or a streaming bridge
- cross-process transport for `IOSurfaceRef` handles in the IPC path
- cleanup/teardown fixes for the optional deeper frame-submit probe path
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
  - advertise real native swapchain formats without a probe-only fallback
  - report `IOSurfaceRef` as the macOS graphics-buffer handle model
  - export native swapchain images as `IOSurfaceRef`
  - import those images back through `xrt_comp_import_swapchain`
  - validate acquire/release on the imported swapchain
- with `MACOS_RUNTIME_PROBE_SUBMIT_FRAME=1`, the same probe can still continue
  into the older frame-submit path for deeper compositor debugging
- the earlier branch result that the compute compositor consumes submitted
  projection-layer pose data is still valid, but that path is no longer the
  default probe exit because the cleanup path after deeper submission still
  trips a macOS-only teardown bug

## Recommended workflow

1. Reconfigure with the no-SDL macOS recipe from `doc/macos-port.md`
2. Build with `ninja -k 1`
3. Use `tests/tests_macos_runtime_probe` for native runtime regression checks on
   macOS while the handle model is still in flux
   Default run validates the IOSurface round-trip.
   `MACOS_RUNTIME_PROBE_SUBMIT_FRAME=1` keeps going into the older frame-submit
   path.
4. Fix only the first blocker each round
5. Keep notes here so the blocker order stays explicit
