<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Common projection-plus-quads specialization — 2026-10-13

`XRT_MACOS_COMPOSITOR_COMMON_LAYERS=1` opts into a smaller compute layer shader.
**Default off; Linux selection disabled.** It preserves two-pass filtering,
quantization, color adjustment, alpha blending, chromatic distortion, scanline
timewarp and output/intermediate resolution. Quad bounds is a separate option;
both acceptance variants have it enabled. Presentation remains CA copy.

## Selection and implementation

Shader specialization constant 6 removes the general layer-type switch and
inactive projection features. The cache key defaults false; both common
timewarp variants are prewarmed only when opted in, alongside generic pipelines.
There is no per-frame pipeline compilation or new GPU wait.

The CPU checks final per-eye shader inputs after visibility filtering and sampler
limits: at least two layers, projection first, only quads afterward, no depth
flags or foveation slots, and chroma curves <=0 (NaN rejected). Mismatches use the
generic pipeline; views can select independently. Single-projection fast-path
behavior is unchanged. A one-time log records the first actual common dispatch,
separately from enabling the option.

## Validation and measured benefit

Production-shader pixel checks use a 16-sampler layout, 67x39 per-eye output and
sRGB intermediate sampling. All **1,280 final images are byte-identical** across
scales 1/1.4, timewarp, color correction and output alpha on/off, one/four quads,
ten geometry/UV cases and four alpha blend/inversion combinations. Eligibility
and cache tests cover fallback and distinct specialization keys. These are
synthetic inputs, not optical validation.

Full-resolution offscreen measurements use 12 alternating paired rounds of six
serial frames after warmup. Timestamps include barriers and exclude readback;
a finite independent same-GPU worker covers every saturated sample. Repeat two
reverses load-mode order. Savings are median paired ratios, not ratios of medians.

| Intermediate scale | Quads | Idle savings, repeats 1 / 2 | Saturated savings, repeats 1 / 2 |
| --- | ---: | ---: | ---: |
| 1.0 | 1 | 17.85% / 19.41% | 14.18% / 15.91% |
| 1.0 | 4 | 18.40% / 18.88% | 19.45% / 16.06% |
| 1.4 | 1 | 23.14% / 23.15% | 18.53% / 19.67% |
| 1.4 | 4 | 22.03% / 22.06% | 16.46% / 20.59% |

Eight 15-second requested headset captures use located stereo projection plus
four opaque head-locked quads from a uniform 64x64 Metal image. Normal scale,
bounds, CA copying and pass queries off are fixed; variant/load order reverses.
Startup/shutdown seconds are excluded; candidates log actual common selection.

| Load | Generic physical Hz | Common physical Hz | Generic Vulkan median, ms | Common Vulkan median, ms |
| --- | ---: | ---: | ---: | ---: |
| Idle | 119.73 / 119.11 | 119.57 / 119.57 | 4.709 / 4.719 | 4.459 / 4.683 |
| Saturated | 95.00 / 93.25 | 102.91 / 104.11 | 9.029 / 9.172 | 8.055 / 8.052 |

Vulkan medians include all retired steady frames, not just displayed ones. Loaded
intervals shrink ~11–12%; gaps over 1.5 refreshes fall 26.1%/28.4% → 16.4%/15.1%.
Median commit-to-display falls 20.99/21.86 → 16.24/16.24 ms, without establishing
motion-to-photon latency. Idle gains vary; do not substitute the larger offscreen
saving for the runtime result. An enabled quad-only hardware smoke verifies
generic fallback, but is not a performance control. These captures predate the
later GPU-priority option; their combined gain has not been measured.

## Actual client applicability and acceptance

Observed workloads do **not** establish projection-plus-quads as dominant:

- SteamVR Home driver logs mostly show one projection, occasionally two with
  dashboard content. Every resolve layer becomes an OpenXR projection, not a
  quad; the additional projection uses source-alpha blending. Periodic log
  samples do not establish residence percentages.
- Tested Wine/OpenComposite Underture and InMind use one stereo projection.
- Native UE traces show ordinary projection content on the general compute
  path, but record the first projection rather than the entire layer list.
- Native OpenBrush/Unity works; no complete retained layer inventory was found.

No regular-game benefit or selection is claimed. Keep the option off pending
representative-client layer/feature/path census and user visual checks of head
motion, chromatic edges and overlays. A single ordinary projection may already
use distortion-only rendering; measure the selected path before tuning it.
See the [investigation summary](macos-compositor-gpu-efficiency.md) for rejected
approaches, methodology, build limitations and retained capture tools.

The later [projection-only census](macos-projection-workload-census.md) confirms
one-projection fast-path rendering in native Unity, Wine Hyperbolica and sampled
Home, and an
identity color-scale/bias flag blocking native UE. That is the next relevant
extra-pass hypothesis; no regular-game quad benefit has been established.
