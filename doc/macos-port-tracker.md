<!--
Copyright 2026, Collabora, Ltd.
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# macOS Port Tracker

> **2026-10-02 Wine hardware gate:** the experimental in-process runtime passed
> PS VR2 runtime-owned 2D/array image pixel checks and Opaque hello_xr. Hosted
> compositing initializes inside Wine, but currently runs at 60 Hz on the 120 Hz
> display. Visual confirmation and full-rate validation remain pending. See
> [the hardware evidence](macos-wine-in-process-endpoint.md#ps-vr2-hardware-gate-2026-10-02).

This is the concise branch-oriented companion to
[macos-port.md](macos-port.md). The main document is the authoritative
high-level status/roadmap.

## Current integration branch

- Repository: [NikNakk/monado](https://github.com/NikNakk/monado)
- Integration branch:
  [`macos-game-mode-upstream-sync-2026-10`](https://github.com/NikNakk/monado/tree/macos-game-mode-upstream-sync-2026-10)
- Upstream-oriented cleanup branch:
  [`macos-upstream-clean`](https://github.com/NikNakk/monado/tree/macos-upstream-clean)
- Canonical upstream remains the Monado project on freedesktop.org.
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

Game Mode work is on
[`claude/game-mode-priority-issue-xkx6m7`](https://github.com/NikNakk/monado/tree/claude/game-mode-priority-issue-xkx6m7),
which builds on the integration branch and is not merged into it yet.

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

- [`macos-pssense-6dof`](https://github.com/NikNakk/monado/tree/macos-pssense-6dof)
- [tracking notes](https://github.com/NikNakk/monado/blob/macos-pssense-6dof/doc/pssense-optical-tracking.md)
- [camera calibration notes](https://github.com/NikNakk/monado/blob/macos-pssense-6dof/doc/psvr2-camera-calibration.md)

This is intentionally separate from the integration branch. Camera calibration,
LED phase/bootstrap and multi-camera pose solving work, but sustained dynamic
two-controller tracking, fusion/filtering and reacquisition are not yet reliable
enough to merge.

### Earlier camera calibration

- [`macos-psvr2-camera-calibration`](https://github.com/NikNakk/monado/tree/macos-psvr2-camera-calibration)

Historical/base work for the current optical tracking branch.

### Depth development history

- [`macos-depth-aware-reprojection`](https://github.com/NikNakk/monado/tree/macos-depth-aware-reprojection)

The useful depth path has since been integrated into the main macOS integration
branch. Keep this branch mainly for development history/comparison.

## Immediate priorities

1. Finish client-side compositing for Game Mode: validate handoff on
   hardware, merge into the integration branch, then overlays and Chromium,
   and make it the default
   ([design](macos-client-compositor-design.md)).
2. Make PS Sense optical 6DoF reliable enough to merge.
3. Turn existing PS VR2 camera acquisition into a calibrated passthrough/MR
   pipeline.
4. Bring the validated FB/META foveation path to engines and Chromium,
   and refine the experimental Metal rendering companion for upstream review.
5. Continue hardening Chromium's sandboxed IOSurface/shared-event graphics path.
6. Keep the generic macOS/Metal handoff stable for external compatibility
   clients; Wine/OpenVR breadth is tracked in `macos-wine-xr`.
7. Continue compositor pacing/reprojection robustness work, including depth +
   foveation coordinate handling.
8. Add broader OpenXR regression/conformance coverage.
9. Package and notarize the runtime with a simple settings/diagnostics surface.

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
