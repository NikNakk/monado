<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->

# KosmicKrisp / OpenXR assessment: decision update — 2026-10-14

Keep MoltenVK for the native OpenXR path. The useful findings are explicit GPU
frame handoff and shared Metal texture import; both work through MoltenVK's
standard API. The [implemented follow-up](../doc/macos-vulkan-metal-swapchains.md)
contains the current compatibility matrix, prototype results, measured evidence,
reproduction instructions and prioritised plan.

The full earlier assessment and Mesa patches are preserved at local tag
`archive/kosmickrisp-2026-10-14` (`1025b3623230965052df1e3c1373adf24c52037b`).
Read them with `git show archive/kosmickrisp-2026-10-14:docs/kosmickrisp-openxr-assessment.md`.
This preserves the source, licensing, Hadron/steamac inventories, GPU-event and
texture proofs, failed experiments and full benchmark evidence without bringing
a private Mesa ABI into the active runtime branch.

| Requirement | Inspected KosmicKrisp snapshot / archived experiment | Active MoltenVK follow-up |
| --- | --- | --- |
| Native Vulkan OpenXR | Diagnostic patched driver worked | Standard API, native Metal-backed array/depth path; default-on in integration candidate |
| Metal texture interoperability | Upstream Metal external-memory support was MTLHeap-only; diagnostic hooks added texture import | `VK_EXT_metal_objects`; exact imported/exported texture identity; no image allocation/bind |
| Cross-process storage and ordering | Patched texture/event proof, archived | Shared texture handles; exact pixels at GPU values 1–4 in an independent process; missing-wait negative control |
| Additional compositor/copy | No inherent requirement demonstrated | Existing compositor and Metal transport; no production image copy introduced |
| Performance | Eight synthetic GPU workloads 1–18% slower than MVK | Shared-event improvement is also available on MVK; no new driver benefit established |
| D3D12/VKD3D-Proton | Hadron architecture/patch inventory only | Separate future WineOpenXR assessment; not implemented |
| D3D11 | No measured XR advantage over DXMT | Existing DXMT path retained |
| steamac/Venus | VM transport work does not prove native OpenXR texture sharing | No VM dependency |

These are claims about inspected Mesa `aac5108bbfa3051f03a71c695b3a6f961ea0fc46`
(26.3.0-devel), Hadron `4308bcd0a76395e22688e485cb892bdf0f9bffa2` and steamac
`6ef3992d869e41291bb2841bef2e71ae318d2a02`, not a fresh assessment of their
latest branches. Benchmark means included arithmetic ×4: 0.684/0.634 ms,
warped sampling ×4: 0.648/0.564 ms, and 200 draws: 0.31/0.26 ms (KK/MVK).
Same M5, 600 frames, four A–B–B–A runs per workload; not a game benchmark.

The experimental Monado branch is `codex/macos-vulkan-metal-swapchains`, retaining
Claude's shared-event work and ten general commits by ancestry. The reviewed `codex/macos-graphics-integration` candidate defaults both Vulkan
sharing switches to on with independent fallbacks; see the
[cumulative review](../doc/macos-graphics-integration-review.md). The user confirms service/hosted `hello_xr` and stereo arrays with readbacks
disabled after the probe visibility fix. Real-game Game Mode comparisons and Wine graphics
paths remain outstanding. The Linux gate now passes 48/48; the user-approved
local integration fast-forward to `c2c608a3f` includes these changes. No push is included. The user subsequently requested installation: service and
Wine x64 Monado client now both carry `v25.1.0-2310-g84b3749c2`; the
[deployment record](../doc/macos-service-direct-xpc.md#integration-service-and-matching-wine-client-installation--2026-10-14)
contains version/load checks and preserved settings. No Mesa driver or Windows
bridge change was required.
