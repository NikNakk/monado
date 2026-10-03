<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# Inherited compositor audit — 2026-10-03

This review covers the inherited macOS compute-renderer, Vulkan allocation,
distortion and compatibility changes associated with Kyle Graehl's bring-up,
against the locally available upstream `22d5c936c` and their introduction
commits. It is not an exhaustive review of all his commits or a claim about
the latest upstream checkout. Git commit dates describe the inherited history,
not when Nick started working on this fork. The investigation used `codex/macos-shared-tracking`, based on `2276cfba9`
with the later per-thread scheduling fix `1b400a8e7`. Its fixes and evidence
are now packaged for integration into `macos-upstream-clean`.

The follow-up cleanup described below is now implemented; the findings section
records what was present at the start of this audit.

## What this investigation actually changed

- Selected CADisplayLink as the default pacing source, retaining CVDisplayLink
  as a diagnostic/automatic fallback. Kept asynchronous drawable acquisition
  and Metal presentation. Rendering on the CA callback thread and the deferred
  CA run-loop revision were tested and removed after worse results.
- Added an opt-in, coherent version-2 PS VR2 tracking snapshot and client-local
  prediction, removing synchronous pose RPC from that compositor path. The
  service still acquires the sensors; this does not move controller tracking
  or all application pose queries into the client.
- Corrected trace buffering and added acknowledged flushes, sensor-health
  checks, moving-head/workload capture tools, and comparisons between queried
  poses and the poses actually consumed by the renderer.
- Removed the submitted-pose override from the macOS compute path. Previously,
  a projection layer within three compositor periods replaced the fresh
  scanout target with the application's source pose, cancelling rotational
  timewarp until the layer aged past the cutoff. In the low-cost pre-fix run,
  source and target matched on 96.8% of compared rows; both moving-head runs
  after the fix retained the queried target on every compared row. The user
  reported that regular judder disappeared.
- Enabled the existing per-image GPU-reuse timeline protection for local
  Metal swapchains, including direct and ordinary local paths. App readiness
  and output-presentation shared-event waits were already active: the missing
  direction was waiting for the compositor's previous GPU sampling before
  allowing the app to overwrite an image. A logging-only check recorded
  2,111 nonzero reuse waits and 2,111 completions. The user confirmed that this
  fixed partly black frames/noise. This omission belongs to later local Metal
  work; the audit does not attribute it to Kyle.

Build and all 36 macOS CTests passed after the reuse fix; the 15 Python
diagnostic checks passed. Linux CI has not been run for these changes.
Occasional stalls remain. See the [timing evidence](macos-psvr2-timing-diagnostics.md)
and [judder ledger](macos-psvr2-judder-evidence.md) for captures and limits.

## Findings before the cleanup

### 1. Fix inherited pixel-readback diagnostics

`62c8d7877` added source sampling and `0b42a7f47` added target sampling.
`1f09aca40` subsequently gated log output, but did not gate GPU work.
`render_resources.c` still allocates/maps the sample buffers on macOS;
`comp_render_cs.c` still records source copies and barriers for projection
layers; `render_compute.c` still records target copies and barriers each
dispatch. Turning off `XRT_COMPOSITOR_LOG_APPLE_SAMPLES` only suppresses printing.

There are two correctness problems in addition to unnecessary work:

- Source sampling assumes four-byte texels (buffer size 8, offsets 0 and 4),
  without checking the source format. Larger texels exceed the buffer range
  and can violate copy-offset alignment. Both source and target logs assume
  RGBA8 byte interpretation. Make diagnostics format-aware or restrict them
  explicitly to supported four-byte color formats.
- The macOS target diagnostic hard-codes `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`
  instead of honoring `target_final_layout`; it also replaces the ordinary
  final-layout transition with the diagnostic block. Disabling buffer
  allocation alone would therefore remove that transition. Restore the normal
  transition when diagnostics are off, and honor the requested layout when
  sampling is on. The current PS VR2 target requests PRESENT_SRC, so this is
  not evidence of a current headset layout failure.

Recommendation: gate allocation and copy/barrier recording with the existing
sample option, and fix format/layout handling together. Measure performance
afterward; no trace currently proves that these copies explain the remaining
stalls. The follow-up now gates allocation/recording with the existing option, restricts
readbacks to four-byte RGBA/BGRA color images with transfer-source usage, fixes
BGRA reporting and honors the requested final layout.

### 2. Restore upstream pose selection on non-macOS too

The confirmed override originated in `816d372cf`. The initial hardware-validated fix was guarded
to macOS; the same override remained under `#ifndef XRT_OS_OSX`, with the
same source/target confusion there. The follow-up removes that remaining block/helper too, restoring upstream
pose selection on all platforms. Linux build/run validation remains pending. It is not an upstream bug.

### 3. Review scope and remove dormant compatibility leftovers

`3f92cc854` changed the common sampler border to transparent black and the
common distortion shader to preserve source alpha. These changes affect Linux
as well as macOS. Retain the required Apple alpha behavior, but review whether
it should be target-specific before restoring upstream behavior elsewhere;
this audit found no demonstrated alpha regression.

`24aec54c9` added temporary WiVRn fields/layout declarations, including
`view_cbcr` and `render_compute_distortion_foveation_data`. Repository searches
find their declarations but no users. They are cleanup candidates, not proven
runtime bugs. Likewise the obsolete macOS-plus-FD handle-export branch returns
invalid handles with success, but normal macOS builds now use IOSurface
handles. Remove obsolete scaffolding rather than reviving the FD workaround.

## Implemented follow-up and validation

- The source/target override and its helper are gone on all platforms. The
  user clarified that it was deliberate work toward a remote renderer. That
  explains the original purpose, but does not make app-source poses suitable
  native scanout targets. Remote targets can explicitly disable Monado
  timewarp when their downstream compositor owns reprojection; they should
  not silently replace target poses in the generic compute renderer.
- Pixel readbacks allocate no debug buffers and record no copies/barriers by
  default. Opt-in samples check format, transfer-source usage and source
  sample count. Unreported target usage disables target sampling safely.
- Alpha preservation is a shader specialization enabled on macOS; other
  platforms regain upstream opaque distortion output and opaque sampler
  borders. No demonstrated Linux alpha regression was needed to justify
  restricting an Apple adaptation to Apple.
- Removed unused WiVRn fields/layout declarations and the obsolete
  macOS-plus-FD allocation/export special cases. Kept active IOSurface paths.
- The macOS build is warning-free and all 36 CTests pass. The pipeline
  fixture now requests MoltenVK portability extensions; with desktop access
  and the installed ICD selected, all eight GPU pipeline cases run and pass
  (78 assertions), including distinct opaque/alpha output keys. No new headset run or measured
  stall improvement is claimed. Linux validation remains pending: Docker is
  installed locally but its daemon is unavailable.

## Changes to keep

- `1edd29354`: initializes compute fast-path pose inputs. It already repaired
  an earlier inherited uninitialized-input problem; reverting it reintroduces
  that problem.
- `9a04b9763`: keeps Vulkan `pNext` nodes alive through the Vulkan call. The
  earlier block-local chain nodes were invalid; retain the lifetime fix.
- MoltenVK portability enumeration/subset support and the IOSurface/Metal
  import/export paths: needed platform adaptations.
- Separate storage views (`2e3e5a94e`): preserve the compute/storage-format
  distinction rather than broadly reverting target creation.
- Identity distortion for genuine NONE-distortion devices (`0f7c8db59`): the
  current predicate requires NONE, excludes COMPUTE and requires no compute
  distortion callback. PS VR2 advertises COMPUTE and has that callback, so this
  bypass does not explain its judder.
- Native timewarp enabled by default. The inherited blanket macOS ATW disable
  (`ff1cde9e4`) has already been narrowed to the opt-in WiVRn diagnostic
  `XRT_COMPOSITOR_FORCE_ATW_OFF_ON_APPLE`; do not restore the blanket disable.

Prefer selective restoration of upstream algorithms over reverting whole
bring-up commits that mix workarounds, necessary platform support and later
corrections. Do not revert upstream timing/renderer changes merely because
the inherited diagnostics failed to follow a newer interface.

## Follow-up regression: output-image visibility

The user reported black corruption again with samples off. Restoring samples
removed the noise and the log confirmed 453 reuse waits/completions. Only
target samples were active. The macOS path now retains a full GPU output
image barrier even when sample copies are off; the ordinary TOP_OF_PIPE
destination had not retained that memory dependency. This additional change
is built and passes 36 CTests, but headset confirmation remains pending.
See the [timing evidence](macos-psvr2-timing-diagnostics.md#readback-on-result-and-explicit-output-barrier--2026-10-03).

The user subsequently confirmed the explicit output barrier fixes black noise
with readbacks off. The earlier pending headset confirmation for this barrier
is now satisfied; Linux validation and quantitative residual-stall work remain
separate.
