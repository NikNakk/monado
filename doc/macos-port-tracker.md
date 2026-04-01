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
- fixed macOS runtime-dir fallback so the service uses a real absolute socket
  path instead of the literal `~/.cache`
- added `IOSurfaceRef` transport across Unix IPC by carrying `IOSurfaceID`
  values inline in the message stream
- added `tests/tests_macos_ipc_swapchain_probe.c` to validate the service-backed
  macOS swapchain import/export path over real IPC
- added `tests/tests_macos_openxr_loaderless_probe.c` to validate the macOS
  OpenXR state tracker by loading `libopenxr_monado.dylib` directly and
  creating a headless session against `monado-service`
- added `tests/tests_macos_openxr_vulkan_probe.c` to validate a real
  graphics-bound OpenXR Vulkan session and swapchain path on macOS
- added Apple-specific reply framing for server-to-client IPC traffic so reply
  boundaries survive macOS `SOCK_STREAM` short reads
- fixed the IPC protocol generator so graphics-buffer reply capacity uses
  `XRT_MAX_SWAPCHAIN_IMAGES` instead of `XRT_MAX_IPC_HANDLES`

## Still expected after this patch set

- more IPC/server portability work after the first `poll()` conversion
- real OpenXR state-tracker validation on top of the now-working IPC image path
- frame submission and layer submission after the new graphics-bound Vulkan
  session/swapchain probe
- cleanup/teardown fixes for the service-backed probe shutdown path
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
- the new macOS IPC probe can now:
  - launch against a running `monado-service`
  - create a native swapchain over the IPC client path
  - receive exported `IOSurfaceRef` images from the service
  - import them back through `xrt_comp_import_swapchain`
  - validate acquire/release on the imported swapchain over the real
    service/client boundary
- the new loaderless OpenXR probe can now:
  - dlopen `libopenxr_monado.dylib` directly on macOS
  - negotiate `xrGetInstanceProcAddr` without a separately installed loader
  - create an OpenXR instance with `XR_MND_headless`
  - select the simulated HMD system through the OpenXR state tracker
  - create a headless OpenXR session over the real Monado service boundary
- the new Vulkan OpenXR probe can now:
  - negotiate `XR_KHR_vulkan_enable2`
  - create a Vulkan instance and device through Monado's OpenXR runtime
  - create a graphics-bound OpenXR session on macOS
  - create an OpenXR swapchain and enumerate three Vulkan swapchain images
    over the real service-backed path
  - complete `xrWaitFrame` with a valid predicted display time over the live
    service boundary
  - submit one projection frame through the live macOS service/runtime path
- with `MACOS_RUNTIME_PROBE_SUBMIT_FRAME=1`, the same probe can still continue
  into the older frame-submit path for deeper compositor debugging
- the earlier branch result that the compute compositor consumes submitted
  projection-layer pose data is still valid, but that path is no longer the
  default probe exit because the cleanup path after deeper submission still
  trips a macOS-only teardown bug
- the current service-backed probe still exits with noisy but non-fatal
  shutdown logging (`ipc_call_session_destroy` and server-side broken-pipe
  output), which should be cleaned up before this moves beyond spike status
- the per-client Unix IPC loop now treats a zero-byte `MSG_PEEK` as a normal
  client disconnect on macOS, so the loaderless and Vulkan probes no longer end
  with the misleading `Invalid command received.` server error

## Recommended workflow

1. Reconfigure with the no-SDL macOS recipe from `doc/macos-port.md`
2. Build with `ninja -k 1`
3. Use `tests/tests_macos_runtime_probe` for in-process native-image regression
   checks on macOS
   Default run validates the IOSurface round-trip.
   `MACOS_RUNTIME_PROBE_SUBMIT_FRAME=1` keeps going into the older frame-submit
   path.
4. Use `tests/tests_macos_ipc_swapchain_probe` against a running
   `monado-service` to validate the real service/client image path
5. Use `tests/tests_macos_openxr_loaderless_probe` with
   `MONADO_OPENXR_RUNTIME_PATH` set to `libopenxr_monado.dylib` to validate the
   headless OpenXR path over the real service boundary
6. Use `tests/tests_macos_openxr_vulkan_probe` to validate the first
   graphics-bound OpenXR Vulkan session and swapchain path on macOS
7. Fix only the first blocker each round
8. Keep notes here so the blocker order stays explicit
