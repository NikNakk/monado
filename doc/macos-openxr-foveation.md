<!-- SPDX-License-Identifier: BSL-1.0 -->

# OpenXR foveation architecture on macOS

This document is the authoritative design note for foveated rendering in the
macOS Monado work.

The core rule is:

> **OpenXR owns foveation policy and eye-tracked semantics; graphics-specific
> code only exposes the native rendering mechanism needed to implement that
> policy.**

The implementation is currently split across draft branches/PRs and remains
disabled by default pending hardware validation.

## Standards layers

The design uses the registered OpenXR extensions for the public policy surface:

- `XR_FB_swapchain_update_state` — mutable swapchain state;
- `XR_FB_foveation` — foveation profiles and per-swapchain application;
- `XR_FB_foveation_configuration` — coarse strength, vertical offset and
  dynamic-level policy;
- `XR_META_foveation_eye_tracked` — runtime-owned eye-tracked foveation.

The runtime maps those structures onto the graphics-API-independent
`u_foveation` / `xrt_foveation_state` policy.

Metal does not currently have a registered graphics-specific companion
equivalent to `XR_FB_foveation_vulkan`. The experimental
`XR_MNDX_foveation_metal` extension therefore supplies only that missing
rendering transport.

It does **not** define a second foveation policy.

## Layering

```text
application / browser XR process
        |
        | XR_FB_foveation
        | XR_FB_foveation_configuration
        | XR_META_foveation_eye_tracked (optional)
        v
OpenXR state tracker
        |
        | resolved, graphics-neutral policy
        v
xrt_foveation_state
        |
        +----------------------+-----------------------+
        |                      |                       |
        v                      v                       v
     Metal                 Vulkan (future)        other backend
        |
        | XR_MNDX_foveation_metal
        | id<MTLRasterizationRateMap>
        v
application render pass
        |
        | compact physical-coordinate image
        v
ordinary OpenXR projection layer
        |
        v
Monado compositor
        |
        | exact matching 129-sample logical -> physical map
        | existing distortion/timewarp compute path
        v
display
```

The 129-sample mapping is an **internal Monado compositor detail**. It is not
part of the public Metal extension.

## Metal companion

The experimental Metal extension exposes:

```c
xrGetFoveationMetalStateMNDX(
    XrSwapchain swapchain,
    uint32_t viewIndex,
    uint32_t arrayLayer,
    XrFoveationMetalStateMNDX *state);
```

The returned state contains:

- whether foveation is active;
- a borrowed `id<MTLRasterizationRateMap>` represented as an opaque pointer;
- the physical render width/height;
- a revision.

`viewIndex` and `arrayLayer` are distinct deliberately. Two separate eye
swapchains may both use array layer zero.

A returned Metal map is runtime-owned. The application must not release it.
It is valid for future encoding until a later successful
`xrUpdateSwapchainFB` changes the revision, or until the swapchain is
destroyed. Applications should query again after updating foveation state.

The cache only changes revision when the actual rasterization pattern changes.
Eye movement within the same quantized Metal rate-map cell updates META's
reported NDC centre but reuses the existing immutable Metal map.

## Compositor reconstruction

A Metal application renders directly into the OpenXR swapchain with an
`MTLRasterizationRateMap`. Only the compact physical-coordinate region of the
logical texture contains the rendered image.

The same Metal cache retains the exact dense logical-to-physical transform used
to construct the rate map. At projection submission the OpenXR state tracker
copies that map into `xrt_foveation_map_data`.

The existing compute distortion/timewarp shader already understands this data,
so the compositor performs, in one pass:

1. timewarp;
2. optical/chromatic distortion;
3. logical-to-physical foveation remapping;
4. source sampling.

No separate application reconstruction pass is required.

## Runtime-owned eye tracking and privacy

`XR_META_foveation_eye_tracked` is intentionally independent of
`XR_EXT_eye_gaze_interaction`.

For eye-tracked foveation Monado:

1. acquires the runtime's eye-tracking device feature;
2. creates a private XRT gaze pose space;
3. locates that gaze relative to the runtime view space;
4. projects the gaze direction into each eye's actual asymmetric FOV;
5. stores only the resulting per-eye NDC foveation centres;
6. sends those centres to the graphics backend.

The application does not need an eye-gaze action and does not receive a gaze ray
through this path.

`xrGetFoveationEyeTrackedStateMETA` exposes only the standardized applied
foveation centres and validity state defined by the META extension.

For Chromium/WebXR, the intended privacy boundary is therefore:

```text
web page
   |
   | requests permitted foveated XR rendering
   v
Chromium XR policy / privileged process
   |
   v
OpenXR FB/META
   |
   | raw gaze remains runtime-private
   v
Metal rate map / compositor mapping
```

Opting into foveated rendering must not implicitly grant a page access to
`XR_EXT_eye_gaze_interaction`.

## PS VR2 gaze activation

The PS VR2 gaze USB interface is provisioned by default when available so
runtime-owned eye-tracked foveation does not require
`PSVR2_GAZE_STREAMS=1`.

Provisioning the interface does **not** enable the eye tracker.

The gaze transfer/calibration/control path starts lazily on the first
eye-tracking or face-tracking feature reference. Feature reference counting
keeps the tracker active while it is needed and disables it when the last user
releases it.

`PSVR2_GAZE_STREAMS=0` remains an explicit capability kill switch.

## Diagnostic modes

The native diagnostic contains two standard-path tests:

```sh
XR_RUNTIME_JSON="$PWD/build-wine/openxr_monado-dev.json" \
  ./build-wine/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
  --fb-foveation --foveation-profile aggressive
```

and:

```sh
XR_RUNTIME_JSON="$PWD/build-wine/openxr_monado-dev.json" \
  ./build-wine/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
  --fb-eye-foveation --foveation-profile aggressive
```

The eye-tracked mode deliberately does not enable
`XR_EXT_eye_gaze_interaction`.

The legacy `--gaze-foveation` and `--gaze-foveation-fused` modes remain
useful as reference implementations, but they should not be used as the basis
for new client integration.

## Profile mapping

The internal profiles remain finer-grained than the registered FB levels. The
diagnostic currently maps them as:

| internal profile | FB level |
| --- | --- |
| `reference` | LOW |
| `strong` | MEDIUM |
| `aggressive` | HIGH |
| `aggressive-plus` | HIGH |
| `near-extreme` | HIGH |
| `extreme` | HIGH |

The registered FB level is the public policy. The finer profiles are runtime
implementation choices and diagnostics.

## Depth

The standard foveation diagnostic currently rejects `--depth-layer`.

Color rendering and compositor reconstruction use the same compact-coordinate
mapping. Depth needs an equivalent audit/implementation before
`XR_KHR_composition_layer_depth` is combined with the new foveated swapchain
path.

## Feature gates

The current work remains default-OFF:

```text
XRT_FEATURE_OPENXR_FB_SWAPCHAIN_UPDATE_STATE
XRT_FEATURE_OPENXR_FB_FOVEATION
XRT_FEATURE_OPENXR_FB_FOVEATION_CONFIGURATION
XRT_FEATURE_OPENXR_META_FOVEATION_EYE_TRACKED
XRT_FEATURE_OPENXR_MNDX_FOVEATION_METAL
```

CI explicitly enables them so disabled code is still compiled and unit-tested.

## Branches and review split

The work is intentionally divided into two reviewable layers:

- `standards/fb-foveation` / PR #5 — registered FB/META control plane and
  graphics-neutral policy;
- `standards/fb-foveation-metal` / PR #6 — stacked experimental Metal
  transport, compositor reconstruction and runtime-owned PS VR2 eye tracking.

This separation should be preserved when upstreaming.

## Standardization direction

Do **not** generalize the experimental Metal companion into a second
cross-platform foveation API unless Khronos specifically requests that.

The existing registered FB extensions already provide the generic policy and
runtime-owned eye-tracked semantics. Metal is the missing graphics-specific
last mile, analogous to the role played by `XR_FB_foveation_vulkan`.

The intended incubation path is:

1. keep `XR_MNDX_foveation_metal` while the API is experimental;
2. validate fixed and eye-tracked rendering on hardware;
3. integrate the same mechanism into Chromium;
4. strip Monado-specific assumptions from the extension text;
5. propose a small multi-vendor Metal rendering companion, initially as an
   EXT-style extension;
6. consider broader generalization only if Khronos wants to replace or
   generalize the existing FB-family graphics companions.

The prospective standards description should be narrow:

> `XR_FB_foveation` remains authoritative for whether, how strongly and where
> foveation occurs. The Metal companion introduces no foveation policy. It only
> exposes the Metal rendering object required for an application using
> `XR_KHR_metal_enable` to render consistently with the runtime-selected
> foveation state.

## Validation status

The branch has macOS CI coverage with the normally-disabled FB, META and Metal
feature gates enabled. The current implementation has compile/unit coverage for:

- FB level/configuration parsing;
- META profile chaining;
- fixed vertical-offset to per-view NDC conversion;
- runtime gaze-direction to per-view NDC conversion;
- backend-neutral XRT policy resolution;
- Metal swapchain transport across vanilla, direct and service/XPC paths;
- compositor use of the matching dense map;
- the end-to-end diagnostic build.

Hardware validation remains the merge gate for the eye-tracked path.
