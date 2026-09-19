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
- producer GPU completion uses the same DXMT-backed `MTLSharedEvent` as a
  native Vulkan timeline semaphore when the GPU-sync DXMT patch is installed;
  the original CPU fence wait remains as a diagnostic fallback.

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
5. registers that manifest as `HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\\ActiveRuntime`
   inside the private Wine prefix (the Windows loader ignores `XR_RUNTIME_JSON`
   when Wine reports a high-integrity process);
6. sets `MONADO_WINE_TCP_PORT` and `DXMT_BASALT_IOSURFACE=1`;
7. launches the pinned official sample as:

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
- GPU-only D3D11 fence / Metal shared-event / Vulkan timeline synchronization
  when using the patched private DXMT, with CPU-wait fallback;
- one-RPC single-projection submission, with compact chunk fallback for
  multi-layer frames;
- development loopback TCP without authentication.

The principal remaining performance work is measurement and tuning: compare
the GPU-only path with `MONADO_WINE_GPU_SYNC=0`, inspect the timing CSV, and
correlate any remaining stalls with the native compositor/presentation trace.


## Performance instrumentation

The Wine D3D11 compositor records a client-side timing CSV when
`MONADO_WINE_TIMING_TRACE` is set. The hello_xr runner enables this by default
and writes:

```text
/tmp/monado_wine_d3d11_timing.csv
```

Columns are:

```text
frame_id,fence_value,gpu_sync,wait_frame_us,producer_wait_us,ipc_commit_us,layer_commit_total_us
```

The runtime also logs a `Wine frame stall` warning when either producer
synchronization or frame-submit IPC exceeds 1.5 ms.

Summarise a run with:

```sh
scripts/macos/summarize-wine-timing.py /tmp/monado_wine_d3d11_timing.csv
```

For a direct A/B comparison, run once normally (GPU sync) and once with
`MONADO_WINE_GPU_SYNC=0`, using a different
`MONADO_WINE_TIMING_TRACE_HOST` for each run.

### End-to-end native Wine/OpenVR timing capture

To correlate the Wine frame with DXMT GPU readiness, the multi-compositor,
CAMetalDisplayLink and Metal presentation traces, use the traced runner:

```sh
git pull
cmake --build build-wine --parallel
scripts/macos/build-wine-openxr-d3d11.zsh

zsh scripts/macos/run-wine-openvr-native-trace.zsh
```

For Wine/DXMT tracing the helper temporarily unloads the normal LaunchAgent
and starts `monado-service` directly from the invoking shell. This deliberately
keeps the service in the same Mach bootstrap namespace as Wine/DXMT so the
current bootstrap-name MTLSharedEvent bridge remains usable. The direct capture
disables the native Metal XPC listener, which the IOSurface-ID Wine path does
not require. After the smoke run it stops the service cleanly to flush
fully-buffered CSVs and restores the ordinary development LaunchAgent.

The runner also saves `smoke.log` and reports the fraction of Wine frames that
actually used GPU shared-event synchronization. A zero fraction is treated as
a diagnostic warning and the relevant shared-event/fallback messages are
printed automatically.

The native trace set includes:
- `wine_submit.csv`: arrival, layer reconstruction and commit of the copied
  Wine frame, keyed by client frame ID and shared-event timeline value.
- `client_gpu.csv`: previous-frame blocking, shared-event wait/ready and
  scheduled-slot timing.
- `client_frame_map.csv`: client frame -> system compositor frame mapping.
- `frame_pipeline.csv`: system compositor prediction/render stages.
- `present.csv`, `presented.csv`, `present_complete.csv`: drawable,
  Metal command-buffer and actual presentation timing.
- `wine.csv`: PE/Wine-side wait, producer and IPC timings.

Analyze the whole capture with:

```sh
python3 scripts/macos/analyze-wine-native-pipeline.py /tmp/monado-wine-openvr-YYYYMMDD-HHMMSS
```

Use `MONADO_WINE_NATIVE_TRACE_DIR=/path` to choose a fixed output directory and
`MONADO_OPENVR_SMOKE_FRAMES=N` to change capture length.

The common one-projection-layer submission path uses one TCP request/reply.
Multi-layer frames retain the compact chunked fallback.

## GPU-only DXMT synchronization

The original bridge intentionally used a conservative D3D11 fence CPU wait:

```text
ID3D11DeviceContext4::Signal
 -> Flush
 -> SetEventOnCompletion
 -> WaitForSingleObject
 -> layer commit
```

A patched DXMT v0.80 build can instead expose the bootstrap registration name of
the shared fence's existing `MTLSharedEvent`. Native Monado resolves that Mach
port, reconstructs the event on the MoltenVK Metal device, imports it through
`VkImportMetalSharedEventInfoEXT`, and submits the layer with a Vulkan timeline
semaphore value. No Wine CPU fence wait is required.

Build and install the matched private DXMT set with:

```sh
scripts/macos/build-wine-dxmt-gpu-sync.zsh
```

This uses the pinned BasaltVR v0.1.0 build harness, its MIT v0.80 DXMT patches,
and `scripts/macos/dxmt-patches/0005-monado-shared-fence-bootstrap-name.patch`.
Only the private `build-wine-dxmt` Wine tree/prefix is modified.

GPU-only sync is selected automatically when the patched fence metadata is
available. For an A/B comparison, force the old CPU path with:

```sh
MONADO_WINE_GPU_SYNC=0 \
MONADO_WINE_TCP_PORT=4242 \
scripts/macos/run-wine-hello-xr-d3d11.zsh
```

## OpenComposite / OpenVR

OpenComposite remains an external GPLv3 component. No OpenComposite code is
linked into or vendored by Monado.

Provision an x64 OpenComposite `openvr_api.dll`:

```sh
scripts/macos/provision-opencomposite.zsh
```

The provisioner accepts a specific local DLL or artifact URL via
`MONADO_OPENCOMPOSITE_DLL_SOURCE`. Without an override it asks OpenComposite's
AppVeyor project for the current x64 `openxr`-branch artifact and records the
downloaded SHA-256.

Build and run the isolated OpenVR initialization smoke test:

```sh
scripts/macos/build-wine-openvr-smoke.zsh
MONADO_WINE_TCP_PORT=4242 \
scripts/macos/run-wine-openvr-opencomposite-smoke.zsh
```

The smoke executables dynamically load OpenComposite directly, so they cannot
accidentally initialize SteamVR. The first requests an OpenVR scene
application, `IVRSystem`, and `IVRCompositor`. The second creates D3D11
render targets at OpenVR's recommended eye size and submits visible left/right
frames through `IVRCompositor::Submit`, exercising the full
OpenVR -> OpenComposite -> OpenXR -> DXMT -> IOSurface -> native Monado path.
It renders 360 frames by default; override with
`MONADO_OPENVR_SMOKE_FRAMES`. In-headset success is alternating/animated
left/right solid colours with valid HMD poses reported by the smoke program.

For a real OpenVR game, use the reversible per-game replacement:

```sh
scripts/macos/install-opencomposite-game.zsh install \
  '/path/to/game/openvr_api.dll'

MONADO_WINE_TCP_PORT=4242 \
scripts/macos/run-wine-openvr-game.zsh '/path/to/game/game.exe'
```

Restore the game's original DLL with:

```sh
scripts/macos/install-opencomposite-game.zsh restore \
  '/path/to/game/openvr_api.dll'
```

The installer also writes an `opencomposite.ini` with
`initUsingVulkan=false`, keeping OpenComposite on the D3D11-first path
currently supported by this Wine bridge.

## Wine/TCP pacing feedback and CAMetalDisplayLink timing

The OpenComposite timing capture from 2026-09-19 exposed a positive feedback
loop in the ordinary adaptive app pacer. With the default policy, the second
OpenXR eye swapchain could block in `xrWaitSwapchainImage` for tens of
milliseconds. Because that runtime-controlled wait occurs between
`xrBeginFrame` and `xrEndFrame`, the app pacer counted it as draw time,
selected a many-refresh application period, and thereby kept swapchain images
in use for longer. In the failing capture the median predicted app period was
about 75 ms and the compositor reused each client frame for about eight
120-Hz refreshes.

Forcing `U_PACING_APP_USE_MIN_FRAME_PERIOD=1` broke the loop: the predicted
period became 8.3417 ms, the problematic swapchain wait fell from about 61 ms
to about 6 ms median, and mapped client frames were normally used for one
system refresh. That experiment is now represented as a per-session pacing
hint. IPC clients using the Wine/macOS framed TCP transport request minimum
display-period pacing automatically; native Unix-domain-socket clients retain
Monado's adaptive app-period policy. The environment option remains available
as a global diagnostic override.

The same capture also clarified the apparent one-refresh
`presented - desired` offset in CAMetalDisplayLink-driven mode. In this mode
the compositor intentionally maps:

- `desired_present_time_ns` to CAMetalDisplayLink `targetTimestamp`, the
  deadline associated with the current update;
- `predicted_display_time_ns` and the Metal presentation target to
  `targetPresentationTimestamp`, the expected display time.

At 120 Hz those values are separated by one 8.3417 ms refresh period. Actual
Metal `presentedTime` was typically within tens of microseconds of the latter
target, so the +8.3 ms relative to `desired_present_time_ns` is expected
deadline-to-presentation separation, not evidence that Metal missed a frame.
The Wine timing analyzer therefore reports `target - desired/deadline`
explicitly and labels the latter comparison accordingly.

