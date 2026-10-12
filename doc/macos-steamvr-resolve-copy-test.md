<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# SteamVR backend resolve-copy reduction, 2026-10-12

**Three-image compatibility enables the production SteamVR backend to remove
its final per-eye resolve-to-OpenXR copy for the tested format.** This is a real
Wine/DXMT cross-process and Monado GPU result, not a SteamVR Home or headset run.
Keep the mode opt-in pending the [hardware trial](macos-gpu-sync-hardware-tests.md).

## Test and result

The test compiles the existing external
`macos-wine-xr/src/steamvr_driver/xr_backend.cpp`, including its production
`CreateSharedSwapchain`, `AcquireImage` and `Present` methods. A generated copy
in the temporary output directory adds one counter immediately before the
existing `CopySubresourceRegion` call. No production source is edited. The
runner requires exactly one matching call site and records source/artifact
hashes. This is copy instrumentation, not a mocked D3D11/OpenXR implementation.

| Path | Resolve-image allocation/export | Backend copies across 18 stereo frames | Composed stereo GPU samples |
| --- | --- | ---: | ---: |
| Ordinary four images | Production export rejects the four-image count; fallback uses shared source textures and separate runtime images | 36 | 4 |
| Three-image compatibility | Both eyes export all three runtime-owned textures successfully | 0 | 7 |

The source format is `DXGI_FORMAT_R8G8B8A8_UNORM` (28), 128×128, single-sample.
Each eye has its own swapchain/resolve set. A **separate Windows process** opens
the exported handles through `OpenSharedResource`, writes red to the left eye
and blue to the right, and reads back the real GPU storage. All three indices
are visited and reused on each eye. The same child-write mechanism is used in
the copying control. The parent submits through the production backend;
Monado's completed Vulkan output readbacks confirm red left/blue right in both
cases. The service's compatibility marker appears only in the three-image run.

Three startup frames precede the counted window because the first OpenXR frame
can legitimately have `shouldRender=false`. The final source remains delivered
for 4.5 seconds so periodic compositor GPU readbacks can sample it before
teardown. Sample counts include reprojection of the same delivered frame;
they do not prove that every submitted frame reached the display.

Monado source: `6f96e49ee` plus this new test harness. Native ARM64 service and
x86_64 runtime were rebuilt incrementally to match their IPC version tags.
The first setup attempt correctly rejected mismatched older service/new client
tags; the test did not bypass the version check. External SteamVR/Wine/DXMT
working-tree edits and installed Steam configuration were preserved.
Raw evidence and exact artifact hashes:
`/tmp/steamvr-resolve-copy-verified-20261012/{manifest.json,results.json,four,three}`.

## Meaning and limits

For a stereo resolve layer, the measured reduction is **two backend copies per
rendered frame to zero**. SteamVR's own application-texture composition and
resolve work remain. Monado's distortion/timewarp/output work also remains;
“zero-copy” does not mean the entire application-to-display pipeline has no
rendering or transfers. The child uses staging copies solely for test readback;
these are outside the backend counter and are not part of the proposed driver
path.

The probe calls the real backend directly. It does not instantiate the HMD
driver, run `vrcompositor.exe`, exercise the driver's process-name eligibility
gate, or validate keyed-mutex handoff under SteamVR load. The production driver
only offers these runtime-owned sets to the compositor process, with zeroCopy
enabled and supported single-sample textures; ordinary application sets still
use their usual shared textures. Other formats/export failures can still fall
back to copying. The worker waits for real GPU completion through staging Map
before the parent releases an image; this intentionally makes this an export,
reuse and copy-elimination test rather than a production latency benchmark.

The separate [native pacing comparison](macos-three-image-comparison.md) found
no additional three-image starvation in its bounded workload. Neither test
establishes SteamVR performance, Game Mode behavior, or physical presentation
correctness.

## Reproduction

Requires the existing matching Wine/DXMT runtime, built bridge in
`~/Code/macos-wine-xr/build-in-process/gate-steamvr`, and the SteamVR driver's
cached OpenXR loader. It uses a fresh private prefix and isolated simulated
Monado services, and stops only that prefix's wineserver on exit.

```sh
cmake --build build/arm64 --target monado-service openxr_monado --parallel 4
cmake --build build/x86_64 --target openxr_monado --parallel 4
python3 scripts/macos/run-steamvr-resolve-copy.py \
  --output /tmp/steamvr-resolve-copy-new
```

Use a new directory each time. The runner checks both exact copy counts, actual
exports/reuse, the service mode, and at least two matching composed stereo
readbacks. `--bridge`, `--dxmt` and `--wine` override external checkout paths.
