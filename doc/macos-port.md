<!--
Copyright 2026, Collabora, Ltd.

SPDX-License-Identifier: BSL-1.0
-->

# macOS / PS VR2 Port Status

> **Visual regression resolved:** the user confirms the explicit macOS
> output-image barrier removes black noise with diagnostic readbacks off.
> Client-hosted GPU-reuse protection remains active. Keep both protections;
> rapid-rotation edge borders are a separate reported phenomenon. See the
> [recurrence record](macos-psvr2-timing-diagnostics.md#black-corruption-recurrence-after-cleanup--2026-10-03).


The [2026-10-03 inherited compositor audit](macos-inherited-compositor-audit.md)
summarises the validated pose/reuse fixes and the implemented pixel-readback,
non-macOS pose-selection and compatibility cleanup. This follow-up restores
upstream pose selection on every platform and removes default debug readbacks;
The output-barrier correction is headset-confirmed; Linux CI and residual-stall
work remain separate.

> **2026-10-03 target-pose correction:** the macOS compute renderer now retains
> freshly predicted scanout poses rather than overwriting them with recently
> submitted application poses. A low-cost UE preflight verifies all 609 compared
> renderer/query orientations match. The subsequent moving-head UE pair confirms
> regular judder is gone, while separate presentation stalls persist. Build,
> all 36 macOS CTests and 15 Python diagnostics pass. See the
> [fix and next workload pair](macos-psvr2-timing-diagnostics.md#fresh-scanout-target-fix--2026-10-03).

> **Local Metal reuse correction:** shared-event readiness and presentation
> waits are active, but the local swapchain path lacked GPU-read completion
> protection before returning source images to the app. It now enables the same
> timeline guard as service-owned Metal swapchains. Actual reuse-wait checks
> pass; the user now confirms this fixes the partial black frames/black noise.
> Regular judder and source-image corruption are resolved in the tested UE path;
> occasional stalls remain unresolved.
> See the [handoff evidence](macos-psvr2-timing-diagnostics.md#moving-head-confirmation-and-local-metal-image-reuse--2026-10-03).

> **2026-10-02 Wine hardware gate:** the experimental in-process runtime passed
> PS VR2 runtime-owned 2D/array image pixel checks and Opaque hello_xr. Hosted
> compositing initializes inside Wine, but currently runs at 60 Hz on the 120 Hz
> display. Visual confirmation and full-rate validation remain pending. See
> [the hardware evidence](macos-wine-in-process-endpoint.md#ps-vr2-hardware-gate-2026-10-02).

This document is the high-level status and roadmap for the experimental Monado
port to Apple Silicon macOS, with PS VR2 as the primary headset.

**Status date:** 2026-09-30

**Current integration branch:** `macos-upstream-clean`

**PS Sense 6DoF integration, 2026-10-04:** the joint multi-camera solver, EKF,
LED bootstrap, calibration/session tools and source evidence are now ported from
`~/Code/monado`, including its uncommitted runtime hookup. The opt-in path builds
with full-runtime optimisation and matches source recorded-session replay byte
for byte. Local tests pass. The first OpenXR controller trial reports the right
working well; the left remains invisible after optical/LED-lock loss and failed
reacquisition. Full hardware validation and Linux CI remain pending. See the
[integration and launch procedure](macos-pssense-6dof-integration.md).
Display-pacing benefits from optimisation remain to be measured independently.

The integration includes the
[failed CADisplayLink-owned compositor experiment record](macos-cadisplaylink-owned-compositor-experiment.md)
as required documentation. Its rejected execution modes remain removed;
ordinary CA pacing and client-hosted compositing remain the chosen architecture.
The record and detailed evidence accompany this integration.

The user confirmed this integration target on 2026-10-03, superseding the older
Wine-named and Game Mode sync targets. It contains the native macOS, PS VR2, compositor,
depth, passthrough, foveation, service/XPC, Chromium-sharing and Wine/OpenVR
work, plus the `standards/*` branches for `XR_KHR_generic_controller` and
`XR_FB_foveation` / `XR_META_foveation_eye_tracked`. PS Sense optical 6DoF
development continues separately, with an experimental opt-in runtime port
now available here for hardware validation.

Game Mode work (client-side compositing, below) is on
`claude/game-mode-priority-issue-xkx6m7`, which builds on the integration
branch and has not been merged into it yet.

This is development work, not an upstream-supported or packaged Monado target.

## Current state at a glance

| Area | Status | Notes |
| --- | --- | --- |
| Native Monado service/runtime on Apple Silicon | **Working** | `monado-service`, OpenXR runtime, Unix IPC and macOS launchd/XPC integration all run natively. |
| PS VR2 HMD discovery and 6DoF head tracking | **Working** | Uses the headset's own SLAM/IMU path. |
| PS VR2 display/compositor output | **Working** | Vulkan/MoltenVK distortion compositor with native Metal/CAMetalLayer final presentation. |
| Game Mode (fullscreen games) | **Working, opt-in** | macOS throttles `monado-service` under Game Mode. With `XRT_MACOS_CLIENT_COMPOSITOR=1` the client composites in its own process and the service hosts its layer; hardware-validated with Unreal at 120 Hz under Game Mode. Handoff between clients is implemented but not yet run on hardware. |
| Native OpenXR Metal clients | **Working** | Used by native samples and the engine/browser ports below. |
| Unity | **Working proof** | Open Brush is the main Unity validation application. |
| Unreal Engine | **Working proof** | Native Metal/OpenXR path exists in the UE fork. |
| Godot | **Working proof** | Native macOS OpenXR path exists in the Godot fork. |
| Chromium WebXR | **Working, still being hardened** | Immersive WebXR works; the sandboxed SharedImage/IOSurface/shared-event path is still active work. |
| Swift OpenXR wrapper | **Working** | SwiftXR provides the native Swift-facing layer used by shell experiments. |
| Swift VR home/shell | **Working experimental shell** | SwiftXRShell provides launcher/home, immersive video, desktop/panel support and system-overlay experiments. |
| PS Sense 3DoF, buttons and haptics | **Working experimental** | Native IOKit HID discovery/input is present on the integration branch. Sense also maps to `XR_KHR_generic_controller` (opt-in, `XRT_FEATURE_OPENXR_INTERACTION_KHR_GENERIC`). |
| PS Sense optical 6DoF | **Integrated, experimental opt-in** | Joint tracker + EKF; optimised build and replay/tests pass. OpenXR hardware validation pending. See the [launch procedure](macos-pssense-6dof-integration.md#build-and-run-the-opt-in-openxr-trial). |
| Depth layers | **Off by default** | `XR_KHR_composition_layer_depth` is not exposed on macOS unless configured with `-DXRT_FEATURE_OPENXR_LAYER_DEPTH=ON`; depth-aware reprojection additionally needs `XRT_COMPOSITOR_DEPTH_REPROJECTION=1`. Depth swapchain formats (including `Depth32Float_Stencil8`) are still creatable. |
| Wine OpenXR / OpenVR | **External compatibility project** | Wine/OpenVR integration has moved to [NikNakk/macos-wine-xr](https://github.com/NikNakk/macos-wine-xr); Monado retains only generic macOS/Metal runtime and resource-handoff support. |
| SteamVR games under Wine | **External experimental path** | Game compatibility and launch policy are tracked in [NikNakk/macos-wine-xr](https://github.com/NikNakk/macos-wine-xr) and the relevant OpenVR compatibility projects. |
| PS VR2 passthrough in Monado | **Working experimental path** | Stock-headset BC4 cameras are wired to `XR_FB_passthrough` on macOS using a GAV-derived initial fisheye projection; hardware validation/calibration refinement remains. |
| PS VR2 eye tracking | **Working experimental** | `XR_EXT_eye_gaze_interaction` using the Sony calibration blob plus an optional 9-point user calibration; gaze activates lazily. Accuracy still needs broader hardware validation. |
| Foveated rendering | **Hardware-validated, opt-in at build time** | Fixed `XR_FB_foveation` / `XR_FB_foveation_configuration` works without gaze; `XR_META_foveation_eye_tracked` adds runtime-owned gaze. Validated on PS VR2 through `monado-service`. Metal is the only rendering backend, via the experimental `XR_MNDX_foveation_metal` companion. |
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

CADisplayLink drives compositor pacing by default, using actual refresh
timestamps and the measured period. Corrected 2026-10-03 headset captures
closely match CV; the heavy Unreal/Game Mode pair favours CA in completion
cadence. `XRT_MACOS_DISPLAY_LINK=cv` retains the legacy fallback, also selected
automatically if CA is unavailable. Fully buffered hosted UE repeats physically favour CA under heavy load
(105.18 vs 100.08 Hz); 90 Hz and subjective coverage remain pending. See [the default decision](macos-psvr2-timing-diagnostics.md#cadisplaylink-promoted-to-default--2026-10-03).
Presentation is
asynchronous: a newest-frame worker acquires drawables off the compositor
thread and presents with a minimum duration of 8 ms, and the compositor thread
runs under a Mach time constraint. GPU hand-off uses Metal shared events. The
CAMetalDisplayLink driven and hybrid modes, stale-frame substitution and most
other pacing experiments tested worse and have been removed; see
[the toggle inventory](macos-env-toggles.md). Frame pacing and reprojection
are still active engineering areas rather than finished product behaviour.

See:

- [PS VR2 timing diagnostics](macos-psvr2-timing-diagnostics.md) (current
  defaults at the top)
- [judder evidence and analysis](macos-psvr2-judder-evidence.md)
- [latest-frame worker](macos-psvr2-latest-frame-worker.md) and
  [stale-frame substitution](macos-psvr2-stale-substitution.md) (history)

### Game Mode and client-side compositing

When a fullscreen game has Game Mode, macOS backgrounds `monado-service` from
outside the process: every service thread drops to priority 4 on the E-cores,
and the headset compositor falls to 13–18 fps. XPC importance and launchd
`ProcessType` cannot undo this; an importance lease was tried and removed.

The fix moves the compositor into the game's process. With
`XRT_MACOS_CLIENT_COMPOSITOR=1` in the application's environment, the IPC
client creates Monado's main compositor in-process, presents into a
`CAMetalLayer` inside a `CAContext`, and the service shows that context on the
headset window through a `CALayerHost`. Tracking stays in the service: phase 1
measurements showed PS VR2 USB delivery is essentially unaffected by Game Mode.
The distortion mesh, a distortion grid and passthrough camera frames are
copied from the service once or through shared memory, so no per-frame IPC
round trip is needed except pose queries.

```text
game process (Game Mode favours it)          monado-service (throttled)
-----------------------------------          --------------------------
OpenXR state tracker                          PS VR2 driver: USB, SLAM, IMU
main compositor (distortion/timewarp) <-poses- IPC head device
CAMetalLayer in a CAContext  --------------->  CALayerHost on the headset window
```

With Unreal under Game Mode, the in-process compositor held 120 Hz (interval
p50/p99 8.34/8.6 ms) against 62/107 ms through the service. See
[the client compositor design](macos-client-compositor-design.md) for the
phases, handoff protocol and test procedure, and
[remote layer hosting](macos-remote-layer-hosting.md) for the probe
measurements behind it.

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
- IOSurface transfer for native clients' swapchains. The client creates the
  surfaces and publishes them to the service under a token only its own PID
  can redeem, so they travel as Mach ports and are not global. (Surfaces sent
  by ID must be `kIOSurfaceIsGlobal`, which lets any process open them; new
  native clients should prefer the PID-scoped XPC/Mach-port path.);
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
- `XR_KHR_composition_layer_depth` for native Metal clients (opt-in at
  configure time; off by default on macOS);
- `XR_FB_passthrough` layers rendered by the final Metal presentation pass;
- `XR_EXT_eye_gaze_interaction` on PS VR2;
- opt-in `XR_FB_foveation`, `XR_FB_foveation_configuration`,
  `XR_META_foveation_eye_tracked` and experimental `XR_MNDX_foveation_metal`;
- opt-in `XR_KHR_generic_controller`, mapped for PS Sense and for Touch-family
  and Index devices, and preferred over `simple_controller` as a fallback;
- experimental depth-aware positional reprojection in the compute compositor;
- multi-process Metal/IOSurface resource sharing;
- application GPU-completion waits before compositor reuse;
- launchd/XPC service activation and per-client Metal-resource ownership;
- opt-in in-process compositing for clients, hosted by the service's headset
  window (Game Mode). Hosted bindings now have explicit ownership, with
  read-only passthrough descriptors and synchronous teardown hiding; the
  refactor and handoff still await macOS/on-headset validation (see
  [the design note](macos-client-compositor-design.md#ownership-and-teardown-hardening-2026-10-01)).

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

Environment blending can be compared with `--blendmode Opaque` (the default)
and `--blendmode AlphaBlend`. The app logs the runtime's advertised primary-stereo
blend modes and rejects an unsupported selection before creating a session.
AlphaBlend clears the projection background transparent and enables source-alpha
composition. It does not require `XR_FB_passthrough`; that extension is tested
separately by `--passthrough`. `XR_RUNTIME_JSON` can select Meta XR Simulator for
a native Metal reproduction without Wine/DXMT or a connected PS VR2. This mode
has not yet been validated on hardware.

On 2026-10-02, the user confirmed that the native test app ran against Meta XR
Simulator with `--blendmode AlphaBlend`, without `--passthrough`, and did not
crash. The tested source was `7fd7f2835693d447d46da933e9a54c9f71ddfae9` plus the
local blend-mode selection changes. Run duration and Metal validation settings
were not recorded. This does not reproduce the Wine `hello_xr` assertion through
the generic bridge; compare swapchain configuration and frame/layer submission
before attributing that failure to general runtime alpha-blend support.

Controlled repeats later that day isolated Metal validation: native Opaque
survived 6 seconds with `MTL_DEBUG_LAYER=1` and exited cleanly on SIGINT; native
AlphaBlend aborted with the same IOSurface shared/managed-storage assertion.
With `MTL_DEBUG_LAYER` unset, native AlphaBlend survived 8 seconds and exited
cleanly on SIGINT. Both runs used Meta XR Simulator 207.0.0 on Apple M5, the
same source above, and no PS VR2 hardware. This gives a native reproduction of
the assertion independent of Wine/DXMT. The bridge comparison also corrected an
invalid shared-event device check; bridged hello_xr then completed 348 alpha
frames without validation. See the
[external evidence ledger](https://github.com/NikNakk/macos-wine-xr/blob/main/docs/native-openxr-backend.md)
for swapchain/layer differences and the local follow-up results. The correction
and evidence updates are local pending publication.

The same target also has `--passthrough` / `--passthrough-only`,
`--generic-controller` (cyan left/orange right grip cubes and aim rays; grey
when valid but untracked, hidden when invalid), `--gaze` / `--gaze-calibrate`, and `--fb-foveation` /
`--fb-eye-foveation` modes for the corresponding extensions. The generic
controller and foveation modes need their opt-in CMake features.

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

### External Wine / OpenVR compatibility

The external bridge now has a runtime-neutral native Khronos-loader host and
thin Win64 OpenXR thunk. Its simulated-HMD test on `macos-upstream-clean`
wrapped normal runtime-owned Metal images directly in DXMT (zero bridge
copies). This adds no Windows transport/state tracker to Monado. See the
external project's `docs/native-openxr-backend.md` for evidence and limits;
physical PS VR2 pacing and game regression testing remain pending.

- Wine/XR bridge and compatibility tooling:
  [NikNakk/macos-wine-xr](https://github.com/NikNakk/macos-wine-xr)
- OpenVR compatibility fork:
  [NikNakk/xrizer](https://github.com/NikNakk/xrizer)
- Earlier streaming/interoperability experiments:
  [NikNakk/BasaltVR](https://github.com/NikNakk/BasaltVR)

Wine-specific D3D clients, launch/provisioning scripts, transport policy and
game compatibility shims no longer live in the Monado tree. The external bridge
consumes Monado's generic macOS/Metal handoff mechanisms instead.

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
- OpenXR interaction-profile experiments, including an opt-in
  `XR_KHR_generic_controller` mapping (L1/R1 drives squeeze/value, and
  grip_surface prefers a calibrated palm pose);
- HMD-relative synthetic position/arm-model experiments used while optical
  position is unavailable.

These are useful for application bring-up and have also enabled initial
Wine/OpenVR interaction testing.

### Optical 6DoF branch

Tracking development and diagnostic evidence originated on a separate branch;
the current joint + EKF path is now ported here behind `PSVR2_SENSE_6DOF=1`.
Use the [current integration procedure](macos-pssense-6dof-integration.md) for
runtime testing. Historical branch references:

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

That path already covers the OpenXR API (`XR_FB_passthrough` create/start/
layer/resume), carrying passthrough layer state through IPC and the multi
compositor, and compositing the camera image behind application content in the
final Metal presentation pass. It uses a tunable equidistant-fisheye
approximation rather than real camera calibration.

What is still missing before this should be considered calibrated MR support:

- a stable camera/calibration API in the PS VR2 driver, using real camera
  intrinsics/extrinsics instead of the fisheye/convergence approximation;
- rectification plus hardware timestamps and head-pose alignment for camera
  frames, and camera reprojection/late correction;
- passthrough style/colour-map controls, projected passthrough and
  depth-aware MR occlusion;
- robust stream restart when camera delivery stalls;
- permission and shell UX (for example the HMD function-button toggle).

## Eye tracking and foveation

The macOS PS VR2 driver now has a usable eye-gaze path and an experimental
standards-facing foveation stack. The two are independent: foveation does not
need eye tracking, and eye tracking does not depend on foveation.

Public gaze input is exposed through `XR_EXT_eye_gaze_interaction`, using the
Sony calibration blob plus an optional 9-point user calibration.

Foveation uses the registered `XR_FB_foveation` and
`XR_FB_foveation_configuration` semantics, with `XR_META_foveation_eye_tracked`
as an optional extra:

- **Fixed foveation** (`XR_FB_foveation` levels, dynamic flag and vertical
  offset) is resolved into per-view centres from the session's view FOVs and
  works with no gaze at all.
- **Eye-tracked foveation** (`XR_META_foveation_eye_tracked`) is runtime-owned:
  Monado locates a private gaze space and projects it into per-view centres.
  The application never receives the gaze ray.

The coarse FB levels map onto graphics-API-independent `u_foveation` profiles,
and the resolved policy is carried through `xrt_foveation_state`.
Metal is the first rendering backend. Because OpenXR currently has no registered
Metal foveation companion equivalent to `XR_FB_foveation_vulkan`, the branch
uses the experimental `XR_MNDX_foveation_metal` extension only to expose the
runtime-selected `MTLRasterizationRateMap` and physical render size to a Metal
client.

The compositor receives the exact matching dense logical-to-physical map from
the swapchain cache and reuses the existing distortion/timewarp remapper, so no
separate full-resolution reconstruction pass is required.

PS VR2 gaze capability is provisioned by default but activated lazily through
feature reference counting. Eye-tracked foveation does not implicitly grant an
application `XR_EXT_eye_gaze_interaction` access.

See:

- [OpenXR foveation architecture on macOS](macos-openxr-foveation.md)
- [PS VR2 eye gaze](macos-psvr2-eye-gaze.md)
- [PS VR2 gaze-driven foveation](macos-psvr2-gaze-foveation.md)

The standard FB and FB + META paths have been validated on PS VR2 hardware
through `monado-service`. The implementation remains opt-in at build time
(`XRT_FEATURE_OPENXR_FB_FOVEATION` and related options, default-OFF) pending
standards/upstream review of the Metal companion. Metal is the only backend that implements swapchain
foveation; Vulkan, D3D and OpenGL clients get no foveation yet. Compact depth
coordinates also need to be audited before depth submission is combined with
the new foveated swapchain path.

## External Wine, OpenVR and SteamVR applications

Wine/OpenVR compatibility is developed outside Monado in
[NikNakk/macos-wine-xr](https://github.com/NikNakk/macos-wine-xr).
The native arm64 Monado service remains the headset runtime/compositor, while
the external project owns the Windows/Wine graphics binding, transport,
OpenComposite/xrizer integration, launch tooling and game-specific policy.

```text
Windows VR application
       |
    Wine / D3D11
       |
  macos-wine-xr
       |
 generic macOS Metal resource handoff
       |
 native monado-service
       |
     PS VR2
```

This separation is deliberate: Monado should understand Metal textures,
IOSurfaces and shared synchronization primitives, but should not contain
translation-layer-specific or game-specific behavior.

SteamVR Home remains an experiment in the external compatibility layer rather
than a Monado runtime target. Recreating Valve's compositor is not a goal.

## What is left

### Major capability gaps

1. **Reliable PS Sense 6DoF**
   - validate the integrated joint + EKF path in dynamic, two-controller OpenXR sessions;
   - confirm world alignment, reacquisition, shutdown and validity expiry on hardware;
   - measure latency/jitter and display load before considering default enablement.

2. **Passthrough / mixed reality**
   - replace the fisheye approximation with calibrated camera geometry;
   - align camera timing and pose with the HMD and add camera reprojection;
   - add projected passthrough, style controls and depth-aware occlusion.

3. **Eye tracking and foveation**
   - hardware-validate gaze accuracy and the fixed FB and eye-tracked META
     foveation paths;
   - add a foveation backend for non-Metal clients (for example
     `XR_FB_foveation_vulkan`);
   - validate lazy/private gaze activation and lifetime;
   - integrate the standards-facing path into Chromium;
   - audit compact depth coordinates;
   - refine and upstream the Metal rendering companion.

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
   - finish client-side compositing: hardware-validate handoff between hosted
     clients, pause hidden presenters, handle overlays, Wine and Chromium, then
     make it the default;
   - move pose queries to a shared-memory ring;
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
    - keep the macOS and Linux Monado CI builds green;
    - add targeted CI for the engine/browser/wrapper forks where practical;
    - maintain known-good cross-repository revisions;
    - split generally useful macOS/OpenXR work from PS VR2-specific changes so
      patches can be reviewed or upstreamed independently.

## Branch map

Use these as the current mental model rather than assuming every experimental
branch is an alternative complete port:

- **`macos-wine-openvr-legacy-unity`** — current integration branch and source
  of truth for the broad macOS runtime.
- **`claude/game-mode-priority-issue-xkx6m7`** — client-side compositing for
  Game Mode, on top of the integration branch; to be merged into it once
  handoff is validated on hardware.
- **`macos-pssense-6dof`** / **`claude/pssense-mr2940-evaluation`** — Sense
  optical development and evaluation history. The latest joint + EKF path is
  now ported here for opt-in hardware testing; default enablement remains gated.
- **`macos-psvr2-camera-calibration`** — earlier camera/calibration work that
  underpins the Sense branch.
- **`macos-depth-aware-reprojection`** — development history for depth-aware
  reprojection; the integration branch already contains the usable depth path.
- **`standards/khr-generic-controller`**, **`standards/fb-foveation`** and
  **`standards/fb-foveation-metal`** — standards-facing slices, now merged into
  the integration branch; kept as smaller review units for upstreaming.
- Older `macos-wine-*`, Metal-array, service-XPC, timing and presentation
  branches should be treated primarily as development history unless a specific
  experiment still refers to them.

The tracker in [macos-port-tracker.md](macos-port-tracker.md) is the shorter
branch-oriented companion to this document.

## Upstream sync

On 2026-10-03, `macos-upstream-clean` merged GitLab upstream `main` through
`22d5c936c` (2026-10-02), preserving existing commit history. The ten new
commits add configurable remote-HMD resolution and null-compositor frame rate,
use the HMD's per-view resolution in the null compositor, correct SteamVR
lighthouse pose-time units, and add runtime checks and static-analysis fixes.
The macOS `.build/native-service-check` build succeeded and all 35 CTest suites
passed (three IPC/layer suites required a rerun outside the sandbox). Headset
validation and Linux CI for this follow-up remain pending.

The integration branch was forked from upstream at `ac3f23f71` (2026-03-31).
Upstream `main` up to `045931d12` (2026-09-30) has been merged on
`macos-upstream-sync-2026-10`; it builds and passes tests on Linux, and
awaits macOS CI and a headset run before it replaces the integration branch.
Server shutdown retains the three-second graceful-exit window, then stops all
client loops and cancels/disconnects Windows pipe I/O before joining threads.
Joined slots become `READY`, so teardown cannot join them twice. A delayed
client startup cannot override a shutdown request. Regression tests cover
blocked readers, repeated shutdown and threads that have not started yet; the
Windows test also exercises a real synchronous named pipe.

Points that change behaviour or need checking on hardware:

- **PS Sense** now uses upstream's rewritten driver: constellation (optical)
  tracking support, USB, clock sync, factory IMU calibration, corrected
  grip/aim poses and a Touch-controller binding profile. Our generic, simple
  and optional Index profiles remain, and synthetic position and the arm
  model still apply when constellation tracking is not in use (the PS VR2
  builder does not add the controllers to a constellation tracker). The
  driver now requires Ceres: `brew install ceres-solver`, otherwise
  `XRT_BUILD_DRIVER_PSSENSE` is silently off. Check 3DoF orientation, the
  grip/aim offsets and the synthetic modes on hardware.
- **Compositor**: the shared render code moved to `auxiliary/render` with a
  pipeline cache. Depth reprojection, Metal foveation and the identity
  distortion bypass were carried over; the layer shader also gained
  upstream's chroma key, inset blending and frustum rejection. Check depth
  reprojection and foveated layers on hardware. The layer UBO is 63,536
  bytes, close to the 64 KiB limit, so a static assert guards it.
- **PS VR2 prediction**: dead reckoning now reports failure instead of
  asserting; the driver falls back to the latest SLAM pose. With our IMU
  FIFO index fix this is not expected in normal running.
- `macos-pssense-6dof` keeps its own optical frontend (M1/M2/M3) and IMU EKF.
  An offline comparison against upstream's sliding-window fusion (MR 3015,
  not yet merged upstream) found the same pose coverage and similar accuracy:
  upstream was modestly better on some consistency metrics, at about 300x the
  cost and with a Ceres dependency. Upstream's Ceres frontend (MR 2940, in
  this merge) was not benchmarked. See
  [the evaluation](https://github.com/NikNakk/monado/blob/codex/pssense-upstream-fusion-evaluation/doc/macos-pssense-upstream-fusion-evaluation.md).
  The 2026-10-04 selective port now brings its joint frontend and EKF forward
  onto the newer tracker API, preserving the Ceres per-camera fallback. See
  [the completed integration](macos-pssense-6dof-integration.md).


### PS VR2 eye-gaze calibration tuning

The PS VR2 eye tracker can use the standard `XR_EXT_eye_gaze_interaction` path on macOS.
The gaze interface is provisioned by default and activates lazily when a tracking feature is
requested; `PSVR2_GAZE_STREAMS=1` is no longer required. The driver loads the Sony calibration blob from
`~/Library/Application Support/monado/psvr2/eye_calibration.bin` when present.

Small residual user-specific calibration errors can be corrected without modifying that blob:

```sh
export PSVR2_GAZE_YAW_OFFSET_DEG=0
export PSVR2_GAZE_PITCH_OFFSET_DEG=0
export PSVR2_GAZE_YAW_GAIN=1.0
export PSVR2_GAZE_PITCH_GAIN=1.0
```

Use offsets for a roughly constant displacement across the field of view. Use gains only when the
centre is approximately correct but error grows toward the edges. The gain range is intentionally
clamped to 0.5–1.5.

### IPC security and contribution follow-up

Native macOS socket clients now have a kernel-verified UID/PID separate from
application metadata. Socket lifetime locking preserves live endpoints and
recovers stale sockets. XPC pending-resource quotas and 60-second expiry apply
to both the direct service and standalone probe; the ownership-bypassing
runtime broker override is retired. Compatibility-bridge authentication and
transport policy are maintained in the external macOS Wine XR repository. See [the XPC ownership note](macos-service-direct-xpc.md#per-client-ownership-and-pending-resource-limits).

CI now checks formatting, spelling and REUSE metadata, builds both upstream
Android ABIs, and runs the macOS default/all-feature configurations on macOS 14
and 15. Native macOS tests exercise the actual XPC registry and private-layer
context/host lifecycle. These checks do not replace headset or Wine application
validation. [The upstream contribution preparation](macos-upstream-contribution.md)
contains the proposed review units and draft changelog text; human DCO
certification remains a prerequisite for upstream submission.


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
