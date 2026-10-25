<!-- Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0 -->

# Native ARM64 Alyx: dual Vulkan investigation — 19 October 2026

**20 October follow-up:** ARM64 game binaries are now available. The
[read-only binary and host-patch audit](macos-alyx-arm64-static-audit.md) establishes
CPU-authored FDM and per-eye offset code paths, plus a possible callback-based
replacement for the Lua instruction repair. No game has been run. The
19 October unavailability statements below describe that initial checkpoint.

**Stop-point result:** selectable ICDs, a built native xrizer host adapter, initial
foveation tracing and offscreen tests are ready. **KosmicKrisp is not yet a working
Monado/OpenXR backend.** It lacks `VK_EXT_metal_objects`, required for the existing
zero-copy texture and shared-event transport. Keep MoltenVK as the default. Do not
start a general FDM implementation before obtaining actual Alyx API evidence.

No native ARM64 game files were available (confirmed by the user). No game,
headset, Steam login, installed runtime or global OpenVR registry was changed.
No Wine/FEX/Proton/VM/Venus dependency was introduced. No driver extension was
added or advertised. All changes are local; nothing was pushed.

## Sources and isolation

| Component | Inspected/tested sources | Workspace and disposition |
|---|---|---|
| Monado | Integration base `4534aceaf`; test prerequisite `914b89368` | `/private/tmp/monado-alyx-dual-vulkan`, branch `codex/alyx-dual-vulkan`; build `/private/tmp/monado-alyx-build` |
| xrizer | `claude/macos-port`, `03e5dac`; implementation `4c5dc14` | `/private/tmp/xrizer-dual-vulkan`, branch `codex/macos-dual-vulkan` |
| Klepton | VeRantyx-pinned upstream `ce74582`; prerequisites `67ef51f`; selection/trace `debda5f` | `/private/tmp/alyx-klepton`, branch `codex/alyx-dual-vulkan` |
| VeRantyx | `native`, `38c02b9492c9148f6a4ca86dc607ce52aa6f7403`; adapter `4c354a6` | `/private/tmp/alyx-verantyx`, branch `codex/native-xrizer-vulkan`; host build `/private/tmp/alyx-native-host-build` |
| Mesa | Current mirror `0913fce718feb08571529f1ca97ebf05d1df634a`, 27.0.0-devel | `/private/tmp/alyx-mesa`, clean; release build `/private/tmp/alyx-mesa-build` |
| Old Mesa/archive | `aac5108bbfa3051f03a71c695b3a6f961ea0fc46`; existing local tag `archive/kosmickrisp-2026-10-14`, `1025b3623` | Read locally before retrieval; archive left intact |

Tests compiled the source changes before final commits, so embedded build git tags
can name the base with working changes; the commits above identify the equivalent
source. [Manifest](alyx-vulkan-results/manifest.json) records full source hashes and
evidence hashes. Host: fanless M5 MacBook Air, macOS 26.7.1 / 25G241, Xcode 27.1 /
27A9275, Homebrew LLVM 23.1.3. Mesa built with release optimisation, no private patches.

Initial primary Monado checkout: `claude/wsi-fragment-output`, `72ef4518e`, ahead 8,
with untracked `tools/macos-present-compare/`. Initial xrizer checkout:
`windows-openxr-d3d11`, `c823e31`, ahead 2, clean. Neither was switched or reset.
Claude subsequently edited the two legacy-Vulkan Monado files in the primary
checkout; those edits remain untouched. My isolated equivalent changes are a
**duplicate test prerequisite**, not a second integration proposal. Prefer Claude's
reviewed candidate and do not merge both fixes. His fixes are now committed in
the primary checkout as `4a873825d` and `90a6cdb64`; my source-equivalent isolated
prerequisite remains separate.

Klepton, VeRantyx and Mesa were initially absent and fetched as public source.
The earlier xrizer macOS port was compile-only; its existing submission probe had
not been run on this Mac. There was no configured native Alyx launcher to test.
The archived worktree paths had been cleaned up; the tag preserved all experiments.

## Implemented changes and launch commands

Klepton originally `dlopen`s MoltenVK directly. `KL_VULKAN_LIBRARY` now explicitly
selects a common native Vulkan loader and fails without falling back when the
selected library is missing. Leaving it unset preserves the bundled MoltenVK path.
No KosmicKrisp dependency is introduced to the default path.

xrizer's `XRIZER_VULKAN_LIBRARY` previously tried the requested path and then
silently searched alternatives. It now fails on explicit load errors and rejects
a different globally visible `vkGetInstanceProcAddr`. Locally scoped unrelated
libraries remain undetectable through this check: all participants must obey the
same-loader contract. Monado's legacy Vulkan path also uses that native loader;
no raw ICD handles are mixed with loader handles.

The xrizer launcher sets one `VK_DRIVER_FILES`, plus the same loader path for
Klepton and xrizer, verifies driver ID in a separate probe before launch, and
records ICD JSON, library SHA256, loader, command, physical device, driver/version,
UUID and offered extensions. A negative test rejects a MoltenVK ICD when
KosmicKrisp was requested. It does not install anything. Build:

```sh
cd /private/tmp/xrizer-dual-vulkan
cargo build --locked
sh tools/macos/build-probe.sh
```

ICD-only tests, requiring ordinary logged-in GPU access:

```sh
python3 tools/macos/run-vulkan-backend.py --output /tmp/mvk-new -- \
  "$PWD/target/vulkan_submit_probe" --driver-only
python3 tools/macos/run-vulkan-backend.py --driver kosmickrisp \
  --icd /private/tmp/alyx-mesa-build/src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json \
  --output /tmp/kk-new -- "$PWD/target/vulkan_submit_probe" --driver-only
```

With a separately started matching simulated Monado service, set
`XR_RUNTIME_JSON=/private/tmp/monado-alyx-build/openxr_monado-dev.json` and
`XRIZER_OPENXR_LOADER=/path/to/libopenxr_loader.dylib`. Replace the command after
`--` with `"$PWD/target/vulkan_submit_probe" "$PWD/target/debug/libxrizer.dylib" 180`.
Use fresh output directories. The complete tested isolated-service recipe is in
[the experiment README](../experiments/alyx-vulkan/README.md).

The probe now logs physical-device/driver identity and extensions, supports
`--driver-only`, enables available timeline semaphores on its Vulkan 1.2 device,
and returns failure for OpenVR wait/submit errors. It records startup-to-first
handoff and CPU frame median/p95/p99 after 30 warmup frames. xrizer's temporary
macOS instance requests Vulkan 1.2; its refreshed lockfile fixes a pre-existing
`--locked` build failure. Fake Vulkan extension queries now support the macOS
unit-test path, which previously aborted on a missing core function.

VeRantyx's opt-in `VR_RUNTIME=xrizer` build links a small factory-forwarding
adapter in place of all custom runtime/render/readback objects. It uses
`XRIZER_VRCLIENT` and the existing guest `/vrclient.so` host-module redirect.
`VERANTYX_OPENVR_ROOT` allows an isolated path registry. The ARM64 host builds;
the adapter test loads actual xrizer and forwards its factory. Game C++ ABI and
interface compatibility remain untested. No extra GPU compositor/copy/readback
was added. xrizer retains its existing two GPU eye copies into the OpenXR array.

**Game-launch blocker beyond absent content:** VeRantyx's host and Klepton
prerequisites translate ELF code and patch loaded guest instructions, including
LuaJIT/video repairs. The existing approach cannot simply be run under the user's
no-game-binary-modification constraint. Audit/replace these behaviours first;
no game patching was executed. Steam authentication remains with the signed-in
owner's macOS Steam client. No depot was downloaded.

## Actual test results

| Test | Result |
|---|---|
| Native Mesa release build | Pass; LLVM/SPIR-V dependencies available; duplicate-library linker warnings |
| ICD identity | MVK driverID 14, MoltenVK 1.4.2; KK driverID 28, Vulkan 1.4.363 / Mesa git-0913fce718; both Apple M5 |
| xrizer unit tests | 83 pass, 3 already-known input failures excluded as in branch CI; two existing unused stage-function warnings |
| Monado build/regression | Pass, **77/77 CTests** including 18 GPU-labelled tests |
| Final xrizer/MVK | **180 frames, zero errors**, 896×1007 per eye; 488.168 ms startup-to-first handoff; warmed CPU frame median/p95/p99 **16.607/19.513/19.910 ms** |
| xrizer/KK | **Fails temporary device creation**, `ERROR_EXTENSION_NOT_PRESENT`; required `VK_EXT_metal_objects` absent |
| Native Monado Vulkan array probe/MVK | 60 projection frames, 60 Hz, zero skipped predicted display periods; layers=2, three shared images |
| Native texture/event import/MVK | **491,520 exact pixels**, colour/sRGB/float/depth/stencil, one/two layers, event values 1–4; independent XPC colour peer passes |
| Klepton Vulkan boundary | Both ICDs create an instance, enumerate M5 and answer FDM feature queries; FDM=false; missing library fails |
| Metal rate-map API | Two independently offset eye layers, 2000×2040 logical → **1056×1082 physical** each; inverse coordinates and MTL4 descriptor identity pass |
| VeRantyx/xrizer adapter | Native ARM64 host links; actual factory forwarding and unknown-symbol rejection pass |
| Full Vulkan CTS | Not run; API version/census is not a new conformance certification |
| Linux CI / Intel Mac | Not run; macOS guards preserve the existing Linux extension branch, but this is not a Linux validation result |
| Actual Alyx / headset | Not executed; no game API, gaze, stereo visual or presentation-latency result |

The initial sandbox GPU run failed with `VK_ERROR_INCOMPATIBLE_DRIVER`; the host
GPU run passes. The first service probe used an installed client manifest and
correctly failed the version check. A matching build-tree manifest resolves that.
The uncorrected legacy Monado path then rejected array creation; the isolated
Metal flag/FD-list prerequisites resolve this for MVK. KK's later failure is a
separate, genuine capability blocker. Raw evidence is [checked in here](alyx-vulkan-results/manifest.json).

The successful xrizer path still uses Monado's legacy CPU handoff fallback:
timeline availability is not proof that OpenXR knows the externally created
device enabled the feature. Native enable2 texture/event tests validate the GPU
handoff separately. Prefer explicit enable2 creation/feature tracking in future;
do not infer enabled device features from an availability query. Offscreen first
handoff and predicted cadence are not physical display latency measurements.

## Paired performance evidence

Eight workloads, 4000×2040, 600 measured frames, two frames in flight; separate
first observations followed by warmed KK–MVK–MVK–KK runs. Entries below are the
mean of each driver's two run medians (ms). Full per-run GPU median/p95/p99, CPU
record+submit median/p95/p99, compilation, process time, RSS and thermal snapshots
are in [the data](alyx-vulkan-results/benchmark.jsonl) and
[summary](alyx-vulkan-results/benchmark-summary.json).

| Workload | KK GPU ms | MVK GPU ms | KK difference |
|---|---:|---:|---:|
| ALU ×4 | 0.683 | 0.634 | +7.7% |
| ALU ×16 | 2.492 | 2.426 | +2.7% |
| ALU ×64 | 10.696 | 11.191 | −4.4%, inconclusive |
| Sampling ×1 | 0.484 | 0.438 | +10.6% |
| Sampling ×4 | 0.645 | 0.558 | +15.6% |
| Sampling ×12 | 1.289 | 1.206 | +6.9% |
| 200 draws | 0.335 | 0.264 | +26.9% |
| 2000 draws | 1.107 | 0.981 | +12.8% |

All four warm samples within each workload have matching public thermal states:
nominal for the first four cases, fair for the last four. ALU ×64 drifts from
10.145 to 11.247 ms across KK's pair despite remaining nominal; **do not claim a
reliable KK win**. GPU frequency and power were unavailable; nominal/fair do not
prove identical clocks or absence of throttling. Some early CPU measurements
overlap source builds/probes, so CPU tails and maximum RSS are exploratory.
Repeat with background activity suppressed and cool-down gates before making
an investment decision based on small performance differences.

Observed process RSS ranges: KK 26.5–42.2 MiB, MVK 29.2–32.5 MiB, not whole-system
GPU memory. Pipeline compile observations span 0.6–225.1 ms KK and 1.0–98.7 ms MVK.
First observations are kept separately; **they are not guaranteed cold caches**.
The per-user Metal cache was not deleted or purged. This is a rendering workload
comparison, excluding presentation and game shader compilation/stalls. It does
not supersede the archived 1–18% result with a universal performance claim.

## Archive comparison and zero-copy requirements

Only four KK source files differ between archived `aac5108bb` and current
`0913fce718`: `nir_to_msl.h`, `kk_image.c`, `kk_image_layout.c/.h`. The main change
calculates mip-tail offsets without relying on an API that returns unusable
values for ordinary textures on macOS 26. The named physical-device,
command-draw and Metal render-state files are unchanged. All three archived
interop patches pass `git apply --check` on the current tree; none was applied.
This is source applicability, not validation or an upstream-ready implementation.

Still useful: same-texture identity checks, image format/usage validation,
residency and ownership accounting, preserving imported event values, exact
GPU-gated cross-process pixels, failed-wait lifetime protection, array/depth
coverage, render-pass load/store handling and shader compilation benchmarks.
Do not restore the private `kk_get_metal_interop_api` ABI or Monado's retired
special driver frontend. The earlier patch's duplicated forward declaration and
narrow texture restrictions are additional reasons not to promote it unchanged.

KK's `EXT_external_memory_metal` supports **MTLHeap** handles; that does not import
Monado's existing `MTLTexture` or `MTLSharedEvent`. KK already backs internal
Vulkan timelines with Metal shared events and GPU waits, but its `vk_sync_type`
has no direct opaque-FD/sync-file import/export callbacks. Its advertised FD
extensions therefore do not establish compatibility with Monado's transport.

An upstream-compatible solution should implement the required **standard Metal
object** lifetime, import/export, dispatch and capability semantics: device,
shared texture/IOSurface and shared-event paths with format/usage/residency
validation, retained ownership and initial event values. Port the archived
proofs into driver tests and cover imported textures as already bound, aliases,
arrays/depth, views, failure/reuse and cross-process handles. Partial private
hooks must not be advertised as full `VK_EXT_metal_objects`. No Monado-specific
workaround or mandatory copy is justified at this point.

## What Alyx actually does: still unknown

Neither driver exposes `VK_EXT_fragment_density_map`, `...map2`, EXT/QCOM offset
or KHR fragment shading rate in the tested extension census. The smoke fixture
queries FDM features and observes false. **That is not evidence of what Alyx
queries or uses.** The shipped Adreno-library name in VeRantyx's source inventory
also proves no runtime foveation mechanism.

Initial `KL_VK_FOVEATION_TRACE=1` instrumentation records guest function resolution,
extension enumeration and requests, pre-/post-clamp device feature chains,
FDM/FDM2/FSR feature values, image usage/geometry, render-pass/dynamic-rendering
FDM attachments, and original SPIR-V capabilities/BuiltIn decorations. No trace
branch changes Vulkan answers. Existing Klepton extension filtering, in-place
feature clamps, async pipelines, identity spoof options and missing-function
success stubs are pre-existing behaviours that need auditing/strict handling
before interpreting a game trace as its native requirements.

Still needed: offset payload decoding; descriptor/buffer/image-view correlations;
map uploads and compute writes (including copy2/secondary-command aliases);
actual shader capture/disassembly; OpenVR property/capability queries and engine
configuration. xrizer currently returns false for both eye-tracked foveation
centre APIs. GPU identity/Steam Frame/OpenVR gating, map generation on CPU/GPU,
stereo arrays versus per-eye targets, shader-visible density and unavailable-
extension fallback are **all unverified for Alyx**. Do not spoof identity or
extensions to force a path. A future capture must cover unmodified requests,
driver answers, enabled features and actual map attachments before extracting
a small standalone replay with no copyrighted game shaders redistributed.

## FDM implementation assessment

Metal's `MTLRasterizationRateMap` accepts one horizontal and one vertical sample
function per layer and produces compact physical-coordinate attachment data.
The local API test confirms M5 supports two differently offset eye layers and
`MTL4RenderPassDescriptor.rasterizationRateMap`; it does not demonstrate Vulkan
semantics or reduced GPU rendering time. See Apple's [rate-map API](https://developer.apple.com/documentation/metal/mtlrasterizationratemap).

A general 2D Vulkan density field is not necessarily separable. A separable
approximation must be checked against allowed Vulkan fragment-area constraints;
choosing lower density than requested is unsafe. Rendering more finely may
reduce acceleration and still needs semantic validation. Simply binding a
Metal rate map leaves several problems:

- Vulkan logical `FragCoord`, derivatives, fragment size/invocation built-ins,
  viewport/scissor, sample positions and storage-image coordinates need analysis
  and possibly NIR/MSL lowering. A correct shader-visible density answer is not
  provided by a Metal descriptor alone.
- Compact colour/depth attachments must share the exact map. Load/store, input
  attachments, sampling and depth comparison need coordinate transforms and
  reconstruction. Monado's existing native Metal foveation intentionally still
  rejects depth-layer use pending its compact-depth audit.
- Track immutable map/revision per image/subresource and submitted frame, not a
  mutable current gaze value. Separate eyes, multiview and shader layer selection
  must agree. Dynamic gaze means map regeneration/caching and per-eye offsets.
- GPU-generated arbitrary maps cannot be fed directly to the inspected public
  Metal map-descriptor creation API. CPU readback would violate the intended
  path and add latency. A known CPU-generated separable pattern is a much
  smaller prototype; it cannot be assumed to match Alyx.
- KK must carry the selected map through its lazy pass creation,
  `kk_flush_render_pass`, suspended/resumed rendering, compute interruptions,
  attachment reloads and custom resolves. No current setter or density
  attachment processing exists in its bridge/render path.
- Imported textures and timeline ownership must remain correct across those
  passes. xrizer currently copies ordinary eye images: copying compact pixels
  into a logical OpenXR array without mapping metadata would be wrong.

Turnip provides reusable Vulkan-side patterns for attachment state, per-view
coordinate transforms, offset lifetime, subsampled-image metadata, shader
lowering and apron/reconstruction tests. Its GMEM binning, register/LRZ rules
and layout choices are Adreno-specific, not a Metal implementation template.
See Mesa's [FDM design discussion](https://docs.mesa3d.org/drivers/freedreno/fdm.html)
and `tu_cmd_buffer.cc` / `tu_subsampled_image.*`. Preserve the distinction between
logical, raster and stored coordinates rather than copying its hardware rules.

Three outcomes remain open:

**A — accelerated Alyx-compatible subset:** plausible only if the observed map,
offsets and shader/attachment use fit Metal's separable mechanism, with matching
colour/depth and correct reconstruction. Demonstrate actual fragment-work GPU
savings including any reconstruction cost before making this claim.

**B — correct, non-accelerated compatibility:** possible as an emulation project,
but ignoring the map or returning success does not establish correctness. All
advertised features and shader-visible semantics still need implementation/tests.
There is no performance reason to prioritize this now.

**C — substantial emulation required:** likely for unrestricted GPU-generated,
nonseparable, dynamically offset maps or attachment interactions that cannot map
to the public Metal API. This is a limitation of the required mapping, not proof
that all conceivable implementations are impossible.

## Required changes and next milestone

| Project | Next required work |
|---|---|
| KosmicKrisp | Standard texture/IOSurface/shared-event object interoperability before XR testing; then conditional rate-map bridge, rendering-state tracking, coordinate/shader/sampling lowering, capabilities and conformance tests |
| Klepton | Keep common loader selection; audit feature filtering/no-op stubs/async scheduling for a strict native path; finish full observational capture and replay extraction |
| xrizer | Keep dispatcher selection and diagnostics; audit actual OpenVR ABI; implement truthful gaze-centre support only after observed need; explicit Vulkan enabled-feature tracking; handle compact images/reconstruction if a future FDM path needs it |
| Monado | Use Claude's legacy Vulkan fixes; retain existing shared texture/event transport. No KK private frontend. Only add compact-map/depth transport if real client requirements demonstrate the need |
| VeRantyx host | Adapter is built; replace/audit binary translation and runtime instruction repairs under the user's constraint; authenticate normally; validate actual game interfaces |

**Recommendation:** invest first in the native MVK Alyx/xrizer baseline and real
API capture. No KK performance or FDM advantage is established; Metal's mapping
constraints apply to both drivers. If the observed subset fits Metal, MoltenVK
has the shorter route to this existing Monado transport. KK becomes attractive
if its standard interoperability lands and Mesa NIR/Vulkan infrastructure makes
the observed feature set materially easier. Do not fund a general FDM extension
on the assumption that conformance to core Vulkan 1.4 includes it.

Next milestone: resolve the guest-code-modification constraint, obtain the owner's
ARM64 files, run the MVK host through xrizer with a matching isolated Monado build,
and capture foveation negotiation/attachments/shaders. In parallel, propose a
standard Metal-object interoperability design for KK and validate it with the
existing texture/event fixtures. No need to change the installed service.

Planning estimates (one experienced graphics engineer; not delivery guarantees):
1–2 weeks for capture, a replay and a two-eye CPU-authored separable Metal
render/depth test; another 3–6 weeks for a narrow **MoltenVK** accelerated prototype
if the observed semantics fit; **KK adds roughly 3–6 weeks** for standard Metal
interop/lifetime tests before comparable XR validation. Unrestricted FDM,
GPU-produced maps, offset variants and conformance can take months and may need
an API change or substantial emulation. Stop here before substantial FDM work.

## Native engine checkpoint

The [20 October Valve Lua/headless startup report](macos-alyx-first-headset-check.md)
now provides actual Valve-library and Source2Main evidence, an isolated cloned
game root and the first user headset command. No headset rendering or actual
Alyx foveation negotiation is claimed yet.
