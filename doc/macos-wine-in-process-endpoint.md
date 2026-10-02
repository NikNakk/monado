<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->

# Isolated Wine native-client endpoint validation

The experimental in-process Wine OpenXR runtime lives in `NikNakk/macos-wine-xr`,
branch `codex/in-process-wine-openxr`; its DXMT companion is on
`codex/in-process-metal-import`. Wine and the native OpenXR client are x86_64;
the simulated Monado service is ARM64. This removes the bridge's TCP/RPC host,
while retaining normal native Monado client/service IPC.

A distinct Unix socket alone does not isolate Metal sharing: the XPC publisher
must reach the same service that receives the swapchain-import message. The
initial experiment hit the registered hardware endpoint and failed token
ownership validation. The approved fix adds an opt-in shared endpoint accessor
in `ipc_metal_xpc.h`. No protocol, graphics or default endpoint change is made.

## Validation on 2026-10-02

Built `openxr_monado` in `.build/in-process-monado-x64` and `monado-service` in
`.build/native-service-check` against the modified header. Both build directories
belong to this checkout. Simulated-only service: PS VR2 and PS Sense disabled.

An independent LaunchAgent used label
`org.freedesktop.monado.inprocess-test.501`, Mach service
`org.freedesktop.monado.metal-ipc.inprocess-test.501`, and socket directory
`/private/tmp/mwxr-inprocess-gate`. The client and service used matching
`XRT_MACOS_METAL_IPC_SERVICE_NAME` values. The registered hardware LaunchAgent
was neither replaced nor reconfigured.

Khronos D3D11 `hello_xr` ran opaque against the simulated HMD with Metal
validation enabled and exited successfully. Both eye swapchains enumerated four
images, each imported as the runtime's own Metal texture. The GPU probe then
cleared and inspected all four images of a 2D swapchain and both slices of all
four images of a two-layer array swapchain. All 12 image/slice checks passed,
including shared-event producer synchronization and native OpenXR release.
Readback blits are confined to the diagnostic; production image handoff has
no copy fallback.

`U_PACING_APP_USE_MIN_FRAME_PERIOD` was unset in the service and client. The
service recorded its app-pacer inputs in `monado_psvr2_85937_app_pacing.csv` in
the bridge's `build-in-process/traces`. This is a desktop simulated test;
it makes no claim about headset pacing, Game Mode, alpha blending or PS VR2.
The bridge's `docs/in-process-openxr.md` holds the acceptance ledger and commands.
