<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Native ARM64 Alyx: first headset checkpoint — 20 October 2026

Actual Valve Lua tests, engine/module constructor probes and a bounded headless
Alyx startup now pass their defined checkpoints. Source2Main reaches native
xrizer and fails exactly at the deliberately unavailable OpenXR runtime.
The first user test reached the Monado connection stage but exposed a launcher
cache-isolation bug, now corrected. The next test needs the user and PS VR2/Monado. **There is no rendered Alyx
frame or headset result yet.**

The user explicitly authorized necessary private-memory ABI translation.
Installed game binaries remain unchanged. No game-specific Lua/video text
patch, installed Monado change, Wine/FEX/Proton/Venus dependency, extra GPU
compositor or FDM driver extension was introduced.

## Source and local preparation

- Klepton `codex/alyx-dual-vulkan`: `2ba3b7f`, with raw-SVC guard/traps `b29292e`
  and real floor forwarding `fa65227`, after strict Vulkan `66e30eb`.
- VeRantyx `codex/native-xrizer-vulkan`: host/test implementation `eecb8cc`;
  launcher identity capture `ca6f3ca` (dry-run validated). Public native base
  remains `38c02b9492c9148f6a4ca86dc607ce52aa6f7403`.
- xrizer remains `4c5dc14`. MoltenVK1.4.2 / loader1.4.363.0 were selected by
  the actual headless run's ICD census. Mesa/KosmicKrisp remain unchanged.
- Debian ARM64 libstdc++6/libgcc-s1 `14.2.0-19`, libuuid1 `2.41.5-0+deb13u1`.
  HTTPS package-index size/SHA256 checks pass; archives/copyright notices remain
  in isolated directories, with provenance manifests in the evidence directory.
  These are downloaded runtime dependencies, not installed packages.
- Game root `/private/tmp/alyx-native-game-root` combines the owner's updated
  Windows shared content and original ARM64 depot through APFS clonefile.
  3219 file-clone operations cover 74,167,277,039 logical input bytes. These are
  separate inodes, without write-through links to installed content. A native
  clone-write fixture verifies that source bytes stay unchanged.
- Flat ELF search directory `/private/tmp/alyx-native-guest-libs-ready` contains
  105 names. Exact filenames take precedence over generated SONAME aliases;
  this matters for distinct supplied libvorbis.so/.so.0 files. The established
  Adreno ICD/mimalloc exclusions remain. No original library files are rewritten.

Signed source patches are archived as `klepton-abi-traps-lua.patch` and
`verantyx-valve-lua-startup.patch` under the experiment patch directory.

## What changed and what actually passed

| Check | Observed result |
| --- | --- |
| Real Valve Lua allocator | Three rounds of strings up to8MiB, byte integrity, 262144-element table growth/readback, full GC and close pass. |
| Allocator teardown | Zero live test-owned mmap bytes after each close; peak17,321,984bytes. Tier0 constructor/global mappings are counted separately by scope. |
| mremap fallback | Three calls return ENOSYS; allocator growth/data integrity still pass. |
| Allocation failure | Injected anonymous mmap ENOMEM makes newstate return NULL; subsequent state creation/close recovers. |
| Real setter binding | Actual ELF GOT slot matches the interposer; original low-address Lua range branch is unchanged before/after tests. |
| Imports/ABI rewrites | All five Lua-test libraries have zero unresolved imports, no refused TLS/x18/CTR sites and no code SVC. glibc records zero zero-stub calls. |
| Module relocation | Engine, Vulkan renderer, VR, video and client pass, with up to20 dependencies in the client probe. No constructors execute in this diagnostic. |
| Constructors | All five subsequent probes pass: engine387ms, renderer379ms, VR357ms, video98ms, client905ms. These are single load timings, not frame latency or benchmark distributions. |
| Video SVC handling | 1703 code SVC sites become reserved fatal BRK markers. All are accounted for; none is NOPed or allowed to execute as a Darwin syscall. No marker was reached during the constructor probes. |
| Headless engine | Source2Main runs, the actual native Steam client and native xrizer load, OpenXR explicitly reports ERROR_RUNTIME_UNAVAILABLE, and the engine exits through its VR-init failure path. |
| Existing fixtures/builds | Lua setter fixture, section/segment SVC guards, native trap, guest HOME, native kernel probes, nine managed-thread cases plus100 fast threads pass. Default CLI rejection/explicit opt-in and excluded patch symbols pass. Legacy and constrained host builds link. |
| Original files | All15 original inspected ELF hashes/inventories match the earlier read-only audit after guest execution. Five Lua input libraries are also hashed before/after each test. |

`floor` was a real unresolved import and now forwards through the identical
AArch64 double-to-double C ABI. It is tested with positive/negative fractions.
The Lua path uses Valve's built-in allocator, without dropping max-address limits
or changing its range check. Other engine allocator/page-size paths remain
subject to gameplay validation.

The first broader SVC scan correctly refused a matching data word in Debian's
.gcc_except_table. Section-aware `reject-code` now validates executable-section
ranges and mapped correspondence, retaining the original broad `reject` option.
For modules containing real SVCs, `trap-code` converts them to named fatal BRKs.
It is an unsupported-operation stop, **not Linux syscall emulation** and not an
assumption that every site is a harmless debug break. Reaching one prints
image/offset and x8/x16, then terminates. Owned code verifies SIGTRAP behaviour.
The former generic bare-SVC NOP heuristic is not used by the constrained host.

`--run-constrained` is now an explicit opt-in. It forces strict Vulkan, managed
thread tracking, generic patch-table off, SVC trapping and real constructors;
pre-constructor checks reject imports/refusals/untrapped SVCs. The Lua/video
logic-patch objects remain excluded. Guest HOME, OpenVR registry, Steam discovery,
config and cache are isolated per launch; native Darwin HOME is unchanged.
The real Steam client must be running. No ownership/authentication results are
fabricated. Early SteamAPI callbacks are observed, but full ownership/gameplay
progression is not proven by an early OpenXR-failure exit.

## First user result and IPC-directory correction

The 20 October 08:52 user run loaded native xrizer and the OpenXR loader. XPC
reported the service ready, but the client tried the wrong Unix socket:
`/private/tmp/alyx-monado-first/cache/monado/monado_comp_ipc`.
This was a launcher environment bug, not evidence of an array-import failure.
The installed service socket was observed at
`/Users/nickkennedy/Library/Caches/monado/monado_comp_ipc`.

VeRantyx signed commit `8345f0c` captures Monado's native runtime directory
before isolating game caches and supplies it through `XDG_RUNTIME_DIR`. It
preserves inherited runtime-directory overrides first, inherited cache/monado
second, otherwise native HOME/Library/Caches/monado, matching macOS Monado's
`u_file_get_runtime_dir`. The game still has private cache/config directories.
No host rebuild or installed service/runtime change is required.

Validation: the inert launcher dry-run regression covers all three precedence
cases and private-cache/config preservation; all pass. The real headset-command
dry-run records the normal native directory, verifies its socket currently
exists and leaves the second output directory uncreated. No new headset run was
performed by the agent; connection/session success remains for the user to test.
The signed patch is archived as `verantyx-native-runtime-dir.patch`.

## Next user test

Native macOS Steam must be running and signed in to the account that owns Alyx.
Use the normal PS VR2/Monado setup. From the isolated VeRantyx checkout:

```sh
cd /private/tmp/alyx-verantyx
python3 NativeARM/scripts/launch_xrizer.py \
  --root /private/tmp/alyx-native-game-root \
  --libs /private/tmp/alyx-native-guest-libs-ready \
  --output /private/tmp/alyx-monado-second \
  --runtime-json /Users/nickkennedy/Code/monado-2/build/arm64/openxr_monado-dev.json \
  --timeout 120
```

This selects MoltenVK by default and uses the existing runtime JSON without
editing it or the service. The output directory must be new; change its name on
repeat runs. The wrapper stops its own process group after120seconds, and Ctrl-C
also stops that group. No `-noasserts`/`-nopassiveasserts` flags are supplied.

Report whether it reaches the menu, whether both eyes display correctly, and
any visible/stated error. Logs and selected-driver census are under
`/private/tmp/alyx-monado-second/backend/`; launch metadata also hashes the host,
xrizer, OpenXR loader, engine and selected Monado client library. An exit status
of0 alone does not establish success: the engine used0 for its deliberate
VR-init failure in the headless test.

A headless reproduction uses `--no-xr-runtime` instead of `--runtime-json PATH`.
The explicit unavailable override was checked against the local Khronos loader
source: it does not fall back to a registered runtime. That run did not load
Monado or claim the headset. Its selected-driver census and focused startup
excerpt are in `doc/alyx-valve-lua-results/`, alongside the Lua/module evidence.

## Still unverified / next milestone

There is no actual Alyx Vulkan device/pipeline/FDM negotiation trace yet:
startup stopped at VR initialization before that rendering checkpoint. The
existing driver-boundary tests remain separate evidence. Stereo/depth images,
actual Steam/gameplay progression, video playback, nonzero signals/unwind/raw
syscalls and headset frame timing remain unverified. A reached SVC marker needs
a specific implementation or trap-semantics analysis; do not change it to NOP.

KosmicKrisp still lacks the VK_EXT_metal_objects path required by the present
Monado sharing architecture. Its standalone device tests pass, but end-to-end
OpenXR use is blocked. The next milestone is a MoltenVK first frame plus an
observed Alyx foveation trace, then a minimal reproduction of the actual API
operations. The prior Metal rasterization-rate-map/FDM assessment remains valid;
no substantial FDM implementation has begun.
