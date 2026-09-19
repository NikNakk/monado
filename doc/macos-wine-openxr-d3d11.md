# Wine D3D11 OpenXR client for native macOS Monado

This branch builds a Windows x86-64 Monado OpenXR runtime under MinGW and runs
Windows D3D11 OpenXR applications through Wine/DXMT against the existing native
macOS `monado-service`.

The native service remains the only hardware runtime and compositor. The PE
runtime is another Monado client frontend.

## Architecture

```text
Windows OpenXR application
        |
        | Khronos OpenXR loader
        v
openxr_monado.dll (PE x86-64)
        |
        | Monado st_oxr
        v
Wine D3D11 client compositor
        |
        +-- D3D11 textures --> DXMT --> IOSurfaceID[]
        |
        +-- Monado scalar IPC over 127.0.0.1 TCP
        v
native arm64 monado-service
        |
        | IOSurface -> MTLTexture -> VkImportMetalTextureInfoEXT
        v
existing compositor / reprojection / presentation
        v
PSVR2
```

The TCP endpoint is development-only, loopback-only, and opt-in. Native macOS
OpenXR applications continue to use the ordinary Unix socket and XPC Metal
side-channel unchanged.

## Why TCP

The upstream Windows Monado IPC client normally expects Windows named pipes and
Windows shared-memory/native handles. Those cannot be passed directly to the
native macOS service through Wine.

For the Wine bridge:

- scalar generated Monado IPC calls use a framed loopback TCP stream;
- the initial static `ipc_shared_memory` metadata is copied once instead of
  transferring an OS shared-memory handle;
- completed composition-layer slots are copied inline at frame submission;
- D3D11 swapchain resources are transferred by process-independent IOSurface IDs;
- producer GPU completion initially uses a local D3D11 fence before native
  layer commit.

This deliberately preserves Monado's existing OpenXR state tracker rather than
creating a second OpenXR implementation.

## Build

First provision the pinned private Wine 11.10 + Basalt DXMT v0.80 stack if it
does not already exist:

```sh
scripts/macos/provision-wine-dxmt.zsh
```

Build the Windows Monado OpenXR runtime:

```sh
scripts/macos/build-wine-openxr-d3d11.zsh
```

The default cross-build directory is `build-wine-openxr/`.

Build the pinned official Khronos OpenXR-SDK-Source 1.1.61 `hello_xr` D3D11
sample:

```sh
scripts/macos/build-wine-hello-xr-d3d11.zsh
```

The sample build uses the official Khronos sources and loader. A small local
compatibility header supplies two Windows SDK D3D11 convenience constructors
that MinGW omits.

## Native service

The PE runtime and native service must come from the same Monado commit.

For a manually started service:

```sh
IPC_WINE_TCP_PORT=4242 \
<native-build>/src/xrt/targets/service/monado-service
```

For the development launchd service, capture the Wine port and disable the
normal short idle exit for this interactive test:

```sh
IPC_WINE_TCP_PORT=4242 \
IPC_EXIT_WHEN_IDLE=0 \
<native-build>/src/xrt/targets/service/monado-service-xpc-control bootstrap

launchctl kickstart -k gui/$(id -u)/org.freedesktop.monado.service
```

The service should log:

```text
Wine OpenXR IPC bridge listening on 127.0.0.1:4242
```

## Run Khronos hello_xr

With the native service listening:

```sh
MONADO_WINE_TCP_PORT=4242 \
scripts/macos/run-wine-hello-xr-d3d11.zsh
```

The runner:

1. verifies the private Wine/DXMT stack;
2. builds the PE runtime and sample if they are missing;
3. verifies the native loopback bridge is listening;
4. creates a Wine-readable OpenXR runtime manifest pointing at
   `openxr_monado.dll`;
5. sets `MONADO_WINE_TCP_PORT`, `XR_RUNTIME_JSON`, and
   `DXMT_BASALT_IOSURFACE=1`;
6. launches the pinned official sample as:

```text
khr_hello_xr_d3d11.exe --graphics D3D11 --space Local --verbose
```

The first visual gate is fused stereo, world-fixed cubes, and correct head
tracking. Controller actions/haptics can be validated separately once the
Sense-controller path is ready.

## Milestone limits

The first implementation intentionally supports:

- D3D11 only;
- simple 2D color swapchains;
- BGRA8/RGBA8 linear and sRGB;
- no transported depth swapchain;
- D3D11 fence + CPU wait for producer completion;
- copied layer slots rather than shared cross-OS memory;
- development loopback TCP without authentication.

The intended next optimization is DXMT `MTLSharedEvent` export so the Windows
client can feed Monado's existing compositor timeline semaphore path without a
CPU fence waiter.
