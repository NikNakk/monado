<!-- SPDX-License-Identifier: BSL-1.0 -->

# OpenXR foveation architecture on macOS

This document is the authoritative design note for foveated rendering in the
macOS Monado work.

The core rule is:

> **OpenXR owns foveation policy and eye-tracked semantics; graphics-specific
> code only exposes the native rendering mechanism needed to implement that
> policy.**

The standards path has been validated on PS VR2 hardware through
`monado-service` on native macOS (see [Validation status](#validation-status)).
It remains **opt-in at build time**: the feature gates below are default-OFF.
That is a build-configuration choice, not a statement that the path is
unvalidated.

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
It is valid for future encoding at least until a later successful
`xrUpdateSwapchainFB` changes the revision, or until the swapchain is
destroyed. Applications should query again after updating foveation state.

Version 2 also supports packed multi-view render targets. A client chains
`XrFoveationMetalPackedStateMNDX` to the query and supplies the OpenXR view
index plus `imageRect` for each view sharing an array layer. The runtime then
builds one Metal rate map over the whole packed target. The chained structure
returns the exact horizontal/vertical Metal descriptor samples and dense
logical-to-physical mapping. This is specifically useful to multi-process
clients such as Chromium: the XR process can publish the small numeric recipe
while the GPU process reconstructs an equivalent `MTLRasterizationRateMap`
on the same Metal device.

### Per-image map association

The compositor must sample each submitted image with exactly the map that image
was rendered with. That is not necessarily the current map: an application may
re-submit a previously released image without rendering it again (sparse or
repeated frames, video playback) while eye tracking keeps moving the runtime
state on.

Each Metal swapchain therefore keeps a cache of immutable, reference-counted
map entries with three kinds of owner:

- a lookup list for the current revision (bounded, so entries for one
  revision are reused rather than rebuilt);
- a per-array-layer **selection**: exactly the map most recently returned to
  the application by `xrGetFoveationMetalStateMNDX` for that layer;
- a per-**(image index, array layer)** **binding**.

The concrete swapchain's `release_image` binds every layer's selection to the
released image after the native release succeeds. At `xrEndFrame` the state
tracker asks for the binding of `sc->released.index` and the submitted array
layer; it does not consult the current FB state or current gaze. The same code
runs for vanilla, direct in-process and service/XPC Metal swapchains, because
`xrEndFrame` runs in the client process in every mode.

Consequences:

- image 0 rendered with map A, state changes to B, image 0 re-submitted:
  the compositor samples it with A;
- image 0 rendered again after querying C: its binding becomes C;
- a disabled or failed query, or disabling foveation with
  `xrUpdateSwapchainFB`, clears the selection, so images released afterwards
  are sampled unfoveated; images released earlier keep their map;
- an image never released with a selected map is sampled unfoveated;
- the selection survives state changes until the application queries again,
  so an application using a fixed map only queries once and every released
  image still carries it;
- entries are freed when the last owner drops them; swapchain destruction
  releases every owner.

Query the map before releasing the image it applies to. An application must
not query the map for a later image while an earlier image rendered with a
different map is still waited.

`tests/tests_metal_foveation_cache.mm` covers these cases, the packed layouts
below and the equivalence of a map rebuilt from the transported samples.
`OXR_DEBUG_FOVEATION_BINDING=1` logs the revision and physical extent that
`xrEndFrame` attaches to each submitted image.

### Packed views

Each view's centre is placed at `imageRect.offset + local * imageRect.extent`
in the packed target and the profile extents are converted back to the view's
own normalized extent, so the left eye of a side-by-side target is not given
twice the intended extent. Offsets, unequal sizes, non-zero Y offsets and any
view order are supported. Because Metal rate maps are separable, each sample
row/column takes the maximum rate over all views; a centre can therefore raise
quality slightly in a neighbouring view but never lower it.

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
so for a single projection layer the compositor performs, in one pass:

1. timewarp;
2. optical/chromatic distortion;
3. logical-to-physical foveation remapping;
4. source sampling.

No separate application reconstruction pass is required.

Frames that need the layer squasher (more than one layer, passthrough, colour
bias/scale) apply the same reconstruction in `layer.comp` from a small per-run
foveation UBO holding up to eight maps per view. A projection layer carrying a
map that cannot be applied is not composited rather than being sampled with the
wrong transform. The graphics (non-compute) compositor path cannot reconstruct
foveated images and logs an error once if it sees one; compute is the default
on macOS.

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

`--fb-foveation-sparse-check` (with either mode) alternates rendered frames with
frames that switch to a second FB level, query its map, and re-submit the
previous images unrendered. Run it with `OXR_DEBUG_FOVEATION_BINDING=1` to see
that `xrEndFrame` keeps submitting the render revision. Add `--passthrough` to
force the layer-squasher path.

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

The extensions remain opt-in at build time (default-OFF) so that ordinary,
Unity/OpenVR and Wine builds of this branch do not advertise experimental
extensions. A development build that uses them needs:

```sh
cmake -S . -B build -G Ninja \
  -DXRT_FEATURE_OPENXR_FB_SWAPCHAIN_UPDATE_STATE=ON \
  -DXRT_FEATURE_OPENXR_FB_FOVEATION=ON \
  -DXRT_FEATURE_OPENXR_FB_FOVEATION_CONFIGURATION=ON \
  -DXRT_FEATURE_OPENXR_META_FOVEATION_EYE_TRACKED=ON \
  -DXRT_FEATURE_OPENXR_MNDX_FOVEATION_METAL=ON
```

`XRT_FEATURE_SERVICE` defaults ON on macOS; add `-DXRT_FEATURE_SERVICE=OFF` for
the in-process (direct Metal swapchain) runtime.

CI enables the gates in both an in-process and a service build so the code is
compiled and unit-tested even though it is off by default.

## Branches and review split

The work was developed as two reviewable layers:

- `standards/fb-foveation` / PR #5 — registered FB/META control plane and
  graphics-neutral policy;
- `standards/fb-foveation-metal` / PR #6 — stacked experimental Metal
  transport, compositor reconstruction and runtime-owned PS VR2 eye tracking.

Both are integrated into `macos-wine-openvr-legacy-unity`, which is now the
authoritative combined macOS/PS VR2 branch; continue development there. The
layer separation should still be preserved when upstreaming.

## Standardization direction

Do **not** generalize the experimental Metal companion into a second
cross-platform foveation API unless Khronos specifically requests that.

The existing registered FB extensions already provide the generic policy and
runtime-owned eye-tracked semantics. Metal is the missing graphics-specific
last mile, analogous to the role played by `XR_FB_foveation_vulkan`.

The intended incubation path is:

1. keep `XR_MNDX_foveation_metal` while the API is experimental;
2. validate fixed and eye-tracked rendering on hardware (done, see below);
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

### Hardware (PS VR2, native macOS, 2026-09-29)

Validated on a real PS VR2 through `monado-service` on native macOS with the
controlled `psvr2-openxr-test` scene, at or before
`4f459a289df12ce63aaca3bef557326ddda1c779`:

- `XR_FB_swapchain_update_state`, `XR_FB_foveation`,
  `XR_FB_foveation_configuration`, `XR_META_foveation_eye_tracked` and
  `XR_MNDX_foveation_metal` v2 are exposed;
- standard FB foveation and FB + META runtime-owned eye-tracked foveation
  render through the service;
- runtime-private eye tracking becomes valid and produces changing per-eye
  foveation centres; `XR_EXT_eye_gaze_interaction` is not enabled or needed;
- the Metal companion transport works across processes, with client-created
  IOSurfaces imported by IOSurface ID in the service;
- observed logical size 2800 x 2856 per eye; the aggressive (FB HIGH)
  physical raster was about 1376 x 1404;
- visual quality looked good, with no obvious difference in the headset.

Intermittent invalid-gaze transitions were seen, at least partly while the
headset was removed. They are only a bug if they persist while the headset is
worn or fail to recover.

### Not yet validated on hardware

- the per-image map association and `--fb-foveation-sparse-check`
  (unit-tested; see above);
- foveated projection layers through the layer squasher, e.g. with
  `--passthrough` or multi-layer Chromium immersive media;
- the in-process (direct) Metal swapchain path with the standard extensions;
- Chromium using the standard path;
- performance comparisons between off, fixed and eye-tracked foveation.

Depth combined with foveation remains unsupported (see [Depth](#depth)).

### CI and unit coverage

The branch has macOS CI coverage with the normally-disabled FB, META and Metal
feature gates enabled in in-process and service builds. Compile/unit coverage
includes:

- FB level/configuration parsing;
- META profile chaining;
- fixed vertical-offset to per-view NDC conversion;
- runtime gaze-direction to per-view NDC conversion;
- backend-neutral XRT policy resolution;
- per-image Metal map association, lifetime and packed layouts
  (`tests_metal_foveation_cache`, skipped where Metal lacks rate maps);
- Metal swapchain transport across vanilla, direct and service/XPC paths;
- compositor use of the matching dense map;
- the end-to-end diagnostic build.
