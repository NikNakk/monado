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
driver uses Monado's native IOKit HID backend on macOS. It is restricted to the
Sony Sense controller product IDs and supports Bluetooth discovery, factory IMU
calibration, inputs, battery state, haptics, and 3-DoF orientation.

On macOS, the HMD driver starts in a conservative USB mode which claims and
submits transfers only for the status and SLAM interfaces needed for headset
tracking. Camera, gaze, LED detector, relocalizer, and VD interfaces are left
untouched. This matches the smallest USB path already demonstrated to work on
macOS and keeps controller and eye-tracking work out of the initial HMD
bring-up.

Set `PSVR2_AUXILIARY_STREAMS=1` to restore the full set of interfaces and
streams. Other platforms retain the existing full-stream behaviour by default;
setting the variable to `0` selects the same minimal mode there for testing.

Controller-camera development has a narrower opt-in which leaves gaze, LED
detector, relocalizer, and VD interfaces untouched:

```sh
PSVR2_CAMERA_STREAMS=1 PSVR2_CAMERA_MODE=4 \
  ./build-macos-psvr2-cli/src/xrt/targets/cli/monado-cli psvr2-camera 5 /tmp/psvr2-mode4
```

The diagnostic records raw packet sizes and headers plus USB completion times.
When given the optional final path prefix, it also writes one lossless PGM
snapshot from each of the four camera-image sinks.
It also maps the camera VTS timestamp into Monado's monotonic clock, but does
not pretend that the USB completion time is the exposure time. Hardware testing
on macOS found that mode 4 delivers 520448-byte `VI` packets in camera-set 4/5
pairs. Each pair shares a device timestamp and hardware sequence ID; successive
pairs are 16683 us apart (about 60 Hz). Each packet describes two 512x508
controller-tracking images in contiguous L8 planes, giving all four headset
cameras across the pair. Four captured PGM snapshots were successfully decoded
and visually inspected at 512x508.
The first observed packet was roughly 13-27 ms newer in host time than its VTS
timestamp. Mode 12 alternated 409856- and 260352-byte packets at only about 1 Hz
in the same test, so mode 4 is the current tracking choice.

When the camera-only stream is enabled, the PS VR2 builder forwards those real
VTS exposure timestamps and hardware sequence IDs to both Sense controllers.
The controller LED-sync refinement remains stable at a 16683000 ns period even
when USB delivery skips frames, because gaps are derived from the hardware
sequence counter. On macOS, the driver projects delayed exposure phases at
least 50 ms forward before programming the repeating Sense PRESCAN schedule.
It also uses the IOKit input callback timestamp for the controller clock and a
measured 3.6 ms mode-4 phase correction. `PSSENSE_FUTURE_LED_SCHEDULE=0`
restores the older scheduling for comparison; `PSSENSE_TIMING_FUDGE_100US`
overrides the phase correction and `PSSENSE_LED_PERIOD_ID` overrides the pulse
width for diagnostics. Until a constellation tracker is attached, future
scheduling holds the initial refinement offset instead of pointlessly scanning
without optical samples.

Sense output reports are assembled under the controller lock but the blocking
IOKit write happens after releasing it. This prevents camera timing callbacks
from stalling behind Bluetooth output. A six-second mode-4 capture with both
controllers active consequently improved from roughly 64-107 packets to about
719-721 packets, including 359-360 complete camera-set pairs, with median
arrival age around 27 ms. Camera mode is explicitly turned off at teardown so
rapid diagnostic restarts do not wedge the stream. Camera streaming remains
opt-in while per-camera calibration and constellation pose solving are
unfinished.

For a standalone cross-mode capture, the macOS HID probe can hold the Sense
tracking LEDs in the same opt-in, continuous-equivalent PRESCAN pattern without
claiming the headset camera interface. Run it in one terminal for slightly
longer than the camera survey, then run the survey in another terminal:

```sh
./build-sense/src/xrt/auxiliary/os/pssense_hid_probe \
  --hand left --force-ir-seconds 40

.venv/bin/python scripts/psvr2_camera_mode_survey.py \
  /tmp/psvr2-crossmode-12-4-sense-test \
  --sequence 12,4,12 --repeat 2 --settle 0.5 --sample 4 --examples 4
```

Pair and wake the selected controller first, do not run the Monado service
concurrently, and power the controller off after capture. One controller is
enough for this diagnostic; selecting a hand also avoids serial LED holds when
both are awake. The `--force-ir-seconds` option is diagnostic-only and does not
alter the normal Sense LED scheduling path.

The four `led_blink` bytes initially looked like possible spatial LED masks.
They are instead a shared temporal waveform: in a stationary hardware scan,
one asserted bit illuminated the same four to six constellation points
together, and several different bits selected the same complete set at
different camera exposure phases. They therefore cannot identify individual
LEDs. The following 16-second sparse mode-4 recording preserves the experiment
as a reproducible protocol test. Keep the controller and headset rigidly
stationary and start it in one terminal:

```sh
.venv/bin/python scripts/psvr2_camera_mode_survey.py \
  /tmp/psvr2-mode4-mask-test \
  --sequence 4 --settle 0.5 --sample 16 \
  --examples 200 --save-every 10 --no-contact-sheet
```

Immediately start the mask scan in a second terminal. It records all-off,
all-on, bits 0--16 individually, then repeated all-on/all-off reference
segments. Each segment lasts 500 ms:

```sh
./build-sense/src/xrt/auxiliary/os/pssense_hid_probe \
  --hand left \
  --force-ir-mask-scan /tmp/psvr2-mode4-mask-test/led-mask-schedule.csv \
  --mask-segment-ms 500
```

The camera CSV and mask manifest record both process-local monotonic and shared
realtime nanoseconds plus the names of every saved raw and decoded frame. The
first capture predated the realtime fields and is recoverable from image and
manifest modification times. The analyser discards 100 ms transition edges
before associating retained images with waveform segments:

```sh
.venv/bin/python scripts/psvr2_tracking_mask_analyze.py \
  /tmp/psvr2-mode4-mask-test \
  /tmp/psvr2-mode4-mask-test/led-mask-schedule.csv \
  --output /tmp/psvr2-mode4-mask-test/mask-analysis.json
```

The measured result is `led_blink_semantics_status:
temporal_waveform_supported`: bits 0, 1, 2, and 7--11 were observed at the
tested exposure phases, and every detected bit produced multiple all-on
constellation points rather than one LED. Direct calibration must consequently
obtain LED identities geometrically. The preferred bootstrap is Monado's
existing neighbour/P3P constellation search using approximate rays composed
from the measured central-overlap mapping. Candidate poses must agree across
the synchronized cameras in the fixed rig before their blob-to-model matches
are admitted to a subsequent mode-4 fisheye refinement.

`psvr2_tracking_geometry.py` implements that offline bootstrap. It parses the
authoritative 17-point controller model from the driver, generates
neighbour-limited AP3P candidates, checks one-to-one matches and LED-facing
constraints across the fixed four-camera rig, and jointly refines the rig pose.
It deliberately exits with status 2 unless at least 12 blobs match, at least two
cameras contribute three matches, and the refined RMS is at most 5 px:

```sh
.venv/bin/python scripts/psvr2_tracking_geometry.py \
  /tmp/psvr2-camera-calibration-reviewed.json \
  /tmp/psvr2-crossmode-12-4-sense-pose-03 \
  --hand left \
  --output /tmp/psvr2-crossmode-12-4-sense-pose-03/geometry-bootstrap-left.json
```

The output hashes the calibration, camera images, survey, and LED-model source,
and preserves each admitted LED ID with its observed and projected mode-4
pixel. It always says `runtime_usable: false`: the visible-to-tracking overlap
only supplies approximate central rays, and the wider mode-4 field remains
uncalibrated.

On the five stationary left-controller captures, poses 03 and 04 pass with 14
matches each at 1.975 px and 2.997 px RMS. Pose 03 uses both front cameras plus
one upper-camera point; pose 04 uses the two front cameras. The other three
captures and the mask-waveform capture fail the guardrails. The mirrored right
model is worse (13 matches at 4.524 px for pose 03 and rejection for pose 04),
which supports but does not independently prove the recovered left-hand
identities. These two admitted poses are seeds for a multi-pose mode-4 fisheye
bundle adjustment, not a final tracking calibration. Peripheral observations
from the outward cameras cannot be judged by the central-overlap bootstrap and
must be recovered during that refinement.

### PS VR2 four-camera calibration

Hardware captures establish the mode-3 physical ordering and raster layout:

| Solver camera | Mode-3 source | Physical camera |
| --- | --- | --- |
| 0 | set 0, left SBS half | lower-left |
| 1 | set 0, right SBS half | lower-right |
| 2 | set 3, left SBS half | upper-left |
| 3 | set 3, right SBS half | upper-right |

Each packet contains one 1280x640 L8 side-by-side raster, not two contiguous
640x640 planes. Sets 0 and 3 have identical VTS and hardware sequence values,
so all four images are synchronized. Mode-12 visible 320x320 images are exact
0.5x versions of corresponding mode-3 images with the same ordering, no flip,
and no translation (ZNCC about 0.996--0.999). Mode-12 set-9 tracking images map
camera-for-camera to mode-4 512x508 images at exactly half the dimensions.
Mode-12 visible-to-tracking geometry remains estimated rather than final.

Four captures used a rigid 7x5-square `DICT_4X4_50` ChArUco target with 30 mm
markers and a square length measured with digital calipers as **40.00 mm**.
Together they provide 520, 529, 116, and 156 strong views for cameras 0--3:

```sh
.venv/bin/python scripts/psvr2_charuco_calibrate.py solve \
  /tmp/psvr2-charuco /tmp/psvr2-charuco-top \
  /tmp/psvr2-charuco-2 /tmp/psvr2-charuco-3 \
  --square-length-mm 40.00 \
  --cross-mode-registration /tmp/psvr2-crossmode-registration.json \
  --output /tmp/psvr2-camera-calibration.json
```

The solver fits four fisheye models, performs one conservative MAD rejection
pass capped at 10%, calibrates pairs after fisheye undistortion, and builds a
camera-0 rig graph. Per-camera PnP results only initialize a final six-parameter
board pose which jointly minimizes every observed ChArUco corner across the
fixed rig. Single-camera poses remain in the diagnostics, but hand-eye defaults
to board poses constrained by at least two cameras. Intrinsically rejected
views cannot re-enter stereo or board solving. The versioned JSON retains
per-view reprojection results, coverage, pairwise baselines, closure, joint
board-pose residuals, hashed input provenance, and SLAM fixed-board and
relative-motion residuals. Low RMS alone is insufficient:
coverage warnings, closure, baseline plausibility, and accepted/rejected counts
must be reviewed together. Lengths are SI units except fields ending `_px` or
`_deg`; `scripts/psvr2_camera_calibration.schema.json` describes the shape.

The four-run fit gives fisheye RMS values of about 0.418, 0.459, 0.371, and
0.385 px. Camera-0-to-1 and camera-0-to-2 baselines are about 78.6 and 74.9 mm;
camera-1-to-3 is about 74.5 mm. Diagonals are about 123--127 mm. Redundant
paths disagree by at most about 0.19 degrees and 3.4 mm. These are measured
results, not hard-coded priors.

The recorder stores the wire-remapped SLAM pose before runtime correction.
`process_slam_record()` maps position to `(wire_z, wire_y, -wire_x)` and
quaternion XYZW to `(-wire_qy, -wire_qx, wire_qz, wire_qw)`, enforces quaternion
continuity, then applies the default +90-degree Z `slam_correction_pose` to
orientation while only adding its position. The corrected relation is stored
and interpolated/predicted in `slam_relation_history`. The relation chain later
returns `T_slam_head = T_slam_tracker * T_tracker_head`; current `T_imu_head`
(that is, `T_tracker_head`) translates by approximately
`(0.000247, -0.000273, 0.104826)` metres.

With `T_A_B` meaning B coordinates transformed into A, fixed-board consistency
is `T_slam_tracker * T_tracker_rig * T_rig_board = T_slam_board`. Applying the
exact runtime orientation correction reduces the original two-run translation
residual from about 209 mm to about 46 mm while retaining roughly 0.44-degree
median rotation consistency. `T_imu_head` changes the recovered transform but
not residuals, as a complete hand-eye solve must absorb it. A full rigid
+90-degree correction does not improve the raw result: the runtime's
orientation-only correction matters because position is already in remapped
tracker axes.

The remaining failure is capture-specific. With jointly refined, at-least-two-
camera board poses, three captures have about 2.0--3.9 mm median fixed-board
translation residual. The first has about 83 mm in the four-run solve and a
smooth apparent fixed-board drift dominated by about 253 mm on one axis. On the
original two captures alone, the corresponding combined and first-session
medians are about 30 and 60 mm; this confirms that exact translation residuals
are somewhat sensitive to camera coverage and rig construction even though the
qualitative session diagnosis is stable. A
0.6--1.4 SLAM scale sweep leaves it poor (best median about 76 mm at 0.75x),
while every good capture selects exactly 1.0x. This rejects a global unit-scale,
transform-direction, `T_imu_head`, or quaternion-convention explanation and
identifies anomalous SLAM translation drift in the first capture. Rotation is
trustworthy; translation remains conservatively untrusted because one complete
session contradicts the other three. Runtime behavior remains unchanged.

New visible captures record `headset_serial` from the USB descriptor. If that
descriptor is unavailable, the recorder requires `--headset-serial`; the
solver rejects multiple non-null serials and warns about legacy unbound input.
For mode-12 tracking to mode-4, the 2x dimensions, camera ordering, and standard
pixel-centre transform are experimentally established. Five stationary Sense
captures at different controller/headset poses supplied 126 uniquely matched
blinking LED centroids across the four cameras. The measured relationship is
`(u4, v4) = 2 * (u12, v12) + (0.5, 0.5)`; per-camera median residual was
0.31--0.40 mode-4 pixels and p95 was 0.64--0.92 pixels. The registration tool
can pool these captures with repeated `--additional-capture` options.

The visible-to-tracking estimator now pools independent stationary mode-12
viewpoints, records RANSAC inlier hulls, and performs leave-one-capture-out
checks. Use `--bridge-capture` for ordinary illuminated captures which should
contribute cross-spectral scene features but not LED evidence for the exact
mode-12-to-mode-4 mapping. For example:

```sh
.venv/bin/python scripts/psvr2_cross_mode_register.py \
  /tmp/psvr2-crossmode-burst \
  --bridge-capture /tmp/psvr2-crossmode-12-4-pose-03 \
  --bridge-capture /tmp/psvr2-crossmode-12-4-pose-04 \
  --bridge-capture /tmp/psvr2-crossmode-12-4-pose-05 \
  --additional-capture /tmp/psvr2-crossmode-12-4-sense-test \
  --additional-capture /tmp/psvr2-crossmode-12-4-sense-pose-02 \
  --additional-capture /tmp/psvr2-crossmode-12-4-sense-pose-03 \
  --additional-capture /tmp/psvr2-crossmode-12-4-sense-pose-04 \
  --additional-capture /tmp/psvr2-crossmode-12-4-sense-pose-05 \
  --output /tmp/psvr2-crossmode-registration.json
```

Across those nine viewpoints, cameras 0--3 have 90, 94, 66, and 38 affine
RANSAC inliers. In-fit p95 error is about 1.21--1.39 tracking pixels, but the
camera-3 leave-one-capture-out p95 rises to about 8.75 pixels. More
importantly, the inlier convex hulls cover only about 14--16% of each visible
image and 6--7% of each tracking image. Direct inspection also shows the
tracking readout seeing the controller outside the corresponding visible
frame. The v4 report therefore labels these matrices `estimated_overlap_only`
and `runtime_usable: false`. The calibration JSON can embed the complete report
for provenance, but deliberately leaves mode-4 intrinsics unresolved: applying
the affine outside its measured hull or composing it with the visible fisheye
model would fabricate calibration for the tracking-only field of view. A
direct IR calibration, for example a bundle adjustment using identified Sense
LEDs and their known 3D model, is still required before constellation tracking.

Set `PSVR2_CAMERA_BLOBS=1` on the `psvr2-camera` command to pass each of the
four mode-4 L8 streams through Monado's existing IR blob detector on a separate
queue. The command prints aggregate observation counts and, when a snapshot
prefix is supplied, writes `*-cameraN-blobs.csv` traces containing blob centres,
bounding boxes, and peak brightness. `PSVR2_BLOB_PIXEL_THRESHOLD` (default
`80`), `PSVR2_BLOB_REQUIRED_THRESHOLD` (default `180`), and
`PSVR2_BLOB_MAX_WIDTH` (default `50`) permit diagnostic tuning. This validates
the optical input independently of pose solving. It does not enable
constellation poses: the repository and GAV reference do not yet provide the
four mode-4 cameras' calibrated intrinsics and poses required by the tracker.
Hardware validation with both stationary controllers found 360 blob
observations per camera in six seconds without reducing camera throughput. The
normal 450 us schedule produced clearly visible controller rings in snapshots
from all four cameras and mean blob counts of 5.04, 7.37, 5.24, and 5.94 per
frame. With future scheduling disabled, the same fixed scene produced no rings
in its four snapshots and means of 3.55, 5.88, 3.43, and 4.25. The latter is
not a completely dark control because the legacy no-sample refinement sweep
periodically crosses the camera phase; the saved observations retain the
per-frame evidence needed to measure that behaviour.

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
