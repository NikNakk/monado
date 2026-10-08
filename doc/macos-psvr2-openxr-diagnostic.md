<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# PS VR2 macOS OpenXR diagnostic scene

`psvr2-openxr-test` is a small OpenXR application built directly in the Monado tree for the native macOS PS VR2 work. It is intended to replace ad-hoc patches to Khronos `hello_xr` when investigating motion judder.

The application deliberately has no build-time dependency on the Khronos sample applications or OpenXR loader library. It compiles against Monado's bundled OpenXR headers (`xrt-external-openxr`) and opens the Khronos loader dynamically at runtime. The graphics backend is Metal via `XR_KHR_metal_enable`.

## Build

From the Monado checkout:

```sh
cmake --build build/arm64 \
  --target psvr2-openxr-test \
  --parallel 4
```

If the existing build directory predates the target and CMake does not regenerate automatically, rerun the same CMake configure command used to create `build/arm64`, then run the build command above.

The executable is normally produced at:

```text
build/arm64/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test
```

## OpenXR loader

The executable tries, in order:

1. `--loader /path/to/libopenxr_loader...dylib`
2. `PSVR2_OPENXR_LOADER`
3. an OpenXR loader found by CMake when the target was configured
4. normal dyld lookup for `libopenxr_loader.1.dylib` / `libopenxr_loader.dylib`
5. common Homebrew paths under `/opt/homebrew/lib` and `/usr/local/lib`

If using the loader from an OpenXR-SDK-Source build, a useful way to locate it is:

```sh
find /path/to/OpenXR-SDK-Source/build -name 'libopenxr_loader*.dylib' -print
```

The runtime is still selected in the normal OpenXR way with `XR_RUNTIME_JSON`.

For example:

```sh
XR_RUNTIME_JSON="$PWD/build/arm64/openxr_monado-dev.json" \
PSVR2_OPENXR_LOADER="/path/to/libopenxr_loader.1.dylib" \
./build/arm64/src/xrt/targets/psvr2_openxr_test/psvr2-openxr-test
```

Use Ctrl-C to exit.

## Scene

The scene is intentionally built from a single instanced cube mesh so application rendering remains simple and predictable. It contains:

- a world-locked floor grid for translation and parallax;
- symmetric markers from roughly -40 to +40 degrees for centre/periphery and left/right comparison;
- targets at 0.75, 1.5, 3 and 6 metres whose physical size grows with distance to keep their angular size approximately comparable;
- a yellow world-locked central fixation cross;
- symmetric peripheral vertical references; and
- a small bright magenta cross 55 cm in front of the predicted head pose. This is reconstructed from the OpenXR `VIEW` reference-space pose every frame and acts as the head-locked reference.

The diagnostic world is anchored once, using the first valid head pose. Its forward direction is projected onto the local horizontal plane so the world does not inherit a small initial head pitch or roll.

## What the comparison tells us

If the magenta head-locked cross remains visually stable while the world-locked grid and markers judder, the problem is downstream of application rendering but tied to transforming/presenting world geometry with predicted tracking. If both judder together, presentation cadence or scanout remains a stronger candidate. If the effect changes strongly with target depth or eccentricity, that helps distinguish positional prediction, rotational prediction, distortion/scanout and simple whole-frame cadence errors.

The application currently uses the OpenXR Metal graphics binding. This is deliberately native and small, but it should not be treated as a graphics-API-neutral control against a Vulkan `hello_xr` run; where graphics API itself is under test, compare like with like.


## Standard foveation diagnostics

The diagnostic also exercises the standards-facing foveation path described in
[OpenXR foveation architecture on macOS](macos-openxr-foveation.md).

Fixed FB foveation:

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

The eye-tracked mode deliberately does not enable
`XR_EXT_eye_gaze_interaction`. Its logs report the META validity flag and
standardized per-eye NDC foveation centres, not a public gaze action.

The standard modes render directly into the OpenXR Metal swapchain with the
runtime-provided `MTLRasterizationRateMap`; Monado's compositor obtains the
matching dense map from swapchain state and reconstructs logical sampling in
the existing distortion/timewarp pass.

`--depth-layer` is currently rejected with these modes until compact depth
coordinates are implemented and validated.

The older `--gaze-foveation` mode remains as a reference/regression path only.
`--gaze-foveation-fused` was removed with `XR_MNDX_foveation`.
