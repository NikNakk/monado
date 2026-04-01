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

The branch now also carries a first remote-rendering semantic change borrowed
from WiVRn's Monado patch set:

- the compute compositor path can prefer the submitted projection layer's pose
  and timestamp when that data is close to the predicted display time
- device pose lookup remains the fallback path

This is important for a Mac-to-Quest streaming runtime because the host
compositor should not assume that a local headset device is always the source of
truth for eye poses.

The branch now also has a first real runtime probe on macOS:

- `tests/tests_macos_runtime_probe.c` creates an in-process session against the
  simulated HMD path
- with the current spike patches, the compositor can create a MoltenVK Vulkan
  instance and device on Apple Silicon, start a session, create a native
  swapchain, and submit a non-fast-path frame
- when the probe leaves the session alive long enough for the multi-compositor
  render thread to consume queued layers, the compute compositor does in fact
  use the submitted projection-layer pose data path that was ported from WiVRn
- the compositor now also advertises real native swapchain formats on macOS
  instead of an empty format table, because local/native format support is no
  longer incorrectly filtered through Linux-style external-handle export checks

Two macOS-specific Vulkan adjustments were required for that probe:

- opt into `VK_KHR_portability_enumeration` and the portability enumeration
  instance flag so MoltenVK can be selected correctly
- stop hard-requiring `VK_KHR_external_memory_fd` for the native macOS probe
  path, and allow internal image allocations to proceed without FD export
  metadata

This does not mean the macOS handle model is solved. It only proves that the
native compositor can run far enough to validate the remote-pose compositor
behavior.

The branch now also takes the first real step away from the placeholder macOS
FD handle model:

- `xrt_graphics_buffer_handle_t` is now `IOSurfaceRef` on macOS instead of
  pretending to be a Unix file descriptor
- the compositor now requires and uses `VK_EXT_metal_objects` on macOS for
  native-image export/import
- the native runtime probe can now export compositor swapchain images as
  `IOSurfaceRef`, import them back through `xrt_comp_import_swapchain`, and
  successfully acquire/release the imported images
- the default probe path exits after that verified round-trip; setting
  `MACOS_RUNTIME_PROBE_SUBMIT_FRAME=1` keeps going into the older frame-submit
  path for deeper compositor debugging

This is the first real macOS-native shared-image path, not just scaffolding.
The next step after that was to prove the same handle model across the real
service/client boundary:

- `u_file_get_runtime_dir` now gives macOS a real absolute runtime directory
  and creates it on demand instead of returning the literal string `~/.cache`
- the Unix IPC message transport on macOS can now carry `IOSurfaceRef` handles
  by sending `IOSurfaceID` values inline in the message payload
- `tests/tests_macos_ipc_swapchain_probe.c` can connect to a running
  `monado-service`, create a native IPC swapchain, import the returned
  `IOSurfaceRef` images back through `xrt_comp_import_swapchain`, and validate
  acquire/release on the imported swapchain

That means the first real cross-process macOS image path is now working in the
Monado spike, not just the earlier in-process loopback probe.

The next validation step now also exists:

- `tests/tests_macos_openxr_loaderless_probe.c` dlopens
  `libopenxr_monado.dylib`, negotiates `xrGetInstanceProcAddr` directly via
  `xrNegotiateLoaderRuntimeInterface`, enables `XR_MND_headless`, and creates
  an OpenXR instance, system, and headless session against a running
  `monado-service`

This is the first proof on macOS that the OpenXR state tracker itself can come
up over the service boundary, not just the lower-level IPC compositor APIs.

## Likely next blocker classes

After the first successful runtime probe, the next blocker classes are clearer:

- additional Linux-only event loop assumptions in server/service code
- desktop compositor assumptions around display/window targets
- moving from the new headless OpenXR probe to a real graphics-bound OpenXR
  session path
- cleanup/teardown issues after the service-backed IPC and loaderless OpenXR
  probes, currently visible as noisy shutdown-side protocol logging
- missing macOS-native process/service integration

## Strategic note

For a Mac-to-Quest streaming runtime, direct mode is not the first problem to
solve. A headless or offscreen compositor path plus working IPC/runtime control
would already be enough to support a remote-streaming experiment.
