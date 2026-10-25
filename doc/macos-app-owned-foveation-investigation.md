<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# Application-owned gaze and foveation investigation, 2026-10-25

Source audit at `0daaffc4ac7c0784b368f1ee0bfbd35c3c367352`, with the existing
working-tree changes present. No runtime changes, builds, installations or
hardware runs were made for this investigation.

## Finding

**Scope correction after the native Alyx follow-up:** the initial investigation
covered this Monado checkout and Wine, but missed the existing native Alyx
FDM implementation. For native Alyx, the recommended next step is the OpenVR
centre / Vulkan offset route below, not a new Unity/Unreal prototype. The
generic engine comparison later in this note remains applicable to new native
Metal clients only.

Passing gaze to a game through a standard API is already supported:
`XR_EXT_eye_gaze_interaction`. The remaining work is in the game's rendering
backend, or a compatibility layer that controls its rendering. Gaze access by
itself does not implement foveated rendering.

Keep the existing FB/META + Metal route available while prototyping an
application-owned route. These serve different clients. The previous guidance
to prefer the runtime-owned path remains the recommendation for compact Metal
images submitted directly to Monado; it does not prohibit an application using
standard gaze input and submitting a fully reconstructed ordinary image.

“FDM” should be distinguished from the broader technique: Vulkan fragment
density maps, Direct3D variable-rate shading (VRS), and Metal variable-rate
rasterization (VRR) have different rendering and coordinate contracts.

## What the code already does

- `src/xrt/drivers/psvr2/psvr2.c` exposes
  `XRT_INPUT_GENERIC_EYE_GAZE_POSE` and the eye-gaze interaction binding.
- `src/xrt/state_trackers/oxr/oxr_system.c` reports gaze support;
  `oxr_space.c` reference-counts the eye-tracking feature when the public gaze
  action space is used.
- `create_gaze_resources` in
  `src/xrt/targets/psvr2_openxr_test/psvr2_openxr_test.mm` binds a pose action
  to `/user/eyes_ext/input/gaze_ext/pose` using
  `/interaction_profiles/ext/eye_gaze_interaction`.
- `locate_gaze_relative_to_view` synchronizes the action, checks activity and
  pose validity, and locates gaze against VIEW at predicted display time.
  This requests a pose at that time; gaze prediction quality still needs
  measurement. The public combined gaze pose is not independent per-eye gaze.
- `--gaze-foveation` already builds application-owned Metal maps, renders to
  compact intermediate images and reconstructs into ordinary OpenXR images.
  `encode_gaze_foveation_resolve` is the explicit reconstruction pass.
- `oxr_api_foveation.c` separately acquires private gaze for META foveation.
  `oxr_session_frame_end.c` retrieves the exact map bound to a released Metal
  image for compositor reconstruction. Passing gaze to the application cannot
  replace that image-coordinate contract.

The historical diagnostic measured about 0.45 ms saved in scene rendering and
1.54 ms spent reconstructing. Those are old, small-workload measurements, not
an engine benchmark. They explain why the compositor-fused path was retained;
do not repeat that experiment expecting a different conclusion without a
materially different workload or resolve implementation.

## Steam Frame and Windows comparison

Valve's published Unity foveation feature explicitly requests
`XR_FB_foveation`, `XR_FB_foveation_configuration`,
`XR_FB_foveation_vulkan`, `XR_FB_swapchain_update_state`,
`XR_META_foveation_eye_tracked` and `XR_META_vulkan_swapchain_create_info`.
It also exposes applied centres through `xrGetFoveationEyeTrackedStateMETA`.
This is evidence for an FB/META integration, not evidence that all Frame
games consume a public gaze action. The feature targets Android; it does not
establish the implementation used by every native or streamed Frame title.
Foveated streaming is a separate transport optimization and should not be
used as proof of game-side rendering savings.

Windows has multiple routes. D3D12 VRS Tier 2 lets a renderer provide a
screen-space shading-rate image. Coarse shading broadcasts results to pixel
groups in the ordinary render target. That differs from Metal VRR's compact
physical image, which requires coordinate reconstruction. Windows software
such as OpenXR Toolkit also supplies foveation through an API layer; it is
not a universal OpenXR capability that existing games automatically use.

Primary references checked on 2026-10-25:

- [Khronos public gaze extension](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_EXT_eye_gaze_interaction.html)
- [Valve Unity foveation feature source](https://github.com/ValveSoftware/Unity/blob/main/com.valvesoftware.openxr.utils/Runtime/Features/ValveOpenXRFoveatedRenderingFeature.cs)
- [Microsoft D3D12 VRS](https://learn.microsoft.com/en-us/windows/win32/direct3d12/vrs)
- [Apple Metal rasterization-rate-map explanation](https://developer.apple.com/videos/play/tech-talks/608/)
- [OpenXR Toolkit source and project description](https://github.com/mbucchia/OpenXR-Toolkit)

## Candidate routes

| Route | Public interface | Work remaining | Main constraint |
| --- | --- | --- | --- |
| Game-owned Metal VRR | EXT gaze + ordinary OpenXR swapchains | Engine owns maps and reconstructs before submission | Resolve and screen-space effects can consume the saving |
| Existing runtime-owned Metal VRR | FB/META + MNDX Metal | Integrate companion in a real engine | Experimental Metal transport; exact per-image map required |
| Vulkan FDM | FB/META + FB Vulkan companion | Real runtime backend and translator/device capability proof | Passing gaze or advertising extensions alone is insufficient |
| Game-owned VRS | EXT gaze + D3D/Vulkan shading-rate API | Renderer or API-layer support; translator capability proof on macOS | Cannot assume Metal VRR implements the same semantics |
| Foveated inset views | OpenXR stereo with foveated inset / VARJO ecosystem | Audit PS VR2 view generation, gaze-steered FOVs and composition | Requires a four-view-aware game; generic quad-view code is not end-to-end support |

For Wine, inspect the actual route before changing Monado. In the external
`~/Code/macos-wine-xr` checkout, the RPC `src/windows/openxr_thunk.cpp`
enumerates only `XR_KHR_D3D11_enable` and rejects other extensions: that route
cannot currently expose EXT gaze. The in-process Proton generator does not
explicitly blacklist EXT gaze, but this is not proof that the generated,
installed runtime forwards its properties, bindings and pose/sample-time
chains correctly. Neither route has been validated here for gaze. An OpenVR
application through xrizer or SteamVR needs a separate consumer/adapter audit.

## Native Alyx: existing FDM implementation and replacement path

Read-only follow-up inspected `~/Code/alyx-native/GAZE-FDM-IMPLEMENTATION-PLAN.md`,
the `workspace/xrizer-claude-gaze` and `workspace/kk-fdm` sources, the
`macos-alyx-arm64-static-audit.md` note in `workspace/monado-alyx-dual-vulkan/doc/`,
and the Stage 2b headset evidence in `workspace/monado-claude-custom-resolve/doc/`.
These are separate experimental trees; inspecting them does not establish which
binaries a particular current launcher/run loaded.

The implemented experimental gaze path is:

```text
Monado EXT gaze -> xrizer Input::update_fdm_gaze
  -> per-eye [0,1] projection -> private kk_fdm_set_gaze
  -> KK shifts separable Metal rate profiles derived from Alyx's density map
  -> Alyx's existing reconstruction -> ordinary OpenXR submission
```

`fdm_gaze.rs` locates the loaded KK library and resolves the private symbol.
`kk_fdm.c` shifts the highest-rate bands toward those gaze coordinates and
latches gaze at submission with a 10 ms interval. That heuristic is different
from faithfully applying application-specified density-map offsets. The Stage
2b ledger records an attended run where the user reported foveation following
gaze well; no new hardware result is claimed here.

The proposed application-directed route is:

```text
Monado EXT gaze -> xrizer OpenVR GetEyeTrackedFoveationCenter[ForProjection]
  -> Alyx chooses per-eye Vulkan FDM offsets
  -> KK implements the offset extension -> existing reconstruction/submission
```

The checked OpenVR header defines the two methods in projection-consistent NDC.
Both implementations in xrizer's `system.rs` currently return false. Existing
`fdm_gaze::project` returns clamped top-left-origin [0,1] coordinates, so it
cannot be forwarded unchanged. Supplied projections, asymmetric FOVs, coherent
two-eye sampling, invalid/stale gaze and action/session lifecycle need coverage.

The ARM64 renderer audit identifies `vr_fdm_offset`, `vr_fdm_offset_flip_y` and
`vr_fdm_offset_debug`, a runtime-facing centre query, per-eye scaling/rounding
to offset granularity, and a two-eye
`VkRenderPassFragmentDensityMapOffsetEndInfoEXT`. This is static evidence;
the live predicate, wrapper mapping and exact command entry still need tracing.
Do not assume Steam Frame identity spoofing is necessary.

In the checked KK FDM source, `kk_CmdEndRendering2KHR` marks its end-info
argument unused. Implementing the offset extension also requires its advertised
features/properties, correct density-fetch semantics and command coverage.
The main difficulty is that Vulkan supplies offsets at pass end while Metal
needs its rate map before draws are encoded. Deferred encoding or command
pre-scan/replay must preserve one-time buffers, suspend/resume chains, secondary
commands, ordering and GPU resource lifetime. The existing gaze latch is not
that contract. The private shift must be disabled in application-directed mode
to avoid applying gaze twice.

This route reuses Alyx's FDM renderer and existing reconstruction. The old
Monado diagnostic's extra resolve benchmark does not measure Alyx's path and
is not grounds to reject this migration. No new Monado foveation API is
presently indicated. Follow the existing Alyx implementation plan, first proving
the OpenVR ABI and signed per-eye Vulkan offsets in owned probes, then tracing
the complete game-issued chain with the private handoff disabled, and finally
performing user-operated headset quality/performance checks.

## Recommended next implementation

The following sequence is for new native Metal engine integration. Native Alyx
should instead follow the existing application-directed FDM plan above.

1. Choose one native engine scene, preferably the existing Unity/OpenBrush or
   Unreal port. Use the existing public gaze API; no new Monado extension is
   needed. Keep ordinary unfoveated rendering as the fallback.
2. Implement game-owned Metal VRR and reconstruction in that engine, using
   the diagnostic as a reference. Project the combined gaze into each actual
   asymmetric eye FOV at the same frame time, account for packed/array layouts
   and vertical flips, and handle invalid or stale gaze conservatively.
   Audit postprocessing, temporal history and depth in the engine's chosen
   coordinate system. Do not submit compact images as ordinary stereo images.
3. Compare off, fixed and gaze-directed rendering on the same heavy scene.
   Measure scene GPU time, reconstruction, complete application GPU time and
   compositor GPU time separately. Record physical pixel count, map-update
   cost, gaze age/alignment, quality and frame cadence. Hardware runs remain
   user-operated; start with service mode, then client-hosted/Game Mode.
4. Retain the application-owned implementation if total GPU time and image
   quality justify it. Otherwise integrate the existing Metal companion to
   eliminate the extra reconstruction pass. Keep browser gaze permissions
   separate: rendering foveation must not grant pages a public gaze ray.
5. For existing Windows games, first prove gaze forwarding in the selected
   Wine route and a real shading-rate implementation in its graphics
   translator. Only then choose a game or compatible API layer. This work
   belongs in `macos-wine-xr` and the translator, with Monado changes only
   where an actual runtime gap is demonstrated.

The investigation establishes feasibility and ownership boundaries. Engine
performance, Wine gaze forwarding, Vulkan capabilities and headset quality
remain untested. It does not change the build gates or recommend removing the
hardware-validated FB/META implementation.
