<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# PS VR2 gaze-driven foveation on macOS

For the current architecture and standards direction, see
[OpenXR foveation architecture on macOS](macos-openxr-foveation.md).

This document records the PS VR2/Metal implementation and diagnostic paths.

## Current preferred path

New client integration should use:

- `XR_FB_swapchain_update_state`;
- `XR_FB_foveation`;
- `XR_FB_foveation_configuration`;
- optionally `XR_META_foveation_eye_tracked`;
- the experimental Metal rendering companion `XR_MNDX_foveation_metal`.

The Metal companion is deliberately narrow: it exposes the current
`MTLRasterizationRateMap` required to render the FB-selected foveation state.
It does not define policy and it does not expose raw gaze.

The compositor obtains the exact matching dense logical-to-physical mapping
that was bound to the submitted swapchain image when it was released, and feeds
it into the existing distortion / timewarp compute path (or the layer squasher
for multi-layer frames). See
[Per-image map association](macos-openxr-foveation.md#per-image-map-association).

This path has been validated on PS VR2 hardware through `monado-service`; it
remains opt-in at build time via the `XRT_FEATURE_OPENXR_*FOVEATION*` gates
listed in the architecture document.

## Running the standard path

Fixed foveation:

```sh
XR_RUNTIME_JSON="$PWD/build-wine/openxr_monado-dev.json" \
./build-wine/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
  --fb-foveation --foveation-profile aggressive
```

Runtime-owned eye-tracked foveation:

```sh
XR_RUNTIME_JSON="$PWD/build-wine/openxr_monado-dev.json" \
./build-wine/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
  --fb-eye-foveation --foveation-profile aggressive
```

The eye-tracked mode deliberately does **not** enable
`XR_EXT_eye_gaze_interaction`. The runtime acquires gaze privately and the
application receives only the standardized META foveation-centre state plus the
native Metal rate map needed for rendering.

Sparse-frame check (either mode; add `--passthrough` to exercise the layer
squasher):

```sh
OXR_DEBUG_FOVEATION_BINDING=1 \
XR_RUNTIME_JSON="$PWD/build-wine/openxr_monado-dev.json" \
./build-wine/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
  --fb-eye-foveation --fb-foveation-sparse-check --foveation-profile aggressive
```

On sparse frames the application log shows the runtime's new revision while
the `xrEndFrame foveation` lines must keep reporting the revision the
re-submitted image was rendered with.

`PSVR2_GAZE_STREAMS=1` is no longer required. The gaze interface is
provisioned by default and activation is lazy/reference-counted. Set
`PSVR2_GAZE_STREAMS=0` to disable the capability explicitly.

The standard path currently rejects `--depth-layer` until the compact depth
coordinate path has been audited.

## Rate-map implementation

Each eye uses a 16x16 Metal rate-map grid. The generic foveation policy is
resolved in `src/xrt/auxiliary/foveation/u_foveation.*`; Metal translates that
policy in `src/xrt/auxiliary/metal/m_metal_foveation.*`.

The useful diagnostic profile remains `aggressive`, which resolves to
approximately:

- centre rate 1.00;
- middle rate 0.50;
- peripheral rate 0.25.

The Metal cache changes revision only when the actual rasterization pattern
changes. Eye movement within the same quantized grid cell can update META's
reported NDC centre without creating a new immutable Metal rate map.

## Render and compositor path

```text
XR_FB_foveation / XR_META_foveation_eye_tracked
                  |
                  v
runtime-owned resolved foveation state
                  |
                  v
MTLRasterizationRateMap
                  |
                  v
Metal render directly into OpenXR swapchain
                  |
                  v
ordinary projection layer
                  |
                  v
129-sample dense map bound to the submitted image
                  |
                  v
Monado distortion / timewarp compute pass
  - timewarp
  - optical/chromatic distortion
  - logical -> physical VRR mapping
  - source sampling
                  |
                  v
display
```

This avoids the explicit full-resolution reconstruction pass used by the first
proof of concept.

## Legacy/reference diagnostic paths

Two older paths remain useful for comparison:

### `--gaze-foveation`

This creates a public `XR_EXT_eye_gaze_interaction` action, renders through a
Metal rate map into an intermediate physical-size texture, and performs an
application-side reconstruction into the normal OpenXR swapchain.

It is useful as a reference implementation and for timing the cost of an
explicit resolve pass, but it is not the desired client/runtime architecture.

### `--gaze-foveation-fused`

This also uses app-owned `XR_EXT_eye_gaze_interaction`, renders directly into
the OpenXR image and supplies an experimental
`XrCompositionLayerFoveationMapMNDX` with the projection view.

That path proved the compositor reconstruction concept and remains useful for
regression comparison. New clients should not build against it.

The legacy `XR_MNDX_foveation` policy/layer mechanism should therefore be
treated as development history and compatibility scaffolding, not the target
standards surface.

## Historical performance result

The original application-resolve proof at roughly 2800x2856 per eye showed:

- Metal VRR scene render falling from about 0.91 ms to about 0.46 ms;
- the explicit full-resolution reconstruction costing about 1.54 ms;
- the rate-mapped physical image using about 41.6% of full-resolution pixels.

Those measurements motivated the fused compositor design: the VRR rendering
saving is useful only if reconstruction can be folded into work the compositor
already has to perform.

## Validation status and next steps

Done on hardware (PS VR2, native macOS, `monado-service`, 2026-09-29):

- `--fb-foveation` and `--fb-eye-foveation` render correctly;
- runtime-private gaze activates and produces changing per-eye centres without
  `XR_EXT_eye_gaze_interaction`;
- logical 2800 x 2856 per eye, aggressive/HIGH physical about 1376 x 1404,
  with no obvious visual difference.

Remaining:

1. run `--fb-foveation-sparse-check` on the headset, with and without
   `--passthrough`, to confirm the per-image association and layer-squasher
   reconstruction end to end;
2. compare fixed and eye-tracked image quality against the legacy fused path;
3. record GPU timing and rate-map revision frequency;
4. audit compact depth coordinates before combining depth + foveation;
5. validate the standard FB/META + Metal companion path in Chromium;
6. use that implementation experience to refine the proposed Metal companion
   before taking it upstream.
