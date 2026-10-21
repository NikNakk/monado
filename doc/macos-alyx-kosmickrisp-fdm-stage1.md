<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Alyx FDM on KosmicKrisp, stage 1: full-rate tracing — 21 October 2026

**Result:** native Half-Life: Alyx now runs its real fragment density map
(FDM) path on macOS. An opt-in KosmicKrisp build offers
`VK_EXT_fragment_density_map` with a correct full-rate implementation. Alyx
enables it, builds its density map, attaches it to every stereo pass and uses
subsampled render targets. The user reached the menu and their saved level;
the picture looks normal. Frame pacing felt somewhat worse, but this run used
the logging observer, so no timing comparison is made (see below).

This follows Codex's FDM observation (`doc/macos-alyx-fdm-observation.md` and
`doc/alyx-valve-lua-results/density-map-analysis/` in `workspace/monado-alyx-dual-vulkan`,
branch `codex/alyx-density-map-analysis`), where
MoltenVK offered no FDM extension and Alyx stayed on its non-FDM path. It builds
on the [custom resolve fix](macos-alyx-kosmickrisp-custom-resolve-fix.md):
custom resolve stays enabled.

## Why full rate is correct

The Vulkan [fragment density operations](https://docs.vulkan.org/spec/latest/chapters/fragmentdensitymapops.html)
let an implementation use a fragment area no larger than the one the map
requests. Shading every pixel is therefore valid. So are `FragSizeEXT = (1,1)`,
`FragInvocationCountEXT` equal to the sample count under per-sample shading
(else 1), and storing subsampled images as ordinary ones. Nothing is faked: the
build behaves as a real FDM implementation that chooses not to reduce shading.

## KosmicKrisp changes

Mesa branch `claude/kk-fdm-stage1` in `workspace/kk-fdm`, on top of the custom
resolve fix (`1574cde`), built at `build/kk-fdm`. Everything is gated by
`MESA_KK_EXPERIMENTAL=fdm` and is off by default.
[Patches](../experiments/alyx-vulkan/kosmickrisp-fdm/mesa-patches).

| Commit | Change |
|---|---|
| `425a224` | Advertise the extension with `fragmentDensityMap` and `fragmentDensityMapNonSubsampledImages` (not `Dynamic`). Texel sizes are 32x32 to 1024x1024, as Turnip reports. Give R8G8_UNORM the density-map format feature and map density-map usage to it. Lower `FragSizeEXT` and `FragInvocationCountEXT` to full-rate constants |
| `6f2ade0` | Add a fully backed `DEVICE_LOCAL \| LAZILY_ALLOCATED` memory type, usable only by transient-attachment images. On the FDM path Alyx creates its multisampled target as transient and requests that type. Without it the game bound `VK_NULL_HANDLE` and crashed in `kk_BindImageMemory2` (first run, `claude-kk-fdm-1`) |

The memory type is fully backed, not memoryless, because the custom resolve
shader reads the multisampled samples back.

## Verification

- [FDM probe](../experiments/alyx-vulkan/kosmickrisp-fdm) (`build.sh OUT`;
  exit 3 means the extension is not offered):
  - With the flag off, the extension is absent.
  - With it on, the features, properties and R8G8 format feature are correct,
    and a subsampled colour image is supported.
  - A 2-view multiview pass whose map requests 4x4 shading still produces an
    exact per-pixel checkerboard with `FragSizeEXT = 1x1`. This holds for both
    a normal target and a subsampled target read back through a subsampled
    sampler.
- The custom resolve probe (including `--dummy-descriptors`) and the 26-check
  Metal interop test still pass, with and without the flag.
- Alyx (`runs/claude-kk-fdm-2`, launched with `kk-tests/run-alyx-kk-fdm-trace`
  and `+vr_fdm 1 +fove_dump 1`): menu and saved level reached, picture normal,
  no faults.

## What Alyx does on its FDM path

[Trace summary](alyx-valve-lua-results/kk-fdm-stage1/trace-summary.json):

- **Negotiation:** it enables `VK_EXT_fragment_density_map` only. FDM2, the
  EXT/QCOM offset extensions and KHR fragment shading rate are not offered and
  not requested. Device creation succeeds.
- **Density map:** "Creating fragment density map with size: 21x24x2 (RT:
  672x768x2)". This is an R8G8_UNORM 2-layer image (one layer per eye) with
  32-pixel texels, uploaded from the CPU.
  [Alyx's own dump](alyx-valve-lua-results/kk-fdm-stage1/fdm_square_21_24.ppm)
  (`+fove_dump 1`) shows the `ultra` square preset that Codex reconstructed
  offline: a centred 9x8-texel block (288x256 px per eye, about 14% of the area)
  at 255, and 56 (about 0.22 per axis, roughly 4x4 fragments) everywhere else.
  No other values appear.
- **Use:** every stereo dynamic-rendering pass attaches the map (one view,
  `FRAGMENT_DENSITY_MAP_OPTIMAL_EXT`); about 29,000 records in this run. No
  offsets are used, since the offset extension is not offered and there is no
  eye tracking.
- **Subsampled images:** the 2-sample colour target (B10G11R11, usage 0xd7,
  now transient) and the single-sample resolve target (usage 0x1f, including
  storage) are both created with `VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT`. Depth
  falls outside the observer's 128-image budget and was not seen.
- **Shaders:** none use `FragmentDensityEXT`, `FragSizeEXT` or
  `FragInvocationCountEXT`. Alyx relies on the driver for reconstruction, and on
  subsampled samplers (`r_subsampled_sampler_desc`) when it reads the
  subsampled resolve target later.

## Stage 2 outline (not started)

A shaped map makes acceleration on Metal more practical than Codex's
general-case analysis assumed:

- **Rate map:** a centred full-rate rectangle in a uniform 0.22 surround is
  exactly what a per-axis Metal `MTLRasterizationRateMap` can express. Use full
  rate on the centre's columns and rows and 0.22 elsewhere. The only
  over-shading is the cross-shaped strip where one axis is full rate. In theory
  about 27% of full-rate fragments are shaded in that pass, against about 19% if
  the map were followed exactly. This is grid arithmetic, not a measured speed-up.
- **Work needed:**
  - Apply the map to the multisampled pass and the custom resolve stage.
  - Store the subsampled images at physical size.
  - Make subsampled-sampler reads go through Metal's rate-map coordinate
    mapping.
  - Expand to full size anywhere a non-subsampled image or the compositor needs
    it.
- **Measurement:** do a fair timing comparison first. Run FDM on and off with
  the same non-observer launcher (`kk-tests/run-alyx-kk-cr-debug` with
  `MESA_KK_EXPERIMENTAL=fdm` and `+vr_fdm 1`), so the stage 1 overhead is known.

## Limitations

The observer's image log is bounded, so depth and later images were not
recorded. Frame timing from the observer run is not comparable with untraced
runs. The lazily allocated memory type is only exposed with the FDM flag.
Metal's residency-set validation message from earlier runs is unrelated and
still open.
