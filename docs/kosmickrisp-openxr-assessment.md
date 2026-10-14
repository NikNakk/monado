<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->

# KosmicKrisp / OpenXR assessment — 2026-10-14

## Decision and milestone checkpoint

**KosmicKrisp is technically viable as an additional native Vulkan graphics
path for simple 2D color images.** An isolated Mesa diagnostic patch now renders
real Vulkan shaders into existing IOSurface-backed Metal textures, and a second
process observes the correct output with GPU-side shared-event ordering. RGBA8
and BGRA8 pass storage identity, patterned-pixel, descriptor-rejection and
missing-wait checks on this M5/macOS 26.7.1.

Unmodified upstream still lacks the required texture/event import-export APIs.
The prototype uses private diagnostic hooks, not a supported or conformant Vulkan
extension. Standard API implementation, stereo arrays, lifetime/failure recovery
and OpenXR application validation remain work before production integration.
The current driver architecture does not appear to require another compositor
or mandatory image copies for this demonstrated 2D case.

Milestone 1 is assessed below. Milestone 2 now has a **successful minimal 2D
cross-process zero-copy prototype on a diagnostic patched driver**, with clear
limits and reproduction evidence. See [the texture follow-up](#shared-texture-follow-up--2026-10-14).
The initial upstream baseline and event-only checkpoints are retained as
historical evidence. Stop here for review: no production integration, hello_xr
claim, Wine/DXMT replacement, merge or push is included.

Verified means inspected source or executed test, explicitly distinguished below.
A proposal is not an implemented feature. No headset run was performed.

## Repository and environment inventory

| Component | Branch / exact revision inspected | Qualification |
|---|---|---|
| Monado working baseline | `codex/macos-gpu-sync-harness`, `95c0c64256d6a6c3e6fbca67f55cb679b4f5fa5a` | Existing edits in `doc/macos-port{,-tracker}.md` and untracked Alyx note preserved. |
| Assessment work | `codex/kosmickrisp-openxr-assessment`, branched from the baseline | Standalone experiment and documents only. |
| Local integration branch | `macos-upstream-clean`, `805316bebe976256a16c591ee135a5422ffe73a3` | Not checked out or edited. |
| Remote integration branch | `bc9591f0313508cdb19fcb3ddf8e2c39c1e2232b` | Queried with `ls-remote`, fetched for comparison. Versus local integration: four files differ, including runtime-manifest CMake and docs; do not assume identical trees. |
| WineOpenXR | `claude/steamvr-openxr-driver`, `534ffe00b047f6f4ab521c568a56932c5c245ba1` | Local ahead 15; untracked `.DS_Store`; read only. |
| DXMT fork | `codex/cross-process-presentation-hardening`, `d116ce4890a557fe9f1043dbcbf717037518dd5f` | Ahead one, multiple existing uncommitted sync/presentation changes; read only. Deployed minimal baseline is separately documented, not inferred from HEAD. |
| Mesa upstream | `main`, `aac5108bbfa3051f03a71c695b3a6f961ea0fc46` | Unmodified source, version `26.3.0-devel`; the executed driver. |
| Hadron | `main`, `4308bcd0a76395e22688e485cb892bdf0f9bffa2` | Patch/build/source inventory inspected, not built. |
| steamac | `main`, `6ef3992d869e41291bb2841bef2e71ae318d2a02` | Host build/patch queue inspected, not built or run. |

The shared Monado checkout advanced concurrently during this investigation to
`e393b5dff` through unrelated work. The inventory's Monado baseline is the
original inspected revision; its Vulkan/compositor/IPC-sharing/OpenXR graphics
sources do not differ from that later HEAD. No concurrent changes were reverted
or included in the isolated Mesa patches.

Test host: Apple M5 MacBook Air, 10-core CPU, 10-core GPU, 24 GB unified memory;
macOS **26.7.1 (25G241)**; Xcode **27.0 (27A266a)**, Apple clang **21.0.0**.
The physical PS VR2 is outside this offscreen test. GPU access required execution
outside the filesystem/process sandbox; sandboxed physical-device enumeration
returned `VK_ERROR_INITIALIZATION_FAILED`.

Build dependencies: Meson 1.12.0, Ninja 1.13.2, LLVM 23.1.3 (Homebrew
23.1.3_1), libclc 23.1.3, SPIRV-LLVM-Translator 23.1.2 (Meson reports library
23.1.0.0), SPIRV-Tools 1.4.357.0, Vulkan headers/loader 1.4.357.0,
Python 3.14.7, Mako 1.3.10, MarkupSafe 3.0.4, packaging 25.0, PyYAML 6.0.3.
LLVM/libclc/translator were installed for this experiment; Homebrew also installed
Z3 and upgraded xz. No driver was installed into system ICD discovery, no runtime
manifest changed, and no production service was restarted.

## Compatibility matrix

| Requirement | Upstream tested snapshot | Hadron / steamac relevance | Existing Monado / bridge implication |
|---|---|---|---|
| Native Apple Silicon Vulkan | **Verified build/run**, Vulkan 1.4.363 advertised | Both use native host KosmicKrisp | Useful new client API; does not require a VM. |
| Metal / OS baseline | Metal 4; documented macOS 26+; executes on 26.7.1 | Hadron requires/tests macOS 27; steamac host supports 26+ | Do not upgrade OS merely to test upstream. |
| Basic renderable RGBA8 | **Verified** 64×64 single-sample, one layer, one mip render-pass clear/store | Reference workloads are broader, independently patched | Shader rendering, XR formats, stereo arrays/MSAA/depth remain untested here. |
| `VK_EXT_metal_objects` | **Absent** from runtime extension list and driver implementation | Neither inspected patch queue adds the missing object path | Current macOS Vulkan binding requires this extension. Not drop-in compatible. |
| `VK_EXT_external_memory_metal` | **Present**, only `MTLHEAP` supported | References do not add IOSurface interoperability | Extension name alone is insufficient. |
| `MTLHEAP` image memory | **Verified** import/export capability query; actual export and same-offset Metal alias | Can help same-process experiments | Export is an Objective-C object pointer, not a transferable process handle. Actual heap import not tested. |
| `MTLTEXTURE` / `MTLBUFFER` external image handles | **Verified rejected** with `VK_ERROR_FORMAT_NOT_SUPPORTED` | No relevant addition located | Cannot wrap our runtime-owned shared texture this way. Buffer external queries were not exercised. |
| IOSurface image import/export | **Missing source/API path** | Host-pointer patches concern guest allocations, not IOSurface import | Existing XPC/Mach-port IOSurface architecture cannot be reused without driver work. |
| Cross-process zero-copy | **Blocked, not demonstrated** | VM shared memory is a different contract | Heap alias has `iosurface=nil`, `shareable=false`; no supported public carrier identified. |
| Vulkan timeline semaphores | **Verified** exact values 1–32; GPU signal, CPU observation | Native implementation uses `MTLSharedEvent` internally | Internal event exists, but no public import/export of it. |
| Vulkan GPU wait | Queue implementation encodes Metal 4 event waits; delayed binary test executes behind a Vulkan timeline gate | Useful foundation for event interoperability | Does not establish Metal consumer ordering. |
| Metal / cross-process timeline handoff | **Missing public API**, not executed | Neither queue inspected implements shared-event exposure | Needs import/export of real event plus existing readiness and reuse protocol. |
| Opaque FD binary/timeline semaphore | **Verified capability flags zero** | No solution for macOS timeline transfer | Cannot use ordinary Unix external semaphore path. |
| Sync FD binary semaphore | **Advertised import/export; actual export CPU-blocks, returns -1** | Mesa generic binary emulation, not a native shareable fence | Reject as frame-critical explicit synchronization mechanism. Nonnegative imports are rejected in source. |
| Sync FD timeline semaphore | **Verified capability flags zero** | No equivalent timeline transfer | Cannot transport expected producer/consumer values. |
| Host memory import | Source supports buffers; image query restricts imported host memory to LINEAR images | steamac adds tiled host-pointer images and device-local type for Venus | Does not prove compatible IOSurface storage/layout or GPU ordering. |
| D3D12 / VKD3D-Proton | Upstream alone is not assessed as game-ready | Substantial downstream feature/correctness queues | Desktop success does not supply OpenXR D3D12 resource wrapping/fences. |
| D3D11 / DXVK | No XR or performance proof; core geometry/wireframe gaps below | Hadron retains DXMT; steamac runs DXVK through Venus with patches | Retain current DXMT; no evidence of improvement here. |
| Performance / pacing | Tiny serialized diagnostic only | External desktop/VM claims use different stacks/hardware | No XR GPU-time, CPU-overhead or frame-pacing advantage established. |

The complete **146 runtime device extensions** are in the probe log (count the
`extension=` records for the exact list). Important positive capabilities include
timeline semaphores, buffer device address, descriptor indexing, synchronization2,
mutable descriptor types and memory budget. Absence of an extension name is not
used to deny functionality promoted to core. The runtime core-feature table records
geometry shaders, non-solid fill, sparse binding/residency and pipeline statistics;
these gaps matter to DXVK/VKD3D and motivate the downstream queues.

## Implementation evidence and semantics

Primary driver files at the tested Mesa commit:

- [`kk_physical_device.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/aac5108bbfa3051f03a71c695b3a6f961ea0fc46/src/kosmickrisp/vulkan/kk_physical_device.c): actual extension/feature tables and registered sync types.
- [`kk_device_memory.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/aac5108bbfa3051f03a71c695b3a6f961ea0fc46/src/kosmickrisp/vulkan/kk_device_memory.c): only heap handles; import retains heap; export returns the backing heap without extending its lifetime unless caller retains it.
- [`kk_image.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/aac5108bbfa3051f03a71c695b3a6f961ea0fc46/src/kosmickrisp/vulkan/kk_image.c): external-format query rejects texture/buffer handle types; image bind creates placement textures on heap offsets.
- [`mtl_heap.m`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/aac5108bbfa3051f03a71c695b3a6f961ea0fc46/src/kosmickrisp/bridge/mtl_heap.m) and `kk_image_layout.c`: Metal descriptors, resource options, optimized contents and offsets. The probe's alias is deliberately limited to this simple descriptor; no generic layout ABI is promised.
- [`kk_sync.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/aac5108bbfa3051f03a71c695b3a6f961ea0fc46/src/kosmickrisp/vulkan/kk_sync.c), `kk_queue.c`, `bridge/mtl_sync.m`: event creation, GPU queue waits/signals, host signal/query/wait; native sync export/import callbacks are null.
- [`vk_sync_binary.c`](https://gitlab.freedesktop.org/mesa/mesa/-/blob/aac5108bbfa3051f03a71c695b3a6f961ea0fc46/src/vulkan/runtime/vk_sync_binary.c): `export_sync_file` waits indefinitely for the underlying timeline then returns `-1`; nonnegative sync-file import returns invalid external handle. The special `-1` import is handled by common sync code as satisfied.

The Khronos definitions distinguish
[`VK_EXT_external_memory_metal`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_external_memory_metal.html)
(memory handles) from
[`VK_EXT_metal_objects`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_metal_objects.html)
(texture/IOSurface/event objects). Neither extension by itself transports an
Objective-C pointer across processes. Native Metal handles or IOSurface Mach
ports and matching device identity still matter.

## Current Monado architecture and synchronization

The authoritative overview is [macos-port.md](../doc/macos-port.md), with
[completion evidence](../doc/macos-gpu-sync-completion.md) and
[Wine synchronization evidence](../doc/macos-gpu-sync-wine.md). The compositor
is **Vulkan/MoltenVK**, followed by native Metal/CAMetalLayer presentation;
calling this an all-Metal compositor would conceal a relevant API boundary.

Current code uses `vk_helpers.c` for IOSurface import via
`VkImportMetalIOSurfaceInfoEXT`; `vk_image_allocator.c` exports textures and
IOSurfaces through `vkExportMetalObjectsEXT`. The Metal direct/service swapchain
implementations choose existing shared textures or client-created IOSurfaces;
XPC authenticates ownership and transfers native handles/Mach ports.
`comp_semaphore.c` imports/exports the actual `MTLSharedEvent` behind a Vulkan
timeline semaphore. `comp_window_macos.m` has the render-complete event and
native presentation wait. `oxr_vulkan.c` currently requires
`VK_EXT_metal_objects` for this platform. Removing that requirement without
replacing the semantics would be incorrect.

The recent contract observes the exact producer value before admitting its GPU
wait, preserves the imported wait even when already satisfied, and observes
consumer completion before returning an image for reuse. Deferred readiness,
finite fault deadlines, retained/quarantined unresolved storage, allocation
budgets and bounded shutdown protect other clients. A producer must join all
queues that write the image before signaling. CPU readiness observation alone
is not the cross-API GPU memory-ordering mechanism.

Game Mode keeps composition in the client with hosted CALayer presentation and
tracking in the service. A new driver must work in both service-backed and hosted
modes. It must not select KosmicKrisp globally for Monado's MoltenVK compositor
just because the application selects it. The runtime's existing UUID selection
also needs cross-driver verification: equal physical GPU is required, but a
MoltenVK UUID is not assumed to equal KosmicKrisp's. Compare native registry/device
identity and loader dispatch ownership before enabling both ICDs in a process.

## Hadron: reusable design versus downstream work

Inspected [source pins](https://github.com/Broyojo/hadron/blob/4308bcd0a76395e22688e485cb892bdf0f9bffa2/sources.conf): Mesa
`30da39187d8d3cf03c58ecd2bebb497b84501f51`, Wine
`6880117619afdf62b4ccc40a8c6268c86613131f`, FEX
`0df84d3844bcdb87bb7d3f5b8fb0959cd009c038`, DXMT
`e86484e585a00d33f2750f96ff7c1ff56bccb06d`, VKD3D-Proton **v3.0.1**, DXVK
**v3.1.1**. This is not the unpatched Mesa revision executed here.

Its [native architecture](https://github.com/Broyojo/hadron/blob/4308bcd0a76395e22688e485cb892bdf0f9bffa2/docs/architecture.md)
uses arm64 Wine, FEX ARM64EC/WoW64 emulation, DXMT for D3D10/11, and VKD3D-Proton
through winevulkan/KosmicKrisp for D3D12. FEX is CPU translation, not Vulkan sharing.
Our current x86-64 Wine/DXMT path need not migrate to that emulator to explore
KosmicKrisp. Its entitlement/low-address and JIT changes are a separate loader
project; the native probe needs none of them.

The [Vulkan build script](https://github.com/Broyojo/hadron/blob/4308bcd0a76395e22688e485cb892bdf0f9bffa2/scripts/build-vulkan.sh)
keeps driver and D3D12 libraries separate and builds only DXVK's DXGI, explicitly
leaving D3D11 on DXMT. Launcher code dispatches foreign D3D12 desktop swapchains
through `dxgi_dxvk.dll`. That presentation mechanism does not allocate or release
OpenXR swapchain images.

Its 84 Mesa patches include device-generated commands, texel alignment,
timestamp pools, geometry/transform feedback, sparse resources, sampler reduction,
64-bit atomics, shader/correctness fixes and Zink/EGL presentation. These are
**downstream changes at the inspected pin**, not upstream capabilities inferred
from game reports. Patches 0002/0003 and sparse/geometry work are candidates to
review independently if D3D12 becomes the next milestone. No texture/IOSurface
or shared-event interoperability implementation was found in this queue.

VKD3D-Proton's three patches address mutable-null descriptor type compatibility,
DGC stage requirements, and desktop minimum-latency frames. The first two are
plausible reusable correctness fixes; the latency option is desktop policy and
should not drive XR pacing. Merge status upstream is not established by their
presence in Hadron. Upstream comparison/rebase and targeted tests are required
before adapting any patch. Hadron's CTS results are project-reported results on
its patched stack, not conformance certification for this local build.

## steamac: host improvements versus VM transport

The [host build script](https://github.com/fxgl/steamac/blob/6ef3992d869e41291bb2841bef2e71ae318d2a02/host/kosmickrisp/build.sh)
pins Mesa `ce576c29ed90822ea15564e1e60cb48e455881bb` plus 47 patches. The
architecture is guest Vulkan/Venus → virtio GPU/libkrun → host virglrenderer →
KosmicKrisp or MoltenVK → Metal. Guest FEX and Proton sit above that transport.
Native OpenXR should call the native ICD directly; none of Venus, libkrun,
virglrenderer, dma-buf scanout or the Linux VM is required.

The script labels upstream MR provenance: geometry !44786/transform feedback
!44928, tiled host-pointer images !44929, device-local memory !44221, linear
attachments !44782 (closed unmerged), and linear array views !44222. These are
queue provenance claims at its pin, not a fresh assertion that each MR is now
merged. The queue also carries local row-pitch, input-attachment, wireframe,
query-pool, sparse, compiler, residency and render-pass performance work.

Native-relevant candidates are resource residency, asynchronous callback teardown,
query capacity, actual shader features, shared compiler lifetime and reducing
unnecessary render-pass/command-buffer splits. Host build tooling's two-stage
`mesa_clc` build removes runtime LLVM dependency and is useful for future packaging.
VM-specific 16K blob alignment, guest host-pointer/dma-buf layout and Venus fence
transport do not implement IOSurface or `MTLSharedTextureHandle` sharing.
Sample-count substitution and zero timestamp fallback must not be adopted as XR
capability claims without correctness review. No reference patch was applied here.

## Executed standalone prototype

Source: [probe.mm](../experiments/kosmickrisp/probe.mm).
Reproduction wrapper: [run.sh](../experiments/kosmickrisp/run.sh).
[Final run](kosmickrisp-evidence/final.log),
[earlier Metal validation run](kosmickrisp-evidence/metal-validation.log),
[negative control](kosmickrisp-evidence/negative-control.log),
[configuration](kosmickrisp-evidence/mesa-setup.log),
[build](kosmickrisp-evidence/mesa-build.log).

The probe refuses non-KosmicKrisp devices, records every extension, queries actual
image/semaphore handle properties, creates a Vulkan graphics device, allocates
an exportable RGBA8 optimal image, exports its actual heap, and creates a matching
Metal placement texture at offset zero. Identity checks verify the exported heap
and Metal alias heap/offset; GPU pixel observations demonstrate shared storage
for this descriptor. There is no intermediate image or image copy in the handoff.
A **diagnostic texture-to-buffer readback copy** is used to check all pixels.

It executes 32 alternating red/green render-pass clears, stores the attachment,
signals values 1–32 on a Vulkan timeline, waits on the CPU with a 5 s limit, checks
the exact value, consumes the alias on a Metal queue, and checks all 4,096 RGBA
pixels per frame. It waits for diagnostic Metal completion before reusing the
image. A deliberately reversed expected-color control fails at the first pixel.
This is clear-only rendering, not a shader/pipeline/hello_xr test. CPU waits make
it unsuitable as a production frame path and do not prove external GPU memory
ordering. The wrapper kills a stuck process after 30 s, including a stuck driver
export or Metal completion wait.

Measured initial runs: 39.670 ms and 38.630 ms for 32 serialized frames; Metal
validation run 36.567 ms. Reproduction-wrapper run 35.051 ms. These include CPU
waits/readback and exclude device startup; they are **not** GPU rendering times,
XR frame latency, throughput or a comparison with MoltenVK/DXMT.
All runs passed 131,072 pixels and exact values. Final-source results and timings
are retained in `final.log` (**35.662 ms** serialized pixel run; **58.181 ms**
blocking sync-FD export). [The manifest](kosmickrisp-evidence/manifest.json) records
source SHA-256 values and exit statuses. Metal validation reports enabled with no API error. The final feature table is
`geometryShader=0`, `fillModeNonSolid=0`, `sparseBinding=0`,
`sparseResidencyImage2D=0`, `shaderInt64=1`, `pipelineStatisticsQuery=0`.

The alias reports `iosurface=0`, `shared_texture=0`. MTLTEXTURE/MTLBUFFER external
image queries return -11; heap query returns success with import/export bits
`0x6`. No cross-process texture consumer is launched: there is no public storage
carrier to transfer through the intended route. Heap pointers must not be sent to
another process. No undocumented/private driver accessor is substituted.

Binary sync-FD export is tested behind a Vulkan timeline gate released from a
CPU test thread after 50 ms. Export returns **fd=-1** and takes **60.886 ms**;
Metal-validation repetition takes **58.826 ms**; wrapper repetition **60.589 ms**.
The controlled delay and source identify CPU blocking, not an exportable GPU
fence. Timeline sync-FD and opaque FD capability queries return zero. No external
Metal event consumer is executed, because the native event import/export API is
absent. This explicitly leaves the cross-process completed-output-at-value
success criterion **unmet**.

### Rebuild and run

Use an isolated temporary directory. Do not replace the system runtime or ICD:

```sh
brew install meson ninja cmake pkgconf llvm libclc spirv-llvm-translator spirv-tools vulkan-headers vulkan-loader
study=$(mktemp -d /tmp/kosmickrisp-study.XXXXXX)
git clone https://gitlab.freedesktop.org/mesa/mesa.git "$study/mesa"
git -C "$study/mesa" checkout --detach aac5108bbfa3051f03a71c695b3a6f961ea0fc46
python3.14 -m venv "$study/venv"
"$study/venv/bin/pip" install mako==1.3.10 MarkupSafe==3.0.4 packaging==25.0 pyyaml==6.0.3
export PATH="$study/venv/bin:$(brew --prefix llvm)/bin:$PATH"
export PKG_CONFIG_PATH="$(brew --prefix llvm)/lib/pkgconfig:$(brew --prefix)/lib/pkgconfig"
meson setup "$study/build" "$study/mesa" --buildtype=debugoptimized \
  -Dplatforms=macos -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= \
  -Dopengl=false -Dzstd=disabled --prefer-static
ninja -C "$study/build" -j 6
icd="$study/build/src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json"
MTL_DEBUG_LAYER=1 experiments/kosmickrisp/run.sh "$icd"
experiments/kosmickrisp/run.sh "$icd" --negative-control
# Last command must return 1 and report a bad pixel; normal command returns 0.
```

Run on an unlocked host with GPU access. Python 3.9 failed configuration; 3.14
passed. Initial wrong ICD filename produced -9; use the generated **devenv** JSON,
which points to the actual built dylib. Successful build emitted duplicate-library
linker warnings; these were not suppressed. `otool -L` shows system frameworks and
Homebrew SPIRV-Tools, not runtime LLVM; this build is still a development artifact.

## Recommended architecture and upstreamable work

**Proposed storage direction:** Monado creates/reuses its ordinary shareable Metal
swapchain images (IOSurface where eligible, shared texture handles otherwise),
transfers them through existing authenticated XPC, and the client imports that
same texture into a KosmicKrisp `VkImage`. Preserve the MoltenVK compositor and
Metal OpenXR path. This avoids new compositors and mandatory intermediate images
*if* texture import and event semantics are implemented and proven.

Prefer upstream `VK_EXT_metal_objects` support for real texture/IOSurface image
imports and real shared-event semaphore import/export. Implementing the full
advertised extension may be a larger task than the minimum OpenXR path; do not
advertise a partially implemented extension. Alternatively investigate extending
`VK_EXT_external_memory_metal` with MTLTEXTURE support, but that extension still
needs an independent standardized event route. Choose with Mesa maintainers
before committing to a private interop ABI.

Driver changes must distinguish imported texture-backed images from heap-backed
memory: do not silently allocate a replacement heap/texture when binding; query
correct memory requirements/dedication; validate dimensions, format, sample/mip
counts, usage and GPU; retain imported objects; add base resources to Metal 4
residency; release views before backing resources. IOSurface import requires
correct plane/row-pitch/layout handling, not a raw host-pointer cast. Event import
must retain the actual event; queue submission signals after all producer writes,
waits at the correct dependency, and exposes consumer completion for reuse.
These are plausible upstreamable additions because the driver already uses Metal
textures and shared events internally. They remain engineering proposals, not a
promise of extension acceptance or cross-process correctness.

A copy fallback could eventually render into a driver image then copy into a
shareable image with explicit ordering. Current public APIs also lack the event
handoff, so even that requires CPU blocking or driver work. It is not the preferred
native path and was not implemented or timed here. The placement-heap alias is
useful diagnostic evidence, not a stable generic texture export API.

## Wine paths: independent assessment

D3D12 is second priority. Hadron demonstrates a native desktop architecture with
VKD3D-Proton/KosmicKrisp, but our bridge currently opens an **ID3D11Device** backend.
A D3D12 capability enum is not an implemented D3D12 OpenXR binding. A separate
backend must wrap runtime swapchain storage as `ID3D12Resource`, match adapter and
native Vulkan device through winevulkan, preserve resource states/ownership,
translate `ID3D12Fence` ordering onto actual exported events, and join submitted
command queues before frame release. VKD3D-Proton resource/native Vulkan wrapping
APIs and their availability through our CrossOver Wine must be inspected and
prototyped separately. No raw PE-side Vulkan handle or Objective-C pointer should
be assumed callable/valid in the native host. Keep this backend in WineOpenXR,
general Vulkan/Metal resource support in Mesa, and generic XR sharing in Monado.
Desktop DXGI dispatch may be reusable for a mirror window, but is not the OpenXR
swapchain or headset presentation contract. No D3D12 bridge test was run.

D3D11 is third priority. DXMT already wraps runtime-owned shareable Metal images
and shared events, with validated offscreen direct sharing and a copy fallback
where handles cannot be reopened. Keep it. Hadron also keeps DXMT; steamac's
DXVK/Venus desktop results do not establish native XR benefit. Consider DXVK only
for a concrete compatibility or measured performance gap, after Vulkan interop
works. Do not add a VM dependency or replace the deployed presentation libraries.

A later matched comparison should fix app scene, stereo resolution/formats,
refresh mode, service versus hosted composition, image count, GPU priorities,
power/thermal state and tracing settings. Measure image allocation identity,
per-eye copies, queue wait/signal placement, app/compositor GPU timestamps, CPU
submission/readiness cost, physical presentation intervals and tail latency.
Use paired A–B–B–A runs, fault/reuse cases and user-run visual PS VR2 checks. Prior
Monado/DXMT measurements remain baseline evidence, not KosmicKrisp measurements.

## Prioritized implementation plan after review

1. Agree with Mesa maintainers on texture/IOSurface and shared-event API scope.
   Keep driver changes on a separate Mesa branch, with dependency pins and focused
   regression/CTS tests. No Monado workaround for driver-private heap layout.
2. Extend the isolated harness to two real processes and the actual public
   carrier: same storage identity, patterned shader output/eye attribution,
   expected event values, GPU waits, consumer-done reuse and wrong/missing/delayed
   signals. Test process death, concurrent producers, invalid descriptors and
   object/device teardown. Only call zero-copy successful after this passes.
3. After that review, opt-in Monado Vulkan client path: preserve MoltenVK
   compositor/Metal binding, negotiate real capabilities/formats, verify GPU
   selection/dispatch with both ICDs, reuse XPC and current readiness/quarantine
   policy. Begin with hello_xr Vulkan, then arrays/depth/MSAA and shader workloads.
4. Separately prototype WineOpenXR D3D12/VKD3D native resource/fence wrapping,
   retain DXMT and current Wine launcher. Evaluate required downstream correctness
   patches independently of ARM64EC/FEX or Steam installation.
5. Compare DXVK/D3D11 only after a concrete need; otherwise keep the established
   direct Metal path. No performance win is presumed from API consistency.

## Provenance and contribution boundaries

No code was copied from Hadron/steamac or modified in Mesa/Wine/FEX/DXMT. New
standalone probe code uses this fork's BSL-1.0 notice. Mesa's inspected bridge and
sync files are MIT; individual Mesa files remain authoritative. Hadron's own
code is BSD-3-Clause; its component patches retain upstream licenses. steamac's
project code is Apache-2.0 with documented exceptions. WineOpenXR is
LGPL-2.1-or-later with preserved BSL/Proton/Khronos notices; DXMT is LGPL-2.1-or-later; bundled components retain their own notices.

[Mesa contribution rules](https://gitlab.freedesktop.org/mesa/mesa/-/blob/aac5108bbfa3051f03a71c695b3a6f961ea0fc46/docs/submittingpatches.rst)
require disclosure for AI-assisted creative work and human-authored comments/
commit messages for upstream submissions. Asahi-specific restrictions must be checked before any work there; no Asahi
files were changed.
[Hadron's policy](https://github.com/Broyojo/hadron/blob/4308bcd0a76395e22688e485cb892bdf0f9bffa2/CONTRIBUTING.md)
requires component patch queues, clean pin application and graphics conformance
results; its FEX AI-written work stays downstream because FEX does not accept it.
[DXMT's contribution policy](https://github.com/NikNakk/dxmt/blob/d116ce4890a557fe9f1043dbcbf717037518dd5f/CONTRIBUTING.md)
does not accept AI-authored contributions and permits research findings without
an AI-generated PR. No DXMT implementation changes are proposed for submission
by this task. Wine's clean-room restriction still applies. No separate steamac contribution
policy was found in the checked-out root; inspect component policies before
adapting patches. This assessment creates no upstream submission and does not
certify third-party patch acceptance or licensing of an eventual binary bundle.

## Shared-event follow-up — 2026-10-14

This event-only checkpoint is superseded by the texture follow-up below. The
user's follow-up authorizes further isolated investigation. **The underlying
shared events work in both directions and across processes with a diagnostic
Mesa patch.** Unmodified upstream still has no supported event import/export
entry point; the matrix above describes that unmodified snapshot.

The patch is [mesa-shared-event-diagnostic.patch](../experiments/kosmickrisp/mesa-shared-event-diagnostic.patch),
on a separate Mesa worktree/branch `codex/openxr-shared-event-diagnostic`, based
on the same `aac5108bbfa3051f03a71c695b3a6f961ea0fc46`. It adds 37 lines to
`kk_sync.c`/`.h`: a retained export of a timeline semaphore's actual event and
replacement of a newly created, never-submitted timeline semaphore's event with
a retained native event. Both check that the semaphore is timeline-backed by
`kk_sync_type`; binary export rejection is exercised. No extension is advertised,
no queue/wait/signal implementation is changed, and the unpatched driver build
is preserved separately.

These **private diagnostic hooks are not a production API or upstream-ready
patch**. The test invokes exported hook symbols via `dlsym` in a controlled
single-ICD/no-layer configuration. Set-event requires a trusted compatible
`MTLSharedEvent` and an unused semaphore; it does not validate arbitrary native
object types/device ownership or make replacement safe during submission.
Copy-event returns a +1 retained object. Production implementation must follow
standard export lifetime, creation-time import and validation rules instead.

### Executed ordering proof

[Shared-event probe](../experiments/kosmickrisp/shared-event-probe.mm),
[XPC peer](../experiments/kosmickrisp/shared-event-peer.mm), and
[runner](../experiments/kosmickrisp/run-shared-events.py) establish these chains:

```text
CPU-controlled test gate (unsignalled while checks run)
  -> imported Vulkan GPU wait
  -> Vulkan image render/store
  -> exported actual producer MTLSharedEvent signal(value)
       -> local Metal GPU wait -> shared-heap pixel readback -> local done signal
       -> XPC peer Metal GPU wait -> GPU buffer fill/check -> peer done signal
  -> Vulkan GPU waits for both imported done events
  -> Vulkan acknowledgment signal(value)
```

The peer is a real second arm64 process. `MTLSharedEventHandle` objects are
transported with NSXPC and recreated using `newSharedEventWithHandle:` on the
matching GPU registry ID. The peer's done event is also transferred this way
and signals the event that the driver's imported Vulkan semaphore waits on.
The peer checks all 4,096 GPU-written bytes against the expected value before
replying; it uses a command-buffer completion callback, not a CPU wait to signal
the event. Producer/local done/peer done/Vulkan acknowledgment values are checked
against each frame's exact expected value.

All work, including Vulkan's return wait and acknowledgment, is enqueued while
the test gate is held. After a 20 ms test observation window, all relevant event
values and local Metal completion must still be pending. Only then does the test
release the gate. CPU waits after enqueue/release are diagnostic completion and
pixel verification; they do **not** supply the producer-to-consumer ordering.
Host sleeps and the future GPU gate are trusted test instrumentation, not a
proposal for Monado's frame-critical readiness policy.

Results on the same M5/macOS 26.7.1, with Metal API validation enabled:

| Case | Result | Evidence |
|---|---|---|
| Same-process Vulkan → Metal → Vulkan | PASS, 32 delayed values, 131,072 correct image pixels | [Initial log](kosmickrisp-evidence/shared-events/same-process.log) |
| Cross-process exported/imported events, first run | PASS, 32 values, local image pixels and 131,072 peer GPU bytes | Repeated with final source below. |
| Cross-process events, final source | PASS, distinct PIDs 37136/37143, 32 pending-before-gate checks and exact values | [Final log](kosmickrisp-evidence/shared-events/cross-process-probe.log) |
| Missing peer GPU wait, final source | Expected failure at value 1; premature completion detected | [Negative log](kosmickrisp-evidence/shared-events/negative-control-probe.log), [runner result](kosmickrisp-evidence/shared-events/negative-control-summary.log) |
| Binary semaphore diagnostic export | Rejected with `VK_ERROR_FEATURE_NOT_PRESENT` | Executed before each successful run. |
| Metal API validation | Enabled in producer and peer; no API errors recorded | [Peer log](kosmickrisp-evidence/shared-events/cross-process-peer.log) and final probe log. |

Final 32-frame test takes **827.525 ms**, including the deliberate 640 ms of gate
observation sleeps, IPC and diagnostics. Vulkan producer-submit median/max are
**8.667 / 71.625 µs**, with the gate still unsignalled after return. This is evidence
that enqueue does not require producer completion, not an XR performance benchmark.
The earlier same-process run takes 943.244 ms; first cross-process run 854.415 ms.
No headset, sustained-load, thermal, Game Mode or physical pacing claim is made.

The local readback still consumes a texture alias at the same heap offset; the
peer writes/checks **its own buffer**. **No Vulkan image storage crosses the
process boundary in this event test.** Cross-process image zero-copy remains
unproven and blocked by the texture import/export gap. The event result does not
validate existing Monado failure/quarantine policy against KosmicKrisp, nor client
kill/recovery, binary semaphore interop, `VkEvent`, concurrent Vulkan devices,
wrong-GPU imports or event lifetime after pending device destruction.

### Reproduction

Configure the baseline Mesa sources/dependencies as above, then create a second
worktree and build directory. Keep the baseline build and system ICD unchanged:

```sh
# From the Monado checkout, with $study and its build environment from above:
repo=$(pwd)
git -C "$study/mesa" worktree add -b codex/openxr-shared-event-diagnostic \
  "$study/mesa-events" aac5108bbfa3051f03a71c695b3a6f961ea0fc46
git -C "$study/mesa-events" apply "$repo/experiments/kosmickrisp/mesa-shared-event-diagnostic.patch"
meson setup "$study/build-events" "$study/mesa-events" --buildtype=debugoptimized \
  -Dplatforms=macos -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= \
  -Dopengl=false -Dzstd=disabled --prefer-static
ninja -C "$study/build-events" -j 6
icd="$study/build-events/src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json"
driver="$study/build-events/src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib"
python3 experiments/kosmickrisp/run-shared-events.py "$icd" "$driver"
python3 experiments/kosmickrisp/run-shared-events.py "$icd" "$driver" --negative-control
```

The runner builds standalone binaries in a unique temporary directory, bootstraps
only a uniquely named test Mach service in the user's GUI launchd domain, and
boots it out in `finally`. It preserves logs and prints their directory. It needs
host GPU/XPC/launchd access and an active GUI login. GPU execution has a 30 s
process deadline, with 5 s configuration/reply waits; expected negative failure
returns runner success only if premature completion is detected. On that failure,
the probe releases its test gate and drains its queue before exiting. The runner
has no production service, runtime manifest, Steam prefix or headset interaction.

[Patch/source hashes](kosmickrisp-evidence/shared-events/manifest.json),
[build log](kosmickrisp-evidence/shared-events/build.log), and
[configuration log](kosmickrisp-evidence/shared-events/setup.log) pin this result.
The Mesa diagnostic patch is AI-generated research code under the existing MIT
file licenses; it is not submitted upstream. Any eventual upstream change must
follow Mesa's contribution rules already recorded above.

### Consequences for the next driver work

The evidence narrows the event problem to **API exposure and lifecycle semantics**,
rather than a need to invent a new synchronization backend. Reuse KosmicKrisp's
existing event-backed timeline and Metal 4 queue wait/signal operations. For a
standard implementation, import at `vkCreateSemaphore` using
[`VkImportMetalSharedEventInfoEXT`](https://docs.vulkan.org/refpages/latest/refpages/source/VkImportMetalSharedEventInfoEXT.html),
and export through
[`VkExportMetalSharedEventInfoEXT`](https://docs.vulkan.org/refpages/latest/refpages/source/VkExportMetalSharedEventInfoEXT.html).
Account for the specification's initial-value behavior: when a timeline type
structure is chained on import, it sets the imported event's `signaledValue` to
`initialValue`. Do not accidentally reset an already-shared live timeline;
initialization and handoff must be coordinated. The diagnostic hook intentionally
preserves the supplied event value and is therefore **not a conformance test of
standard creation-time import semantics**.

`VkEvent` is a separate complication: KosmicKrisp currently implements it with a
small CPU/GPU memory allocation, not `MTLSharedEvent`. Binary semaphores use the
runtime's timeline emulation. Neither can be advertised as supported Metal-event
interop just because timeline export succeeds. A full `VK_EXT_metal_objects`
implementation still needs its remaining object contracts and tests, or a
maintainer-agreed narrower standard API.

Prioritize shared-texture/IOSurface image import next in an isolated Mesa
prototype; keep standard event API work alongside it. The original full
Milestone 2 gate remains unmet until a second process consumes **the actual
Vulkan-produced image storage** with correct GPU ordering and reuse. Production
Monado/Wine integration remains paused.

## Shared-texture follow-up — 2026-10-14

**The combined minimal 2D storage/event proof now passes.** This result applies
to an isolated diagnostic Mesa patch, not unmodified upstream. The driver imports
the exact native Metal texture backed by a non-global IOSurface; a real second
process reads the actual Vulkan shader output from that same IOSurface after
waiting on the exported producer event. Consumer completion returns through an
imported shared event and a Vulkan GPU acknowledgment. No image backing-memory
allocation or image-to-image copy is needed in this route.

### Driver change and source provenance

The complete [diagnostic patch](../experiments/kosmickrisp/mesa-shared-texture-diagnostic.patch)
is **110 added lines across six Mesa files**, including the earlier 37-line event
hook. Apply it once to pristine Mesa
`aac5108bbfa3051f03a71c695b3a6f961ea0fc46`; do not apply the event-only patch first.
The isolated Mesa branch is `codex/openxr-texture-diagnostic`, worktree
`/private/tmp/kosmickrisp-study/mesa-textures`, build
`/private/tmp/kosmickrisp-study/build-textures`. Baseline/event builds are retained.
Exact patch/probe/shader hashes are in the [manifest](kosmickrisp-evidence/shared-textures/manifest.json).
Dependency versions and host remain as above; shader compilation adds
**glslangValidator 16.5.0**.

The useful existing precedent is `kk_wsi.c`: the driver already retains a
Metal drawable texture as a Vulkan image plane without allocating replacement
image storage. The diagnostic hook follows that resource representation for an
external texture, validates GPU registry, format, dimensions, texture type,
mips/samples/usage and the restricted Vulkan image descriptor, then retains the
supplied object. It inserts the texture in the device's Metal residency set,
tracks import state, removes residency at image destruction and prevents a later
ordinary memory bind from replacing the imported backing. Views are created
using the existing image-view code.

This is **not** standard Vulkan allocation/import semantics. The test creates a
normal driver image, then invokes a private hook before creating views or submitting
work; it performs no `vkAllocateMemory`/`vkBindImageMemory` for that image. No
`VK_EXT_metal_objects` or new memory-handle support is advertised. A supported
implementation must implement creation-time native import, memory requirements,
any dedicated-memory wrapper/bind behavior and error/lifecycle contracts for its
chosen standard extension. Validation layers for that future standard API have
not been exercised. Unsupported formats, arrays, mips, multisampling and flags
are rejected by the restricted hook, rather than presented as working features.

### Storage and GPU ordering proof

Sources: [producer](../experiments/kosmickrisp/shared-texture-probe.mm),
[peer](../experiments/kosmickrisp/shared-texture-peer.mm),
[protocol](../experiments/kosmickrisp/shared-texture-peer.h),
[vertex shader](../experiments/kosmickrisp/pattern.vert),
[fragment shader](../experiments/kosmickrisp/pattern.frag),
[runner](../experiments/kosmickrisp/run-shared-textures.py).

The producer creates one 64×64 IOSurface with a deliberately padded **512-byte
row stride**, twice the minimum 256 bytes, then creates a single-sample Metal
texture on plane zero. A retained driver texture export must return that **exact
Objective-C object pointer**, with the same IOSurface pointer. The peer receives
the `IOSurface` object and shared-event handles over NSXPC, opens it on the
matching GPU, and returns its IOSurface ID. Every consumed frame must return the
same ID. No global-ID lookup or global surface is used to establish transport.
The normal Monado architecture uses the same native IOSurface/NSXPC mechanism;
the fixture uses its own service and does not depend on production Monado.

The Vulkan graphics pipeline draws a fullscreen triangle with real vertex and
fragment SPIR-V shaders. Push constants carry the frame value; the fragment
shader writes distinct frame/X-tile/Y-tile values and alpha. All pixels in both
processes are checked against the exact frame-specific pattern. BGRA byte order
is checked independently. This improves on the earlier render-clear-only proof.

```text
trusted test gate
  -> Vulkan GPU wait -> Vulkan shader writes imported texture
  -> real producer shared-event signal(value)
       -> local Metal GPU wait -> diagnostic pixel readback -> done signal
       -> peer Metal GPU wait -> same-IOSurface pixel readback -> peer done signal
  -> Vulkan GPU waits for both consumer events -> acknowledgment signal(value)
  -> diagnostic CPU completion/pixel checks -> permit next image reuse
```

As in the event fixture, every GPU operation is enqueued before gate release.
The 20 ms pre-release observation must show no producer/consumer/ack completion.
There is no CPU producer-completion wait that supplies cross-API ordering.
The CPU waits after enqueue are verification and serialization for a small
reusable readback buffer; this is not a production scheduling implementation.
The peer signals done on its GPU after its source reads; the producer does not
reuse the texture until the Vulkan acknowledgment and actual consumer completion
are observed. The negative control omits the peer wait and must detect premature
consumer completion. On that expected failure, the trusted gate is released and
Vulkan work drained before exit.

There are **two diagnostic texture-to-buffer copies per frame**, one in each
process, solely to inspect pixels. There is no image-to-image handoff copy, no
staging image and no extra compositor. Zero-copy here means the demonstrated
producer/consumer image handoff shares backing storage; it does not mean that
pixel verification has no readback copy or that Vulkan pipeline setup has no
other internal allocations.

### Measured results and limits

Final source, Metal API validation enabled in producer and peer:

| Case | Result | Evidence |
|---|---|---|
| RGBA8, 64×64, one layer/mip/sample, stride 512 | PASS, 32 shader-rendered values, 131,072 correct pixels per process; same surface ID 520 | [Producer log](kosmickrisp-evidence/shared-textures/rgba-probe.log), [peer validation](kosmickrisp-evidence/shared-textures/rgba-peer.log) |
| BGRA8, same descriptor | PASS, 32 values, 131,072 correct pixels per process; same surface ID 246 | [Producer log](kosmickrisp-evidence/shared-textures/bgra-probe.log), [peer validation](kosmickrisp-evidence/shared-textures/bgra-peer.log) |
| Width/format/layer/mip/usage mismatches and duplicate import | All six rejected before GPU use | `descriptor_rejections` in both producer logs. |
| Missing peer GPU wait | Expected failure on value 1; premature completion detected | [Negative log](kosmickrisp-evidence/shared-textures/negative-control-probe.log), [runner result](kosmickrisp-evidence/shared-textures/negative-control-summary.log) |
| Deferred producer values | All 64 normal pre-gate checks and exact producer/local-done/peer-done/ack values pass | Both normal producer logs. |
| Producer/peer Metal API validation | Enabled, no API errors in successful final runs | Producer and peer logs. |
| Teardown | Ordinary completed-work drain and resource release execute without recorded validation errors; temporary test services removed | This is not fault-recovery/lifetime stress coverage. |

Combined final positive runs check **262,144 pixels per process**, 524,288 pixel
observations total, across 64 frames. No stale or uninitialized pixels are observed.
Surface IDs differ between independent test allocations as expected; matching
IDs within a run are required, alongside native texture identity and GPU contents.

The 32-frame loops take **925.175 ms RGBA / 945.013 ms BGRA**, including 640 ms
of deliberate test-gate sleeps and diagnostic IPC/readbacks. Vulkan producer
submit median/max are **44.271 / 138.042 µs** and **44.521 / 119.958 µs**.
These exclude pipeline setup/shader compilation, include fixture noise, and are
not GPU render times or XR performance gains. No matched MoltenVK comparison,
thermal/clock sampling, physical PS VR2 frame pacing or subjective acceptance
was performed. Preliminary render-clear versions also pass, but the final
shader/descriptor checks above are the deliverable's evidence.

The build initially found a missing C forward declaration and missing Metal
`MTLDevice` header in the new descriptor helper; both were fixed. The successful
build retains duplicate-library linker warnings. [Build log](kosmickrisp-evidence/shared-textures/build.log)
and [configuration](kosmickrisp-evidence/shared-textures/setup.log) preserve the
failed attempt and corrected result instead of counting a partial build as a pass.

Remaining limits: stereo arrays/shared-texture handles, sRGB/float/depth formats,
MSAA, mips, Vulkan sampled/storage image operations, host image copies, multiple
queues/GPUs/devices, concurrent clients, sustained reuse, killed/failed producer
or consumer recovery, quota accounting, production XPC authorization, Game Mode
and OpenXR lifecycle. The hook takes trusted native objects and is not hardened
against arbitrary object types or concurrent mutation. Its lifecycle behavior
must not replace Monado's quarantine/readiness policy without separate tests.
The basic experimental storage result is established; production readiness is not.

### Reproduction and next gate

Use the same isolated build environment as the baseline. This full patch includes
the event hooks and applies cleanly to the pristine tested commit:

```sh
repo=$(pwd)
git -C "$study/mesa" worktree add -b codex/openxr-texture-diagnostic \
  "$study/mesa-textures" aac5108bbfa3051f03a71c695b3a6f961ea0fc46
git -C "$study/mesa-textures" apply "$repo/experiments/kosmickrisp/mesa-shared-texture-diagnostic.patch"
meson setup "$study/build-textures" "$study/mesa-textures" --buildtype=debugoptimized \
  -Dplatforms=macos -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= \
  -Dopengl=false -Dzstd=disabled --prefer-static
ninja -C "$study/build-textures" -j 6
icd="$study/build-textures/src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json"
driver="$study/build-textures/src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib"
python3 experiments/kosmickrisp/run-shared-textures.py "$icd" "$driver" --format rgba
python3 experiments/kosmickrisp/run-shared-textures.py "$icd" "$driver" --format bgra
python3 experiments/kosmickrisp/run-shared-textures.py "$icd" "$driver" --negative-control
```

The runner also needs `glslangValidator` (`brew install glslang` if absent). It
compiles the checked-in shaders into its temporary directory, boots only its
unique test service, uses a 30 s GPU-process deadline and 5 s XPC reply limits,
and removes the service in `finally`. Normal cases return zero; the negative
runner returns zero only when the probe returns 1 for premature completion.
Current final source hashes and descriptor/version details are in the manifest.

**Pause for the requested Milestone 2 review.** The prioritized next work is:

1. Agree on and implement supported standard event/texture APIs in Mesa, including
   creation-time import, memory-bind/ownership rules and conformance tests; remove
   the diagnostic hooks from any production build.
2. Extend isolated proof to stereo arrays and relevant sRGB/float/depth descriptors,
   exercise Vulkan sampling/storage and image reuse/failure cases. Treat IOSurface
   eligibility and native shared texture handles as separate carriers.
3. After review, implement opt-in native Vulkan OpenXR clients while preserving
   Monado's MoltenVK compositor and existing Metal path. Verify both-ICD GPU
   selection/dispatch and begin with hello_xr. Keep D3D12/DXVK work separate.

No Monado runtime sources, Wine/DXMT code, deployed libraries, system ICD/runtime
manifests or headset services were changed. Only the isolated experiment and
assessment/index documents are updated. No branch was merged or pushed.

## Milestone 3 — authorized experimental integration, 2026-10-14

The user authorized M3 after reviewing M2. Implementation is isolated on
`codex/kosmickrisp-openxr-m3`, based on `5aa80413e`; runtime implementation `416f276fd`. It now has **physical PSVR2 service-path acceptance and a clean hosted-path
run** (see the follow-up below); actual Game Mode remains unvalidated. The original checkout,
`macos-upstream-clean`, installed services and runtime manifests are unchanged.
Wine, DXMT and M4 remain separate. The M2 pause above records the earlier gate.

### Implemented architecture and compatibility

The client Vulkan frontend now optionally wraps the existing Metal service/direct
provider. That provider owns swapchain allocation and XPC transport. KosmicKrisp
imports those exact `MTLTexture` objects; no replacement image storage or texture
handoff copy is introduced. The existing service MoltenVK compositor samples them.
The runtime-owned `MTLSharedEvent` is imported into KosmicKrisp as a timeline
semaphore. A queue submission signals it after application rendering and release
barriers, then the existing native readiness worker schedules the frame. Existing
producer obligations, consumer completion and storage quarantine remain in force.
There is no second compositor, sync-FD export, Metal release CPU wait, or queue-idle
fallback in the experimental frame commit. Normal Vulkan acquire still waits for
safe native reuse, as required by the existing implicit-barrier path.

| Case | M3 result / limit |
| --- | --- |
| Vulkan enable2, API 1.2, timeline, native Apple M5 | Implemented and tested with both ICDs visible; client selects KK, service retains MoltenVK |
| RGBA8/BGRA8 UNORM, 2D, single layer/mip/sample | Supported; per-eye swapchains and shader/clear rendering pass |
| Cross-process native shared texture handles | Tested through existing Metal provider and XPC, using the imported native storage |
| IOSurface-only carrier | M2 shader/event proof remains valid; M3 runs use the provider's preferred shared texture handle carrier, no separate forced-IOSurface M3 run |
| Producer GPU ordering / reuse | Runtime event values observed; native reuse guard and barrier fences preserved |
| Missing signal | Four failed obligations, zero ready/scheduled frames for that client; session loss observed in acquire; unresolved frontend retained |
| Concurrent clients, killed client, replacement | Healthy 600 frames, killed overlay 25 ready frames, replacement 180; same service |
| Arrays, mips, MSAA, static/protected, depth, storage, mutable | Explicit unsupported; formats limited to the two proven UNORM formats |
| Vulkan layers / generic Vulkan Metal extensions | Unsupported by this experimental C ABI; no partial `VK_EXT_metal_objects` advertisement |
| Feature off / current Metal path | Separate build passes normal Metal simulated-service probe |
| Client compositor / Game Mode | Simulated hosting initially fell back; subsequent physical hosted hello_xr passed. Actual Game Mode remains unvalidated |
| Linux | Apple-only adapter/build guard; Linux build and CI not executed here |
| D3D12 / D3D11 / performance advantage | No new evidence; no Wine or DXMT changes |

Mesa uses a **versioned downstream C ABI**, `kk_get_metal_interop_api(1)`, for
retained device access, creation-time texture import and timeline-event import.
This is an experimental bridge, not a supported Vulkan extension implementation
or conformant external-memory binding. The patch does not resolve standard Mesa
interop upstreaming. It validates texture geometry/format/device and retains the
native object; its trusted-object API is not an arbitrary native-object security
boundary. Monado rejects incompatible library ABI and dispatch ownership before
calling it. Physical-device handles are queried through the Vulkan loader; only
verified device dispatch is passed into the driver ABI. Vulkan layers are rejected
because directly created handles would bypass their object registration.

The separate [Mesa patch](../experiments/kosmickrisp/mesa-openxr-metal-interop.patch)
applies to `aac5108bbfa3051f03a71c695b3a6f961ea0fc46` (Mesa 26.3.0-devel). It
supersedes the M2 diagnostic patches; do not stack them. Licensing is MIT for Mesa
changes and BSL-1.0 for Monado/runner. No code was copied from Hadron, steamac or
DXMT. Mesa's contribution restrictions recorded in M1 still apply: this downstream
experiment is not an upstream submission.

### Results and practical limits

[Manifest](kosmickrisp-evidence/m3/manifest.json) records exact source revisions,
patch SHA256, hardware, OS, Xcode and per-client readiness counts.
Tests ran on Apple M5, macOS 26.7.1 (25G241), Xcode 27.0 (27A266a), without headset.
OpenXR-SDK-Source was `3ed64d0f9bb680f24b80a085091e5c8fab38f7b7`.

- [hello_xr](kosmickrisp-evidence/m3/hello/hello.log): official unmodified Vulkan2
  sample, 8-second rendering interval, exit 0; 325 producer values ready/scheduled.
  Service target readback includes centre `(119,151,151,255)` at frame 246.
- [BGRA probe](kosmickrisp-evidence/m3/bgra/probe.log): 180 submitted, ready and
  scheduled frames; ordinary healthy teardown without retained-frontend warning.
- [Fault suite](kosmickrisp-evidence/m3/faults/results.json): suppressed-signal
  client exit 1 with `XR_ERROR_SESSION_LOST`, measured process wait 2.344 s including
  startup and bounded teardown. Missing API, unsupported array and wrong driver
  exit 1. Healthy and replacement exit 0. Trace scope 3 has four failures and no
  readiness/scheduling; scopes 2/7 have 600/180 ready and scheduled values.
- [CTest](kosmickrisp-evidence/m3/monado-ctest-m3-final.log): 74/74 pass, 18.51 s
  with host GPU/IPC access. Initial sandbox failures were environmental and are
  not counted as successful tests.
- Feature-disabled complete build and [Metal probe](kosmickrisp-evidence/m3/metal-off-summary.json)
  pass. Final adapter also rejects a null loader getter before calling it.
- [Hosted attempt](kosmickrisp-evidence/m3/hosted/probe.log) records rejection
  and service fallback; its exit 0 must not be counted as hosted-path validation.

After removing unused diagnostic export helpers from the versioned driver patch,
a final RGBA probe again observed 180 ready/scheduled frames and exit 0;
[final driver build](kosmickrisp-evidence/m3/mesa-final-build.log) records a
missing `llvm-ar` PATH attempt and the corrected successful build. The adapter
also preserves requested Vulkan transfer usage bits; sampled/storage workloads
remain outside this test coverage.

The fault test found an infinite wait in the existing Vulkan implicit acquire
path, before session loss could be polled. In opt-in mode only, native waits are
now sliced at 50 ms and session loss checked between slices; incomplete storage
never becomes reusable. Swapchain destroy retires unfinished barrier resources.
Session teardown polls completion for at most 500 ms and retains an unresolved
frontend until process exit. This favors safety but is **not a bounded-memory
solution for repeated failed sessions in a long-lived process**. Application-owned
VkDevice destruction after unresolved GPU work remains an application/driver
lifecycle boundary. Device loss, long-run allocation stability, multi-GPU/queue
operation and all acquire/destroy permutations need more validation.

These captures are correctness evidence. They run instrumented simulated targets
with startup/compilation overhead, some concurrent GPU work and 500 ms hung policy.
They do not establish GPU timings, CPU savings, frame pacing or a performance gain
against MoltenVK/DXMT. M2's exact pixel/event validation supports the storage and
ordering claim; hello target readback alone is not a per-frame stale-pixel test.

### Reproduction and next gates

See [M3 build/run instructions](../experiments/kosmickrisp/README-m3.md). Compile
option `XRT_FEATURE_MACOS_KOSMICKRISP` defaults OFF; runtime
`XRT_MACOS_KOSMICKRISP=1` and `XRT_MACOS_KOSMICKRISP_LIBRARY` are client-only.
Keep both explicit ICDs visible to the client, while the service uses MoltenVK.
The runner prepares and removes a unique temporary service, writes logs outside
the repository, and considers abnormal hello termination a failure.

Priorities before wider acceptance:

1. User-operated PSVR2 Vulkan hello_xr test, then the real hosted/client compositor
   path under Game Mode. Verify both ICD selections, visual correctness, lifecycle
   and native timing traces. Keep current production paths available.
2. Device-loss and repeated-session resource budgets, finite timeout/failed-acquire
   state tests, direct compositor lifetime, forced IOSurface transport and sustained
   concurrent clients. Resolve retained frontend policy before production use.
3. Standard creation/import ownership and semaphore APIs in Mesa, Vulkan layer
   compatibility and conformance; extend descriptor support only with pixel/event
   and negative tests. Stereo arrays and sRGB are the next useful formats.
4. Matched native Vulkan/MoltenVK performance captures on the headset, separating
   shader compilation, GPU work, reuse waits, transport, compositor and presentation.
5. M4 D3D12/VKD3D-Proton investigation in the separate bridge, after native acceptance.
   Preserve DXMT; compare D3D11 only with matched measurements.

At the initial M3 report, no merge, push, production installation or physical
hardware run had been performed. Subsequent user-requested runs follow.

### First user-requested hardware viewing run (2026-10-14)

Started Vulkan2 hello_xr on the physical PSVR2 at the user's request, using runtime
implementation `416f276fd` / M3 branch `9da3d1bb5`, with a four-minute viewing
interval added to the runner. Logs at
`/private/tmp/kosmickrisp-study/m3-hardware-view-20261014/` show PSVR2 USB tracking,
the real 4000x2040 display selected at 90 Hz, MoltenVK service composition,
KosmicKrisp client import and advancing producer values. This run is still active
when this note is written: graceful completion, visual acceptance, detailed frame
pacing and Game Mode are not yet established. No refresh mode was changed.


### Physical acceptance and hosted follow-up (2026-10-14)

The user reported **“Works well”** for the service-composited Vulkan2 hello_xr
scene, then requested stopping it and proceeding. The initial run was stopped by
interrupting its runner (client wrapper exit -2); its `finally` block terminated
the sample and removed the isolated service. This is a user-requested stop, not a
graceful sample-exit pass. A subsequent stop-file control sends the sample its
normal stdin shutdown; that physical hosted test exited 0 and cleaned up.

[Hardware manifest and source hashes](kosmickrisp-evidence/m3/hardware/manifest.json)
and raw readiness/presentation traces record:

| Run | Ready / scheduled producer values | Actual presentation callbacks | Median presentation interval | Exit / acceptance |
| --- | ---: | ---: | ---: | --- |
| Service composition, user viewing | 8,119 / 8,119 | 8,353 | 11.122167 ms | User accepted scene; requested interrupt |
| Hosted composition, 30 s viewing interval | 2,534 / 2,534 | 2,533 | 11.122167 ms | Sample exit 0; actual hosted selection |
| Hosted graceful early stop | 686 / 686 | 682 | 11.122167 ms | STOP-file stdin shutdown, sample exit 0 |

Counts include startup/teardown; presentation callbacks include compositor-only
frames and repeated outputs, so they are not distinct application frame counts.
Intervals use sorted unique positive `presented_monotonic_ns`, without warmup
exclusion. These are short correctness captures with full tracing, not a latency
or thermal benchmark. The real display stayed 4000x2040 at its existing 90 Hz mode.
No producer readiness failure appeared in those three runs. The hosted log
confirms `Compositing in-process` and a service-hosted CAContext, with MoltenVK for
the compositor and KK for the client adapter. No fallback occurred in the physical
hosted runs. The user visually accepted the service run; hosted visual acceptance
and actual Game Mode activation/throttling still need separate verification.

Presentation command-buffer GPU intervals have medians 1.375 ms (service) and
1.407 ms (hosted). These measure the presenter's Metal command buffer, including
its shared-event ordering, not total client rendering or all Vulkan compositor
passes. They establish no performance advantage and should not be compared as a
controlled A/B experiment: workloads, duration, head motion and thermal history
were not held constant.

The first MoltenVK baseline aborted before a valid application presentation:
[baseline log](kosmickrisp-evidence/m3/hardware/baseline-abort/hello.log) reports
`MTLPixelFormatDepth32Float` forbidden for an IOSurface texture. The official sample
selected sRGB colour and optional depth on the normal driver, versus UNORM colour
without exported depth on KK. The crash stack places the assertion in MoltenVK
`MVKImagePlane::getMTLTexture` -> Metal IOSurface texture creation. This is a
normal Vulkan-path descriptor/transport blocker found by the comparison, not
proof that KK is faster. No baseline timing result is valid. A useful comparison
must force matching colour-only descriptors or first fix normal depth transport.
No ordinary Metal/Wine path was replaced or altered to accommodate the sample.

Runtime implementation remains `416f276fd`. Runner changes add early stop,
viewing duration and baseline selection; source SHA256 is captured in the hardware
manifest. No production service or library installation, merge or push occurred.
All temporary headset tests have been stopped. Next priorities are actual Game
Mode verification, a matched colour-only baseline, device-loss/repeated-session
resource budgets and standard Mesa import APIs before wider application trials.

### Matching colour-only comparison and frontend cap (2026-10-14)

Runtime and fixture commit `ac96a07fa` adds a process-local cap of **eight active
or retained experimental frontends**, with atomic reservation/release. Failed
initialization releases its reservation; successful safe destroy does too.
Unresolved GPU teardown keeps it charged. The ninth session is rejected before
allocating another Metal provider, returning `XR_ERROR_INITIALIZATION_FAILED`
with an explicit budget diagnostic. Metal clients and feature-disabled builds
are unaffected. This bounds frontend accumulation by count; it does not by itself
bound every byte, reclaim unresolved resources or recover an actually lost GPU.
Existing native swapchain byte/object quotas still govern backing allocations.

The final [budget runner](kosmickrisp-evidence/m3/followup/budget/results.json)
passes with eight failed sessions in one process and exactly one rejection for
session nine. It observes 32 failed producer obligations and no ready/scheduled
frames from those sessions. A separate healthy process creates and destroys
**12 sessions**, with 72 ready/scheduled values and no budget exhaustion. The
runtime is pinned throughout each process, so reloading a fresh module cannot
reset the count and manufacture a pass. [Regression suite](kosmickrisp-evidence/m3/followup/monado-cap-ctest.log)
passes **74/74** after the cap change. The tests simulate missing signals; actual
GPU device loss, driver reset and concurrent reservation races remain untested.

The matching colour-only probes avoid the previous optional-depth crash. Both
allocate two swapchains: BGRA8 UNORM (`VkFormat 44`), 2800x2856, one layer/mip/sample,
three images each. Both request Vulkan 1.2 and submit 180 projection frames,
compositing in-process through the physical 4000x2040 PSVR2 target at its existing
90 Hz mode. They run sequentially with identical tracing and no concurrent test
suite. [Raw summary](kosmickrisp-evidence/m3/followup/matched-summary.json) and
[reproducible analyzer](../experiments/kosmickrisp/summarize-matched.py) preserve
source consumption, driver-call metrics and presentation validity separately.

| Short colour-clear probe | MoltenVK client | KosmicKrisp client |
| --- | ---: | ---: |
| Client exit / submitted frames | 0 / 180 | 0 / 180 |
| Last observed compositor source frame | 179 | 179 |
| Runtime `vkQueueSubmit` calls / total elapsed | 720 / 20.373 ms | 900 / 22.418 ms |
| Runtime `vkQueueWaitIdle` calls / total elapsed | 180 / 9.986 ms | 0 / 0 ms |
| Presenter Metal command-buffer GPU median | 0.997 ms | 1.590 ms |
| Compositor thread CPU median between predictions | 0.375 ms | 0.500 ms |
| Presented callbacks / positive actual presentation timestamps | 186 / 0 | 180 / 0 |

This verifies a useful **additional synchronization path**: KK adds one timeline
signal submit per frame and avoids the normal Vulkan frontend's queue-idle frame
handoff. It does not prove higher performance. The tiny clear workload, short
runs, startup, driver work, tracing, render/present scheduling, uncontrolled power
state and differing underlying transport carriers prevent a general CPU/GPU gain
claim. API elapsed totals exclude direct application calls and are not full CPU
usage; the Metal command-buffer interval is not total Vulkan rendering time.
In particular the KK presenter/CPU medians are higher in these captures, so they
must not be described as a speedup.

Every actual-presentation timestamp is zero in these short probe runs. The cause
has not been established; an executed callback does not establish a displayed
frame. **No scanout cadence or frame-pacing comparison is valid.** The earlier
user-viewed/hosted hello_xr runs retain their independent positive-timestamp and
visual evidence. An initial baseline runner erroneously required semaphore
readiness events on the queue-idle path; it was corrected to inspect compositor
consumption. Earlier runs with four KK images, Vulkan-version differences or
concurrent CTest are excluded from this matching table.

All physical probes and temporary services were stopped. Game Mode was neither
requested nor independently confirmed. Existing port evidence proves why hosted
composition is needed, but these CLI probes do not reproduce a real application's
fullscreen Game Mode/menu transitions. That acceptance gate remains open and
needs independently observed activation/policy plus physical timing traces.

The recommendation remains an opt-in native Vulkan option, preserving Metal and
MoltenVK compositor paths. Next steps: standard Mesa import ownership APIs and
layer compatibility; a real fullscreen native Vulkan client for Game Mode;
long-run allocation/device-loss testing; format/array extensions with pixel/event
proof; and only then D3D12 bridge work and broader performance comparisons. There
is no evidence yet to replace DXMT or alter the Wine bridge.

The follow-up manifest includes final runtime, probe and driver binary SHA256.
The runtime version banner retains its initial configure-time `5aa80413e` label;
source commit `ac96a07fa` and the binary hashes identify these later tests.

### Offscreen driver benchmark (2026-10-14)

This is a direct comparison of the two drivers' rendering cost, outside OpenXR.
[`kkbench`](../experiments/kosmickrisp/driver-benchmark/kkbench.c) renders the
same work through whichever ICD the loader selects. Each frame fills a
4000×2040 BGRA target with one fullscreen triangle, or with strips drawn
separately for the many-draws cases, with two frames in flight. GPU time is
from Vulkan timestamps; both drivers' values agree with wall-clock time per
frame. [`sweep.py`](../experiments/kosmickrisp/driver-benchmark/sweep.py) runs
600 frames per run (30 warm-up frames excluded), four runs per case, ordered
KosmicKrisp, MoltenVK, MoltenVK, KosmicKrisp. KosmicKrisp is the unpatched
`aac5108bb` debug-optimised build; MoltenVK is Homebrew 1.4.2. Same M5 MacBook
Air, no headset. [Raw results](kosmickrisp-evidence/m3/followup/driver-benchmark/results.jsonl).

| Workload | KK GPU ms | MVK GPU ms | KK slower by |
| --- | ---: | ---: | ---: |
| Arithmetic loop ×4 | 0.684 | 0.634 | 8% |
| Arithmetic loop ×16 | 2.492 | 2.426 | 3% |
| Arithmetic loop ×64 | 9.76 | 9.66 | 1% |
| Warped texture sampling, 1 tap | 0.467 | 0.415 | 12% |
| Warped texture sampling, 4 taps | 0.648 | 0.564 | 15% |
| Warped texture sampling, 12 taps | 1.273 | 1.170 | 9% |
| 200 draws | 0.31 | 0.26 | 18% |
| 2,000 draws | 1.01 | 0.945 | 7% |

Repeat runs agreed to within about 0.5%. KosmicKrisp adds roughly 0.05–0.1 ms
per frame and samples textures about 10% more slowly; arithmetic-bound work
converges, as expected when both drivers end in Apple's shader compiler.
Recording and submitting a frame took about 60 µs on KosmicKrisp against
20–48 µs on MoltenVK. At 2,000 draws both were near 0.5–0.66 ms. The
KosmicKrisp build probably retains assertions, so its CPU figures are
pessimistic. At 90–120 Hz the GPU differences are about 1% of the frame budget.
Single synthetic shaders do not represent a game's mix, but nothing here
suggests a rendering advantage for either driver.

The matched OpenXR probe's apparent synchronization benefit, no
`vkQueueWaitIdle` per frame, is not specific to KosmicKrisp. On MoltenVK the
ordinary Vulkan client path falls back to that wait only because macOS has no
opaque-FD timeline or sync-FD fence. An opt-in Metal shared-event handoff for
MoltenVK clients (`XRT_MACOS_VK_CLIENT_SHARED_EVENT=1`, branch
`claude/vk-client-shared-event`, commit `b7bcef7fd`) removes it. Offscreen with
9 ms of CPU and a heavy GPU load per frame, that raised the probe from 26–28
fps (pacing at 30 Hz) to 60 fps. See `doc/macos-env-toggles.md` on that branch.
