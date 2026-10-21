<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Alyx custom resolve on KosmicKrisp: fixed — 21 October 2026

**Result:** with `VK_EXT_custom_resolve` enabled, native Half-Life: Alyx now
renders its scene on KosmicKrisp. The user confirmed the menu background and
logo, the intro video and in-game rendering (a locker scene). Previously the
scene was black, with only the hands and menu panels visible.

This continues the [custom resolve diagnosis](macos-alyx-custom-resolve.md).
Codex's fixed-depth change is necessary but was not enough on its own.

## Cause

The [extension proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_custom_resolve.html)
says reads from a resolving fragment shader's input attachments "will still read
from the multisampled color or depth attachment". KosmicKrisp instead lowered
those reads to ordinary descriptor reads.

Alyx doesn't bind the multisampled images in those descriptors. Across a whole
run, the only input-attachment descriptor it writes is a **single-sample 2D**
view. Turnip (the Steam Frame driver) reads input attachments from tile memory,
so it never looks at that descriptor. On KosmicKrisp, Alyx's resolve shader
read a 2-sample array texture through that placeholder and got zeros. The
resolved scene came out black, and the hands and menu, drawn afterwards, showed
on top.

## How it was found

Each step used Alyx with custom resolve on, through the paired temporary Monado
service. Runs are under `~/Code/alyx-native/runs/claude-kk-cr-*`.

| Experiment | Result | Meaning |
|---|---|---|
| Metal API validation (`MTL_DEBUG_LAYER=1`) | Only a residency-set message, also present in the working fallback | Not an API misuse |
| Alyx-shaped probe (2-sample stereo multiview, B10G11R11 + D32S8, 3-segment suspend/resume, CUSTOM colour, SAMPLE_ZERO depth/stencil) | Passed on Codex's build | KK's resolve mechanics are correct |
| `KK_CUSTOM_RESOLVE_DEBUG=average`: replace Alyx's resolve output with a meta average resolve | Scene visible | The multisampled image holds the scene |
| `KK_CUSTOM_RESOLVE_DEBUG=magenta`: clear the resolve target before Alyx's resolve draws | Still black | Alyx's resolve draws run and write black |
| `MESA_KK_DEBUG=msl` | Resolve shader is a tonemapped 2-sample average reading a `texture2d_ms_array` | Shader logic is sound; its input is wrong |
| `KK_CUSTOM_RESOLVE_DEBUG=log` | Only single-sample 2D views are written as input attachments | Alyx binds a placeholder |

The probe then reproduced the black output exactly with `--dummy-descriptors`,
which binds a single-sample 2D placeholder as Alyx does.

## Changes

Mesa branch `claude/kk-custom-resolve-debug` in `workspace/kk-cr-debug`,
on top of `c8e17c9` (`claude/kk-metal-objects`). Patches are in
[`experiments/alyx-vulkan/custom-resolve/input-attachments/mesa-patches`](../experiments/alyx-vulkan/custom-resolve/input-attachments/mesa-patches).

| Commit | Change |
|---|---|
| `467b892` | Codex's fixed-depth change, committed unchanged as the base |
| `54f3eab`, `f09f2fe` | `KK_CUSTOM_RESOLVE_DEBUG=log\|magenta\|average` diagnostics; inactive unless set |
| `167f74a` | Create input-attachment textures for image views whose usage is input-attachment only. A separate real bug: such views got a zero resource ID |
| `1574cde` | **The fix.** Fragment shaders in custom-resolve pipelines take input-attachment textures from a new root-table array, chosen through the pipeline's input attachment index map. `vkCmdBeginCustomResolveEXT` fills it with the multisampled colour, depth and stencil attachments. The custom-resolve flag and colour input map join the fragment shader key |

Stencil input reads in a custom resolve would use the combined depth/stencil
texture. Alyx doesn't read stencil there, so this isn't handled yet.

The Mesa tree's `AGENTS.md` asks for AI-written commits to be labelled; these
carry `Generated-by: LLM`. Repackage them with human-written messages before any
upstream submission.

## Verification

- Probe ([`input-attachments/`](../experiments/alyx-vulkan/custom-resolve/input-attachments),
  build with `build.sh OUT`): every variant passes on the fixed build with Metal
  validation on. The variants cover 1 or 3 segments, the scene drawn in each
  segment, B10G11R11 or RGBA8, with or without the depth input, restricted views,
  state bound before the switch, and dummy descriptors.
  `--dummy-descriptors` and `--restricted-views` fail on Codex's build.
- The 26-check Metal objects interop test still passes.
- Alyx: `claude-kk-cr-fix-2` logs "Using VK_EXT_custom_resolve", with no driver
  errors, panics or device loss; the user confirmed the picture.

Launch it with the copy of the debug launcher in the experiment folder. It is
the same as `run-alyx-kosmickrisp-custom-resolve`, pointed at
`build/kk-cr-debug`:

```sh
~/Code/alyx-native/kk-tests/run-alyx-kk-cr-debug \
  --output ~/Code/alyx-native/runs/kk-cr-fixed-N --timeout 1800 \
  -- +vr_fidelity_level_auto 0 +vr_fidelity_level 0
```

The shared `run-alyx-kosmickrisp*` launchers are unchanged; they still pass
`-vulkan_disable_ext_custom_resolve`.

## Still open

- Metal validation reports "attachment texture not added to any residency set"
  every frame, with or without custom resolve. It hasn't caused visible problems,
  but KK should add those textures.
- No performance comparison has been made between custom resolve on and off.
- The locker-handle flicker seen on both MoltenVK and KK is Alyx's interaction
  highlight, not a rendering fault. Raising the texture streaming settings to
  the Windows values made no difference.

## Earlier work in the same series

[`experiments/alyx-vulkan/kosmickrisp-metal-objects`](../experiments/alyx-vulkan/kosmickrisp-metal-objects)
holds the minimal `VK_EXT_metal_objects` for KosmicKrisp (Mesa branch
`claude/kk-metal-objects`, patches included), its 26-check interop test and
the Monado fence-fd guard patch. That guard makes the Monado client use sync-fd
fences only when the app enabled `VK_KHR_external_fence_fd`. KosmicKrisp reports
sync-fd support, so without it the client called an unloaded `vkGetFenceFdKHR`.
