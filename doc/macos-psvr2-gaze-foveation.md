<!-- SPDX-License-Identifier: BSL-1.0 -->

# PS VR2 gaze-driven foveation on macOS

The PS VR2 macOS diagnostic application has an experimental gaze-driven
variable-rasterization-rate (VRR) path using native Metal.

This is currently an **application-side proof of concept**, not a general
Monado/OpenXR foveation implementation.

## Why Metal first

Apple exposes variable rasterization rate through
`MTLRasterizationRateMap`. The application renders into a physically smaller
intermediate texture whose local pixel density varies across the logical
viewport, then expands that texture into the normal full-resolution OpenXR
swapchain.

Current MoltenVK does not expose Vulkan fragment-shading-rate support, so this
cannot yet transparently accelerate Vulkan OpenXR clients such as Wine/Alyx.
The native Metal path lets us establish the hardware benefit and gaze behaviour
before designing the wider runtime/client API.

## Running

Eye gaze must be enabled for `monado-service`:

```sh
launchctl setenv PSVR2_GAZE_STREAMS 1
```

Then run:

```sh
XR_RUNTIME_JSON="$PWD/build-wine/openxr_monado-dev.json" \
./build-wine/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
  --gaze-foveation
```

The option implies `--gaze`: the test creates the standard
`XR_EXT_eye_gaze_interaction` action and drives the Metal rate map from the
calibrated gaze pose.

`--gaze-foveation` is intentionally incompatible with `--depth-layer` for
now because the reduced-rate depth target has not yet been reconstructed into a
full-resolution OpenXR depth swapchain.

It can be combined with passthrough:

```sh
.../psvr2-openxr-test --passthrough --gaze-foveation
```

## Rate map

Each eye currently uses a 16x16 Metal rate-map grid. Gaze is projected into the
current eye FOV and quantised to one grid cell. The rasterization rates are:

- two cells around gaze: 1.0;
- next two cells: 0.70;
- remaining periphery: 0.45.

Horizontal and vertical rates are combined by Metal, so the outer corners use
substantially fewer physical pixels than the fovea.

The gaze cell is quantised so normal eye-tracker jitter does not require a new
rate map every frame. A new immutable Metal rate map is created only when gaze
moves into another grid cell.

Whenever the map changes, the diagnostic logs the physical intermediate
resolution and percentage of full-resolution pixels, for example:

```text
eye 0 foveation gaze=(+3.1,-1.8)deg zone=(8,7) physical=... ...% of full pixels
```

## Render path

```text
XR_EXT_eye_gaze_interaction
          |
          v
gaze ray relative to VIEW
          |
          v
project into each XrView FOV
          |
          v
16x16 MTLRasterizationRateMap
          |
          v
smaller color + depth render targets
          |
          v
normal diagnostic scene render
          |
          v
rasterization_rate_map_decoder
          |
          v
full-resolution OpenXR Metal swapchain
          |
          v
Monado compositor
```

The final expansion pass uses Metal's
`rasterization_rate_map_decoder.map_screen_to_physical_coordinates()`, so the
logical projection remains unchanged even though the intermediate image has
nonuniform pixel density.

## Next steps

If the diagnostic demonstrates a worthwhile GPU saving without objectionable
peripheral artefacts:

1. measure GPU duration against the unfoveated diagnostic with a heavier scene;
2. tune the foveal radius/rates and rate-map update threshold;
3. cache/prebuild likely maps or otherwise reduce rate-map creation overhead;
4. keep foveation policy graphics-API-independent while Metal translates it
   into `MTLRasterizationRateMap` state;
5. expose the mechanism cleanly to native Metal OpenXR clients;
6. investigate using the same Metal mechanism in Chromium's Metal/ANGLE path;
7. add a general OpenXR foveation interface only once the client/runtime
   ownership model is clear;
8. revisit Vulkan clients when MoltenVK exposes a suitable Vulkan VRS feature.

Compositor-only foveation of an already full-resolution application image is
not expected to save meaningful application rendering work, so it is not the
primary path.


## Foveation policy and graphics backends

Foveation strength is deliberately separated from the Metal implementation.
`src/xrt/auxiliary/foveation/u_foveation.*` owns the named policy profiles
and their normalized centre / middle / peripheral rates. The currently useful
diagnostic profile is `aggressive` (1.00 / 0.50 / 0.25).

`src/xrt/auxiliary/metal/m_metal_foveation.*` does not select profiles. It
accepts a resolved `u_foveation_profile` and translates that policy into a
16x16 `MTLRasterizationRateMap`, returning both Metal's actual physical render
size and the dense 129-sample logical-to-physical transform used by the fused
compositor path.

This leaves room for future Vulkan or D3D backends to implement the same policy
with their native VRS mechanism without exposing Metal-specific concepts to the
OpenXR-facing policy layer.

## Fused compositor proof

The original `--gaze-foveation` mode intentionally used an explicit
application-side reconstruction pass. Measurements on the 2800x2856-per-eye
diagnostic scene showed that the Metal VRR scene render itself fell from about
0.91 ms to about 0.46 ms, while the full-resolution reconstruction pass cost
about 1.54 ms. The rate-mapped physical image was about 41.6% of the normal
pixel count and was not perceptibly different in the headset.

The experimental fused mode removes that application reconstruction pass:

```sh
XR_RUNTIME_JSON="$PWD/build-wine/openxr_monado-dev.json" \
./build-wine/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test \
  --gaze-foveation-fused
```

The client renders with `MTLRasterizationRateMap` directly into the ordinary
full-sized OpenXR Metal swapchain texture. Only the compact physical-coordinate
region contains meaningful rendered pixels. For every new gaze-rate-map cell,
the client samples Metal's logical-to-physical mapping at 129 positions per
axis (128 intervals) and attaches those normalized lookup arrays to the
projection view using an internal, experimental structure chain. The lookup
is intentionally denser than the requested 16 rate zones because Metal may
refine the requested zones or raise their actual rasterization rates.

The OpenXR state tracker copies those boundary arrays into the normal Monado
projection layer data. Because layer data already travels through the shared
IPC layer slot, no extra per-frame IPC operation is required.

On the compositor's single-projection fast path, the existing distortion /
timewarp compute shader applies the same piecewise-linear logical-to-physical
mapping immediately before sampling the source image. This folds VRR
reconstruction into the compositor pass that was already required:

```text
Metal gaze VRR render into OpenXR image
          |
          v
projection layer + 129 x/y boundary values per eye
          |
          v
Monado distortion / timewarp
  - optical distortion
  - chromatic source UVs
  - timewarp
  - logical -> physical VRR UV mapping
  - source sampling
          |
          v
display
```

The first proof is intentionally restricted to one projection layer without an
OpenXR depth layer or passthrough. Those cases can use the layer-squashing path
or additional source images and need equivalent foveation-aware sampling before
they are enabled.

Set `XRT_MACOS_FUSED_FOVEATION` only for this internal experiment. The
diagnostic sets it automatically when `--gaze-foveation-fused` is requested.

The old `--gaze-foveation` mode remains available as a two-pass reference
implementation for timing and visual comparisons.
