<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# Identity rate-map cost: native diagnostic versus EXD/DXMT

Read-only audit on 2026-11-11 of the current, largely uncommitted
`~/Code/macos-wine-xr` work, at bridge HEAD `97d6309` plus its working tree.
Monado is at `42c99bb1d` plus the all-1.0 diagnostic changes. No Wine/DXMT
source, game, settings or running processes were modified. No new GPU run was
made for this audit.

## Per-resource specialization follow-up

Patch 0020 compiles each pixel shader for the exact SRV slots mapped at a draw,
removing the runtime metadata test from both mapped and ordinary reads. Fourteen
60-second EXD trials (2560×1440, 4× MSAA, off/on/on/off with a baseline at each
end) give:

| Workload | Specialization off (GPU ms) | Specialization on (GPU ms) |
| --- | ---: | ---: |
| Ordinary baseline, no feature flags | 16.19, 16.77 | — |
| Flags enabled, no map | 17.98, 18.44 | 16.57, 16.71 |
| Actual mapped rate 1.0 | 19.59, 20.14 | 17.93, 18.22 |
| Actual mapped rate 0.5 (at the 60 Hz cap) | 14.22, 14.20 | 14.09, 14.10 |

The full-rate mapped overhead falls from about 3.4 ms to about 1.6 ms over the
baseline, with separated run ranges, and the no-map penalty essentially
disappears. Rate 0.5 remains capped and shows nothing. This is the first change
that reduces cost in actually mapped EXD frames. Fixture, MSAA-chain and EXD
image checks pass with the option on. It stays opt-in
(`DXMT_RATE_MAP_RESOURCE_READ=1`). Evidence is in the sibling checkout's
`docs/dxmt-resource-read.md` and `docs/results/dxmt-resource-read-20261111/`.

## Selective-read optimization follow-up

An opt-in per-pass prototype now removes read-translation shader code only
when the fully recorded/merged pass has no mapped reads. Position translation
is retained, mapped reads keep full shaders, and geometry/tessellation/compute
fall back to their original pipelines. Shader and PSO cache keys distinguish
the alternatives. New late-binding and merged-pass regressions, six MSAA/HDR/
depth/stencil cases and the automatic-foveation suite pass on the rebuilt
private runtime/prefix.

Twelve matched 60-second EXD trials (26,355 loaded frames) give:

| Workload | Selection off (GPU ms) | Selection on (GPU ms) |
| --- | ---: | ---: |
| Flags enabled, no map | 18.927 | 17.027 |
| Actual mapped rate 0.5 | 12.729 | 12.782 |
| Actual mapped rate 1.0, full translations | 20.408 | 20.430 |

The no-map feature penalty falls by 1.90 ms / 10%, with separated repeat ranges.
**Neither actual mapped workload demonstrates a gain.** Rate 0.5 reaches the
60 Hz simulator cap; the full-rate mapped control is GPU-bound and also
unchanged within variation. It is not the earlier debug-bit-3 map-only case.
Actual mapping selects 41 eligible ordinary / 52 full pipeline commands per
frame, so expensive passes with any mapped read retain the full shader. Finer
per-resource specialization or active-lookup optimization is still needed;
this prototype does not implement it.

`DXMT_RATE_MAP_SELECTIVE_READ=1` is off by default and remains a bounded private
candidate. No installed runtime or service default changed, and no physical
headset run was made. Full evidence and the reproducible patch are in the
sibling checkout's `docs/dxmt-selective-read.md`, patch 0019, and
`docs/results/dxmt-selective-read-20261111/`.

## Shader-component follow-up

The subsequent no-map ablation uses ten 60-second trials in forward/reverse
order and a fresh private shader cache. A guarded selector separates the two
compiler bits that `DXMT_RATE_MAP_POSITION=1` normally enables together.
Host buffer bindings and layout bookkeeping stay enabled for every non-baseline
case; only the selected shader components change.

| Case | Mean of two GPU run medians (ms) |
| --- | ---: |
| Ordinary baseline | 16.617 |
| Host setup, ordinary shaders | 16.950 |
| Position shader component only | 17.396 |
| Texture-read shader component only | 19.058 |
| Both shader components | 19.175 |

All 22,280 loaded frames have zero mapped render nodes. The read-only run range
(18.988–19.129 ms) is separated from host-only (16.221–17.679 ms), identifying
the read-translation shader component as the dominant reproducible no-map
penalty. Adding position to reads changes the mean by only 0.116 ms. The smaller
host-only and position-only differences overlap control drift and are not
isolated causal costs. CPU preparation remains about 0.43–0.48 ms across cases;
render-node, command-buffer and counted-fence medians remain 59 / 7 / 409.

This isolates presence of the read-capable shader code/signature/control flow,
even when its lookup branch is skipped. It does not separately measure metadata
branches, register pressure, occupancy or compiler scheduling. Pixel versus
compute contributions remain unseparated. A fresh cache reproduces the combined
~19.2 ms observation, so stale cache contents are not needed to explain it.

All four no-map masks pass the pixel fixture. Default mapped, identity and
negative controls pass on the deployed private prefix; nonzero stay-warped
profiles reject the diagnostic override. Private settings are hash-restored;
thermal state stays nominal. The selector is off by default. See the sibling
checkout's `docs/dxmt-shader-ablation.md`, patch 0018, and
`docs/results/dxmt-shader-ablation-20261111/` for evidence and provenance.
A potential optimization is ordinary shader variants for passes that do not
use mapped coordinates/resources; that production change is not implemented.

## Warning fix and retest follow-up

The user subsequently authorised fixing the warning and repeating the EXD
measurements. The private DXMT source and patch 0017 now warn only on a real
layout mismatch; rebuilt D3D11/DXGI modules are deployed in both the private
runtime and the private EXD prefix, with originals backed up. Six successful
90-second trials use baseline/flags-only/map-only/map-only/flags-only/baseline
ordering, 2560×1440 and 4× MSAA in the main scene.

| Case | Run medians (GPU ms) | Mean of run medians (ms) |
| --- | --- | ---: |
| Ordinary baseline | 16.483, 17.410 | 16.947 |
| Flags only, no map | 18.942, 19.384 | 19.163 |
| Identity map, translations skipped | 19.279, 19.474 | 19.377 |

Both corrected map-only runs have zero spurious warnings. Their median CPU
preparation work averages 0.466 ms, versus about 1.167 ms in the old noisy
controls: roughly 0.70 ms / 60% lower. Their GPU intervals remain near the old
19.25/19.29 ms observations; suppressing warnings does not demonstrate a GPU
time reduction.

The new no-map flags controls account for most of the increase from the
ordinary baseline. Map-only adds 0.214 ms (1.1%) over flags-only, with overlapping
run ranges and substantial control drift. Neither that increment nor the full
2.430 ms increase over baseline is established as an irreducible Metal map
penalty. The original claim that shader variants alone cost nothing is not
reproduced in this sequence. Shader variant/branch/binding effects and clock or
scheduling variation remain to be separated.

All successful trials have nominal thermal state, Low Power Mode off, 59
render nodes, seven command buffers and 409 counted fence waits per loaded
frame (medians). Only map-only trials have mapped render nodes (19). Private
game settings are hash-restored after every successful trial. The exact updated
prefix passes the existing mapped-read/identity/negative-control fixture and
both ordinary audit controls (8,192 exact pixels each, Metal validation on).
Initial trials without usable audit records and an early service-running guard
failure are excluded; these are setup failures, not discarded timing outliers.

Full evidence is in the sibling checkout's
`docs/dxmt-warning-fix-retest.md` and
`docs/results/dxmt-warning-fix-20261111-run3/`. The historical audit below is
retained to explain the discovery; its recommendation to fix the warning is
now complete. Installed runtimes and service defaults were not changed.

## Finding

The measurements do not conflict. The native diagnostic measures a small,
single-sample render into ordinary swapchains. The EXD full-rate experiment
measures a multi-pass application pipeline, including shader translation of
positions and texture reads. Its separate map-only control is more comparable
in intent, but still uses different attachments, rendering work and timing
boundaries. The available evidence does not establish why its entire residual
cost occurs, and does not establish a universal or irreducible Metal map tax.

Sources in the sibling checkout:

- `docs/dxmt-exd-stay-warped-trial.md` and
  `docs/results/dxmt-exd-stay-warped-20261111/summary.json`.
- `patches/dxmt/0017-automatic-stay-warped-stereo-profile.patch` and the current
  `build-in-process/dxmt-warp-source/src/dxmt/dxmt_context.cpp`.
- `tools/analyze_dxmt_tbdr_audit.py` and the raw `game/game.log` files under
  `build-in-process/exd-warp/headset-20261111-*`.

## Different workloads and boundaries

| Property | Monado native diagnostic | EXD/DXMT loaded scene |
| --- | --- | --- |
| Target | Two 896×1007 eye textures | 2560×1440 double-wide scene targets |
| Samples | 1× throughout | Main scene 4× MSAA, mixed intermediate attachments |
| Mapped passes | 2 per frame, or 8 in the four-pass check | Median 19 post-optimisation mapped render nodes in the inspected 4× runs |
| Attachments | One colour and Depth32Float per eye; depth discarded | HDR colour, combined depth/stencil, MRTs, intermediate loads/stores and resolves |
| Translation | None | Position and texture-read translation, unless disabled by debug bits |
| Timing | One stereo scene command buffer | Per-frame union of seven DXMT command-buffer intervals; includes internal waits |
| Summary | Means of complete 240-frame batches after warmup | Median per-frame interval unions over loaded-scene cohorts |

A single EXD scene-sized pass has about 4.1 times the pixels of one diagnostic
eye pass, before considering MSAA or overdraw. Multiplying the native test's
microsecond difference into a prediction for EXD is not justified. Our four-pass
check repeats clear/render/store work; it does not reproduce an MSAA/HDR
post-processing chain with preserved depth/stencil.

The native test's measured differences were +24.7 µs for two passes and
+8.5 µs for eight passes, both within run variation. EXD reports 17.06 ms
baseline, 19.25 ms map-only, and 22.27 ms with both translations before its
optimisations. Thus roughly 3 ms of that full mechanism's measured increase
is outside what the native test enables. The later optimisations lower the
full mechanism to around 21 ms, without resolving its map-only residual.

## Checks against raw EXD logs

Re-ran the existing Python analyser in memory, selecting frame IDs 300–3999
and then loaded frames with `render_after >= 55`. Medians below may differ
slightly from the curated summary's frame cohort:

| Run suffix | GPU interval union (ms) | Render nodes before / after | Mapped nodes | Command buffers | Fence waits | CPU prepare work (ms) |
| --- | ---: | --- | ---: | ---: | ---: | ---: |
| `154356-msaa4-s143-cost-flags-only` | 16.697 | 63 / 59 | 0 | 7 | 409 | 0.449 |
| `154529-msaa4-s143-cost-debug3` | 19.236 | 63 / 59 | 19 | 7 | 409 | 1.165 |
| `155222-msaa4-s143-cost-baseline` | 17.049 | 63 / 59 | 0 | 7 | 409 | 0.444 |
| `161101-msaa4-s143-opt2-baseline` | 16.427 | 63 / 59 | 0 | 7 | 409 | 0.451 |
| `161238-msaa4-s143-opt2-rate1-debug3` | 19.287 | 63 / 59 | 19 | 7 | 409 | 1.169 |

Standalone clears/resolves after optimisation remain zero, and the nominal
attachment load/store proxies are identical (1,066,674,736 / 1,513,253,424
bytes per frame). Explicit event-wait counts remain zero and initializer-wait
counts remain one. Map-related merge rejection attempts rise from 0 to 26,
but **the final render-node count does not rise**. These logs therefore do not
support extra render passes or extra counted fences as the explanation.
They do not measure fence stall durations, driver-internal passes, compression
or actual memory bandwidth.

## A concrete flaw in the map-only control

In `dxmt_context.cpp`, around lines 378–390, this condition combines layout
compatibility with the read-translation debug bit:

```cpp
if (read.handle == srv.texture->rate_map_layout.handle && !(warp_debug & 2)) {
    // Enable read translation.
} else {
    WARN("Rate-map read: second layout in one pass left untranslated");
}
```

`DXMT_STAY_WARPED_DEBUG=3` deliberately disables both translations. The
condition consequently emits the warning even for a matching layout. Counting
the actual raw logs gives:

- First map-only run: **621,038** spurious warnings, 58,800,306-byte log.
- Second map-only run: **639,070** spurious warnings, 59,894,893-byte log.
- Baseline and full-translation controls: zero such warnings.

The debug control also retains layout tags, translation-capable shader
variants, buffer bindings and layout-based merge checks. It disables the
expensive shader lookup branches; it does not bypass the entire DXMT
mechanism. CPU preparation work rises by about 0.72 ms in the map-only runs.
The warning flood is a concrete confound. Its effect on the GPU interval union
is **not measured**, so it cannot be asserted to explain all, or any fixed
portion, of the 2.2–2.9 ms residual.

## What would settle the residual

First correct the debug warning condition in Claude's own working tree, then
repeat a matched baseline/flags-only/map-only comparison, checking the final
pass/fence counts and thermal state. Do not infer an unavoidable hardware
cost from the existing noisy control.

The native follow-up should independently vary sample count, HDR format,
depth/stencil preservation and full-size pass chains, using the same shaders
and attachments with and without an identity map. Start at EXD's 2560×1440
size. If that reproduces a penalty, use per-pass GPU measurements or a Metal
trace to distinguish rasterization/MSAA/attachment effects from waits. The
present evidence identifies these as candidates, not demonstrated causes.

See [the native runs](macos-psvr2-gaze-foveation.md#all-10-rate-map-control-2026-11-11)
for their exact scope and reproduction commands.

## DXMT fixed-map PS VR2 alignment finding (2026-11-11)

The user's first reported wearer run of the private DXMT stay-warped profile
at 5600×2856, 2× MSAA and peripheral axis rate 0.3 feels reduced-rate throughout.
Run folder in `~/Code/macos-wine-xr`:
`build-in-process/exd-warp/headset-20261111-223322-msaa2-psvr2-warped`.
Bridge HEAD is `97d6309` plus its uncommitted DXMT patches, including the
finished per-resource shader selector. This is the DXMT automatic application
profile, not Monado's FB/META runtime-owned map implementation.

The run's environment and startup log confirm `DXMT_STAY_WARPED=5600x2856`,
`DXMT_STAY_WARPED_RATE=0.3`, `DXMT_RATE_MAP_RESOURCE_READ=1` and
`STAY_WARPED map 5600x2856 peripheral_rate=30%`. Composition telemetry reports
two 2800×2856 source views. The user's command correctly enabled the intended
profile.

The profile is fixed at the geometric midpoint of each eye image: horizontal
grid columns 3/4 and 11/12 at 1.0 in a 16-column double-wide map, and vertical
rows 3/4 at 1.0 in eight rows. Other entries are 0.3. It is not gaze-tracked or
centred using PS VR2 projection/lens calibration.

PS VR2's asymmetric FOVs (-61.5°/+43.5° for the left eye, mirrored for the
right) put the forward ray at approximately U=0.660/0.340 within each eye
image. Distortion code similarly uses centres 0.6603788/0.3396213. These are
well away from U=0.5. A Metal coordinate-only probe using the exact profile
construction, with no GPU command buffers submitted, reports:

| Location | Horizontal local rate | Vertical local rate at V=0.5 |
| --- | ---: | ---: |
| Left geometric midpoint, U=0.5 | 1.000 | 1.000 |
| Right geometric midpoint, U=0.5 | 1.000 | 1.000 |
| Left PS VR2 centre, U=0.6603788 | 0.344 | 1.000 |
| Right PS VR2 centre, U=0.3396213 | approximately 0.427 | 1.000 |

The probe's active physical size is 2844×1488, about 26.5% of logical pixel
count, confirming that it is not a uniform 0.3×0.3 map. The misplaced full-rate
regions provide a concrete explanation for coarse straight-ahead detail;
this does not exclude other image-quality effects at the previously unvalidated
0.3 rate and headset resolution.

The fix is projection-aware placement and appropriately sized transitions for
the per-eye full-rate regions, followed by a wearer test. Raising the peripheral
rate can soften the symptom but cannot align the regions. No source, binary,
settings or running game state was changed during this diagnosis. The profile
remains opt-in; optical alignment and rate-0.3 headset quality are not validated.

### Fixed PS VR2 candidate prepared (2026-11-11)

After the user quit the hardware run, the private DXMT source gained an opt-in
`DXMT_STAY_WARPED_FIXED_PROFILE=psvr2` constructor profile, independent of gaze.
It uses the asymmetric PS VR2 FOV, 32 horizontal samples per eye and 32 vertical
samples, with a requested ±20° full-rate core and smooth transition to the outer
rate at ±35°. The generic image-centred profile remains the default. The native
module builds, and Metal coordinate checks at three resolutions and four rates
pass: both eyes retain full rate across ±15° on each axis; mappings are monotonic
and all-1.0 maps are identity. Physical tile rounding means the requested outer
rate is approximate, particularly at small resolutions.

At 5600×2856 / 0.3, the new physical map is 3280×1728 (35.4% of logical area),
versus the original 2844×1488 (26.5%). This deliberately spends more pixels on
a correctly placed protected region; it is not a performance measurement.
Source, standalone validation, patch `0021-fixed-psvr2-profile.patch` and integration
notes are in the sibling checkout (`docs/dxmt-fixed-psvr2.md`). The candidate was
built and saved separately, but not deployed while Claude develops gaze support.
No headset test was launched. Worn image quality and application reconstruction
remain user-validation gates.
