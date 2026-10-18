<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Vulkan camera composition for macOS WSI

Updated 18 October 2026: the copy and camera compute pass are replaced by
[fragment output passes](#fragment-output-pass-18-october-2026). Previously updated 17 October 2026. Worktree `build/review/wsi-integration`, based on
`ec4c02591` plus the fork checkpoint recorded below. Native remains the default;
WSI/passthrough remains opt-in. No installed-runtime change or upstream
submission is part of this checkpoint.

## Problem and resulting path

The calibrated Metal camera bridge is geometrically acceptable during gentle
movement, but the user reports virtual-object shimmer during faster movements.
The native control looks better. Increasing the compositor wake budget from
3 to 7 ms improves WSI cadence from about 89 to 102 frames/s on the 120 Hz
panel without consistently fixing the image. Matching camera-queue priority
has no subjective benefit. Preserve that evidence in
[the migration ledger](macos-wsi-migration.md#blocking-bridge-measured-16-october-2026).

Stage tracing measures 5.964 ms median / 7.208 ms p99 blocked inside the bridge:
chiefly waiting for Vulkan distortion and then Metal camera completion. A
roughly 0.13 ms final layout wait is not the main cost. Adding another fixed
offset would leave those serial waits in the compositor thread.

The replacement stays in Vulkan: upload BC4 camera snapshots, project calibrated
camera rays and blend the camera under premultiplied application alpha. The
current WSI compute path renders into private GENERAL-layout images. A
full-screen fragment pass then renders each presentation image, either a plain
pass-through or the camera blend (see the
[fragment output pass](#fragment-output-pass-18-october-2026)). No Metal texture export, private GPU priority call, cross-API event
translation or per-frame CPU completion wait is needed for this camera pass.
The native presenter is unchanged and remains the visual control.

## Review boundaries

- `render/render_camera.c`, its header and `shaders/camera.frag` implement the
  Vulkan operation. They have no macOS, PS VR2, USB, IPC, config-file or
  presentation policy. The shader accepts stereo UV/ray maps, intrinsics,
  distortion and per-frame rotation. Image dimensions come from the inputs.
- `render/render_output.c` owns private output images and queues an ordered
  pass-through fragment pass (`shaders/output.frag`) into caller-owned
  presentation views. `render/render_fullscreen.c` and `shaders/fullscreen.vert`
  are the shared full-screen triangle, render pass and pipeline. None of these
  has platform/camera policy.
- `main/comp_psvr2_passthrough.c` owns the current PS VR2 BC4 source, optical
  map construction, headset-bound calibration selection and header-time capture
  pose. Its PS VR2 name is deliberate: this is not a proposed universal camera
  calibration API. The existing macOS environment names remain compatible.
- `main/comp_vulkan_passthrough.c` takes a retained camera snapshot and the
  exact display orientation attached by the renderer to the output image,
  then calls the Vulkan operation. The macOS WSI target invokes it.

The main compositor in the inspected upstream checkout (`upstream/main` at
`ddeba9f60`, 14 October 2026) has passthrough API/layer declarations and client
forwarding but no camera renderer in `auxiliary/render` or the main renderer.
This is new feature work, not reuse of an established upstream camera path.

A prospective upstream series should keep the Vulkan operation and offscreen
pixel/lifetime tests as a separate review unit, followed by camera-source/API
and PS VR2 policy work. The present source adapter still uses this fork's camera
sink and hosted-frame sharing interfaces, and the serial/calibration policy
needs upstream agreement. The existing private remote-layer hosting and
optional GPU-priority policy are not prerequisites of the generic camera pass.
Follow `macos-upstream-contribution.md`: no upstream submission or human DCO
certification is implied by this implementation.

## Ordering and resource lifetime

The renderer's existing binary `render_complete` semaphore remains the
presentation dependency. One Vulkan batch consumes it at `ALL_COMMANDS` and re-signals it after
composition, under the existing queue lock. The final WSI presenter keeps its existing wait;
no generic target interface or semaphore substitution is introduced.

Each output image has its own framebuffer, BC4 camera images, coherent
upload buffers, uniform buffer, command buffer and completion fence. The camera source and parameters can be released immediately
after the CPU snapshot is copied. The output slot's fence must complete before
any of those buffers or commands are reused. This prevents an older queued
frame from seeing a newer camera texture or rotation. Calibration maps are
immutable and initialized on the same queue before they are sampled.

The private source stays in its layout and is sampled in place; the
presentation image is only a color attachment, starting UNDEFINED and finishing
in the presentation layout. Upload and source barriers order earlier writes
before shader sampling. The pass currently accepts linear RGBA8/BGRA8 UNORM
images with color-attachment/sampling support and BC4 sampled camera support;
it rejects unsupported formats explicitly. (Earlier versions copied the
application image and wrote the output from a compute shader.) The WSI adapter
still requires the compute compositor for transparent application alpha.

Slot reuse and teardown have bounded 500 ms fence waits. A timed-out teardown
keeps the renderer and all GPU-visible resources intact for a retry. The WSI
adapter also retains the output target rather than destroying images still in
use; target recreation/acquisition stops. If the device never recovers, this
storage remains until device/process teardown. Healthy creation failure paths
have no submitted work and free partial resources immediately. Queue OOM leaves
resources and semaphore state unchanged by Vulkan contract, and permits retry;
other submission failures retire the target because device state may be uncertain.
No new runtime timing or synchronization toggle is added.

The old Metal bridge, its separate queue-priority adjustment and its startup
pixel-readback verifier are removed. `--verify-passthrough` and
`XRT_MACOS_WSI_PASSTHROUGH_VERIFY` are no longer active controls. Exact pixel
verification belongs in the offscreen tests, where input pixels and expected
output are known. Historical readback records remain evidence for the old path.
Existing optional `wsi_passthrough` timing CSV now reports snapshot and Vulkan
queueing times; it does not report CPU-observed Metal stages.

## Validation

The GPU tests exercise the production shader and submission code, not a shader
reimplementation. They compare stereo BC4 sampling and premultiplied alpha to
CPU reference pixels; cover positive/negative yaw, pitch and roll, invalid rays,
partial workgroups and padded compressed rows; and queue distinct snapshots for
three outputs. Host source memory and parameters are discarded after submission.

A host-controlled Vulkan event deliberately blocks queue progress after the
producer semaphores are ready. Three
camera submissions return while the GPU remains blocked. Reuse times out
without overwriting old storage; destruction also times out without freeing it.
Releasing the event lets the original pixels finish and permits clean teardown.
An invalid input leaves the producer semaphore available for the caller to
consume. These are ordering/lifetime assertions, not speed thresholds.

The first macOS offscreen run passes 718 assertions in three camera cases.
A fourth case now injects queue OOM and checks that the producer semaphore
remains available and a later retry succeeds.
The subsequent strengthened cases add explicit pending-slot reuse and keep all
three pixel-test slots in flight together. Full regression/build results are
recorded below once complete.

### First physical functional capture

`build/review/wsi-vulkan-camera-service-20261016-1/` records a 20-second
service-composited full-scene blended run at 120 Hz, with Metal API validation
and the ordinary 3 ms budget. Logs confirm saved headset calibration, rotation
active and three independent output slots, without GPU/runtime errors.
Application exit and display/service-registration restoration all return zero.
The manifest saves the exact working-tree diff, new source bytes and binary
hashes; it is not a measurement of the previous Metal bridge binary.

After 240 feedback entries: 119.33 observed presents/s, 8.342 ms p99 present
interval, 0.42% intervals longer than 12.5 ms. Camera CPU snapshot/submission
is 0.534 ms median / 0.986 ms p99, against the earlier blocking bridge's
5.964 / 7.208 ms. Distortion GPU time excludes this separate camera dispatch;
no total-GPU reduction is claimed. This is one short functional capture,
without a matched moving-head acceptance run or statistical tail guarantee.

### Remaining visual gate

After the software and functional checks, repeat the complete blended scene
with the normal 3 ms budget, first service then hosted, including gentle and
faster yaw/pitch/roll. A passed pixel fixture and good presentation cadence do
not prove the reported shimmer is gone. Keep native default until the user
accepts the moving-head result. Calibrated CV WSI may then become default, with
native and adaptive pacing available independently as previously requested.

### Hosted and refresh functional checks

`build/review/wsi-vulkan-camera-hosted-functional-20261016-1/` passes two
hosted full-scene camera sessions with initial 90/120 Hz conditions and Metal
API validation. Each verifies a 90 Hz request before begin, rejects 73 Hz
without changing the display, and completes 90→120→90→120. Both service and
hosted client initialize their source/renderer, saved calibration loads,
rotation becomes active, and both applications exit zero with no runtime/GPU
error lines. Mode changes briefly invalidate camera freshness and then rotation
resumes, preserving the existing static fallback. Display and service restoration
return zero. Linux solver tests ran concurrently: these are functional/lifecycle
checks, not performance measurements or user visual acceptance.

The final macOS suite passes 80/80 after pending-slot reuse and simultaneous
three-slot pixel tests were strengthened. The WSI capture/analysis tests pass
23/23. New C/C++/shader files pass clang-format 23.1.1 and targeted codespell;
whitespace checks pass. Linux build is warning-free; full-suite and actual
software-Vulkan results follow separately.

### Software Vulkan found a submission stall

Adding Mesa Lavapipe to the existing x86-64 Ubuntu container made previously
skipped GPU cases execute. The default Linux run passed 51/52 suites; the
renderer hit its unchanged 60 s limit in the deliberately blocked-queue case.
Progress markers isolated a host stall inside the prototype's two-batch
`vkQueueSubmit`: the second batch waited on an intermediate binary signal.
This was not a shader pixel assertion or a reason to relax the CI timeout.

The final implementation uses one batch, waiting and signalling the existing
binary semaphore around its commands, with no intermediate semaphore. This
satisfies the [Vulkan submission ordering and semaphore-state rules](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html).
The delayed-queue case passes on Lavapipe with Khronos validation enabled.
Restoring the old two-batch implementation with the same ready-producer gate
reproduces the stall and reaches a diagnostic 15 s process timeout. The temporary
progress markers are removed. Vulkan permits driver-dependent host submission
cost; the pass promises no explicit current-frame completion wait, rather than
a universal nonblocking guarantee for every implementation.

Logs: `wsi-vulkan-camera-linux-gate.log`,
`wsi-vulkan-camera-linux-two-submit-control.log`, and the final renderer/validation
reruns in `build/review/`. Full-suite results are recorded separately below.

## Final software validation

- macOS: full RelWithDebInfo build has no compiler warnings; 80/80 CTests pass
  with the single-batch handoff. Final focused camera run passes 870 assertions
  in four cases, including valid replacement-frame reuse while the GPU is
  blocked, retryable OOM, stereo pixels and safe bounded teardown.
- Ubuntu 24.04 / GCC 13, x86-64 emulation: default build has no compiler warnings;
  the original full run passed the other 51 suites. After the submission fix,
  the previously timed-out renderer suite passes in 6.73 s with its existing
  60 s limit. Thus all 52 suites pass across the full run and focused rerun;
  the long solver suites were not repeated for a camera-only fix.
- Mesa Lavapipe 25.2.8 (LLVM 20.1.2): the final four camera cases pass all 870
  assertions with `VK_LAYER_KHRONOS_validation` enabled and no validation error
  output. This executes the same production Vulkan shader and submission code
  as the Apple M5 tests, rather than skipping for lack of a Vulkan device.
- WSI runner/analysis: 23/23 Python tests pass. New source passes clang-format
  23.1.1, targeted codespell and whitespace checks. Whole-branch contribution
  formatting and REUSE lint were not rerun; review-series/DCO work remains
  separate from this local implementation.

Logs are in `build/review/wsi-vulkan-camera-*`, especially
`macos-ctest.log`, `macos-camera.log`, `linux-build.log`,
`linux-final-build.log`, `linux-ctest.log`, `linux-render-ctest.log` and
`linux-camera-validation.log` with that common prefix. The reused Linux
container is returned to its prior stopped state; no host packages are installed.

### Final single-batch physical preflight

`build/review/wsi-vulkan-camera-single-batch-service-20261016-1/` checks the
final one-batch implementation at 120 Hz, full scene, normal timing budget and
Metal API validation. Camera composition has zero errors; application exit and both
restoration statuses are zero. However, the terminal renderer submission logs
`VK_NOT_READY` from the existing zero-time producer-readiness guard. This
capture therefore does not satisfy the runner analyzer's strict no-runtime-error
gate and its timings below are descriptive only, not an accepted performance
comparison. Its `camera-summary.json` records 117.63 observed
presents/s, 1.77% intervals longer than 12.5 ms, and camera CPU submission cost
0.375 ms median / 0.861 ms p99. The 20-second sample has 8.342 ms median/p95
interval and 16.683 ms p99, so occasional missed refreshes remain. Do not infer
that this removes every motion artifact or establishes a strict tail guarantee.

This is a different, later binary from the first two-batch 119.33 Hz functional
capture. Both are much faster than the old blocking bridge, but the two short
runs are not a matched comparison of their tail behavior. The final manifest
records base `ec4c02591`, source diff SHA-256
`1a9cb608a3372139bd520554215f9369340c68ae31f25066248477d2746b6a3b`,
untracked source snapshots and exact binary hashes. Source and parameters still
need the user's moving-head acceptance before default promotion.

The final single-batch hosted capture
`build/review/wsi-vulkan-camera-single-batch-hosted-20261016-1/` passes both
initial 90/120 Hz sessions and their 90→120→90→120 cycles with no runtime/GPU
error lines, zero application exits and successful display/service restoration.
The service terminal `VK_NOT_READY` is returned before real queue submission by
`comp_swapchain_gpu_reuse_submit_info_builder_prepare` when a producer fails
its zero-time readiness check. The guard remains intact; its cause is not
changed or silently suppressed in this camera work. The repeat
`build/review/wsi-vulkan-camera-single-batch-service-20261016-2/` uses the same
binary hashes and passes with no runtime/GPU error lines. The application and
both restoration statuses are zero. After discarding 240 feedback samples,
it records 117.15 presents/s, 2.33% intervals longer than 12.5 ms, and camera
CPU cost 0.421 ms median / 0.929 ms p99. Present intervals are 8.342 ms
median/p95 and 16.683 ms p99. Its manifest records base `ec4c02591` and diff
SHA-256 `3ff37c3af63e40ca80d80bbbdfc9e84479845516815e4270a4e6838a80dd98af`;
the changed snapshot also includes documentation and the strengthened test.
This clean short repeat does not establish that the earlier terminal readiness
rejection cannot recur, or replace moving-head visual acceptance. Native remains
the default until that acceptance.


### Moving-head visual check fails (16 October 2026)

The user tested the final Vulkan pass with full scene, calibrated CV WSI,
120 Hz, normal timing budget and Metal API validation. Feedback:
“Higher frame rate, but just higher frame rate shimmer!” This rejects the
replacement as a fix for the motion artifact despite its throughput gain.
Native remains the default; do not promote calibrated WSI or call its visual
validation complete.

Capture: `build/review/wsi-vulkan-camera-visual-20261016-1/`, base
`ec4c02591`, source diff SHA-256 `bcae7172f5d08226a9fb90adc0943fc3a786e7b6617402564ec23b5717d77f5a`.
The manifest saves untracked sources and binary hashes. Application exit and
both restoration statuses are zero, with no runtime/validation error lines.
After 240 feedback entries the capture records 118.18 presents/s and 1.43%
of intervals above 12.5 ms (median/p95 8.342 ms, p99 16.683 ms, max 25.025 ms).
Across 13,928 matched feedback/renderer target entries, reported present minus
predicted display time is approximately zero at median/p95, +8.342 ms at p99,
and ranges from -8.050 to +25.025 ms. Filtered presentation offset ranges
16.683–24.734 ms (median 16.683, p95 19.086, p99 21.618 ms).
These trace timestamps do not establish physical scanout timing or identify
which intervals the user perceived as shimmer.

The camera pass receives the renderer's exact target orientation; it does not
independently query a later display pose. This rules out that particular code
mismatch, not target-time error or camera capture-pose error. Next investigation
should distinguish scene/presentation timing from camera-specific artifacts
using the same Vulkan WSI scene without camera blending, then compare native
under the same workload. Avoid another fixed offset or new runtime toggle
without evidence, and retain the portable renderer as unvalidated work pending
that investigation.


The user clarifies that the artifact is in the **virtual objects**: “Their
position seemed to be off from the head position and then catch up the next
frame if I stopped moving.” This prioritizes source/target pose selection,
prediction horizon and presentation latency over changing camera calibration.
It is consistent with lag but does not establish its cause or direction from
pose telemetry. Detailed renderer reprojection/late-pose diagnostics were
compiled out (`XRT_FEATURE_MACOS_TIMING_DIAGNOSTICS=OFF`) in the visual capture;
its source-layer trace cannot establish which target pose the shader used.
Enable the existing diagnostic build option for the next matched comparison,
keeping the saved calibration and presentation defaults unchanged.


The same existing build directory is reconfigured with that diagnostic option
on and rebuilt without compiler warnings. All 80 CTests pass (41.50 s), including
GPU tests. Logs: `build/review/wsi-vulkan-camera-diagnostic-build.log` and
`build/review/wsi-vulkan-camera-diagnostic-ctest.log`. This is preparation for
a new moving-head comparison, not additional visual acceptance. No installed
runtime or presentation default changes. The no-camera control must explicitly
keep `XRT_COMPOSITOR_COMPUTE=1`, `PSVR2_CAMERA_STREAMS=1` and
`XRT_MACOS_WSI_PASSTHROUGH=1` while omitting the application's passthrough
request; otherwise settings automatically select graphics and confound the
comparison.


### No-camera compute control changes the render path (16 October 2026)

The user runs the prepared command and reports: “The whole world was head-locked
and flickery.” Capture `build/review/wsi-virtual-only-motion-1/` saves base
`ec4c02591` and diff SHA-256
`ccc209a47f4ab85d2832df4c980f7f803048ed0f5759aec529458b907e9a0274`.
It is interrupted after about 17 seconds of rendered frames, rather than a
completed 60-second run: the manifest has no application-exit field. Display
and service restoration both succeed. No runtime/validation ERROR lines appear.
The application logs 862 world-locked boxes; this is not intentionally the
head-locked-cross-only scene.

The existing detailed trace records 1,878 valid projection frames on the compute
**fast path**, all with timewarp enabled. Target and source orientations both
change; source-to-target angular difference is 0.0705 degrees median,
0.6884 degrees p95, 1.9866 degrees maximum. Source age relative to predicted
display is 7.783 ms median / 8.342 ms p95. These CPU trace inputs establish
neither correct GPU pixels nor correct physical scanout.

Crucially, removing passthrough also removes its explicit fast-path rejection in
`comp_compositor.c`: the blended runs use layer composition, while this control
uses single-projection distortion. Keeping compute enabled did not preserve the
rendering path. The proposed control therefore failed to isolate camera blending;
do not attribute the entire difference to the camera or conclude timewarp was
disabled. Investigate the fast-path/WSI interaction and source-image handoff,
then compare with layer composition held constant. No workaround/default change
is justified by this observation alone.


### User isolates compute selection (16 October 2026)

The user independently tests and reports: “It's XRT_COMPOSITOR_COMPUTE=1”.
On clarification this comparison was **WSI only**; native compute was not tested
in that comparison. Treat compute selection as the reproducible configuration
trigger, without asserting a compute bug across platforms or a specific root
cause. The user's independent run commands/source snapshots were not supplied;
do not attach their result to an invented commit or quantitative capture.

An already-settled architectural distinction is relevant: the Instruments
[direct-display evidence](macos-wsi-generic-experiment.md#direct-by-default-and-gpu-queue-priority-16-october-2026)
found graphics WSI scanned out directly, while compute/storage-image WSI was
WindowServer-composited. Passthrough currently forces compute because the
graphics distortion output is opaque. Therefore the camera-enabled WSI path
also changes compositor and drawable usage; this cannot be treated as only a
camera calibration/rotation port. The direct-display evidence does not prove
that WindowServer composition explains head-locking or flicker.

Next useful discriminator is the native compute fast path with the same opaque
scene and camera streaming, retaining the existing diagnostic build. That
separates a common compute rendering/source-image problem from compute writing
directly to WSI drawables. Native blended passthrough acceptance covers layer
composition, not this single-projection fast path. Avoid disabling timewarp,
changing prediction offsets, or declaring a fast-path-specific cause before
that distinction is established. Keep Linux compute defaults unchanged.


The user subsequently reports native is fine and asks whether the compute
selector affects native. Source inspection confirms `XRT_COMPOSITOR_COMPUTE`
is an existing upstream common-compositor setting, read by `comp_settings.c`
and used by `comp_renderer.c` for either target. The WSI-specific addition
changes only the *unset* default to graphics when WSI passthrough is not enabled;
it does not scope explicit values to WSI. Native retains upstream's compute
default on macOS. Its compute output goes into private images before the native
Metal copy/present; WSI compute writes storage images directly into the WSI
swapchain. Thus native working is relevant evidence; no repeat is needed merely
to establish whether the selector applies to native. The WSI compute/output
interaction is now the investigation focus, without claiming a demonstrated
specific synchronization or WindowServer defect.


## Private compute output candidate

After the user isolates compute selection in WSI and reports native working,
the WSI target is changed to follow native's separation between shader output
and presentation images. This is a candidate correction for the compute/output
interaction, not proof that the driver or WindowServer caused the artifact.
No headset session is started: the user has no headset access for this stage.

When WSI uses compute, `render_output` allocates one private image per acquired
swapchain index, using the selected format and extent. Distortion and optional
camera composition finish in GENERAL layout in those private images. The
camera adapter now uses the target's final layout instead of assuming PRESENT.
The WSI swapchain requests only COLOR_ATTACHMENT and TRANSFER_DST usage;
STORAGE and camera TRANSFER_SRC usage remain on the private images. A full-image
`vkCmdCopyImage` writes the corresponding acquired presentation image and
transitions it to PRESENT. Identical formats preserve bytes and alpha without
additional color conversion. Graphics WSI bypasses this helper.

The generic swapchain retains ownership of the real images/views. The WSI
adapter exposes separate metadata for private render images and restores the
real metadata before generic recreation or cleanup. Renderer recreation already
waits its queue idle before replacing dependent resources. The helper has one
command buffer/fence per output slot, a 500 ms slot-reuse/drain bound and one
GPU submission consuming/re-signalling the existing binary completion semaphore
at ALL_COMMANDS. Camera composition precedes this copy using the same semaphore
chain. Failure retires the target; uncertain work or a drain timeout retains the
private images and swapchain. No synchronous per-frame completion wait, Metal
texture export, new runtime toggle or generic target API change is introduced.

This adds about 93.4 MiB of private output storage for three 4000×2040 RGBA8
images and one full-frame GPU copy per presented frame. Performance, actual
WindowServer/direct-scanout behavior, refresh/recreation on the real display,
service/hosted sessions and moving-head quality are **unvalidated** for this
candidate. Earlier throughput captures in this note describe the preceding
implementation and must not be attributed to this binary. Native remains the
default; the user should next check opaque compute WSI and blended passthrough
with this build before any default promotion.

The Vulkan helper and offscreen tests are portable review units. Only the macOS
WSI target enables the new path; native and Linux compute behavior are retained.
The existing native control and camera calibration must not be changed to hide
this WSI-specific regression. The candidate remains uninstalled; its source is preserved in the fork checkpoint below.


Candidate software validation:

- macOS: warning-free build; final full CTest 80/80 passes in 26.55 s. The
  output helper's three GPU cases pass 63,304 assertions on Apple M5. The final
  renderer suite also exercises the camera pixel matrix both directly and
  through the new copy path, preserving stereo, alpha and calibrated rotation.
- Linux: warning-free default build. Camera/output GPU cases pass 64,610
  assertions in seven cases on Mesa Lavapipe with Khronos validation enabled
  and no validation errors. The complete renderer suite passes in 6.40 s
  within its unchanged 60 s limit. Unrelated Linux suites are not repeated
  for this render-only follow-up.
- Tests execute the production distortion and camera shaders, preserve bytes
  through all three slots and repeated reuse, and cover BGRA output with no
  STORAGE usage on the destination, a deliberately blocked GPU queue, bounded
  reuse/drain retention, and queue OOM without consuming the producer signal.
- New helper/test files pass clang-format 23.1.1 and targeted codespell;
  whitespace checks pass. Whole-branch contribution/REUSE checks remain
  outside this software checkpoint. No upstream submission or DCO action.

Evidence logs in the main checkout's `build/review/`:
`wsi-private-output-build.log`, `wsi-private-output-macos-gpu.log`,
`wsi-private-output-macos-ctest.log`, `wsi-private-output-linux-build.log`,
`wsi-private-output-linux-validation.log` and
`wsi-private-output-linux-render-ctest.log`. Source and binary fingerprints are
saved in `wsi-private-output-source.json` and `wsi-private-output-source.diff`.
The reused Linux container is returned to its stopped state.

When headset access returns, use the existing WSI runner with
`--conditions wsi --service-env XRT_COMPOSITOR_COMPUTE=1`,
full baseline scene and Metal validation, first without an app passthrough
request and then with `--passthrough overlay`, using separate output directories.
Confirm `WSI compute output: ... private images` in each service log and that
pose tracing is still compiled in. Check gentle/rapid motion and sudden stops,
then repeat hosted and refresh/recreation checks. A new display trace is needed
to establish direct scanout; offscreen validation cannot establish that.


## Unified presenter selection (17 October 2026)

The current selector is `XRT_MACOS_PRESENTER=wsi|wsi-adaptive|native`, default
`native`. `wsi` selects calibrated CV pacing and `wsi-adaptive` selects adaptive
display-timing pacing. The former WSI/CV/calibration/adaptive booleans are retired
from executable configuration. This changes selection only; it does not validate
the private-output candidate or promote WSI. Current runner conditions are
`native`, `native-timed`, `wsi`, `wsi-adaptive`; use `--conditions wsi` for the
pending compute checks. See the [migration note](macos-wsi-migration.md#unified-presenter-selector-17-october-2026).


## Fork checkpoint (17 October 2026)

Preserve the current work on `claude/macos-wsi-integration` as separate commits:

- `8fd39638b`: portable Vulkan camera composition, private output copies and
  GPU pixel/lifetime tests.
- `eca298ec0`: PS VR2 source/calibration adapter, removal of the blocking Metal
  camera bridge and private-output integration in the macOS WSI target.
- `b5d331ad5`: unified presenter selector, runtime/parser tests and capture-tool
  migration, retaining native as the default.

The documentation checkpoint retains both successful software tests and failed
moving-head feedback. It does not convert the private-output candidate's pending
hardware checks into acceptance. The tested source snapshots, binary hashes and
raw timing captures remain in ignored local `build/review/` directories; source,
tests and durable evidence summaries belong in Git.

The integration branch separately preserves upstreaming progress notes and the
standalone presentation probe at `92beaeeb0`. The older uncommitted prototype is
saved at `409c1c769` on `experiment/macos-wsi-current-native`, marked WIP and not
merged into the current WSI or integration branch. Its Python tests pass, but
its runtime tree is not revalidated at this checkpoint. Fork backup is separate
from upstream submission, DCO certification and installed-runtime promotion.


## Fragment output pass (18 October 2026)

The private-output candidate removed shader writes from the drawables but kept
two full-frame copies when passthrough was on (application image into the
camera slot, then private image into the drawable). It also requested
`TRANSFER_DST` on the WSI images, which makes MoltenVK set
`framebufferOnly = NO`. This step replaces both copies and the camera compute
dispatch with full-screen fragment passes. No headset run has tested it yet.

- `render_output_present` (replacing `render_output_copy`) draws one
  full-screen triangle into the presentation view. `output.frag` passes the
  private image through with `texelFetch`, alpha included.
- With passthrough on, `render_camera` is the final pass instead. `camera.frag`
  is the former `camera.comp` shader, changed only in its inputs and outputs
  and in sampling the camera at an explicit level of detail 0. It samples the
  private image in place and renders the drawable. The output pass is skipped for that frame. If the camera pass does
  not submit (no fresh camera frame, rejected input or queue OOM), the plain
  output pass still writes the drawable.
- The WSI swapchain now requests only `COLOR_ATTACHMENT`, as the graphics
  compositor does, so MoltenVK leaves `framebufferOnly` on. Shader storage and
  `TRANSFER_SRC` usage stay on the private images.
- Synchronization is unchanged: one batch per frame consumes and re-signals the
  binary `render_complete` semaphore. Per-slot fences, bounded 500 ms reuse and
  drain waits and retain-on-timeout teardown keep their previous behaviour.

Effect per frame with passthrough: one fragment pass instead of two
4000x2040 copies plus a compute dispatch, and no per-slot application image
(about 93 MiB less memory). Without passthrough: one fragment pass instead of
one copy.

### Open question: direct scanout

The Instruments trace showed direct scanout only for the graphics compositor,
whose layer was `framebufferOnly` 1 with BGRA8 sRGB. This pass matches the
`framebufferOnly` setting but keeps the compute compositor's UNORM swapchain
format, because the private image already holds sRGB-encoded values. Only a new
display trace can tell whether that is enough. If it is not, the next step is an
sRGB swapchain with the private image sampled through an sRGB view
(`MUTABLE_FORMAT`), so the values are decoded and re-encoded rather than encoded
twice.

### Software validation

- Linux (Ubuntu 24.04, GCC 13) default build has no compiler warnings, and the
  full CTest suite passes (53/53) with Lavapipe as the Vulkan device.
- Mesa Lavapipe 25.2 (LLVM 20.1.2) runs the camera and output GPU tests, 7 cases
  and 64,662 assertions, with `VK_LAYER_KHRONOS_validation` and synchronization
  validation on. Neither reports an error. Tests cover stereo, alpha and
  calibrated rotation through standalone sources and through `render_output`'s
  private images, exact distortion bytes and alpha through the output pass,
  destinations without STORAGE, SAMPLED or TRANSFER_DST, blocked-queue reuse and
  drain retention, and queue OOM.
- New and changed sources pass clang-format 23.1.1.
- Not run here: the macOS build (the WSI target is Objective-C) and any Apple GPU
  test. Run the macOS CI recipe before a headset session.

### Headset checks

These replace the private-output checks above. Use separate output
directories for each.

1. `--conditions wsi --service-env XRT_COMPOSITOR_COMPUTE=1`, full baseline
   scene, no app passthrough request. Expect `WSI compute output: ... fragment
   pass to presentation images` in the service log and
   `framebufferOnly=1` on the `EXPERIMENT effective layer` line. Check for the
   earlier flicker and head-locking.
2. The same with `--passthrough overlay`. Check that the boxes stay fixed
   against the camera during gentle and fast motion and sudden stops.
3. If both look right, record a display trace to settle direct scanout.
4. Repeat hosted and refresh/recreation checks.


## Simulated room source (18 October 2026)

With `SIMULATED_DESKTOP=1`, the same camera pass draws a simulated room instead
of camera images (`RENDER_CAMERA_MODE_ROOM`), at the exact eye poses used for
timewarp. It tests whether application content and background stay locked
without a headset. `render_camera_params.rotate` is now `mode` (static, rotate
or room), and the parameters grow to 240 bytes with each eye's world pose. In
room mode no camera frames are uploaded. See
[the desktop simulation note](macos-desktop-simulation.md#simulated-passthrough-room).
