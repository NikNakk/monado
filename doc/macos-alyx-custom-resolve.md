<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Alyx custom resolve diagnosis — 21 October 2026

**Follow-up:** an automated Alyx capture now exposes a specific driver defect:
fixed-function depth/stencil resolves are suppressed alongside custom color
resolve. The candidate fixes an owned reproduction in all 16 mono/stereo,
two/four-sample, STORE/DONT_CARE and suspend/resume configurations. A two-minute
Alyx run completes without a recorded crash or device loss; visual restoration
of the main scene is still unverified. See the follow-up below. The initial
color-only results and limitations remain useful historical evidence.

The user confirmed that disabling `VK_EXT_custom_resolve` through Alyx's own
`-vulkan_disable_ext_custom_resolve` switch restores the missing main scene on
KosmicKrisp. This isolates a rendering path, not a particular driver bug.
The working `run-alyx-kosmickrisp` retains that fallback; MoltenVK stays the
original default. No driver implementation change is justified by the tests
below yet. No installed runtime/service, game executable or graphics setting
was changed by this investigation.

## Changes and revisions

- Klepton branch `codex/alyx-custom-resolve-trace`, revision
  `11a1525032d675d93a6fc6fd75615976c15f4829`, based on `32038c2`.
  New opt-in `KL_VK_CUSTOM_RESOLVE_TRACE=1` observes the strict native path.
- Separate constrained xrizer host:
  `~/Code/alyx-native/build/host-custom-resolve/alyx_host`, using unchanged
  VeRantyx `5c7fb013b0bb1e6cc4ae2609e919f53bfa0d4a7b` host sources.
- Unchanged KosmicKrisp interoperability prototype:
  `c8e17c9784475a745e7be26fb5d4cd2313b9197c` on `claude/kk-metal-objects`.
  Its embedded version still says Mesa 27.0.0-devel git-0c9a1f072e.
- Unchanged MoltenVK 1.4.2, Apple M5, as the baseline control.
- Monado branch `codex/alyx-custom-resolve-investigation`, based on
  `0f348ef23`, contains the owned probe, launcher, evidence and this note.
  No Monado runtime code or Mesa source was changed in this step.

[Initial test identities and numeric results](alyx-valve-lua-results/custom-resolve/initial-tests.json)
include actual binary SHA256 hashes. Full owned-test logs and build logs live
permanently under `~/Code/alyx-native/validation/custom-resolve/`; the probe and
trace shim are in `~/Code/alyx-native/build/custom-resolve/`. Source worktrees
and local Git mirrors also live permanently under `alyx-native`.

## Actual results

| Implementation | Dispatch | Pixel cases passed |
|---|---|---:|
| KosmicKrisp prototype | Vulkan loader directly | 10/10 |
| Same KosmicKrisp | New Klepton strict observer through same loader | 10/10 |
| MoltenVK 1.4.2 | Vulkan loader directly | 4/4 baseline; custom resolve unavailable |

Each case verifies all 1,024 output pixels. A fullscreen fill writes a known
color, then an owned input-attachment shader permutes its components. This
distinguishes successful shader execution from a fixed-function average or a
copy of untouched input. Both STORE and DONT_CARE source store operations are
tested. Legacy render passes cover one- and four-sample sources with and
without the custom resolve subpass flag. Dynamic custom resolve covers a
four-sample source and a single-sample destination. All shaders pass
`spirv-val`; all GPU runs use Metal API validation without reported errors.

An exploratory single-sample *dynamic* custom resolve returned black pixels,
but its source violates
[VUID-VkRenderingAttachmentInfo-imageView-06861](https://docs.vulkan.org/refpages/latest/refpages/source/VkRenderingAttachmentInfo.html).
It is excluded from the test matrix and is **not evidence of a driver bug**.
No Vulkan validation layer is installed locally. The matrix is deliberately
narrow: it does not validate depth, stereo/multiview, mixed formats, sample
variation, custom sample locations, secondary buffers or Alyx's shaders.

The owned fake-dispatch fixture also passes in three modes: observation off,
resolve observation alone and resolve plus existing FDM observation. It checks
exact original pointers/pNext, native failures and partial pipeline outputs,
core/KHR aliases, NULL custom resolve info, absent functions and changed native
dispatch targets. In each mode all 157 calls reach the fake driver, including
draws after the logging budget expires. Existing strict-boundary and FDM
observation regression scripts pass. The separate host builds successfully;
its only reported compiler warning is the existing deprecated `tmpnam` use.

These are correctness checks, not a performance comparison. Queue waits and
CPU pixel readback occur only in the owned probe, not in Alyx's rendering path.

## What the new observer captures

The strict path otherwise dispatches these rendering commands directly to the
selected Vulkan implementation. The observer records image and image-view
creation, attachment formats/sample counts/layouts/load/store operations,
legacy render-pass attachments/subpasses, dynamic rendering attachments and
input mappings, `VkCustomResolveCreateInfoEXT`, pipeline depth/sample state,
shader module associations and enumerated original shader metadata. It does
not dump or modify game shader bytes.

It records actual `vkCmdBeginCustomResolveEXT` execution separately from
function lookups, plus pipeline binds and direct draws within a custom resolve
on the same recording thread. Budgets prioritize custom pipelines/rendering
separately from ordinary work and emit explicit exhaustion notices. Observation
does not change requested features, objects, commands, results or missing
function answers. It preserves native dispatch targets; a different target is
left unwrapped. It is not a complete capture/replay layer, and indirect draws,
secondary buffers and cross-thread command-buffer handovers need additional
instrumentation if Alyx uses them. Finite image/view budgets may also require
targeted follow-up after the first capture.

## Next user capture

With Steam signed in, run:

```sh
~/Code/alyx-native/run-alyx-custom-resolve-trace \
  --output ~/Code/alyx-native/runs/kk-custom-resolve-1 --timeout 180 \
  -- +vr_fidelity_level_auto 0 +vr_fidelity_level 0
```

This separate diagnostic launcher deliberately leaves custom resolve enabled.
The grey/missing main scene may recur. Reach the menu, wait about 20 seconds,
then quit using Ctrl+C. Loading a save is unnecessary if the menu already shows
the failure. The output directory must be new. The paired service, xrizer
timing implementation and backend configuration match the working KK candidate.
The launch command passes dry-run validation without starting a service/game;
the ordinary working fallback launcher remains unchanged.

Inspect `backend/run.log` for `resolve-observe schema=1 strict-passthrough`, actual
custom resolve calls, attachment identities and corresponding pipelines. Use
that observed configuration to extend the owned probe until it reproduces the
missing output, then fix the responsible dispatch/compiler/render-pass handling.
If the actual path is more substantial, retain the fallback and report the
specific blocker before implementing broader emulation.

## Rebuild and repeat the owned test

```sh
root="$HOME/Code/alyx-native"
experiment="$HOME/Code/monado-2/build/review/kk-fd-guard/experiments/alyx-vulkan/custom-resolve"
sh "$experiment/build.sh" "$root/build/custom-resolve" \
  "$root/workspace/klepton-custom-resolve"

VK_DRIVER_FILES="$root/build/kk-metal-objects/src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json" \
  MTL_DEBUG_LAYER=1 "$root/build/custom-resolve/probe" "$root/build/custom-resolve"

KL_VK_STRICT=1 KL_VK_CUSTOM_RESOLVE_TRACE=1 \
  KL_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.dylib \
  VK_DRIVER_FILES="$root/build/kk-metal-objects/src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json" \
  MTL_DEBUG_LAYER=1 "$root/build/custom-resolve/probe" "$root/build/custom-resolve" \
  "$root/build/custom-resolve/shim.dylib"

VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
  MTL_DEBUG_LAYER=1 "$root/build/custom-resolve/probe" "$root/build/custom-resolve"
```

No general FDM implementation has begun. Once custom resolve is diagnosed,
resume the owned stereo color/depth and density-map reconstruction experiment;
custom resolve is not a prerequisite for that experiment. Vulkan's custom
resolve source/destination and render-pass semantics are described by the
[Khronos extension proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_custom_resolve.html).

## Automated capture and fixed-depth candidate

The user authorized running Alyx while unavailable. The first attempt started
successfully but produced no observer records: `launch_xrizer.py` deliberately
filters inherited `KL_*` variables, including the new tracing setting. That run
was stopped cleanly (exit 130), not used as an API trace. The previous dry-run
check had verified the command but missed this environment propagation issue.

The correction is an explicit `--custom-resolve-trace` launcher option that sets
the sanitized child environment, records the selection and rejects an old host.
It is committed with DCO as `c9c93b3` on VeRantyx branch
`codex/alyx-custom-resolve-launch` in `workspace/verantyx-custom-resolve`.
Opt-out explicitly sets the variable to zero even if inherited as one. The trace
launcher now invokes that isolated script. Dry-run assertions check both the
manifest setting and child environment; incompatible-host rejection also passes.

The second run (`runs/kk-custom-resolve-2`) records actual custom resolve calls.
Alyx uses two-sample, two-layer stereo arrays, viewMask 3, dynamic rendering
with suspend/resume, B10G11R11_UFLOAT_PACK32 color and D32_SFLOAT_S8_UINT depth.
Color uses CUSTOM resolve, whereas depth and stencil use SAMPLE_ZERO. The
resolving shader metadata includes multisampled input attachments at indices
0 and 1, three image-read instructions and FragDepth/ViewIndex decorations.
The 180-second capture ends at its configured timeout (124), without a recorded
fault/panic/device loss. The observer reaches its explicit 128-command custom
resolve budget. Direct `CmdDraw` records are absent; draw tracing does not yet
cover all indexed/indirect/secondary forms and is not a complete replay.

### Reproduced defect and change

`kk_CmdEndRendering2KHR` originally disables **all** meta resolves whenever the
rendering flags include CUSTOM_RESOLVE. KosmicKrisp separately forces depth and
stencil resolves through its meta path because of a known Metal synchronization
issue. The combination therefore drops ordinary depth/stencil resolves whenever
color is custom-resolved. `kk_CmdBeginCustomResolveEXT` calls this ending path
while switching to the shader-resolve targets.

The [candidate patch](../experiments/alyx-vulkan/custom-resolve/mesa-fixed-depth.patch)
filters CUSTOM on each attachment when choosing meta work, preserving ordinary
resolves on other attachments. The global suppression now applies only when
rendering is suspended. Custom attachments are still resolved by the shader;
they are never passed to the fixed-function meta resolver. No shader changes,
extension spoofing, Monado accommodation or additional compositor is involved.

The owned probe initializes resolved depth to 0.875, draws depth 0.375, then
performs custom color plus SAMPLE_ZERO depth resolve. With the original driver,
all 16 configurations leave the sentinel 0.875; the candidate produces 0.375
for every pixel. Stereo cases also use different per-eye colors to catch layer
mix-ups. The matrix covers two/four samples, one/two views, STORE/DONT_CARE and
uninterrupted/suspended-resumed rendering. It uses RGBA8 and D32 rather than
claiming to replay Alyx's full packed-color/depth-stencil shaders.

[Recorded identities and results](alyx-valve-lua-results/custom-resolve/depth-fix-results.json):

| Test | Original KK | Candidate KK |
|---|---:|---:|
| Color checks, including 16 mixed color/depth configurations | 26/26 | 26/26 |
| Fixed-depth checks in those mixed configurations | 0/16 | 16/16 |
| Same matrix through strict Klepton observation | not repeated | all pass |
| Existing Metal object/shared-event checks | prior baseline | 26 pass, zero failures |
| xrizer with matching temporary Monado, timeline enabled | prior baseline | 180 frames, zero errors |

The current MoltenVK control still passes all four available color baseline
cases; custom resolve is unavailable. All GPU probe runs above enable Metal API
validation; no validation errors were reported. No performance result is inferred
from correctness probes or instrumented game startup.

### Candidate build and gameplay

Mesa source is isolated at `workspace/kk-custom-resolve`, branch
`codex/custom-resolve-fixed-depth`, based on c8e17c9. Only `kk_cmd_draw.c` differs.
Its exact patch is versioned in this Monado branch; the Mesa worktree itself
remains uncommitted. No Mesa contribution or AI-written upstream communication
has been made. The original Claude worktree remains clean.

`build/kk-custom-resolve-final` contains a **single-object overlay build**, not a
clean Mesa build: a new command-draw object replaces the old one in an isolated
copy of libkk.a, linked against the existing tested dependencies. The baseline
archive is hash-checked unchanged. `build-overlay.py` records commands, source,
base archive and final library hashes in `build-identity.json`. A full clean
build remains a promotion gate. Recreate into a new output directory with:

```sh
python3 "$experiment/build-overlay.py" "$root/build/kk-metal-objects" \
  "$root/workspace/kk-custom-resolve" "$root/build/kk-custom-resolve-another"
```

`runs/kk-custom-resolve-fixed-1` uses that candidate and actual custom resolve
for 120 seconds, ending at the configured timeout (124). Device creation succeeds;
custom resolve commands execute; no fault, panic or device loss is recorded.
This is evidence of process/API operation, not visual confirmation of the scene.
All temporary service jobs are removed by their runner.

For the later visual check, a separate launcher uses the ordinary successful
GPU-timing host without the custom metadata observer:

```sh
~/Code/alyx-native/run-alyx-kosmickrisp-custom-resolve \
  --output ~/Code/alyx-native/runs/kk-custom-resolve-visual-1 --timeout 300 \
  -- +vr_fidelity_level_auto 0 +vr_fidelity_level 0
```

Check the main environment, then the usual save and both eyes when available.
The existing `run-alyx-kosmickrisp` still disables custom resolve; MoltenVK remains
default. Do not promote this candidate or infer a performance gain until visual
testing and a clean driver build pass. No substantial FDM implementation begins
at this point.
