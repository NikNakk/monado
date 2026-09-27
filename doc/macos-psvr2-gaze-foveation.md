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
4. expose the mechanism cleanly to native Metal OpenXR clients;
5. investigate using the same Metal mechanism in Chromium's Metal/ANGLE path;
6. add a general OpenXR foveation interface only once the client/runtime
   ownership model is clear;
7. revisit Vulkan clients when MoltenVK exposes a suitable Vulkan VRS feature.

Compositor-only foveation of an already full-resolution application image is
not expected to save meaningful application rendering work, so it is not the
primary path.
