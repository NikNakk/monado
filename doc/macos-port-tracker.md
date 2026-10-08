<!--
Copyright 2026, Collabora, Ltd.
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# macOS Port Tracker

See the [2026-10-03 inherited compositor audit](macos-inherited-compositor-audit.md)
for this investigation's changes, additional defects and selective upstream
restoration changes. Diagnostic readbacks are now opt-in and format/usage
checked, pose selection follows upstream on all platforms, and Apple alpha
behavior is limited to macOS. The user confirmed the output-barrier fix with
readbacks off; residual-stall work and Linux CI remain separate.

> **2026-10-02 Wine hardware gate:** the experimental in-process runtime passed
> PS VR2 runtime-owned 2D/array image pixel checks and Opaque hello_xr. Hosted
> compositing initializes inside Wine, but currently runs at 60 Hz on the 120 Hz
> display. Visual confirmation and full-rate validation remain pending. See
> [the hardware evidence](macos-wine-in-process-endpoint.md#ps-vr2-hardware-gate-2026-10-02).

This is the concise branch-oriented companion to
[macos-port.md](macos-port.md). The main document is the authoritative
high-level status/roadmap.

## Current integration branch

The integration includes the
[failed CADisplayLink-owned compositor experiment](macos-cadisplaylink-owned-compositor-experiment.md).
Only its diagnostics and evidence remain; callback/deferred CA rendering modes
were rejected and removed. The historical run tables and failed-experiment
record are carried with the runtime corrections.

- Repository: [NikNakk/monado](https://github.com/NikNakk/monado)
- Integration branch:
  [`macos-upstream-clean`](https://github.com/NikNakk/monado/tree/macos-upstream-clean)
- Integration target confirmed by the user on 2026-10-03. The prior remote
  head `08619006b` is an ancestor of `codex/macos-shared-tracking` with no
  divergence. The integration carries the intervening upstream sync and
  per-thread scheduling fix, followed by the tracking, pose, image-reuse,
  output-barrier and diagnostics work. Shared tracking stays opt-in.
- Canonical upstream remains the Monado project on freedesktop.org.
- Latest local sync: `macos-upstream-clean` merged GitLab `main` through
  `ec188bb13` on 2026-10-07 in `753698f92`, without conflicts. The fork's
  passthrough and floor-calibration workers now use upstream's replacement
  thread-naming API. The macOS service check build and all 45 CTest suites
  passed (three suites needed a rerun outside the sandbox); headset validation
  and Linux CI remain pending. See [Upstream sync](macos-port.md#upstream-sync).
- Local cleanup branch sync: `macos-upstream-clean` merged GitLab `main` through
  `22d5c936c` (2026-10-02) on 2026-10-03, without rebasing. The macOS service
  check build and all 35 CTest suites passed; headset validation and Linux CI
  for this follow-up remain pending.
- Upstream sync: `main` up to `045931d12` (2026-09-30) is merged on
  `macos-upstream-sync-2026-10`, pending macOS CI and a headset run; see
  [Upstream sync](macos-port.md#upstream-sync).

The integration branch carries the broadest native Apple Silicon runtime,
PS VR2 HMD, Metal/IOSurface sharing, launchd/XPC service, depth, passthrough,
eye gaze and Chromium work. Wine/OpenVR compatibility has been split into
[NikNakk/macos-wine-xr](https://github.com/NikNakk/macos-wine-xr).

It also carries the standards-facing slices, merged in this order:

- [`standards/khr-generic-controller`](https://github.com/NikNakk/monado/tree/standards/khr-generic-controller)
  — `XR_KHR_generic_controller` for PS Sense, Touch-family and Index devices
  (opt-in: `XRT_FEATURE_OPENXR_INTERACTION_KHR_GENERIC`).
- [`standards/fb-foveation`](https://github.com/NikNakk/monado/tree/standards/fb-foveation)
  — `XR_FB_swapchain_update_state`, `XR_FB_foveation` and
  `XR_FB_foveation_configuration` resolved into the backend-neutral
  `xrt_foveation_state`.
- [`standards/fb-foveation-metal`](https://github.com/NikNakk/monado/tree/standards/fb-foveation-metal)
  — the Metal backend: experimental `XR_MNDX_foveation_metal`, compositor
  remap of the rate map, packed multi-view support, and runtime-owned
  `XR_META_foveation_eye_tracked`.

Client-side compositing from `claude/game-mode-priority-issue-xkx6m7` is
present in this integration branch; multi-client hardware validation remains.

All foveation options are opt-in at build time (default OFF). Fixed FB
foveation works without eye tracking; META eye-tracked foveation uses
runtime-private PS VR2 gaze. Both are hardware-validated through
`monado-service`.

The branch has macOS and Linux build/test CI workflows; the macOS PS VR2
diagnostics workflow also compiles and tests each standards slice with its
options enabled.

## Current validated capabilities

- Native Apple Silicon `monado-service` and OpenXR runtime.
- Native PS VR2 HMD discovery, display output and 6DoF head tracking.
- Vulkan/MoltenVK compositor with native Metal/CAMetalLayer presentation.
- CADisplayLink pacing is the default after corrected native headset captures
  and heavy Unreal/Game Mode testing (2026-10-03). CV remains an explicit and
  automatic creation fallback. Fully buffered hosted UE physical presentation favours CA under load
  (105.18 vs 100.08 Hz); 90 Hz remains untested; see [the default decision](macos-psvr2-timing-diagnostics.md#cadisplaylink-promoted-to-default--2026-10-03).
- Native Metal OpenXR clients.
- IOSurface and shared-Metal cross-process graphics transport.
- Launchd/XPC service activation and Metal shared-event support.
- `XR_KHR_composition_layer_depth` plus experimental depth-aware reprojection.
- Native application/engine validation through Unity/Open Brush, Unreal Engine,
  Godot, Chromium WebXR and SwiftXR/SwiftXRShell.
- Native PS Sense HID path for 3DoF, inputs and haptics.
- `XR_EXT_eye_gaze_interaction` on PS VR2 with lazy activation and optional
  user calibration.
- Opt-in (build-time) standard `XR_FB_foveation` /
  `XR_FB_foveation_configuration` and runtime-owned
  `XR_META_foveation_eye_tracked` for Metal clients through
  `XR_MNDX_foveation_metal`, validated on PS VR2 via `monado-service`
  (2026-09-29).
- Opt-in in-process client compositing (`XRT_MACOS_CLIENT_COMPOSITOR=1`),
  hosted by the service through `CALayerHost`: Unreal holds 120 Hz under Game
  Mode (2026-09-30).

## Implemented, awaiting hardware validation

- Native diagnostic `--blendmode Opaque|AlphaBlend` selection, with runtime
  capability checks and transparent projection background for AlphaBlend.
  See [the diagnostic commands](macos-port.md#openxr-and-compositor-features).
  Meta simulator AlphaBlend passes without Metal validation; enabling validation
  reproduces its IOSurface storage-mode assertion natively (2026-10-02).

- Hosted-client handoff that follows the service's focus, and passthrough
  camera frames shared with in-process clients (Game Mode branch). The
  2026-10-01 hardening uses read-only consumer descriptors, hides on session
  teardown without event polling, and gives hosted bindings explicit ownership.
  See [the design note](macos-client-compositor-design.md#ownership-and-teardown-hardening-2026-10-01).

- `XR_FB_passthrough` from the stock-headset BC4 cameras, composited in the
  final Metal presentation pass (uncalibrated fisheye approximation).
- Opt-in `XR_KHR_generic_controller` mapping for PS Sense.
- Per-image foveation map association for sparse/re-submitted frames and
  foveated projection layers in the layer squasher (unit-tested; see
  [the foveation design note](macos-openxr-foveation.md#not-yet-validated-on-hardware)).

## Active side branches

### PS Sense optical 6DoF

The [2026-10-04 integration](macos-pssense-6dof-integration.md) ports
`claude/pssense-mr2940-evaluation` at `e54478a16` and its uncommitted runtime
hookup from `~/Code/monado`. The joint solver + EKF is available here with
`PSVR2_SENSE_6DOF=1` and an explicit calibration path. Optimised runtime builds,
local tests and byte-identical recorded-session replay pass. OpenBrush and
10–20 minute game sessions are user-confirmed; broader hardware coverage and
the latest Linux CI remain pending. The note includes launch commands and
retains the source's calibration/tracking evidence.

- [`macos-pssense-6dof`](https://github.com/NikNakk/monado/tree/macos-pssense-6dof)
- [tracking notes](https://github.com/NikNakk/monado/blob/macos-pssense-6dof/doc/pssense-optical-tracking.md)
- [camera calibration notes](https://github.com/NikNakk/monado/blob/macos-pssense-6dof/doc/psvr2-camera-calibration.md)

Ongoing development remains separate; the opt-in runtime port is integrated.
Camera calibration, LED phase/bootstrap, multi-camera solving and EKF have
recorded-session evidence. Sustained two-controller OpenXR tracking, alignment,
reacquisition and teardown need hardware validation before default enablement.

### Earlier camera calibration

- [`macos-psvr2-camera-calibration`](https://github.com/NikNakk/monado/tree/macos-psvr2-camera-calibration)

Historical/base work for the current optical tracking branch.

### Depth development history

- [`macos-depth-aware-reprojection`](https://github.com/NikNakk/monado/tree/macos-depth-aware-reprojection)

The useful depth path has since been integrated into the main macOS integration
branch. Keep this branch mainly for development history/comparison.

## Immediate priorities (updated 2026-10-08)

1. Finish formatting/documentation cleanup and publish the October 7 upstream
   integration for Linux/macOS CI; complete its headset regression separately.
2. Continue presentation-stall diagnosis and hosted-client handoff/teardown
   validation. Shared tracking remains opt-in.
3. Sense combined defaults have already been used in games for 10–20 minutes,
   confirmed by the user. Broaden reliability coverage without repeating this
   completed gate as outstanding work.
4. Confirm visible camera-only and camera-plus-scene passthrough, then calibrate
   its actual camera mode, geometry and timing.
5. Validate fixed and eye-tracked foveation in Unity or Unreal, then investigate
   Wine graphics-backend integration.
6. Continue Chromium graphics sharing, OpenXR regression coverage and packaging.
7. Upstream submissions are paused while the first MR awaits merge and the
   contributor learns the review process.

## Related repositories

See the main status document for descriptions and branch details. The principal
companion repositories are:

- [Open Brush / Unity](https://github.com/NikNakk/open-brush)
- [Godot](https://github.com/NikNakk/godot)
- [Unreal Engine](https://github.com/NikNakk/UnrealEngine)
- [Chromium](https://github.com/NikNakk/chromium)
- [SwiftXR](https://github.com/NikNakk/SwiftXR)
- [SwiftXRShell](https://github.com/NikNakk/SwiftXRShell)
- [xrizer](https://github.com/NikNakk/xrizer)
- [macOS Wine XR](https://github.com/NikNakk/macos-wine-xr)
- [BasaltVR](https://github.com/NikNakk/BasaltVR)
- [GAV PSVR2 Player for macOS](https://github.com/NikNakk/gav-psvr2-player-mac)
- [PSVR2Toolkit](https://github.com/NikNakk/PSVR2Toolkit)

## Detailed runtime documents

- [OpenXR foveation architecture](macos-openxr-foveation.md)
- [PS VR2 eye gaze](macos-psvr2-eye-gaze.md)
- [PS VR2 gaze-driven foveation](macos-psvr2-gaze-foveation.md)
- [Client-side compositing for Game Mode](macos-client-compositor-design.md)
- [Game Mode and cross-process layer hosting](macos-remote-layer-hosting.md)
- [Environment toggle inventory](macos-env-toggles.md)
- [macOS direct service XPC](macos-service-direct-xpc.md)
- [PS VR2 timing diagnostics](macos-psvr2-timing-diagnostics.md)
- [PS VR2 judder evidence](macos-psvr2-judder-evidence.md)
Wine/OpenVR implementation notes and the preserved embedded implementation now
live in [NikNakk/macos-wine-xr](https://github.com/NikNakk/macos-wine-xr).

Older Metal-array, service-XPC, timing and presentation branches should
generally be considered development history unless a specific experiment still
references them.

## IPC hardening and upstream preparation

The security follow-up verifies native socket peer identity, locks socket
lifetimes and bounds pending XPC resources. Compatibility-bridge transport
authentication now belongs to the external `macos-wine-xr` project. The
former external-broker runtime override is retired.

Contribution checks and Android/macOS version coverage are automated. Hardware
regression runs remain user-owned. Human DCO sign-offs
and upstream MR-specific changelog filenames remain submission prerequisites;
see [the contribution preparation](macos-upstream-contribution.md).


### Clean service build follow-up, 2026-10-01

Removed dormant Unix-channel framing references to deleted fields. Clean
service/XPC targets and three IPC regression suites pass. Wire commands and
schemas are unchanged. See [the validation note](macos-service-direct-xpc.md#clean-unix-channel-build-validation-2026-10-01).


The compositor-on-CA-thread variants tested on
`codex/cadisplaylink-compositor` were retired after a deferred run-loop follow-up
also failed to improve physical timing. Ordinary CA pacing and client-hosted
compositing remain. Passive callback and renderer-stage diagnostics are retained;
see the [final experiment decision](macos-psvr2-timing-diagnostics.md#deferred-run-loop-results-and-retirement).


Five alternating static UE runs per mode on `codex/cadisplaylink-compositor`
favour ordinary CA pacing over compositor execution on the CA callback thread
(mean physical cadence 108.96 vs 102.03 Hz). A deferred follow-up also failed to improve physical timing; the experimental
dispatch and switch were removed. Game Mode was
unconfirmed and moving-head validation remains pending. See the
[five-pair evidence](macos-psvr2-timing-diagnostics.md#five-alternating-pairs-with-fully-buffered-traces--2026-10-03).


A separate hosted UE policy check confirms `ext_darwinbg=1` and priority 4 on
all sampled service threads while UE's compositor threads retain priority 97.
Shared-event readiness waits are active. Client-hosted rendering is retained;
the next architecture priority is removing synchronous pose IPC through shared
tracking state and local prediction, with source-age validation. See the
[follow-up evidence](macos-psvr2-timing-diagnostics.md#deferred-run-loop-results-and-retirement).

## Shared tracking follow-up (2026-10-03)

`codex/macos-shared-tracking` implements opt-in PS VR2 shared tracking for the
client-hosted compositor (`XRT_MACOS_SHARED_TRACKING=1` in the client).
It publishes raw SLAM/gyro/prediction state at USB ingestion and uses the same
future-pose predictor in the client, removing that compositor query's synchronous
IPC dependency. The service still owns tracking and the general space graph.
Full macOS build and all 36 CTests pass. Five alternating static captures per path reduce median run pose-query p99
from 0.185 to 0.016 ms, but physical timing does not consistently improve.
Moving-head smoothness and Linux CI remain unvalidated. Keep it opt-in.
See the [design and freshness limits](macos-client-compositor-design.md#shared-ps-vr2-tracking-experiment--2026-10-03).

Moving-head shared-tracking validation is prepared for the user to run later.
The version-2 trace adds actual sensor receipt/device-clock evidence and returned
poses. A static preflight verified buffering and exact physical pose-target joins;
movement and confirmed Game Mode transitions remain untested. Earlier fully
buffered claims require qualification because some writers still flushed
periodically; these are now corrected. See the
[capture protocol and preflight evidence](macos-psvr2-timing-diagnostics.md#prepared-moving-head-freshness-capture--2026-10-03).

The first buffered moving-head shared-tracking capture is now analysed. Local
pose queries remain fast (p99 0.011 ms) during substantial rotation and observed
service background/menu policy transitions. Internal SLAM disagreement p95 is
0.209 degrees at the pose target and 0.226 degrees at physical presentation,
but rare receipt gaps reach 63 ms and physical gaps 142 ms. The largest physical
hitch occurs after Metal completion while CA callbacks remain on cadence,
with matching drawable backpressure. The user confirms Game Mode in the first
and last thirds and reports improvement, but persisting judder of a different
quality. Keep the path opt-in. Next isolate delayed presentation of completed frames,
rather than treating this result as proof that service acquisition must move.
See the [moving-head evidence](macos-psvr2-timing-diagnostics.md#first-buffered-moving-head-shared-tracking-run--2026-10-03).

Completed-frame presentation diagnostics now join GPU completion to physical
output, with passive scheduled-callback and service AppKit-pump traces. The
moving-head run has 94 frames displayed >20 ms after GPU end, while the largest
pause clears after two old frames rather than producing a persistent latency
ratchet. Capture now acknowledges flushes before UE teardown and checks source
health before measurement. Static lifecycle checks pass; the next moving-head
capture is prepared. See the
[investigation and capture command](macos-psvr2-timing-diagnostics.md#completed-frame-presentation-investigation--2026-10-03).

The next moving capture stopped at its health gate because both sensor streams
were silent. Service-only tests reproduce this without UE, including with camera
and gaze disabled. A user power-cycle/reconnect restores tracking, and the full
warmup/measurement/post-window buffered UE preflight now passes. The cause of the
original stream loss remains unresolved; moving capture 03 is prepared. See the
[recovery evidence](macos-psvr2-timing-diagnostics.md#missing-tracking-during-capture-02--2026-10-03).

The recovered moving run is analysed: 117.59 Hz physical cadence versus 10–12
UE frames/s, fast local poses, but repeated completed-frame presentation stalls
and persisting subjective movement jumpiness. Rotation-only timewarp leaves
source-view translations uncorrected; compare a high-FPS or translation-isolating
scene before attributing regular jumps to the predictor. See the
[second moving capture](macos-psvr2-timing-diagnostics.md#recovered-second-moving-head-capture--2026-10-03).

The UE low-cost A/B raises application FPS to roughly 56–63 and reduces subjective
jump size, but tracking/presentation stalls persist. It exposes an existing
compute-renderer target-pose override: raw local queries may be accurate but
discarded for recent projection layers. Actual renderer orientations require
validation, and correcting macOS target selection is the next concrete step.
See the [A/B evidence](macos-psvr2-timing-diagnostics.md#ue-workload-ab-results-and-effective-timewarp--2026-10-03).

The macOS compute target-pose override is fixed: a low-cost UE preflight matches
actual renderer orientations to the fresh query on all 609 compared frames.
Build, all 36 macOS CTests and 15 Python checks pass; Linux behaviour is retained
but Linux CI and moving-head subjective validation remain pending. The next
workload pair keeps Game Mode on throughout and does not rely on speech cues.
See the [fix and capture commands](macos-psvr2-timing-diagnostics.md#fresh-scanout-target-fix--2026-10-03).

The user confirms regular judder is gone with fresh target poses; occasional
stalls and partly black frames/noise persist. Shared-event waits are active.
Local Metal swapchains now also enable the service path's GPU-read completion
guard before application reuse. Actual guarded returns and build/tests pass;
visual corruption confirmation is pending. See the
[handoff evidence](macos-psvr2-timing-diagnostics.md#moving-head-confirmation-and-local-metal-image-reuse--2026-10-03).

The user confirms the local Metal GPU reuse guard removes the black corruption.
Both it and the fresh-target fix are visually validated in the tested UE path;
occasional stalls remain unresolved. See the
[confirmation](macos-psvr2-timing-diagnostics.md#user-confirms-image-reuse-correction).

## Sense follow-up: Linux CI and Rift isolation (2026-10-04)

The Linux job compiled but failed its warning gate on new replay/test code;
those warnings are fixed without relaxing the gate. Formatting and license
checks pass locally, as do all 44 macOS CTest suites. The Rift driver/builder and
original optimizer remain unchanged by the Sense integration, and an explicit
caller flag now prevents inherited Sense joint settings from switching Rift to
the experimental worker. See the [audit and validation](macos-pssense-6dof-integration.md#linux-ci-warning-fixes-and-rift-isolation-audit-2026-10-04).
Linux rerun and Rift hardware validation remain pending.

## Sense application milestone: OpenBrush (2026-10-04)

The user confirms successful 3D painting in OpenBrush, the first non-test
application validation of integrated Sense 6DoF. This is a historical milestone:
the October 5 profile/reacquisition work and October 8 confirmation of 10–20
minute game sessions supersede its initial reliability status.
See the [application evidence](macos-pssense-6dof-integration.md#first-non-test-application-openbrush-2026-10-04).
