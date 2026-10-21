<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Alyx FDM on KosmicKrisp, stage 2: design — 21 October 2026

Goal: make KosmicKrisp shade Alyx's FDM passes at reduced rate using Metal
rasterization rate maps, while keeping the output correct. Stage 1
([full-rate FDM](macos-alyx-kosmickrisp-fdm-stage1.md)) already gets Alyx onto
its FDM path at no cost.

## Measured Metal facts (Apple M5)

The experiments are in [`kosmickrisp-fdm/metal-rate`](../experiments/alyx-vulkan/kosmickrisp-fdm/metal-rate).
They use Alyx's live map: 672x768 per eye, 21x24 texels of 32 px, a full-rate
centre over columns 6-14 and rows 8-15, and 56/255 elsewhere.

| Fact | Result |
|---|---|
| 2-layer rate maps | Supported; layer chosen by `render_target_array_index` |
| Fragment `[[position]]` | **Physical** coordinates (max 439.5x447.5 for 672x768) |
| Viewport | **Screen** coordinates; a 672x768 viewport covers the screen |
| Decoder | `rasterization_rate_map_decoder` maps physical to screen exactly (218.5,219.5 to 336.5,384.5) |
| Fragment invocations, 1 sample per texel edge | 38.2% of full rate (atomic counter) |
| Physical size, 2 samples per texel | 432x416, **34.8%** of full rate; denser sampling gives no further gain |
| Parameter buffer | 49,200 bytes for 2 layers, independent of sample count |
| Granularity | 32x32 physical pixels |

Metal rounds rates up, never down, so a map built from per-texel maxima is
conservative.

## What Alyx needs (from stage 1)

- One FDM attachment per stereo pass: an R8G8 map with 2 layers, uploaded once
  from the CPU, with no offsets.
- Subsampled images: the 2-sample colour target and the single-sample resolve
  target. Depth is not yet known.
- A custom resolve inside the FDM render pass, reading the multisampled image as
  an input attachment.
- No density built-ins in shaders. Later passes read the subsampled resolve
  target through immutable subsampled samplers.

## Design

All of this sits behind a second experimental flag, `MESA_KK_EXPERIMENTAL=fdm,fdm_rate`.
Without `fdm_rate`, stage 1's full-rate behaviour is unchanged.

1. **Capture the map on the CPU.** When `vkCmdCopyBufferToImage` writes an image
   with density-map usage, keep a CPU copy of the bytes per layer, read from the
   host-visible source buffer at record time. If none is available, fall back to
   full rate.
2. **Build and cache the Metal map.** Key it by density image, content version
   and framebuffer size. Use 2 samples per texel; each sample takes the maximum
   of the texels it touches, per axis (horizontal from R, vertical from G), per
   layer.
3. **Apply it per render pass.** At `vkCmdBeginRendering` with an FDM attachment,
   use the map only if every attachment and resolve target is a subsampled image;
   otherwise use full rate. Keep the map across KK's custom-resolve switch, so the
   resolve stage renders at the same physical layout.
4. **Give each subsampled image a parameter block.** Each subsampled image gets a
   shared 48 KiB buffer: a header with a valid flag and the physical size per
   layer, then Metal's parameter data. Rendering into the image with a map writes
   the block.
5. **Convert `gl_FragCoord`.** For fragment shaders in pipelines created with the
   FDM attachment flag, lower application `FragCoord` to screen space with the
   decoder, using a root-table address that is 0 when no map is active. This runs
   before input-attachment lowering, so KK's own input-attachment fetches keep
   physical coordinates; the multisampled images are stored in physical layout.
6. **Map subsampled-sampler reads.** Texture sampling through an immutable
   subsampled sampler maps the coordinates: UV times the image size gives screen
   pixels, the decoder maps those to physical, the result is clamped to the
   physical extent and divided by the image size. The image's parameter block
   address goes in the sampled-image descriptor, and an invalid block means no
   mapping.
7. **New NIR intrinsics.** Add `rate_map_screen_to_physical_kk` and
   `rate_map_physical_to_screen_kk(addr, coords, layer)`, emitted in MSL as decoder
   calls.

Not covered: `FragSizeEXT` stays 1x1 (Alyx does not read it), as do offsets and
GPU-written maps. Non-subsampled attachments in an FDM pass get full rate rather
than an expansion pass.

## Milestones

- **2a, driver plus probes.**
  - Extend the FDM probe: a subsampled render through a centre-sharp map, read
    back via a subsampled sampler. The centre must match full rate exactly; the
    periphery must stay within a smooth-gradient tolerance.
  - A `FragCoord` pattern must land on the right screen pixels.
  - An atomic counter must show about 35% of full-rate invocations.
  - Custom resolve inside a rate-mapped pass must match.
  - Regression: the stage 1, custom resolve and interop probes still pass.
- **2b, Alyx.**
  - Confirm whether depth is subsampled; try `+vr_render_subsampled_depth 1` if
    it is not.
  - Run with `fdm_rate`, check the picture, and repeat the stage 1 timing
    comparison.

## Milestone 2a results

Mesa branch `claude/kk-fdm-stage1` in `workspace/kk-fdm`, built at `build/kk-fdm`
([patches](../experiments/alyx-vulkan/kosmickrisp-fdm/mesa-patches) 0003-0004):

| Commit | Change |
|---|---|
| `0c4a485` | Design steps 1-4: capture the map, build and cache the Metal rate map, apply it to all-subsampled passes and across the custom-resolve switch, and publish parameter blocks |
| `3761ec4` | Design steps 5-7: decoder intrinsics, screen-space `FragCoord` and subsampled-sampler mapping |

The first version of the `FragCoord` pass returned zeros. It rewrote uses with
`nir_def_rewrite_uses_after`, which assumes the replacement is in the same
block. Because the replacement sits behind an `if`, the phi ended up reading
itself and the position load was dropped. The pass now uses
`nir_shader_lower_instructions`.

[Rate probe](../experiments/alyx-vulkan/kosmickrisp-fdm/rate-probe)
(`build.sh OUT`, then `OUT/probe_rate OUT [--custom-resolve]`). It uses Alyx's
map, 672x768x2 multiview, writes `gl_FragCoord` and reads back through a
subsampled sampler. Runs used Metal API validation, which reported nothing.

| Mode | Flags | Invocations | Centre error | Periphery error |
|---|---|---:|---:|---:|
| Single-sample | `fdm` | 100% | 0.01 px | 0.01 px |
| Single-sample | `fdm,fdm_rate` | **34.8%** | 0.01 px | 2.24 px |
| 2 samples + custom resolve | `fdm` | 200% | 0.01 px | 0.01 px |
| 2 samples + custom resolve | `fdm,fdm_rate` | **69.6%** (34.8% of 200%) | 0.01 px | 2.24 px |

Both layers pass and land on the right layer. With 2 samples, the shader runs
per sample even without reading `gl_FragCoord`, probably because it writes
memory (the invocation counter). The ratio is unchanged.

Regression: with no flags, with `fdm` and with `fdm,fdm_rate`, the stage 1 FDM
probe (normal and subsampled) passes. So do the custom resolve probe (default,
`--segments 3`, `--dummy-descriptors`, `--no-depth-input`, `--restricted-views`)
and the Metal interop test.

The stage 1 probe still gets a per-pixel checkerboard with `fdm_rate`. Its map
is filled with `vkCmdClearColorImage`, which the driver does not capture, so the pass falls back to full rate, as
designed.
