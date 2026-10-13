<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Identity composition color transform — 2026-10-13

Fix commit: `6aa8f0fdb`. Hardware captures used the same production conversion
before commit on base `6393b6592`; binaries report the base tag.

An exact identity `XR_KHR_composition_layer_color_scale_bias` attachment now
behaves like an absent attachment for native layer flags. This is an OpenXR
state-tracker change on every platform, with no UE detection, macOS toggle,
extension suppression, compositor eligibility relaxation or shader change.

## Cause and fix

`fill_in_color_scale_bias()` first checks instance extension enablement, then
finds `XrCompositionLayerColorScaleBiasKHR` in the layer's next chain. Previously
any attachment set `XRT_LAYER_COMPOSITION_COLOR_BIAS_SCALE`. The common
`can_do_one_projection_layer_fast_path()` rejects that flag before examining
projection/projection-depth type and chroma key. Its caller also rejects
multiple layers, passthrough, peek, debug mirroring and disabled fast paths.
These are the same gates used by the macOS hosted and service compositors.
The renderer selects distortion-only for eligible projection or projection-depth
layers, otherwise composition into scratch followed by distortion.

The conversion now uses `oxr_layer_set_color_scale_bias()`: copy the supplied
values and set the transformation flag only when at least one scale component
is not exactly 1 or one bias component is not exactly 0. No epsilon is used;
signed zero is identity, near-identity values and non-finite inputs are not
silently classified as identity. The extension-enabled check and chain lookup
remain unchanged. Non-identity values and all unrelated flags remain unchanged.
All supported layer types share this conversion.

Absent flags already cause render helpers to substitute identity scale/zero
bias. Projection-depth and source-alpha/unpremultiplied-alpha were already
eligible under the existing compositor policy; this fix preserves those flags
and does not add or remove their support. Depth validation remains in projection
submission. Multiple layers still require general composition.

[Khronos' extension specification](https://registry.khronos.org/OpenXR/specs/1.1-khr/html/xrspec.html#XR_KHR_composition_layer_color_scale_bias)
defines the transform's scale and bias on the layer color; exact identity values
need no additional color operation. This does not imply byte-equivalent output
between general composition and distortion-only, which use different sampling.

## Validation and measured impact

The CPU regression test covers absent and identity transforms for projection
and projection-depth, source alpha and unpremultiplied alpha, signed zero, and
each non-identity RGBA scale/bias component, including near-identity values.
It checks retained flags and values (184 assertions, two cases, arm64/x86_64).
Existing compositor diagnostic gate tests retain layer count, type, chroma,
passthrough, peek, mirror and debug-disable checks, with explicit alpha cases.
Full arm64 build and x86_64 OpenXR/changed test targets build successfully.
All 74 arm64 CTests pass with required IPC/Metal/launchd access; an initial
sandboxed run failed 19 restricted IPC/GPU integration tests, then passed on
rerun with that access.

Native UE `MonadoMacTest` captures used hosted composition, priority 0 in both
processes, 2800×2856 sources, PS VR2 120 Hz, no synthetic GPU competitor and the
same launch arguments. The control temporarily built the original conversion;
fixed source and binaries were restored afterwards. GPU query results are
fence-retired, trimmed two seconds at each end, joined by compositor frame ID,
and restricted to nonempty submissions. Whole-compositor timestamps are used;
per-pass timestamp attribution is not the basis of the claim.

| Capture | Nonempty GPU samples | Median compositor GPU time | p95 |
| --- | ---: | ---: | ---: |
| Fixed, first | 374 | 1.262 ms | 1.339 ms |
| Original presence-based flag | 1,069 | 2.645 ms | 2.725 ms |
| Fixed, repeat | 1,131 | 1.255 ms | 1.329 ms |

Median reduction is approximately **52.5% (1.39 ms)**. The control rejects every
nonempty frame for identity color scale/bias; both fixed runs select the fast
path for every nonempty frame. Physical callback counts are 640 / 1,351 / 1,370.
Startup and app frame rates differ, so no game-FPS or latency improvement is
claimed. No worn visual, pixel-equivalence or Linux CI result is claimed.
Raw captures stay in `/tmp/monado-ue-identity-{fixed,control,fixed-repeat}-20261013`;
only this concise record is retained. The installed runtime was not replaced.
