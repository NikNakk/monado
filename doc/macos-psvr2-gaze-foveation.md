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
XR_RUNTIME_JSON="$PWD/build/arm64/openxr_monado-dev.json" \
./build/arm64/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
  --fb-foveation --foveation-profile aggressive
```

Runtime-owned eye-tracked foveation:

```sh
XR_RUNTIME_JSON="$PWD/build/arm64/openxr_monado-dev.json" \
./build/arm64/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
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
XR_RUNTIME_JSON="$PWD/build/arm64/openxr_monado-dev.json" \
./build/arm64/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
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

One older path remains useful for comparison:

### `--gaze-foveation`

This creates a public `XR_EXT_eye_gaze_interaction` action, renders through a
Metal rate map into an intermediate physical-size texture, and performs an
application-side reconstruction into the normal OpenXR swapchain.

It is useful as a reference implementation and for timing the cost of an
explicit resolve pass, but it is not the desired client/runtime architecture.

### `--gaze-foveation-fused` (removed)

This also used app-owned `XR_EXT_eye_gaze_interaction`, rendered directly into
the OpenXR image and supplied an experimental
`XrCompositionLayerFoveationMapMNDX` with the projection view. It proved the
compositor reconstruction concept that the standard path now uses.

The mode, and the `XR_MNDX_foveation` extension behind it (a profile query plus
that per-view map), were removed on 2026-10-01. Registered extensions cover
everything it did: `XR_FB_foveation_configuration` for levels and
`XR_META_foveation_eye_tracked` for a gaze-driven centre, with
`XR_MNDX_foveation_metal` supplying the Metal rate map. A client that still
asks for `XR_MNDX_foveation` (the Chromium fork's fallback path) now simply
does not find it and renders unfoveated unless the standard path is available.

## All-1.0 rate-map control (2026-11-11)

Here “unity” means every rate is 1.0; it does not refer to the Unity engine.

The [EXD/DXMT comparison audit](macos-unity-rate-map-cost-comparison.md) explains
the workload differences and the subsequent DXMT warning fix/retest. Repeated
no-map shader-variant controls account for most of EXD's measured increase.
A subsequent component ablation identifies the texture-read shader variant as
the dominant reproducible cost; an irreducible map cost is not established.
An opt-in per-pass prototype removes 1.90 ms from the no-map control but does
not establish a gain in mapped EXD frames; the audit records that limit.

`psvr2-openxr-test --unity-rate-map` binds a cached application-owned 16×16
Metal rasterization-rate map with every horizontal and vertical rate set to
1.0. Startup checks the rates and all 129 coordinate samples for identity.
The ordinary full-size swapchain, viewport, scene, depth attachment and
compositor path are preserved. There is no gaze acquisition, intermediate
texture, application reconstruction or runtime foveation remap. This measures
steady-state scene GPU cost of binding an identity map, separately from the
older reconstruction experiment. Map construction is outside the measurement.

`--frames N` bounds the diagnostic's frame-loop iterations for repeatable runs.
The comparison runner uses the existing isolated launchd service harness with
a simulated HMD and offscreen debug-image target; it leaves the installed
LaunchAgent and PS VR2 alone:

```sh
python3 scripts/macos/run-unity-rate-map-comparison.py \
  --output doc/results/unity-rate-map-YYYYMMDD
```

The sequence is off/on/on/off, 2,400 loop iterations per run, with the first
240-sample timing batch discarded. Only complete later batches are counted.
The runner saves logs, per-batch GPU summaries, thermal telemetry, source diff,
HEAD and binary hashes. `--passes N` repeats cleared scene render passes per eye
in both cases through the existing stress setting. Timings cover the stereo
scene command buffer; they do not measure CPU encoding, compositor work or
physical presentation. The simulated HMD has its normal wobble motion.

### Simulated-HMD measurements

Tested on Apple M5, RelWithDebInfo, HEAD `42c99bb1d` plus this diagnostic
change, 2026-11-11. Both service and native runtime were rebuilt from that HEAD.
The normal simulated HMD used 896×1007 colour/depth images per eye at the
runner's default 140% render scale. The unity maps reported exactly the same
physical dimensions as logical dimensions, with identity verification passing
for both eyes in every map-enabled run. No GPU command errors were reported.
No PS VR2 run or installed-runtime change was made.

The one-pass off/on/on/off results were:

| Run | Rate map | Scene GPU mean (ms) | Median of batch p50s (ms) | Measured samples |
| --- | --- | ---: | ---: | ---: |
| A1 | None | 0.272 | 0.259 | 1,920 |
| B1 | All 1.0 | 0.317 | 0.303 | 1,920 |
| B2 | All 1.0 | 0.320 | 0.303 | 1,920 |
| A2 | None | 0.316 | 0.296 | 1,920 |

Pooled case means: **0.294 ms without a map, 0.319 ms with a map**, a measured
**+0.025 ms (+8.4%)**. However, A1→A2 baseline drift was **+0.044 ms** and
A2 nearly equalled both map-enabled runs. This does **not** establish a causal
25 µs map penalty. Timing summaries are rounded to 1 µs; the p50 column is a
median of batch medians, not a pooled frame percentile. All thermal samples
were nominal (state 0), with Low Power Mode false. GPU frequencies were not
captured. A brief CPU-only test build overlapped the first baseline; the later
four-pass comparison has no build activity.

Local raw evidence: `doc/results/unity-rate-map-20261111-default-run2/`.
The first sandboxed launch failed before service startup because process
telemetry was unavailable; it is excluded from these results. The successful
comparison used the same isolated runner outside the sandbox.

The four-pass off/on/on/off check (1,440 loop iterations per run, 960
measured samples per run after warmup) produced:

| Run | Rate map | Stereo scene GPU mean (ms) | Median of batch p50s (ms) |
| --- | --- | ---: | ---: |
| A1 | None | 0.770 | 0.738 |
| B1 | All 1.0 | 0.799 | 0.754 |
| B2 | All 1.0 | 0.774 | 0.720 |
| A2 | None | 0.786 | 0.746 |

Pooled case means: **0.778 ms without a map, 0.786 ms with a map**, a measured
**+0.0085 ms (+1.1%)** per stereo command buffer containing four cleared passes
per eye. The two map-enabled means differ by 24.5 µs, more than the pooled
8.5 µs difference; they overlap the baseline range. All thermal samples again
reported nominal. Logs confirm four passes in every case and no GPU errors.
Local raw evidence: `doc/results/unity-rate-map-20261111-four-passes/`.

**Conclusion:** neither simulated workload resolves a consistent map-enablement
penalty above run-to-run variation. This is not proof of zero cost, nor an
upper bound for PS VR2 resolution, a different workload, non-unity maps or a
runtime reconstruction path. It does show that the historical ~1.54 ms
application reconstruction cost must not be attributed to merely binding a
Metal rate map. CPU encoding and map-update costs were not measured.

Validation: native service/runtime and diagnostic builds succeed; the existing
`tests_psvr2_controller_visuals` suite passes. The diagnostic rejects conflicting
foveation modes and invalid frame limits. These are headset-free checks only;
physical headset runs remain user-operated.

The diagnostic was restored over the November 15 upstream integration and
compiled/linked against its runtime helpers. Help, missing/malformed/out-of-range
frame counts and conflicting foveation modes pass CLI checks. The new lens-image
mode is explicitly rejected with the identity-map control so it cannot silently
replace the ordinary scene. Runner syntax/help, source formatting and source
license checks pass. The measurements above remain from the November 11 source;
this integration does not establish new timings.

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
2. compare fixed and eye-tracked image quality;
3. record GPU timing and rate-map revision frequency;
4. audit compact depth coordinates before combining depth + foveation;
5. validate the standard FB/META + Metal companion path in Chromium;
6. use that implementation experience to refine the proposed Metal companion
   before taking it upstream.
