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
