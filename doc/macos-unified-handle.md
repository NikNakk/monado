<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# macOS unified graphics-buffer handle — 17 November 2026

Merged from `spike/macos-unified-handle` (worktree `build/review/handle-spike`).

## Design

`xrt_graphics_buffer_handle_t` is a `void *` to one retained Apple object, either
an `IOSurfaceRef` or an `id<MTLTexture>`. Both are reference-counted with
`CFRetain`/`CFRelease`; `xrt_graphics_buffer_is_iosurface()` tells them apart.
This is the same handle type upstream !3110 uses for Metal textures, widened.

- **Allocation** (`vk_image_allocator.c`): single-layer BGRA images are backed by
  an IOSurface the allocator creates, which Chromium's zero-copy path needs.
  Every other image is replaced by a shareable Metal texture with the same
  description, because only a texture created as shareable can produce an
  `MTLSharedTextureHandle`. Exporting an IOSurface from a driver-allocated image
  works on MoltenVK but not KosmicKrisp, so the allocator never asks for it.
  `XRT_MACOS_VK_SHAREABLE_TEXTURES=0` disables the replacement.
- **Import** (`vk_helpers.c`): `vk_create_image_from_native` picks
  `VkImportMetalIOSurfaceInfoEXT` or `VkImportMetalTextureInfoEXT` from the
  handle's type.
- **Transport** (`ipc_message_channel_unix.c`): both forms travel through the XPC
  registry, scoped to the client's verified PID in both directions; IOSurfaces
  as Mach ports, so they need not be global, and textures as shared texture
  handles. After the payload each message carries one token, an index per
  handle and a kind byte per handle. One message carries one kind. A
  non-shareable texture is refused rather than sent as "no handle".
- **Vulkan client**: arrays and depth use the ordinary native swapchain path in
  process and through the service. The client's lifetime protections (destroy
  fences, retired-swapchain reaping, frontend budget) are unchanged.
- **Removed**: the Vulkan client's Metal-backed swapchain route, the token
  carried in `xrt_image_native.size`, the `swapchain_import_metal` protocol call
  and its handler, and the opt-in `XRT_MACOS_VK_CLIENT_METAL_COLOR` route.
- **Kept**: the thread-local import handoff and the IOSurface ID and token import
  calls, still used by the hosted compositor and external producers.

## Fixed along the way

Single-layer swapchain creation through the service failed before the merge:
the service's IOSurfaces are not global, so a client could not open them by ID.
Sending them as Mach ports fixes it.

## Validation before merge

On MoltenVK 1.4.2 and the integration KosmicKrisp build (Mesa 27.0.0-devel),
simulated HMD unless noted:

- `tests_macos_unified_handle`: export, import into a second device, reopen
  through a shared handle, and read every pixel, for one- and two-layer RGBA8,
  BGRA8, BGRA8 sRGB and D32.
- In-process and service probes: arrays, per-view BGRA and RGBA8, sRGB, three
  sessions, hello_xr with depth, a negative control; per-eye colours matched.
- Chromium (`run-chromium-gpu-sync.mjs`, pixels): ANGLE zero-copy and Dawn
  arrays, all four colour pairs.
- Wine (`run-in-process-simulated.zsh`, `in_process_swapchain.exe`): 12/12 images.
- PS VR2, user-operated: service and hosted arrays and hello_xr on MoltenVK;
  Audio Trip through Wine, DXMT and xrizer on both KosmicKrisp and MoltenVK.

Of the CTests, all pass on MoltenVK. On KosmicKrisp the same 18 fail as on the
unmodified branch: 17 tests require MoltenVK's `VK_KHR_portability_subset`, and
`tests_render_compute` fails a camera timing check.
