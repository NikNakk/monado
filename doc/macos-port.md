<!--
Copyright 2026, Collabora, Ltd.

SPDX-License-Identifier: BSL-1.0
-->

# macOS / PS VR2 Port Status

This document is the high-level status and roadmap for the experimental Monado
port to Apple Silicon macOS, with PS VR2 as the primary headset.

**Status date:** 2026-09-27

**Current integration branch:** `macos-wine-openvr-legacy-unity`

Despite its historical name, this is now the best single integration branch for
the macOS port. It contains most of the recent native macOS, PS VR2, compositor,
depth, service/XPC, Chromium-sharing and Wine/OpenVR work. PS Sense optical 6DoF
development deliberately remains on a separate branch until it is reliable
enough to merge.

This is development work, not an upstream-supported or packaged Monado target.

## Current state at a glance

| Area | Status | Notes |
| --- | --- | --- |
| Native Monado service/runtime on Apple Silicon | **Working** | `monado-service`, OpenXR runtime, Unix IPC and macOS launchd/XPC integration all run natively. |
| PS VR2 HMD discovery and 6DoF head tracking | **Working** | Uses the headset's own SLAM/IMU path. |
| PS VR2 display/compositor output | **Working** | Vulkan/MoltenVK distortion compositor with native Metal/CAMetalLayer final presentation. |
| Native OpenXR Metal clients | **Working** | Used by native samples and the engine/browser ports below. |
| Unity | **Working proof** | Open Brush is the main Unity validation application. |
| Unreal Engine | **Working proof** | Native Metal/OpenXR path exists in the UE fork. |
| Godot | **Working proof** | Native macOS OpenXR path exists in the Godot fork. |
| Chromium WebXR | **Working, still being hardened** | Immersive WebXR works; the sandboxed SharedImage/IOSurface/shared-event path is still active work. |
| Swift OpenXR wrapper | **Working** | SwiftXR provides the native Swift-facing layer used by shell experiments. |
| Swift VR home/shell | **Working experimental shell** | SwiftXRShell provides launcher/home, immersive video, desktop/panel support and system-overlay experiments. |
| PS Sense 3DoF, buttons and haptics | **Working experimental** | Native IOKit HID discovery/input is present on the integration branch. |
| PS Sense optical 6DoF | **In development on a separate branch** | Static/recorded optical results are encouraging, but dynamic tracking is not yet reliable enough to merge. |
| Depth layers | **Working experimental** | `XR_KHR_composition_layer_depth` is exposed for Metal and depth-aware positional reprojection is available. |
| Wine OpenXR / OpenVR | **Working experimental** | Native Monado remains the compositor/runtime; Windows D3D11 clients run through Wine/DXMT and OpenVR through OpenComposite or xrizer. |
| SteamVR games under Wine | **Working for a small tested set** | At least several SteamVR titles have reached runnable/interactive states; Half-Life: Alyx is the most heavily exercised path. |
| PS VR2 passthrough in Monado | **Working experimental path** | Stock-headset BC4 cameras are wired to `XR_FB_passthrough` on macOS using a GAV-derived initial fisheye projection; hardware validation/calibration refinement remains. |
| PS VR2 eye tracking | **Not yet usable** | The low-level stream can be reached, but the required calibration path is not solved. |
| SteamVR Home | **Unresolved** | Not currently working; feasibility depends on how much additional SteamVR/OpenVR behaviour can be reproduced without Valve's compositor. |
| End-user packaging | **Not done** | Development launchd installation exists, but there is no polished signed/notarized installer or settings application. |

## Native macOS architecture

The port keeps Monado's normal split between OpenXR clients and
`monado-service`.

```text
native OpenXR app / engine / browser XR process
                    |
                    | OpenXR + Monado IPC
                    v
             monado-service
                    |
                    | Vulkan compute/distortion compositor
                    | via MoltenVK
                    v
        IOSurface / Metal presentation bridge
                    |
                    v
           CAMetalLayer / PS VR2
```

The PS VR2 is discovered over USB through libusb. The HMD's own SLAM plus IMU
provides native head tracking. The display target finds the PS VR2 macOS display,
keeps Monado's Vulkan compositor, exports the completed compositor images through
Apple-compatible Metal/IOSurface mechanisms, and performs final presentation
through a native `CAMetalLayer`.

CAMetalDisplayLink drives the macOS presentation cadence. The branch also
contains the later presentation/pacing experiments, asynchronous presentation
support, Metal shared-event synchronization, XPC process-importance propagation
and latest-frame work. These have made the runtime substantially more usable,
but frame pacing and reprojection should still be treated as active engineering
areas rather than finished product behaviour.

See:

- [PS VR2 timing diagnostics](macos-psvr2-timing-diagnostics.md)
- [judder evidence and analysis](macos-psvr2-judder-evidence.md)
- [latest-frame worker](macos-psvr2-latest-frame-worker.md)
- [stale-frame substitution](macos-psvr2-stale-substitution.md)

## macOS graphics and process sharing

Several sharing paths coexist because clients have different process and
graphics constraints.

### Normal native Monado clients

The native macOS graphics-buffer abstraction uses Apple objects rather than
pretending Unix file descriptors are portable graphics handles. IOSurface-backed
swapchains can cross the ordinary Monado service/client boundary, and Metal
objects can be imported into the MoltenVK compositor through
`VK_EXT_metal_objects`.

Depth textures are a special case: Metal depth/stencil textures are not
IOSurface-backed, so the depth-swapchain path exports/imports the underlying
Metal texture object directly.

The integration branch now also prefers IOSurface-backed service-created Metal
swapchains when the requested descriptor permits it. That is particularly
important for clients such as Chromium which can then use standard IOSurface
sharing mechanisms.

### XPC Metal side channel

The normal Monado protocol remains on the Unix socket. XPC is a narrow macOS
side channel used for:

- launchd activation of `monado-service`;
- `MTLSharedTextureHandle` transfer where IOSurface is not the right carrier;
- `MTLSharedEventHandle` synchronization;
- process ownership/lifetime of shared Metal resources.

The service can be installed as an on-demand per-user LaunchAgent. See
[macOS direct service XPC activation](macos-service-direct-xpc.md).

### Chromium

Chromium adds another process boundary: the XR process owns the OpenXR session
while the GPU process owns the SharedImage/ANGLE rendering resources.

The Monado branch therefore exposes a small standalone helper,
`libmonado_metal_xpc_client.dylib`, supporting one-time claimable Metal texture
tokens and recreation on the receiving process's exact `MTLDevice`.

The preferred longer-term Chromium path is standard IOSurface sharing wherever
possible, with shared-event fence propagation for GPU synchronization. The
claimable-token helper remains available for cases that genuinely need an
explicit Metal handoff.

## OpenXR and compositor features

The macOS port now supports substantially more than the original bring-up:

- native OpenXR sessions through the normal loader/runtime path;
- native Metal graphics binding;
- service-backed swapchains;
- projection layers;
- cube composition layers used by newer WebXR Layers work;
- `XR_KHR_composition_layer_depth` for native Metal clients;
- experimental depth-aware positional reprojection in the compute compositor;
- multi-process Metal/IOSurface resource sharing;
- application GPU-completion waits before compositor reuse;
- launchd/XPC service activation and per-client Metal-resource ownership.

The native diagnostic target remains useful for runtime regression testing:

```sh
XR_RUNTIME_JSON="$PWD/build-macos-psvr2-display/openxr_monado-dev.json" \
  ./build-macos-psvr2-display/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test
```

Depth submission can be exercised with:

```sh
XR_RUNTIME_JSON="$PWD/build-macos-psvr2-display/openxr_monado-dev.json" \
  ./build-macos-psvr2-display/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test --depth-layer
```

The remaining OpenXR work is now mostly breadth, conformance and polish rather
than "can a native application render to the headset at all?"

## Application and engine ecosystem

The macOS Monado work spans several repositories. These links are part of the
port documentation deliberately: changes often have to be coordinated across
the runtime and a client/engine.

### Unity / Open Brush

- Repository: [NikNakk/open-brush](https://github.com/NikNakk/open-brush)
- macOS OpenXR branch:
  [`macos-openxr`](https://github.com/NikNakk/open-brush/tree/macos-openxr)

This is the main Unity proof-of-concept. The branch contains the macOS OpenXR
loader/startup work and macOS-specific mirror/performance adjustments needed to
run a real Unity XR application against Monado.

### Godot

- Repository: [NikNakk/godot](https://github.com/NikNakk/godot)
- active macOS XR branch:
  [`macos-xr-decoupled-present`](https://github.com/NikNakk/godot/tree/macos-xr-decoupled-present)

This is the Godot native OpenXR/macOS validation path, including work to avoid
letting the ordinary macOS window-present path dictate XR compositor cadence.

### Unreal Engine

- Repository: [NikNakk/UnrealEngine](https://github.com/NikNakk/UnrealEngine)
- active branch:
  [`macos-openxr-metal`](https://github.com/NikNakk/UnrealEngine/tree/macos-openxr-metal)

This contains the UE native Metal/OpenXR bridge, explicit OpenXR swapchain
lifecycle work and render-completion synchronization used for the macOS VR
validation build.

### Chromium / WebXR

- Repository: [NikNakk/chromium](https://github.com/NikNakk/chromium)
- active branch:
  [`macos-openxr-webxr`](https://github.com/NikNakk/chromium/tree/macos-openxr-webxr)

Chromium detects and uses the native macOS OpenXR runtime and can enter immersive
WebXR sessions. Current work is the production-quality SharedImage path:
IOSurface-backed resources, correct descriptor/layer sizes, sandbox-safe sharing
and Metal shared-event fences.

### SwiftXR

- Repository: [NikNakk/SwiftXR](https://github.com/NikNakk/SwiftXR)

SwiftXR is the Swift-facing OpenXR wrapper/support library and contains native
macOS UI/panel and desktop-interaction support used by the shell.

### SwiftXRShell

- Repository: [NikNakk/SwiftXRShell](https://github.com/NikNakk/SwiftXRShell)

SwiftXRShell is the experimental macOS VR home/launcher. Current work includes
immersive video, desktop/panel interaction, application launch/lifecycle and a
controller-accessible system overlay. It should remain a client of standard
OpenXR where possible; Monado-specific behaviour is reserved for functionality
that genuinely requires runtime integration.

### Wine / OpenVR compatibility

- OpenVR compatibility fork:
  [NikNakk/xrizer](https://github.com/NikNakk/xrizer)
- DXMT/Wine support and published development artifacts:
  [NikNakk/BasaltVR](https://github.com/NikNakk/BasaltVR)

The Monado integration branch contains the Wine-side build/provision/run scripts,
the Windows Monado OpenXR client, loopback IPC bridge, DXMT texture sharing,
OpenComposite helpers and xrizer launch paths.

### Native PS VR2 reference player

- Repository:
  [NikNakk/gav-psvr2-player-mac](https://github.com/NikNakk/gav-psvr2-player-mac)

This is not the Monado runtime, but it is a useful independent reference for
native PS VR2 behaviour on macOS. In particular it has demonstrated native
headset display/tracking, immersive video and raw front-camera passthrough.

### PS VR2 reverse-engineering reference

- Repository:
  [NikNakk/PSVR2Toolkit](https://github.com/NikNakk/PSVR2Toolkit)

This is a reference source for Sony/PS VR2 protocol work rather than the active
macOS runtime branch.

### Meta XR Simulator

Meta XR Simulator is an external compatibility target rather than a Monado fork.
It is useful for checking whether engine/client-side work remains portable across
OpenXR runtimes. Only the subset actually tested there should be described as
validated; Monado-specific PS VR2, XPC and Wine paths naturally do not apply.

## PS Sense controllers

### What is already on the integration branch

The current integration branch contains the native macOS IOKit HID path and the
basic PS Sense runtime integration needed for:

- controller discovery;
- orientation/3DoF tracking;
- buttons/analogue inputs;
- haptics;
- OpenXR interaction-profile experiments;
- HMD-relative synthetic position/arm-model experiments used while optical
  position is unavailable.

These are useful for application bring-up and have also enabled initial
Wine/OpenVR interaction testing.

### Optical 6DoF branch

Reliable positional tracking is intentionally being developed separately:

- branch:
  [`macos-pssense-6dof`](https://github.com/NikNakk/monado/tree/macos-pssense-6dof)
- optical tracking notes:
  [`doc/pssense-optical-tracking.md`](https://github.com/NikNakk/monado/blob/macos-pssense-6dof/doc/pssense-optical-tracking.md)
- camera calibration notes:
  [`doc/psvr2-camera-calibration.md`](https://github.com/NikNakk/monado/blob/macos-pssense-6dof/doc/psvr2-camera-calibration.md)

That branch has four-camera calibration, LED timing/bootstrap work, recorded
session tooling and multi-camera constellation solving. Static illumination and
pose experiments have reached strong results, but sustained dynamic tracking,
reacquisition, filtering and absolute controller-to-HMD consistency still need
to be made reliable. It should not be described as production 6DoF yet.

The intended merge gate is not merely "a pose can be solved"; recorded and live
sessions should show stable dynamic tracking, bounded latency/jitter, robust
loss/reacquisition and both-controller operation without special manual setup.

## PS VR2 camera / passthrough

The headset's camera transport is no longer completely unknown. Camera streams
are already used by the Sense tracking work, and the earlier calibration branch
is retained for that history:

- [`macos-psvr2-camera-calibration`](https://github.com/NikNakk/monado/tree/macos-psvr2-camera-calibration)

The independent native player also demonstrates that the front-camera image can
be acquired and displayed on macOS.

The integration branch now has an experimental runtime path from the stock-headset BC4 camera stream through `XR_FB_passthrough` to the final macOS Metal presentation stage. See [PS VR2 passthrough on macOS](macos-psvr2-passthrough.md).

What is still missing before this should be considered calibrated MR support:

- a stable camera/calibration API in the PS VR2 driver;
- distortion/rectification and pose/time alignment suitable for passthrough;
- compositor support for presenting passthrough behind/with application
  content;
- an OpenXR-facing passthrough/MR extension strategy;
- permission, lifecycle and failure handling.

So "camera frames can be obtained" and "Monado supports passthrough" are not yet
the same thing.

## Eye tracking and foveation

Eye-tracking data can be reached at the USB/protocol level, but usable gaze
tracking is still blocked by the headset's calibration path. The current work
does not yet provide a trustworthy OpenXR eye-gaze interaction source.

A complete implementation would need:

1. recover or reproduce the per-user/device calibration needed to turn the raw
   stream into valid gaze;
2. expose gaze through the appropriate OpenXR interaction extension(s);
3. validate timing and coordinate transforms;
4. only then add eye-tracked/foveated rendering support.

This remains a major missing PS VR2 capability.

## Wine, OpenVR and SteamVR applications

The architecture deliberately avoids running a second VR compositor under Wine.
The native arm64 Monado service owns the headset and presentation path.

```text
Windows VR application
       |
       +-- OpenXR --> PE Monado OpenXR client
       |
       +-- OpenVR --> OpenComposite or xrizer
                         |
                         v
                  Wine / D3D11 / DXMT
                         |
           IOSurface / shared Metal texture
                         |
                         v
                 native monado-service
                         |
                         v
                       PS VR2
```

The current branch includes:

- a PE x86-64 Monado OpenXR runtime;
- framed loopback IPC between the Wine client and native service;
- D3D11/DXMT IOSurface and shared-Metal swapchain paths;
- direct Metal array-texture transport for cases that need it;
- GPU shared-event/timeline synchronization plus diagnostic CPU fallback;
- OpenComposite provisioning and per-game helpers;
- xrizer provisioning/rebuild helpers;
- legacy Unity/OpenVR compatibility shims;
- timing capture and analysis tooling;
- controller-binding experiments;
- headset audio-routing helpers.

See:

- [Wine D3D11 OpenXR](macos-wine-openxr-d3d11.md)
- [Wine IOSurface import](macos-wine-iosurface-import.md)
- [Wine XR audio](macos-wine-xr-audio.md)

Half-Life: Alyx is the most demanding application exercised so far. Rendering,
menu/game input and multiple maps have been reached through the current
OpenComposite/xrizer experiments, but compatibility is still game-sensitive.
At least several SteamVR titles have been brought up; this is not yet a claim of
general SteamVR compatibility.

### SteamVR Home

SteamVR Home remains an explicit experiment rather than a supported feature.
The useful goal is to broaden OpenVR/SteamVR API compatibility until Home either
runs naturally or a concrete architectural blocker is identified. Recreating
Valve's full SteamVR compositor is not a goal: Monado should remain the headset
runtime/compositor.

## What is left

### Major capability gaps

1. **Reliable PS Sense 6DoF**
   - make the optical branch reliable in dynamic, two-controller sessions;
   - add robust IMU/optical fusion and reacquisition;
   - validate latency/jitter and then merge into the integration branch.

2. **Passthrough / mixed reality**
   - turn the existing camera acquisition/calibration work into a supported
     Monado camera/passthrough pipeline;
   - align camera timing and geometry with the HMD;
   - expose a usable OpenXR-facing API.

3. **Eye tracking and gaze-driven foveation**
   - solve the missing calibration path;
   - expose reliable eye-gaze input;
   - add/validate foveated rendering afterwards.

4. **SteamVR/OpenVR compatibility breadth**
   - reduce game-specific shims;
   - improve real controller mappings and OpenVR interfaces;
   - establish whether SteamVR Home is practical without Valve's compositor.

### Runtime quality and platform completeness

5. **Tracking-space UX**
   - reliable local-floor/stage behaviour;
   - recenter and persisted calibration;
   - eventually a boundary/guardian-equivalent strategy.

6. **Compositor/presentation robustness**
   - continue reducing pacing sensitivity and late-frame artefacts;
   - harden depth reprojection;
   - verify hot-plug, display-mode changes, sleep/wake and long sessions;
   - keep multi-client/resource ownership predictable.

7. **OpenXR breadth and conformance**
   - run the applicable Khronos conformance/CTS coverage;
   - increase layer/extension coverage where real clients require it;
   - add regression tests around acquire/wait/release, session transitions,
     swapchain lifetime and macOS IPC/graphics sharing.

8. **Remaining PS VR2 hardware integration**
   - expose useful headset/controller state such as battery/wear state where
     available;
   - cleanly integrate headset audio/microphone selection;
   - investigate headset haptics and richer Sense features where practical.

### Making it usable by other people

9. **Packaging and diagnostics**
   - signed/notarized runtime installation;
   - runtime registration and launchd setup without manual commands;
   - a small settings/diagnostics surface for device state, logs and recenter;
   - remove dependence on development-only environment-variable forests.

10. **CI, documentation and upstreamability**
    - keep macOS Monado CI green;
    - add targeted CI for the engine/browser/wrapper forks where practical;
    - maintain known-good cross-repository revisions;
    - split generally useful macOS/OpenXR work from PS VR2-specific changes so
      patches can be reviewed or upstreamed independently.

## Branch map

Use these as the current mental model rather than assuming every experimental
branch is an alternative complete port:

- **`macos-wine-openvr-legacy-unity`** — current integration branch and source
  of truth for the broad macOS runtime.
- **`macos-pssense-6dof`** — active Sense optical-position development; not
  yet merged because reliability is the gate.
- **`macos-psvr2-camera-calibration`** — earlier camera/calibration work that
  underpins the Sense branch.
- **`macos-depth-aware-reprojection`** — development history for depth-aware
  reprojection; the integration branch already contains the usable depth path.
- Older `macos-wine-*`, Metal-array, service-XPC, timing and presentation
  branches should be treated primarily as development history unless a specific
  experiment still refers to them.

The tracker in [macos-port-tracker.md](macos-port-tracker.md) is the shorter
branch-oriented companion to this document.
