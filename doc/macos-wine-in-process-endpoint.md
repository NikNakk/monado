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

## PS VR2 hardware gate, 2026-10-02

The user authorized agent-run tests on the connected PS VR2. The installed
ARM64 service was built from 7fd7f2835693d447d46da933e9a54c9f71ddfae9, so a clean,
matching x86_64 client was built in `.build/in-process-native-hardware/monado-x64`.
The client/service version check stayed enabled. Source checkout:
`.build/in-process-monado-hardware-source`, detached at that exact revision.
No hardware service configuration or Monado source change was made.

After the user power-cycled/reconnected USB to clear a status-interface setup
failure, all four runtime-owned 2D images and both slices of all four array
images passed D3D11 pixel verification and shared-event/release ordering.
Opaque D3D11 hello_xr completed 45 seconds and exited 0. It selected PS VR2,
reported 6DoF tracking support, and imported four images per eye at 2800x2856.
A 30-second client-compositor run also exited 0 and created a visible hosted
CAContext/layer from inside Wine. Client Metal validation was enabled.

Timing remains limited: service-compositor priority was clamped to 4 throughout
and physical presented intervals were 91.757 ms median / 108.441 ms p95. The
hosted Wine compositor retained realtime priority 97, but its display link and
pacer ran at 60 Hz despite the headset's 120 Hz mode: presented intervals
16.683 ms median / 16.684 ms p95. The Game Mode flag was not measured; no cause
for the service clamp or the hosted 60 Hz period is asserted. No pacing fix
was applied. The minimum-frame-period hint was unused in these runs.

These are API/resource/presentation-plumbing tests. The user's confirmation of
the visible stereo picture and head tracking remains pending; full 120 Hz,
OpenComposite rendering and matched three-path benchmarks remain outstanding.
The bridge's `docs/in-process-openxr.md` and
`docs/results/psvr2-in-process-2026-10-02/` contain the receipts, compressed raw
traces, call-latency measurements and exact results. Bridge hardware-evidence
commit: d9c17ed.
