<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Alyx FDM on KosmicKrisp, stage 2b: PSVR2 headset — 21 October 2026

**Result:** native Half-Life: Alyx runs on the PSVR2 with rate-mapped FDM
(`MESA_KK_EXPERIMENTAL=fdm,fdm_rate`, `+vr_fdm 1 +vr_render_subsampled_depth 1`).
Every FDM pass is shaded through a Metal rasterization rate map, and the user
confirmed the picture is correct with auto fidelity on. The frame rate is still
far too low, at 4–12 fps. FDM does not change it measurably, so fragment shading
in FDM passes is not the limit. This follows the
[stage 2 design and 2a results](macos-alyx-kosmickrisp-fdm-stage2-design.md).

These are the first runs of macOS xrizer on a real headset. Two problems not
related to FDM came up first and are recorded below.

## Headset launcher

`experiments/alyx-vulkan/kosmickrisp-fdm/headset/run-alyx-kk-headset` (copied
from `kk-tests/`) uses the guarded Monado build (`build/monado-kk-fd-guard`) as a
temporary launchd service. It differs from the desktop launchers in four ways:

- **Device:** no simulated device, desktop window or QWERTY input.
- **PSVR2 setup:** the PSVR2 environment (6DoF calibration, camera streams, floor
  height) comes from the installed service's LaunchAgent.
- **Safety check:** it refuses to start if another `monado-service` is running.
- **Alyx option:** it adds `+vr_symmetric_fov 0` (see below).

Monado selected the PSVR2 at 120 Hz in every run. The PSVR2 OpenXR test app
renders correctly through this service, as it does through the installed one.

## Problems on the headset not related to FDM

- **Cut and misaligned eyes:** with Alyx's default `vr_symmetric_fov`, each eye
  showed a hard vertical edge and the eyes did not line up. This happened on
  MoltenVK and KosmicKrisp, with in-process and service compositing, with FDM
  off and without the hidden-area stencil. The PSVR2 FOV is asymmetric (left eye
  −61.5° to +43.5°, ±53°). Native Alyx renders a symmetric FOV, but xrizer submits
  full texture bounds with the asymmetric FOV. `+vr_symmetric_fov 0` fixes it.
  xrizer works with Windows Alyx under Wine, so that path must handle symmetric
  FOV differently; this was not investigated further.
- **Wrong fidelity table:** Alyx loads `vr_fidelity_levels_deckard.csv`, so even
  level 0 is 2112×2208 to 3360×3360 per eye. That is about 10 times the desktop's
  672×768.

## FDM problems found on the headset

Mesa commit `22b4fc7` on `claude/kk-fdm-stage1`
([patch 0005](../experiments/alyx-vulkan/kosmickrisp-fdm/mesa-patches)):

| Symptom | Cause | Fix |
|---|---|---|
| All passes at full rate | Depth not subsampled | `+vr_render_subsampled_depth 1` |
| Crash in `mtl_render_pass_descriptor_set_rate_map` | Two recording threads rebuilt the same map, and one released it while the other set it | Lock around map creation and use; rebuild only when the density data changes |
| World warps as you look (menu fine) | Several layout and timing bugs, below | Below |

Alyx's headset passes use dynamic resolution. Render areas from 2112×2208 to
3360×3360 sit in 3360×3427 attachments (4× MSAA), or in 2184×2227 attachments
(2× MSAA) whose colour resolves into the 3360×3427 image. Alyx sizes its density
maps to the render area, with 32 px texels and a fresh map for each fidelity
level. It reads depth through single-layer views of each eye, and multisampled
depth with `texelFetch`.

The warping had four driver causes. All four now have a rate-probe mode that
fails without the fix and passes with it:

| Cause | Fix | Probe mode |
|---|---|---|
| Map stretched over the attachment instead of 32 px per density texel | Lay out the map in density texels; extend the edge texels | `--area` |
| Per-image parameter block rewritten on the CPU at record time, so frames already recorded decoded with the next fidelity level's map | One parameter block per map version; GPU copy into the image's block before its render pass | `--switch` |
| Sampler mapping used the map's screen size, not the image's | Image size in the sampled-image descriptor | `--mixed` |
| Layer taken from the coordinate only, and `texelFetch` reads not mapped | View base layer in the descriptor; map `txf`/`txf_ms` (not input attachments) | `--eye-maps --views`, `--eye-maps --fetch` |

Headset A/B runs isolated the switching cause: full-rate `fdm` showed no warping,
and neither did `fdm_rate` at a fixed fidelity level. The other fixes also apply
to Alyx's paths, although the density layers turned out identical for both eyes.

Known limitation: the last pixel row or column of a render area can read an
unshaded physical pixel, where a coarse pixel straddles the edge. In Alyx this
is the outer image edge.

## Verification

- **Rate probe** ([rate-probe](../experiments/alyx-vulkan/kosmickrisp-fdm/rate-probe),
  `build.sh OUT`): every mode passes with `fdm,fdm_rate` under Metal API
  validation. That includes `--custom-resolve --samples4 --area --fetch --switch`
  and `--mixed --area`, with exact centres and peripheral error ≤ 2.24 px. Fragment
  work is 34.8% of full rate for the whole target, and 36% of the rendered area
  with `--area`.
- **Regression:** the stage 1 FDM probe, the custom resolve probe and the Metal
  interop test pass with no flags, `fdm` and `fdm,fdm_rate`.
- **Headset:** `runs/claude-headset-fdm-rate-auto-2`: every FDM pass rate mapped,
  no faults, picture confirmed correct by the user.

## Frame rate

| Run, 30 s windows | 0–30 s | 60–90 s | 150–180 s |
|---|---|---|---|
| FDM off (`claude-headset-asymfov-1`) | 8.4 fps, GPU 55 ms | 8.6 fps, 92 ms | 4.2 fps, 155 ms |
| Rate-mapped FDM (`claude-headset-fdm-rate-auto-2`) | 9.4 fps, 62 ms | 9.0 fps, 93 ms | 3.8 fps, 149 ms |

The routines differ and auto fidelity is active, so this is not a controlled
comparison. Still, both runs slow down steadily over three minutes, and FDM makes
no visible difference, so the limit is elsewhere. After the runs, swap was at 6.7
of 7 GB on a 24 GB machine, and no thermal warnings were recorded. The 4× MSAA
targets are very large and fully backed, because custom resolve reads them back.
Next steps:

- Track memory and swap during a run.
- Find which passes take the GPU time.
- Run a controlled FDM on/off comparison at a fixed fidelity level and a lower
  render size.

## Frame-rate investigation — 22 October 2026

Tools are in [`kosmickrisp-fdm/perf`](../experiments/alyx-vulkan/kosmickrisp-fdm/perf).
`memsample.py` samples free, wired and compressed memory, pageouts, swap, the
GPU driver's `PerformanceStatistics` (allocated and in-use memory, utilisation,
split scenes) and Alyx's footprint every 2 s. `headset-profile-run` adds that,
plus optional Metal System Traces, to a headset run. Mesa `e9a3c7e` (patch 0006)
adds `KK_LABEL_PASSES`, `KK_ENCODER_STATS` and logging of Metal command queue
errors.

### With Alyx's Deckard fidelity table

Alyx loads `vr_fidelity_levels_deckard.csv`. On the PSVR2 it renders 2184×2227
(2× MSAA) to 3360×3427 (4× MSAA) per eye, and is GPU bound at about 95%.

| Time | Free | Compressed | Pageouts | GPU allocated | Alyx footprint | GPU busy |
|---|---|---|---|---|---|---|
| 8 s | 1.8 GB | 1.5 GB | 366k | 6.1 GB | 5.4 GB | 99% |
| 34 s | 78 MB | 1.4 GB | 366k | 7.1 GB | 6.3 GB | 97% |
| 63 s | 47 MB | 6.7 GB | 485k | 8.1 GB | 10.2 GB | 81% |

The 24 GB machine runs out of memory and pages. Both profiled runs then crashed
with `MTL_COMMAND_QUEUE_ERROR_TIMEOUT` (device lost; xrizer panics afterwards).
Even at level 0, GPU time was 130–140 ms per frame.

A Metal trace of Alyx's process put compute at 79% of its GPU time, fragment at
16% and vertex at 5%, which is why FDM made no difference there. No compute
encoder ended a render pass. Most compute encoders come from KK's built-in
kernels (query reset and copy, predicated draws) and from copies; Alyx's own
dispatches are rare. In a desktop trace the built-in kernels averaged about
0.4 ms each, and every KK encoder ends with a full GPU barrier. Instruments did
not associate most render-pass intervals with their labels, so a complete
per-pass breakdown was not possible.

### With a low-resolution override table

Alyx reads `game/hlvr/vr_fidelity_levels_override.csv` when it exists. The
[override used](../experiments/alyx-vulkan/kosmickrisp-fdm/perf/vr_fidelity_levels_override.csv)
has scales 0.25–0.6 and 2× MSAA. These are paired unattended 300 s runs on the
static menu: fixed level 5 (1344×1344 render area in 1680×1713 targets),
`XRT_MACOS_EXIT_ON_DISPLAY_LOSS=0`, with
`claude-headset-lowres-off-2` (`+vr_fdm 0`) against `claude-headset-lowres-fdm-2`
(`fdm,fdm_rate`, `+vr_fdm 1 +vr_render_subsampled_depth 1`).

| Steady 40–290 s | FDM off | Rate-mapped FDM |
|---|---:|---:|
| Frame rate | 29.9 fps | 33.7 fps |
| GPU time, median / p90 | 25.7 / 32.3 ms | **19.1** / 26.7 ms |
| Frame interval, median / p99 | 33.3 / 52.2 ms | 31.2 / 50.5 ms |
| Alyx footprint / GPU allocation | 4.4 / 4.6 GB | 4.5 / 4.7 GB |
| Paging, swap growth | none | none |

All 53,248 FDM passes were rate mapped. FDM cuts median GPU time by 26%. In both
runs GPU time on the static scene rises over the first three minutes (20 to
29 ms off, 16 to 24 ms on), possibly thermal throttling on this fanless machine;
this was not confirmed. The frame interval is about 4× the 8.33 ms display period.

Unattended runs need `XRT_MACOS_EXIT_ON_DISPLAY_LOSS=0`: with nobody wearing it,
the PSVR2 display turns off, and the service otherwise exits. The headset wrapper
now lets environment variables override the LaunchAgent's PSVR2 settings.

## Encoder overhead and in-pass resolve — 22 October 2026

### Barriers between encoders (Mesa `4563150`, patch 0007)

Alyx records about 160 render passes per frame and a pipeline barrier between
almost every pair (721,950 barriers for 727,011 render passes in a desktop run).
KK ended every encoder with a barrier on all stages and dropped barriers recorded
between encoders. Nearly all of Alyx's barriers are one of two kinds: attachment
and transfer writes (`0x1600`/`0x1500`) before tests, colour output, compute and
transfer (`0x1f00`), with the vertex shader added in about half (`0x1f08`). So the
next pass's vertex work need not wait for the previous pass's fragment work.

`MESA_KK_EXPERIMENTAL=light_barriers`:

- Converts barriers recorded between encoders to Metal stages.
- Applies them at the start of the next render encoder, with no barrier at
  encoder end.
- Keeps full barriers for compute encoders, the start of each command buffer,
  render-pass restarts and splits, resumed passes and the custom-resolve switch.
- Makes `vkCmdWaitEvents2` apply its dependencies.

All probes pass with it. On the desktop menu (`claude-barrier-ab-*-1`, level 5,
FDM on), median GPU time falls from 10.98 to 8.21 ms (p90 from 12.33 to
10.74 ms). It has not yet been checked visually on the headset.

### Custom resolve in tile memory (experiment)

[`tile-resolve/tile_resolve.m`](../experiments/alyx-vulkan/kosmickrisp-fdm/tile-resolve/tile_resolve.m)
renders a 2-layer MSAA target and resolves it in the same render pass with a tile
function. The function reads every sample from the implicit imageblock, gets the
layer from `[[render_target_array_index]]` and writes a texture array. It matches
a two-pass resolve; 4× MSAA needs 16×16 tiles.

| Size, MSAA | Tile resolve, memoryless | Store + resolve pass (KK today) | MSAA memory |
|---|---:|---:|---:|
| 1680×1713×2, 2× | 1.29 ms | 1.14 ms | 90 MB |
| same, rate map | 0.62 ms | 1.04 ms | 90 MB |
| 1680×1713×2, 4× | 0.77 ms | 2.47 ms | 180 MB |
| 3360×3427×2, 4× | 1.56 ms | 8.89 ms | 710 MB |
| same, rate map | 0.61 ms | 3.61 ms | 710 MB |

With a rate map, the last physical row is not covered by the two-pass reference;
those rows are excluded from the comparison.

`KK_SEQ_LOG=1` (Mesa `3d34c42`, patch 0008) shows that this does not carry over
directly to Alyx. Each stereo FDM pass is recorded as three command buffers using
suspend and resume (rendering flags `0x82`, `0x86`, `0x84`), and the custom-resolve
stage is in the last one. The middle segment also contains a `BY_REGION`
colour-attachment-write to input-attachment-read barrier, which KK splits because
the pass is multisampled. Tile memory cannot survive a Metal command-buffer
boundary or a split, so the MSAA images must stay backed. They are stored and
reloaded at every boundary.

Memoryless MSAA would need suspended and resumed passes merged across command
buffers. A tile resolve in the last segment alone would save one MSAA store and
reload per frame, about 0.4 ms at the current settings, but no memory.

## Custom resolve in tile memory and stitched passes — 22 October 2026

Mesa `4cc6e6a` ([patch 0009](../experiments/alyx-vulkan/kosmickrisp-fdm/mesa-patches)),
both opt-in and work in progress.

### Alyx's multisampled FDM pass

`KK_SEQ_LOG` shows each stereo pass as three command buffers joined with suspend
and resume. The rendering flags include custom resolve: `0x82`, `0x86`, then
`0x84`. The colour target and depth are 2× MSAA, transient, cleared on load and
don't-care on store. The colour resolve is custom; depth and stencil use
`SAMPLE_ZERO`, which KK does with a meta pass after the render pass. Alyx's colour
resolve shader is brightness-weighted: each sample is weighted by 1/(1+max(rgb)).

The final segment contains:

- a by-region barrier
- a 4-vertex draw that reads the MSAA colour as an input attachment
- a second by-region barrier
- the custom resolve

KK splits the pass at the first barrier. KK also forces stores and loads at every
segment boundary. With multiview it treats even full-area passes as partial: it
compares the view mask with `layerCount`, which Vulkan ignores for multiview.

### `tile_resolve`

Custom-resolve fragment shaders that read only colour input attachments get a
Metal tile-function variant:

- Input attachment reads become imageblock sample reads.
- Colour outputs become texture writes to the resolve images. Their storage
  handles are in a new root-table field, `custom_resolve_outputs`.
- The layer comes from `[[render_target_array_index]]`.

Tile pipelines are built per pass sample count on first use, because a resolve
pipeline doesn't know the MSAA sample count. In the final segment, the custom
resolve becomes a tile dispatch in the same render pass, using 32×32 tiles at 2×
and 16×16 at 4×. A by-region fragment barrier there is deferred, then split if a
draw follows, or dropped if the resolve follows. Alyx's resolve shader qualifies,
and Alyx renders correctly with it.

On the desktop, ten alternating runs show no measurable change in GPU time
(12.25 against 12.35 ms on average). MSAA is still stored and reloaded at the
segment boundaries.

### Metal 4 suspend and resume

[`stitch_resolve.m`](../experiments/alyx-vulkan/kosmickrisp-fdm/tile-resolve/stitch_resolve.m)
shows that a pass split across three MTL4 command buffers can keep a memoryless
MSAA target in tile memory. It uses `MTL4RenderEncoderOptionSuspending` and
`MTL4RenderEncoderOptionResuming`, and one `commit:count:`. The tile resolve in
the last command buffer then sees all three segments. This holds at 2× and 4×,
at up to 3360×3427 per eye, and with a rate map.

Undocumented constraints found:

- The segments' render pass descriptors must be equal, including load actions
  and tile size.
- Passes with a visibility buffer must use `MTLVisibilityResultTypeAccumulate`.
- Any encoder after the resumed pass in the last command buffer silently breaks
  the result. Encoders before the suspending pass, and separate commits after,
  are fine.

### `stitch_passes`

KK creates suspending and resuming render encoders from the Vulkan flags and
uses accumulating visibility. Equal descriptors are kept by not forcing load on
resumed segments. Suspending encoders don't store.

Submission changes too:

- The joined command buffers are committed together.
- The Metal command buffer is ended after a final resumed pass, and the rest of
  that Vulkan command buffer is committed separately.
- Command buffers submitted between segments, which Vulkan allows when they
  contain no action commands, are committed after the chain.

The custom resolve probe passes in all variants, with and without `tile_resolve`
and `light_barriers`. The fill drawn in segment 0 reaches the resolve only
through tile memory.

Without the deferral, Apple's driver crashed in `mergeRenderEncoders` when an
unrelated command buffer fell inside a chain.

### The `stitch_passes` GPU timeout (Mesa `cfda5f5`, patch 0010)

With the deferral, Alyx hit `MTL_COMMAND_QUEUE_ERROR_TIMEOUT` on its first
stitched chain. Metal validation reported nothing. Bisecting inside KK, with the
probes in
[`tile-resolve/`](../experiments/alyx-vulkan/kosmickrisp-fdm/tile-resolve/):

| Change | Timeout? |
| --- | --- |
| Stitch only the shadow-atlas passes (depth-only, 1×) | yes |
| Stitch only the MSAA eye pass | yes |
| Drop all draws in stitched passes | no |
| Drop only indexed draws | no |
| Drop only indexed draws in the first segment | later, after ~180 commits |
| Queue drains on the CPU before each stitched commit | yes |
| GPU-side queue wait after each stitched commit | yes |
| CPU waits for each stitched commit to finish | no |
| 30 ms CPU sleep after each stitched commit | no |
| Stitched command allocators kept out of the pool | no |

KK returns a command buffer's `MTL4CommandAllocator` to the pool when recording
ends, before the buffer is committed. The next recording then appends to the
same allocator. For ordinary buffers that is fine. For a suspended render
encoder, it races with the driver's merge of the stitched pass, and the GPU
hangs. Anything that slows the CPU after the commit hides it, and fewer commands
make it rarer.

The fix holds the allocators of stitched command buffers until their commit
completes, then returns them to a device-wide list. That alone ran to commit
~27,900. The one remaining timeout was a chain with a barrier split inside a
middle segment, which Metal cannot express. KK now gathers each chain at submit
and re-records chains that break the rules, or are left open at the end of a
submission, without stitching.

Result, desktop, fidelity 5, FDM off, 60 s runs: no timeouts. 5,868 chains were
stitched and 15 re-recorded. GPU time per frame, median of three runs each:

| Configuration | GPU median | p90 |
| --- | --- | --- |
| Stitching off | 13.53 ms | 16.40 ms |
| `stitch_passes` | 13.89 ms | 16.48 ms |

Stitching alone costs about 3%. It is only the enabler for memoryless MSAA.

The shadow probe, `shadow_stitch.m`, did not reproduce the hang. It has no
concurrent recording.

### Left eye losing later segments (Mesa `5476876`, patch 0011)

On the headset, the menu and hands flickered out of the left eye while the
background stayed. The right eye was correct. The background is drawn in the
first segment of the 4-segment eye pass. The menu and hands are drawn in later
segments. There were no queue errors.

The final segment ends in a split. The rest of that Vulkan command buffer,
including the custom resolve and the per-layer depth resolves, is committed after
the stitched group. Metal could start it before the merged pass had stored its
attachments, so the resolve read a stale copy. The queue now signals a shared
event after the stitched group and waits on it before the tail commit. With
that, the user saw the left eye steady in the desktop preview.
`KK_STITCH_LOG=N` logs the encoders of the first N stitched chains.

Desktop timing with the wait, same settings, three runs each, after the laptop
went on a fan stand:

| Configuration | GPU median | p90 |
| --- | --- | --- |
| Stitching off | 11.74 ms | 13.39 ms |
| `stitch_passes` | 11.58 ms | 12.53 ms |

All six runs held 60 fps. The difference is within noise. The fan stand
explains the lower times than the earlier table.

On the headset the user saw both eyes steady. That attended run ran at 2.6 fps,
with frames 383 ms apart, but the cause was not found. Unattended runs straight
after did not repeat it. FDM off, fidelity 5, `+vr_render_subsampled_depth 1`,
150 s each:

| Configuration | fps | GPU median | Frame interval |
| --- | --- | --- | --- |
| Stitching off | 23.7 | 31.7 ms | 42.6 ms |
| `stitch_passes`, no tail wait | 20.5 | 39.9 ms | 47.1 ms |
| `stitch_passes`, tail wait on a plain `MTLEvent` | 17.2 | 47.0 ms | 57.5 ms |
| `stitch_passes`, tail wait on a shared event (as committed) | 22.9 | 32.2 ms | 46.9 ms |
| `stitch_passes`, tail wait on a shared event, rerun | 25.3 | 30.8 ms | 37.7 ms |

Stitching does not cost frame rate on the headset. These runs are below the
earlier 30–33 fps without FDM, which did not use
`+vr_render_subsampled_depth 1`.

### Memoryless targets and stitching

`stitch_resolve.m` with `HEAVY=` draws many small triangles in each segment:

| Target | Triangles | Result |
| --- | --- | --- |
| Memoryless, not stitched | 60M in one pass | OK |
| Memoryless, stitched, 3 segments | 12M per segment | OK |
| Memoryless, stitched, 3 segments | 15M per segment | queue error 1 after ~290 ms |
| Private with load and store, stitched | 60M per segment | OK |

A stitched pass on a memoryless target fails once it has enough geometry. A
single pass doesn't. Alyx's passes are far below that, but the limit needs
watching once MSAA is memoryless.

### Remaining for memoryless MSAA

- **Headset check:** stitching with the tail wait, on the headset.
- **Input-attachment read:** the final segment's read of the MSAA colour must
  come from tile memory instead of a split.
- **Depth and stencil:** the `SAMPLE_ZERO` resolve must be done in the pass. KK
  avoids that because of a Metal synchronisation bug noted in its source, and
  because Alyx's render area is partial.
