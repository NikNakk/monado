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

The local display compositor target is `macos`. It searches for an `NSScreen`
named `PS VR2`, falling back to the first 4000-pixel-wide display, and creates a
borderless window backed by `CAMetalLayer`. MoltenVK's WSI swapchain reports
successful presents but produces black scanout on the PS VR2. The target
therefore allocates ordinary Vulkan compositor images backed by exportable
`IOSurfaceRef` objects, exposes those same surfaces as Metal textures, and blits
the completed image to a `CAMetalDrawable`. This keeps Monado's Vulkan
distortion compositor while using native Metal only for the final display
handoff. The current implementation waits for both queues and uses fake pacing;
display-link pacing and explicit cross-API synchronization remain later work.

The Khronos `hello_xr` sample now runs through the native Metal binding and
produces distorted stereo output on the PS VR2. Build the OpenXR SDK sample,
start `monado-service`, then run:

```sh
XR_RUNTIME_JSON="$PWD/build-macos-psvr2-display/openxr_monado-dev.json" \
DYLD_LIBRARY_PATH=/path/to/OpenXR-SDK-build/src/loader \
  /path/to/OpenXR-SDK-build/src/tests/hello_xr/hello_xr -g Metal -s Local -v
```

Set `XRT_MACOS_PRESENT_PHASE_LOG=1` to log each presented drawable's integer
refresh-offset class and fractional offset from the desired presentation time.
`macOS presentation phase:` lines include `initial`, `steady`, `transition`, or
`out-of-order`, the previous/current class, consecutive class run lengths, actual
presentation interval, timeline semaphore value, drawable/render waits, and
applied feedback sampled at submission and at the presentation callback. This
diagnostic is independent of `XRT_MACOS_PRESENT_TIMING` and does not modify
feedback or pacing. Per-frame logging can itself add overhead.

Absolute `_ns` timestamps are in the monotonic clock domain; Metal's actual and
previous presentation times are translated using the submission's desired-time
clock anchor. `commit_call_ns` is a CPU marker immediately before registering the
diagnostic handler and calling `commit`, not GPU completion. Positive
`commit_minus_scheduled` means that marker was after the scheduled target.
`period_ns` is the physical display period, unaffected by the pacing divisor.
With `immediate 1`, the scheduled target is only a reference: no `atTime:` is
passed to Metal. An initial sample has no previous interval; out-of-order
callbacks are logged but do not advance diagnostic class history. Run lengths
describe consecutive classes, not a hysteretic classification. Paste contiguous
lines around transitions, plus the existing presentation/submission summaries
and missed-frame warnings, to distinguish a persistent extra refresh from a
single late frame. The timeline value can be zero with the queue-idle fallback.

The same option also emits `macOS presentation pipeline:` with the matching
timeline/frame ID. It records the CPU prediction-call marker, requested and
actual wake times, CPU render-begin and Vulkan submit-begin/end markers, the
original predicted display time, and presentation enqueue/worker-start times.
Frame history is copied before dispatch so subsequent predictions cannot change
an outstanding job's diagnostic data. Missing history is explicitly reported;
an absent render-begin marker produces `nan` for its derived age.

`queue_wait` measures enqueue to worker entry (direct-call overhead in synchronous
mode); `render_begin_to_actual` measures the age since CPU rendering began, not
the sensor sample age; `commit_to_actual` measures the Metal commit-call marker
to presentation. `original_prediction_error` compares actual presentation with
the display time returned by the original frame prediction, rather than a later
feedback snapshot. Submit markers measure CPU submission, not GPU execution.
Keep both phase and pipeline lines when sharing a log. These extra per-frame
lines can add logging overhead; no pacing or feedback values are changed.

### Experimental presentation worker gate

With asynchronous presentation enabled, `XRT_MACOS_PRESENT_WORKER_GATE=1`
waits for prior presentation worker jobs to finish CPU submission before the
compositor's render-begin marker and pose sampling. It does not wait for Metal
command-buffer completion or actual presentation. The current frame has not
submitted GPU work at this point, so the prior jobs' render-complete waits do
not depend on it. This gate is off by default and inactive with synchronous
presentation. It leaves the serial queue, feedback, and frame predictions intact.

The experiment moves worker queue waiting ahead of rendering to reduce the age
of the compositor pose when a drawable is shown. It does not acquire the next
drawable ahead of rendering, refresh the application's submitted images, or
revise the already predicted display timestamp. Check actual cadence and
`original_prediction_error` as well as frame age; a smaller reported age alone
does not demonstrate lower end-to-end latency.

With phase logging enabled, pipeline lines add `worker_gate_begin_ns`,
`worker_gate_end_ns`, and `worker_gate_wait`. `render_begin_ns` is sampled after
the gate, so `render_begin_to_actual` excludes the gate wait. Compare with
`actual_ns - worker_gate_begin_ns` to include it. Zero gate timestamps mean the
gate was inactive. A successful test should move waiting from `queue_wait` into
`worker_gate_wait`, reduce render-to-presentation age, and preserve 120 Hz
cadence. Drawable waiting may still remain. Set the option to `0` for the
baseline comparison.

### Compositor pose selection diagnostic

`XRT_MACOS_COMPOSITOR_POSE_LOG=1` independently enables per-view pose input
logging on macOS. `macOS compositor device pose:` records the world-space eye
orientations returned by the existing device-query/eye-transform path, their
requested scanout timestamps, and relation flags. It performs no extra device
queries. `macOS compositor render pose:` records the view inputs after pose
selection, including compute/graphics path, device/submitted source, ATW and
fast-path flags, layer count, and the first projection layer's timestamp and
orientation. `app_present 0` means the zero application quaternion is a missing
value, not a pose. Projection-depth uses the shared projection view layout.

Quaternion components are logged as x, y, z, w. Match `timeline` to the phase
and pipeline logs and compare consecutive *presented* frames offline; this
avoids deriving angular speeds from closely spaced device-query timestamps.
The device timestamps are requested prediction times, not sensor sample times.
The render log captures inputs to the high-level rendering pipeline, not a
readback of final pixels or a measurement of photons. With ATW disabled, a
device target input does not establish that the image was reprojected to it.
Graphics uses one target pose, so its logged end equals begin; compute can use
distinct scanout endpoints. Multiple projection layers are identified by the
layer count, but only the first projection's application pose is logged.

This diagnostic does not change pose selection or ATW gating. It adds several
lines per frame and can affect timing. Keep the other experimental settings
fixed when capturing it.

On Apple platforms the runtime does not currently advertise
`XR_KHR_composition_layer_depth`, because the IOSurface-backed Metal client
swapchains do not support depth/stencil pixel formats. The Metal client also
waits for application command-queue completion when releasing an image to the
separate Vulkan compositor.

With `tests_macos_runtime_probe` built, surface creation and a single compositor
frame submission can be exercised directly:

```sh
VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
  ./build-macos-psvr2-display/tests/tests_macos_runtime_probe --submit-frame
```

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
