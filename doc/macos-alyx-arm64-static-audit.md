<!-- Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0 -->

# ARM64 Alyx read-only audit — 20 October 2026

**Result:** this downloaded engine has a concrete CPU-authored FDM implementation,
subsampled-rendering control, and a dynamic two-eye offset path. This is static
implementation evidence, not proof that the game enables those paths on a Mac.
A promising Lua callback interposition can replace the existing branch patch;
it has not been implemented or run. The existing host is still unsuitable for
a launch with all guest instruction rewriting prohibited.

No game, shared library constructor, Klepton translator or game host was run.
Downloaded binaries, installed runtime, Steam authentication and global path
registries were not modified. Inspection used Python's standard library and
installed LLVM tools. No additional disassembly package is required.

## Files and version alignment

Depot root:

```text
/Users/nickkennedy/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/app_546560/depot_546564
```

Steam cached Linux depot manifest **9198205754520568813**. A direct depot download
does not provide an installed-app build ID in an `appmanifest_546560.acf`, so do
not equate that manifest number with a Steam application build ID.

The Windows installation's manifest initially showed installed build `8694564`,
public target `25487405` and a mounted `previous_release` branch. During the audit
it changed to installed/target **25487405**, public/public, with all 1,454,181,648
download bytes complete. Updating Alyx through Windows Steam was correct.
The content depot is shared. Its installed manifest changed from
**6340340699246199351** before the update to **1436897171613240821** afterwards;
this update therefore refreshed shared assets as well as platform binaries.
`game/hlvr/pak01_dir.vpk` and `maps/startup.vpk` exist. No full VPK verification or
shared-content compatibility test was performed. The old Windows binaries are
not part of the native graphics path; exact equality of platform binary versions
is less useful than matching content manifests and selecting the public branch.

Selected ARM64 ELF GNU build IDs:

| Library | Build ID |
|---|---|
| `libengine2.so` | `b99ac384a6aad379a962d7689507c69745d98182` |
| `librendersystemvulkan.so` | `52e3a74656682c2ccedd9a35ba3e977e33ebe6b4` |
| `librenderingpipelines.so` | `3b85a8fe8706d708bde9f83d328361afecf24175` |
| `libvr.so` | `15aa3fb3b7e37567fc4afb6197b1e2a5f4212ce5` |
| `libvscript.so` | `c83ebd7a671cb30d45253b35aa767b50fe823267` |
| `libvideo.so` | `8fddabbc37dc81f1820136bef558fb5ec5a5d301` |
| `libclient.so` | `457f4f40157ec02aaeefc057295ab23973a0cfa1` |

[Inventory](alyx-arm64-static-results/inventory.json) also records SHA256, size,
relevant strings/symbols and **candidate** direct ADRP/ADD references. Addresses
below are ELF virtual addresses relative to the named module, not file offsets
or ASLR process addresses. The broad candidate scanner is conservative and does
not establish reachability; the findings below were checked in LLVM disassembly.
No game instruction bytes or disassembly listings are redistributed in the repo.

## Foveation findings

### Extension negotiation and requested features

In `librendersystemvulkan.so`:

- `0x2972b4–0x2972d0` checks `-vulkan_disable_ext_fragment_density_map`; the absent-
  switch branch reaches extension-name list construction at `0x298dc4`. The
  disable switch is a concrete fallback test target, not proof of successful
  no-FDM gameplay.
- `0x2993e0–0x29944c` compares available/requested extension names and records
  separate capability flags for `VK_EXT_fragment_density_map` and
  `VK_EXT_fragment_density_map_offset`. There are explicit absent-extension
  branches, so this is not an unconditional hard dependency at the name-list stage.
- `0x298068–0x29809c` conditionally appends
  `VkPhysicalDeviceFragmentDensityMapFeaturesEXT` to a feature chain. It sets
  `fragmentDensityMap=TRUE`, `fragmentDensityMapNonSubsampledImages=TRUE` and
  leaves `fragmentDensityMapDynamic=FALSE` in this path. These are stores in the
  binary's constructed request, not observed calls or successful enabled features.
- `0x298104–0x298134` similarly constructs the offset feature structure with its
  feature enabled, guarded by the offset capability flag. Offset properties are
  constructed near `0x298214`.
- The log site at `0x29b1b0–0x29b1d0` reports offset support for eye tracking.
  It is guarded by the capability flag; finding the message does not show that
  it was emitted.

The render-system module contains neither FDM2 nor QCOM-offset nor KHR-fragment-
shading-rate **extension-name** strings in this inventory. Its embedded compiler
contains SPIR-V/GLSL shading-rate vocabulary, which does not establish Vulkan
KHR shading-rate use. The bundled Adreno driver contains a broader extension
inventory and must not be mistaken for the application's requested features.
EXT and QCOM offset structures have alias values in current Vulkan headers;
a numeric sType alone cannot distinguish the requested extension name.

### CPU generation, render target shape and controls

In `librenderingpipelines.so`:

- `0x68848` starts a CPU routine that sizes storage to `width * height * 2`,
  loops over coordinates and appends two bytes per texel. The routine contains
  discrete region/rate logic and a radial square-root branch near `0x691c8`.
- `0x817b8–0x818e0` calls that routine, optionally duplicates an eye-sized payload
  for a two-layer case, builds a `foveated_fdm_texture_%d_%d_%d` texture request,
  and passes the CPU data pointer and byte count to a render-system virtual
  texture-creation call. The two-layer case is conditional, not proof that an
  actual session uses stereo arrays.
- A `/tmp/fdm_%s_%d_%d.ppm` dump path has a reference near `0x6944c`. This is a
  useful future engine diagnostic that may capture CPU maps without adding a
  GPU readback. The option/default and resulting file contents need runtime
  validation before relying on it.
- Constructor sites identify **`vr_fdm`** (`0x9441c–0x9443c`) and
  **`vr_render_subsampled`** (`0x9448c–0x944ac`). Other descriptions explicitly
  expose foveation level, square/radial selection, inner/outer size, minimum rate
  and dumping. This audit does not claim their defaults or guess names not
  established from the constructor arguments.

This establishes at least one **CPU-authored** map path; it does not rule out
other GPU-generated paths. The image format, actual upload entry point, selected
map pattern, refresh frequency, stereo layer count and shader-visible density
use still need the Vulkan trace. In particular, a radial map must not be assumed
to fit Metal's independent horizontal/vertical rate functions.

### Dynamic gaze offsets

`librendersystemvulkan.so` contains `vr_fdm_offset`, `vr_fdm_offset_flip_y` and
`vr_fdm_offset_debug` constructor references around `0x277ce8–0x277d48`.
The pass-end path beginning near `0x26ade0`:

1. Tests rendering/offset support and a runtime-facing predicate.
2. Obtains two float coordinate pairs through a runtime-wrapper virtual call.
3. Optionally flips Y, scales against rendering dimensions and rounds to the
   queried FDM offset granularity (`lroundf` sites `0x26af58`, `0x26af7c`).
4. Iterates for **two eyes** and prepares two offset pairs.
5. Builds `VkRenderPassFragmentDensityMapOffsetEndInfoEXT`, sType `1000425002`,
   with count 2 near `0x26b024–0x26b05c`, chaining it into the pass-end call.
   The exact called function-table entry needs dispatch-name tracing.

The debug text includes validity, per-eye centres, render area, granularity and
offsets. Missing/invalid gaze branches initialize zero offsets. This suggests a
static/cached density map moved by gaze rather than regenerated every frame,
but actual caching and runtime behaviour remain unobserved.

`libvr.so` requests `IVRSystem_026`, `IVRCompositor_029`, `IVRInput_011` and
`IVRMailbox_001`; these agree with the intended xrizer interface generation.
The renderer calls a wrapper object here, so its vtable offset must not simply
be treated as the same offset in raw `IVRSystem_026`. xrizer's two eye-tracked
centre methods still return false. Correlating wrapper calls to those methods
is a required runtime trace, not a completed ABI validation.

No decisive Steam Frame/vendor-ID gate for this FDM path was established. There
is a Steam Frame tiler-optimization description and an Alyx message about its
bundled driver; neither proves a foveation gate or justifies identity spoofing.

## Guest-code repair audit

Sources inspected: VeRantyx `4c354a6` on native base `38c02b9`; Klepton `debda5f`
on `ce74582` plus the VeRantyx prerequisites. No source implementation changes
were made to these components during this read-only audit.

| Mechanism | What it changes | Replacement / decision |
|---|---|---|
| ELF relocation, symbol interposition, glibc/SDL/OpenVR shims | Loaded data pointers and API bindings | Normal compatibility-layer work; can preserve downloaded files and game text |
| Linux TLS register reads | Instruction encodings in loaded ELF or generated Mach-O | Part of current Klepton ABI translation; disabling it does not create a safe native path |
| Guest uses of Darwin-reserved `x18`; `CTR_EL0` | Veneers/instructions, runtime or generated output | Also ABI/CPU compatibility translation; original files remain intact, but this is not zero rewriting |
| Generic `kl_guestpatch` table | Optional game-logic instruction substitutions | `KL_GUEST_PATCH=0`; table has no Alyx-named entries. Disable for a constrained launch rather than inherit unrelated title patches |
| Bare `svc #0` handling | Runtime loader heuristically replaces selected instructions with NOP | Not safely replaced by a normal libc symbol shim; requires a correct trap/syscall strategy or proof that the affected paths are unused |
| Alyx video guard | Loaded `libvideo.so` prologue at `0x1180a0` | Remove the additional repair in an experimental source build; validate the already-fixed thread-liveness shim and concurrent playback |
| Alyx Lua range check | Loaded `libvscript.so` branch at `0x4dfd8` | Replace allocator registration via the exported callback setter; no text patch should be necessary if callback ABI/default allocator behaviour validates |
| VirtualAlloc/Free alignment/protection adaptation | API behaviour, allocation metadata | Keep page-aware host-side implementation where required; verify 4 KB guest operations do not revoke neighbours on 16 KB host pages |
| Steam bridge/crash-handler interposition | Host symbol binding | Not a game-text patch; retain ordinary owner authentication. Crash-handler policy is separate from entitlement and should be audited independently |

### Video repair: genuinely active for this download

The source's FNV guard for 28 bytes at `libvideo.so:0x1180a0` matches the downloaded
binary: expected/observed **`4dd69d9ecfed2441`**. The hook would therefore apply,
not quietly skip. It is installed unconditionally by `alyx_host.c` before engine
load; `KL_GUEST_PATCH=0` does **not** disable this separate VeRantyx hook.

The source attributes the underlying null profiling object to an erroneous
`tgkill(..., 0)` answer and already contains a host-side thread-liveness fix in
`kl_glibc.c`. That makes removing the extra guard a reasonable first experiment,
not proof that removal is safe. First validate live/dead thread queries and
concurrent decoder/profiler teardown; then a game run without the guard can test
whether the semantic fix suffices. No fixed-address replacement is proposed.

The executable-section scan finds 1,703 exact SVC instruction candidates in
libvideo; the loader's six-preceding-word heuristic classifies 1,641 as bare.
The older README's description of all 1,703 as bare is therefore imprecise for
this source/binary combination. These are static candidates, not executed
syscalls, and literal data in executable sections can also affect raw scans.
Broadly treating them all as harmless debug breaks is not an audited solution.

There is also a source-path difference: runtime ELF `rewrite_tls` has the bare-SVC
NOP rule, while `klepton-ld`'s offline TLS rewrite does not. A translated Mach-O
build cannot be assumed to inherit every runtime ELF repair. Test the path that
will actually launch; do not combine claims from the two loaders.

### Lua: a concrete interposition route

The downloaded `libvscript.so` contains the range-check pattern at `0x4dfc4`, with
the rejecting branch at `0x4dfd8`. Its exported
`lua_VALVE_OVERRIDE_MMAP` is at **`0xc93a0`**, size 24. It stores five callback
arguments into writable allocator state rather than rewriting code.

There is a `R_AARCH64_JUMP_SLOT` relocation at **`0xff6f0`** for that symbol.
The actual Lua initialization path calls its PLT entry at **`0x4f618`**, followed
by `luaL_newstate`. Klepton's `sym_value` checks registered interposers before
binding defined symbols, so this internal call can be redirected through the
normal relocation/interposer mechanism. This is stronger evidence than merely
finding an exported name.

Two small alternatives deserve a fixture before any gameplay:

- Interpose the setter and substitute host page-aware callbacks with matching
  reserve/commit/release semantics and allocation bookkeeping.
- Leave the game-specific callback slots unset and validate Lua's built-in
  mmap allocator. The null-callback branch at `0xc94f0` reaches a routine at
  `0xc7400` which calls `mmap64`, checks a **47-bit** pointer range rather than
  requiring addresses below 2 GB, and falls back through `munmap`/retry paths.

Neither is implemented or validated. The built-in path must be tested for
`mmap64`, growth/shrink, alignment, failure and teardown; the existing host
`mremap` stub returns ENOSYS, so fallback handling matters. Disabling every hook
without testing its whole lifecycle would not be a safe replacement.

This approach can remove the Lua branch-NOP and the max-address-drop workaround
for that allocator. Other legitimate callers of `Plat_VirtualAlloc/Free` may
still require the page-size shim. Keep those concerns separate.

## Constraint and next implementation milestone

“Downloaded game files stay unchanged” and “no game instruction is rewritten,
even in a loaded/generated image” are different constraints. Klepton preserves
the original files but currently needs TLS/x18/CPU translation. The present
audit establishes a plausible route to removing **Alyx-specific logic repairs**;
it does not establish a way to run Klepton with no instruction translation at
all. No launch was made under either interpretation.

Next implementation milestone, before gameplay:

1. Build a source-controlled constrained host mode that never installs the
   separate video/text patch, disables the generic patch table and never forces
   optional graphics features or unavailable-function success stubs.
2. Implement/test Lua callback interposition with a standalone allocator fixture;
   prove there are no writes to executable game pages from this host component.
3. Validate thread-liveness semantics and explicitly resolve the raw-SVC strategy
   for the chosen runtime-ELF or generated-Mach-O path.
4. Once the allowed ABI-translation boundary is explicit, assemble an isolated
   game root using existing shared content and the original ARM64 depot, keeping
   saves/config/cache writable only in isolated directories. Audit the existing
   symlink script: its `rm -rf "$OUT"` and writable linked content subdirectories
   make it unsuitable to run blindly on an existing location.
5. First MoltenVK/xrizer run: record actual requested/available/enabled Vulkan
   features, `vr_fdm` / `vr_render_subsampled` controls, map payload/layers,
   pass-end offsets, shader built-ins and raw OpenVR gaze-centre results. Compare
   the normal extension-unavailable path with the explicit FDM-disable switch;
   never advertise FDM or spoof GPU identity merely to enter it.

This is still before FDM implementation and before any installed-runtime change.
The CPU-authored map and granularity-aligned offsets make a narrower Metal
prototype more plausible than an unrestricted GPU-generated FDM implementation,
but radial maps, non-subsampled semantics, reconstruction/depth and shader
coordinates remain substantive correctness work.

## Implementation follow-up

The [constrained-host follow-up](macos-alyx-constrained-host.md) implements the
Lua setter replacement and native fixtures, with a preflight-only host gate.
A joined pthread still answers alive through the existing kernel query; guest
thread-lifetime tracking is now an explicit prerequisite to retiring the video
patch. Valve Lua allocator lifecycle and actual game execution remain unverified.

A subsequent opt-in managed-thread registry now passes real-shim lifecycle tests,
including guest destructors, cancellation and detached threads. See the linked
constrained-host report for its precise scope and remaining game-execution gates.
