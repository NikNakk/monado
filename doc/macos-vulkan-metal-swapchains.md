<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# Native Vulkan views of Metal swapchains — 2026-10-14

## Decision and scope

Use MoltenVK's standard `VK_EXT_metal_objects` support with the existing Metal
swapchain transport. KosmicKrisp remains archived research: the tested workloads
showed no rendering advantage, and shared-event frame handoff works with
MoltenVK too. This follow-up adds array/depth support to native Vulkan
OpenXR clients. It adds no compositor, Mesa dependency or Wine/DXMT change.

Branch: `codex/macos-vulkan-metal-swapchains`, based on Claude's
`b4dc8e554`. Runtime implementation: `fef029cd7`; fixtures: `ea4ede3cb`; allocation-fallback
guard: `f1841a3bc`.
The ten general commits ending at `e393b5dff` are preserved by ancestry.
The KosmicKrisp work is saved at local tag `archive/kosmickrisp-2026-10-14`
(`1025b3623`), including the driver benchmarks and stereo/sRGB experiments.
`macos-upstream-clean`, the GPU harness branch and installed runtime are unchanged.
Nothing has been merged or pushed.

## Integration candidate

The [cumulative review](macos-graphics-integration-review.md) prepares
`codex/macos-graphics-integration` with both Vulkan switches defaulting to on,
independent `=0` fallbacks, bounded pending-fence retirement and acquired-image
destruction markers. This supersedes the original opt-in recommendation after
the user's four-case visual acceptance. The user subsequently approved the local integration fast-forward to
`c2c608a3f`. The remote branch remains unpublished; the subsequently approved
service and Wine x64 installation matches `84b3749c2`. See the
[deployment record](macos-service-direct-xpc.md#integration-service-and-matching-wine-client-installation--2026-10-14). Historical experiment settings below remain evidence
of the tested revisions.

## User headset checkpoint — 2026-10-14

At runner commit `9075aa35b`, the user reports that service `hello_xr`, service
stereo arrays and hosted `hello_xr` work. Hosted stereo arrays are entirely
black with readbacks off. The hosted-array log confirms direct Metal textures,
an in-process event pair, 900 submitted frames and about 90 FPS; advancing GPU
timelines do not establish visible image correctness. The failure was subsequently resolved by the probe event-loop fix below. The sampled repeat remained black to the user, but all sampled source layers
and composed output have the expected colours. This narrows the failure to
hosted visibility/presentation, rather than missing array pixels. The probe
never polled OpenXR events after READY: hosted visibility changes are applied
by `local_session_poll_events` in `ipc_client_system.c`. Commit `d419f8527` polls
throughout the frame loop. The user confirms the 900-frame hosted-array repeat
now shows colours with diagnostic readbacks disabled. All four requested visual
tests therefore pass: service/hosted `hello_xr` and service/hosted arrays.

The render-pass diagnostic aborted on Metal validation because its async
fixture freed an image view while GPU work still referenced it. The same commit
waits before destroying those test-only resources. Both 60-frame offscreen clear
paths and concurrent fault/replacement recovery pass after the fix. No runtime
sharing, synchronization or compositor defaults were changed.

Raw runs: `/private/tmp/vulkan-metal-hardware-20261014-212814`. Preserve the
working paths; the user subsequently authorised making the Vulkan options default-on.

## Allocation and ordering

Set `XRT_MACOS_VK_CLIENT_METAL_TEXTURES=1` **in the Vulkan client** to force it on, or `=0` to disable it. The
integration candidate defaults to on. Simple single-layer colour images retain the existing IOSurface path.
Arrays and depth use the existing Metal provider: shared textures and XPC
handles for service composition, direct textures for client-hosted composition.
The Vulkan frontend keeps both Metal and native swapchain references. Layer
submission always forwards native swapchains, permitting ordinary IOSurface
colour and Metal depth in the same frame. Acquisition/release uses the owning
Metal wrapper when present, retaining its existing reuse protection.

Each client image is created with `VkImportMetalTextureInfoEXT`, plane 0, then
exported to check exact `MTLTexture` pointer identity. It allocates no Vulkan
image memory and makes no dummy bind. The
[Khronos import semantics](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMetalTextureInfoEXT.html)
treat the imported texture image as already bound. The GPU proof imports a
reopened shared texture and reads the original owner. The independent consumer
receives only `MTLSharedTextureHandle` and shared-event handles; no image pixels
are transported. The macOS SDK documents that reopening this handle shares the
same texture across processes on the same GPU.

Keep the independent `XRT_MACOS_VK_CLIENT_SHARED_EVENT=1` option enabled (also default-on in the candidate) for GPU frame
handoff. It uses Claude's runtime-owned shared-event/Vulkan timeline pair and
preserves its initial value. The signal follows client rendering on the same
queue; native compositor readiness/reuse infrastructure consumes it. Disable
this option to retain the CPU queue-idle fallback. Texture import alone does
not eliminate that fallback. No new blocking wait is added to frame commit.

The provider preserves the native image count (normally three, static images
one), instead of using the Metal service's ordinary four-image allocation.
This fixes unmodified `hello_xr` colour/depth count parity. Other Metal clients
keep their current allocation preference and advertised format list.

Barrier fences protect Vulkan command/image lifetime. Completed retired
swapchains are reaped on subsequent allocation. Frontend destruction polls
retired resources and the final timeline for at most 500 ms, then retains
unresolved objects until process exit. Eight active or retained frontends are
allowed per process; the ninth is rejected. Native per-client/global resource
budgets still apply. Infinite image acquisition is split into 50 ms waits to
poll session loss in the opted-in Vulkan paths. A timeout never grants access
to an unsafe image. Queue locking and successful-submit-only timeline increments
also protect failure handling.

## Compatibility matrix

| Item | Verified result | Limit |
| --- | --- | --- |
| RGBA8/BGRA8 UNORM and sRGB | Exact texture identity, all pixels at GPU values 1–4; independent process too | Same GPU; one/two layers tested |
| RGBA16 UNORM/float, R32 float | Exact identity and GPU-ordered pixels | One/two layers within one process; cross-process exact pixels not tested |
| D16, D32, D32+S8 | Exact identity, depth/stencil pixels and GPU ordering | One/two layers within one process |
| Native OpenXR stereo array | Separate service reads correct changing colours in both layers | BGRA UNORM/sRGB, simulated HMD |
| Ordinary colour + D32 depth | Unmodified SDK `hello_xr` exits cleanly | Separate service, simulated HMD; no headset visual acceptance |
| Existing single-layer Vulkan | IOSurface path passes with event handoff | Opt-in advanced geometry off returns explicit unsupported |
| Texture import + CPU handoff | Array probe passes | CPU queue-idle fallback remains |
| Existing native Metal client | Loaderless Metal probe passes | Wine paths not exercised in this follow-up |
| Missing producer signal | Bounded session loss; unresolved frontend retained | Recovery requires new session/process as described above |
| Concurrent healthy/fault/replacement | Healthy 300 frames, failed overlay, replacement 60 frames | Separate service; fault process exits in 1.93 s in recorded run |
| Repeated sessions | 12 healthy pass; eight unresolved retain; ninth rejected | Per-process frontend budget intentionally finite |
| Hosted Vulkan path | User confirms `hello_xr`; direct provider and event pair initialize | Initial black array probe fixed by polling visibility events; user confirms 900-frame no-readback repeat |
| Cube, MSAA, mips, protected/mutable images | Explicit unsupported before Metal allocation | Cube rejection executed; other restrictions inspected |
| Missing Metal object support | Simple native images retain fallback | Advanced geometry explicitly unsupported; alternate device not tested |
| Linux / x86 Wine | macOS helper guarded; Linux queue safety preserved | No Linux CI or Wine regression run here |

Supported new-path formats are Vulkan 37, 43, 44, 50, 91, 97, 100, 124, 126 and
130. Other formats are rejected on this path. A format being enumerated does
not promise every geometry or usage combination.

## Evidence and reproducibility

Host: M5 MacBook Air (24 GB), arm64, macOS 26.7.1 / 25G241, Xcode 27.0 /
27A266a. MoltenVK 1.4.2, Vulkan headers/loader 1.4.357.0, CMake 4.4.3,
Ninja 1.13.2. No macOS 27 upgrade was needed.

[Recorded evidence](macos-vulkan-metal-results/manifest.json) includes binary
hashes, source commits, exact-pixel logs, source samples, recovery and budget
summaries. All 75 CTests passed at `ea4ede3cb`; the allocation-fallback guard then passed a
60-frame service regression. Complete working logs remain in `/private/tmp/vulkan-metal-tests`.
The standalone proof checks 491,520 pixels across 20 format/layer combinations.
The XPC consumer additionally checks 196,608 colour pixels, with distinct PIDs,
32 format/layer/value checks, and GPU acknowledgement back to Vulkan. Producer
work is held behind a gate until the peer confirms submission. Neither consumer
may complete before that gate opens. Removing the peer's GPU wait fails this
check. Diagnostic readbacks and CPU waits validate the test; they are not
texture-transfer operations in the runtime.

Build in an isolated worktree, using the normal dependencies in `macos-port.md`:

```sh
cmake -S . -B /private/tmp/monado-vulkan-metal-build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /private/tmp/monado-vulkan-metal-build --parallel 6
ctest --test-dir /private/tmp/monado-vulkan-metal-build --output-on-failure
```

The tests below explicitly select Homebrew MoltenVK, create unique launchd
services and remove them in `finally`. They require a logged-in macOS GPU host:

```sh
python3 scripts/macos/run-vulkan-metal-textures.py --build /private/tmp/monado-vulkan-metal-build --output /private/tmp/vk-import --mode import --cross-process
python3 scripts/macos/run-vulkan-metal-textures.py --build /private/tmp/monado-vulkan-metal-build --output /private/tmp/vk-negative --mode import --cross-process --skip-peer-wait
python3 scripts/macos/run-vulkan-metal-textures.py --build /private/tmp/monado-vulkan-metal-build --output /private/tmp/vk-array
python3 scripts/macos/run-vulkan-metal-textures.py --build /private/tmp/monado-vulkan-metal-build --output /private/tmp/vk-srgb --format 50
python3 scripts/macos/run-vulkan-metal-textures.py --build /private/tmp/monado-vulkan-metal-build --output /private/tmp/vk-recovery --suite
python3 scripts/macos/run-vulkan-metal-textures.py --build /private/tmp/monado-vulkan-metal-build --output /private/tmp/vk-repeat --repeat 12 --frames 15
python3 scripts/macos/run-vulkan-metal-textures.py --build /private/tmp/monado-vulkan-metal-build --output /private/tmp/vk-budget --suppress-signal --repeat 9 --frames 120
python3 scripts/macos/run-vulkan-metal-textures.py --build /private/tmp/monado-vulkan-metal-build --output /private/tmp/vk-hello --mode hello --hello /path/to/hello_xr
```

The budget command deliberately exits nonzero: require eight retained warnings
and ninth-session `XR_ERROR_INITIALIZATION_FAILED`. `--unsupported-cube` also
exits nonzero with `XR_ERROR_FEATURE_UNSUPPORTED`. `--cpu-handoff` and `--per-view`
select the two fallback regressions. `--moltenvk` overrides the library path.
For user-run PS VR2 visual tests, add `--headset --no-readbacks`, then
`--hosted` for client composition. `--no-readbacks` disables compositor source
and target pixel readbacks while retaining initialization logs.
The runner rejects a hosted fallback as a successful hosted test. The later user headset runs are recorded in the checkpoint above.

## Performance and remaining gates

The existing shared-event A/B evidence remains in
[macos-env-toggles.md](macos-env-toggles.md#vulkan-client-shared-event-frame-handoff-2026-10-14):
loaded 90 Hz probes improved service throughput from about 34 to 88–90 FPS and
hosted throughput from about 45 to 90 FPS. Those were predicted-display-time
probe measurements, not real game/Game Mode acceptance. This follow-up's
pixel-sampled simulated 60 Hz runs are correctness evidence, not a new speedup
or frame-pacing benchmark. No array/depth overhead gain is claimed.

Next, in priority order:

1. **Visual gate passed:** user confirms `hello_xr` and stereo arrays on PS VR2,
   service and hosted, with diagnostic readbacks disabled after `d419f8527`.
   Longer application stress and compositor depth-reprojection checks remain.
2. Comparable loaded/game/Game Mode runs, with GPU timings, CPU overhead and
   actual presentation timestamps. Both sharing switches are default-on in the
   reviewed candidate after visual acceptance; these comparisons quantify their
   effect on real applications.
3. Linux CI, older Apple devices/OS versions, static-image/geometry limits,
   device loss, killed-client/teardown and longer concurrency stress.
4. Review small general patches for integration/upstream submission; no merge
   is implied. Monado requires human DCO sign-off before an MR. New code uses
   BSL-1.0 and existing Monado architecture; no Hadron, steamac, Mesa or DXMT
   implementation was copied into this runtime path.
5. Revisit D3D12/VKD3D-Proton only with an independent WineOpenXR swapchain proof.
   Retain DXMT; no D3D11 replacement or Linux VM dependency is introduced.
