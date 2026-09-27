<!--
Copyright 2026, Collabora, Ltd.

SPDX-License-Identifier: BSL-1.0
-->

# macOS Port Tracker

This is the concise branch-oriented companion to
[macos-port.md](macos-port.md). The main document is the authoritative
high-level status/roadmap.

## Current integration branch

- Repository: [NikNakk/monado](https://github.com/NikNakk/monado)
- Branch:
  [`macos-wine-openvr-legacy-unity`](https://github.com/NikNakk/monado/tree/macos-wine-openvr-legacy-unity)
- Canonical upstream remains the Monado project on freedesktop.org.

The branch name is historical. It now carries the broadest integration of the
native Apple Silicon runtime, PS VR2 HMD path, Metal/IOSurface sharing,
launchd/XPC service integration, depth support, Chromium handoff and Wine/OpenVR
work.

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
- Wine D3D11 OpenXR plus OpenVR experiments through OpenComposite and xrizer.

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

1. Make PS Sense optical 6DoF reliable enough to merge.
2. Turn existing PS VR2 camera acquisition into a real Monado passthrough/MR
   pipeline.
3. Solve the PS VR2 eye-tracking calibration path, then expose gaze/foveation.
4. Finish Chromium's sandboxed IOSurface/shared-event graphics path.
5. Broaden Wine/OpenVR compatibility and determine whether SteamVR Home can run
   without reproducing Valve's compositor.
6. Continue compositor pacing/reprojection robustness work.
7. Add broader OpenXR regression/conformance coverage.
8. Package and notarize the runtime with a simple settings/diagnostics surface.

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
- [BasaltVR](https://github.com/NikNakk/BasaltVR)
- [GAV PSVR2 Player for macOS](https://github.com/NikNakk/gav-psvr2-player-mac)
- [PSVR2Toolkit](https://github.com/NikNakk/PSVR2Toolkit)

## Detailed runtime documents

- [macOS direct service XPC](macos-service-direct-xpc.md)
- [PS VR2 timing diagnostics](macos-psvr2-timing-diagnostics.md)
- [PS VR2 judder evidence](macos-psvr2-judder-evidence.md)
- [Wine D3D11 OpenXR](macos-wine-openxr-d3d11.md)
- [Wine IOSurface import](macos-wine-iosurface-import.md)
- [Wine XR audio](macos-wine-xr-audio.md)

Older `macos-wine-*`, Metal-array, service-XPC, timing and presentation
branches should generally be considered development history unless a specific
experiment still references them.
