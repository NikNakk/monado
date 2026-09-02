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

## PS VR2 HMD bring-up

The PS VR2 HMD driver only requires libusb. The separate PS Sense controller
driver still requires Monado's internal HID support and is not enabled on macOS.

On macOS, the HMD driver starts in a conservative USB mode which claims and
submits transfers only for the status and SLAM interfaces needed for headset
tracking. Camera, gaze, LED detector, relocalizer, and VD interfaces are left
untouched. This matches the smallest USB path already demonstrated to work on
macOS and keeps controller and eye-tracking work out of the initial HMD
bring-up.

Set `PSVR2_AUXILIARY_STREAMS=1` to restore the full set of interfaces and
streams. Other platforms retain the existing full-stream behaviour by default;
setting the variable to `0` selects the same minimal mode there for testing.

After building `monado-cli` with the PSVR2 driver enabled, the hardware-backed
discovery and pose probe can be run with GAV closed:

```sh
PSVR2_AUXILIARY_STREAMS=0 ./build-macos-psvr2-cli/src/xrt/targets/cli/monado-cli psvr2-pose 10
```

Move the headset during the probe. It succeeds only after receiving poses with
valid and tracked position and orientation and observing a meaningful pose
change. Conservative mode also leaves the system face-tracking role unassigned.

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

The branch now also reaches the first graphics-bound OpenXR success path on
macOS:

- `tests/tests_macos_openxr_vulkan_probe.c` uses `XR_KHR_vulkan_enable2` to
  create a Vulkan instance and device through Monado's OpenXR runtime
- on Apple Silicon it now succeeds through `xrCreateSession` and
  `xrCreateSwapchain`, and can enumerate three swapchain images from the live
  service-backed compositor path
- the key fix there was plumbing `VK_EXT_metal_objects` state through the
  OpenXR Vulkan client bring-up, so the macOS `IOSurfaceRef` import path no
  longer incorrectly fails the generic external-handle importability check

The branch now also reaches the first service-backed graphics submit on macOS:

- the Unix IPC transport now has an Apple-specific server-to-client framing
  path so reply boundaries survive `SOCK_STREAM` short reads instead of letting
  `session_poll_events` spill into the following `wait_frame` reply
- the IPC protocol generator now treats graphics-buffer reply capacity as
  `XRT_MAX_SWAPCHAIN_IMAGES` instead of `XRT_MAX_IPC_HANDLES`, which fixes the
  macOS `IOSurfaceID` reply-size mismatch on `swapchain_create`
- with those two fixes in place,
  `tests/tests_macos_openxr_vulkan_probe.c` can now complete `xrWaitFrame`,
  create the service-backed OpenXR swapchain, and submit one projection frame
  through the live macOS Monado service/runtime path
- the same probe now has an opt-in multi-frame mode via
  `MACOS_OPENXR_VULKAN_PROBE_FRAMES`
- on macOS, sustained stereo submission currently works when the probe uses
  one `arraySize=1` swapchain per eye
  (`MACOS_OPENXR_VULKAN_PROBE_PER_VIEW_SWAPCHAINS=1`)
- the failing case is specifically the current stereo array-swapchain model:
  the `IOSurface` import/export path trips Metal validation on
  `MTLTextureType2DArray`, so the first real macOS remote-render path should be
  treated as per-eye 2D swapchains, not a Vulkan-style stereo array image
- for that temporary per-eye validation path, the probe-side clear helper is
  now intentionally simple:
  `clear_swapchain_image()` uses layout barriers plus `vkCmdClearColorImage`
  and bounded fence waits instead of the older render-pass/framebuffer/readback
  path that was wedging on MoltenVK
- a fresh local rerun with rebuilt WiVRn artifacts in `/tmp` also showed that
  this path needed a real headset/client handshake before the new clear logic
  could be judged
- an ADB-assisted rerun now provides that handshake:
  `wivrn-server-headless` reaches `Initial headset handshake completed`, the
  probe reaches `clear_swapchain_image()`, and the current first explicit
  failure is the bounded `clear_swapchain_image(submit)` fence timeout
- after that timeout is reported, the same probe still hangs during cleanup in
  `vkDeviceWaitIdle()` on MoltenVK

The branch now also validates the first host-side remote-HMD stub path on
macOS:

- Monado's existing remote driver now builds on macOS after a small
  `SOCK_CLOEXEC` portability fix
- the config loader now reads `config_v0.json` on macOS instead of excluding
  Apple entirely
- with a macOS config directory populated and `active` set to `remote`,
  `monado-service` now selects the remote builder, exposes `Remote HMD` plus
  remote controllers, and listens on the configured TCP port

This does not make the headset path done, but it changes the next step. The
first Quest experiment should start by adapting the existing remote driver data
path to the MVP pose/video contract, not by inventing a new host-side HMD
abstraction.

The branch now also has a first remote-pose validation tool on macOS:

- `tests/tests_macos_remote_driver_pose_probe.c` connects to the remote-driver
  TCP socket, consumes the reset/current handshake packets, and streams
  synthetic head poses at 60 Hz
- with `monado-service` running in `active=remote` mode, that pose probe can
  stay connected while `tests/tests_macos_openxr_vulkan_probe.c` creates a
  graphics-bound OpenXR session against `Remote HMD`
- the Vulkan probe now logs `Head: 'Remote HMD'` and still submits projection
  frames successfully on macOS when using the per-eye swapchain workaround

That is the first proof on macOS that Monado's runtime can render through the
remote-device path instead of only the simulated local HMD path.

The branch now also has the first tiny bridge layer between a simpler headset
pose packet and Monado's remote-driver protocol:

- `tests/tests_macos_remote_pose_protocol.h` defines a small `v0` pose packet
  carrying orientation, position, and timestamps
- `tests/tests_macos_remote_pose_bridge.c` listens for that packet on local UDP
  port `4243` and forwards it into the remote driver's TCP stream on port
  `4242`
- `tests/tests_macos_remote_pose_packet_sender.c` is a synthetic sender for
  that bridge packet
- with `monado-service` in `active=remote` mode, the bridge can forward hundreds
  of packets while `tests/tests_macos_openxr_vulkan_probe.c` continues to run
  successfully against `Remote HMD`

This is not the final network protocol, but it is the first clean separation
between "Quest-like pose packet" and "Monado internal remote-driver protocol"
on macOS.

## Likely next blocker classes

After the first successful runtime probe, the next blocker classes are clearer:

- additional Linux-only event loop assumptions in server/service code
- desktop compositor assumptions around display/window targets
- moving from the new graphics-bound swapchain probe to a remote-HMD device
  path and an actual network-fed pose source
- deciding whether the macOS workaround should stay an app-level constraint
  (one swapchain per eye) or become a deeper compositor/runtime policy
- cleanup/teardown issues after the service-backed IPC and loaderless OpenXR
  probes, currently visible as noisy shutdown-side protocol logging
- missing macOS-native process/service integration

## Strategic note

For a Mac-to-Quest streaming runtime, direct mode is not the first problem to
solve. A headless or offscreen compositor path plus working IPC/runtime control
would already be enough to support a remote-streaming experiment.
