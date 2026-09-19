# Windows/Wine IOSurface swapchain import

This branch starts the Windows-PCVR graphics bridge without adding a second compositor.

## Goal

Accept simple color swapchain images created outside Monado as process-independent
`IOSurfaceID` values, recreate `MTLTexture` objects directly on MoltenVK's own
`MTLDevice`, and feed those textures into the existing
`VkImportMetalTextureInfoEXT` swapchain importer.

The intended producer is the MIT-licensed BasaltVR DXMT v0.80 patch, which can make
D3D11 shared textures IOSurface-backed and expose each IOSurface ID through D3D11
private data. The first probe deliberately does not require Wine: a separate native
process creates equivalent globally lookupable IOSurfaces so the Monado half can be
validated independently.

## Architecture

```text
external producer (native probe first, DXMT later)
        |
        | IOSurfaceID[]
        v
Monado IPC client
        |
        v
monado-service
        |
        | IOSurfaceLookup(id)
        v
MTLTexture created on MoltenVK MTLDevice
        |
        | VkImportMetalTextureInfoEXT
        v
normal Monado compositor swapchain
        |
        +-- GPU reuse tracking
        +-- smart acquire
        +-- existing compositor / reprojection / PSVR2 path
```

IOSurface IDs are treated as untrusted. The service validates non-zero IDs, image
geometry, simple 2D/single-mip/single-sample constraints, and the supported color
format before creating a texture.

## Pinned Wine/DXMT toolchain

Provision the exact known-good Windows graphics stack with:

```sh
scripts/macos/provision-wine-dxmt.zsh
```

By default it creates an isolated tree at `build-wine-dxmt/` containing:

- Gcenx macOS Wine 11.10 x86_64, SHA-256 verified
- a private win64 prefix
- the matched BasaltVR v0.1.0 `v0.80-basalt.1` DXMT DLL/`winemetal.so` set
- a wrapper that enables `DXMT_BASALT_IOSURFACE=1`

The script consumes BasaltVR's published developer-preview archive rather than
copying its DXMT patchset into Monado. Basalt's release workflow builds and verifies
those matched artifacts before packaging them.

Run a Windows executable with:

```sh
build-wine-dxmt/bin/wine-dxmt program.exe
```

or load the generated environment:

```sh
source build-wine-dxmt/env.zsh
"$WINE" program.exe
```

No system Wine, Whisky, CrossOver, GPTK installation, or existing Wine prefix is
modified.

## Cross-process native probe

Build the service and tests, start `monado-service`, then run:

```sh
build/tests/tests_macos_iosurface_producer
```

It prints three IOSurface IDs and keeps their owning process alive. In another
terminal, pass those IDs to:

```sh
build/tests/tests_macos_iosurface_import_probe ID0 ID1 ID2
```

Success proves that a resource created in another process can be looked up by ID,
recreated directly on MoltenVK's Metal device, imported as a Vulkan image, and
managed as a real Monado swapchain. The producer can then be replaced by the
Basalt/DXMT D3D11 source without changing the service-side importer.

## Milestone-1 limits

- 2D images only
- array size 1
- one mip level
- one sample
- RGBA8/BGRA8, linear or sRGB
- producer GPU-completion synchronization not included yet

The next milestone is the Wine/DXMT producer plus D3D11 fence completion handoff.
After correctness is established, the preferred optimization is to export DXMT's
underlying Metal shared event and connect it to Monado's existing
`layer_commit_with_semaphore` timeline path.

## BasaltVR fork

A BasaltVR fork is not required for this milestone. The provisioner consumes the
prebuilt matched DXMT artifacts in BasaltVR v0.1.0, while pinning and verifying the
published archive. This keeps attribution and the upstream patch history intact
without duplicating Basalt's fork in Monado.

A fork becomes useful when we need to change that patchset ourselves, most likely
for Metal shared-event export, additional formats/array handling, or Monado-specific
DXMT integration.
