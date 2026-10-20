<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Native ARM64 Alyx: first headset checkpoint — 20 October 2026

Actual Valve Lua tests, engine/module constructor probes and a bounded headless
Alyx startup now pass their defined checkpoints. Source2Main reaches native
xrizer and fails exactly at the deliberately unavailable OpenXR runtime.
The first user test reached the Monado connection stage but exposed a launcher
cache-isolation bug, now corrected. The next test needs the user and PS VR2/Monado. **No rendered Alyx frame has been verified.**

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

## Second user result: Vulkan device creation

The repeat user run gets past the socket failure: xrizer logs creation of an
OpenXR Vulkan session. Alyx selects Apple M5 and attempts its own vkCreateDevice,
which fails. The old trace did not record the VkResult or feature values, so the
specific cause is not yet established. All 22 distinct requested extensions
(23 entries, including duplicate image_format_list) appear in MoltenVK's driver
census. This rules out a simple unadvertised extension-name request; it does not
prove valid feature/extension dependencies, queues or device-create semantics.
The six pNext structs are multiview, dynamic rendering, dynamic rendering local
read, host image copy, extended dynamic state and extended dynamic state3.
No FDM feature struct or FDM extension enable appears in this device request.
That does not establish all foveation queries or future fallback behaviour.

Klepton signed commit `cdc60bc` adds observational request logging for core
feature words, known pNext payload words, queue families/counts/priorities and
the exact driver/enumeration VkResult. Structure tail padding is labelled as
such; payload indexes must be decoded against the Vulkan registry. No feature
clamping, dropped extension, retry or fabricated success is added. The existing
constrained host binary has been rebuilt with this diagnostic.

The fake-driver fixture checks exact feature payload/error logging and unchanged
requests with no retry. The real MoltenVK standalone test passes device creation
and rejects an intentionally unsupported geometryShader with -8 while retaining
the requested bit. The sandbox initially hid Metal (-9); the same standalone
check outside the sandbox passes. Neither test loads Monado or claims the HMD.
Focused user-request and fixture evidence is retained in
`alyx-valve-lua-results/device-create-second-run/`. A third user run is needed
to observe the actual Alyx device failure code and feature request.

## Third user result: exact unsupported feature and engine fallback

Alyx's actual request returns `VK_ERROR_FEATURE_NOT_PRESENT` (-8).
Decoding all core and six extension-structure feature payloads against the
Vulkan registry, then querying MoltenVK in a standalone reproduction, finds
exactly one requested-but-unsupported field:
`VkPhysicalDeviceExtendedDynamicState3FeaturesEXT.extendedDynamicState3ColorWriteMask`.
The other enabled field in that structure is
`extendedDynamicState3SampleLocationsEnable`, which is supported.

On Apple M5 / MoltenVK1.4.2 (driverVersion10402), both Vulkan1.2 and1.3 standalone
runs reproduce the captured request's -8. Clearing only ColorWriteMask in the
standalone fixture succeeds. Omitting the entire state3 structure and extension,
modelling the engine's disabled-extension path, also succeeds. These are native
device tests, with no game, Steam, Monado or headset loaded. The per-feature
comparison and fixtures are retained in `alyx-valve-lua-results/device-create-third-run/`.
The earlier diagnostic's hand-written compact fixture initially used the wrong
second enabled field; it was corrected to SampleLocationsEnable before committing
and the corrected fixture passes both versions. The registry-driven comparison
identified ColorWriteMask correctly throughout.

Read-only inspection of the owner's renderer confirms an existing engine option
`-vulkan_disable_ext_dynamic_state_3`: the CommandLine query conditionally skips
the extension-enable path. Original and cloned renderer hashes still match the
original static audit. No binary or Vulkan feature query is patched.

VeRantyx signed commit `eb01bb2` adds that engine option by default for MoltenVK.
`--extended-dynamic-state3 enabled` restores the original request for diagnosis;
`disabled` applies the same fallback to either driver, while default `auto` leaves
KosmicKrisp unchanged. The launch manifest records the effective choice. Controlled
driver comparisons must explicitly select the same engine policy. Existing strict
dispatch still preserves feature requests and errors; no clamping or retry is used.
Launcher dry-run regression covers default MoltenVK fallback, explicit enable,
KosmicKrisp auto and explicit disable across all three runtime-directory cases.
All pass; the real launch dry-run preserves native IPC discovery and enables the
engine option. Runtime confirmation that Alyx takes this fallback, plus any
subsequent rendering failures, still requires the next user run.

Standalone reproduction (the first invocation uses Vulkan1.3; add any argument
for Vulkan1.2):

```sh
clang -I/opt/homebrew/include -L/opt/homebrew/lib \
  /private/tmp/alyx-verantyx/NativeARM/host/tests/alyx_device_request.c \
  -lvulkan -o /private/tmp/alyx-device-request-test
VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
  /private/tmp/alyx-device-request-test
```

## Fourth user result: device succeeds, sample-location call faults

The built-in state3-disable option removes the broad state3 feature request:
actual vkCreateDevice returns success. Alyx creates its synthetic mirror
swapchain, then jumps through NULL at renderer return VA0x266684. Read-only
callsite/dispatch initialization inspection identifies global slot0xa52830 as
`vkCmdSetSampleLocationsEnableEXT`; the observed call requests VK_FALSE.
The command remains used by this build despite its broad state3-disable option.
Therefore the engine option alone is an incomplete fallback. Missing shader
cache and the engine's unusual printed driver-version formatting are not the
identified fault; the loader census remains authoritative for driver identity.

Signed Klepton commit `23bcf0d` adds opt-in
`KL_VK_SAMPLE_LOCATIONS_COMPAT=1`. For a device requesting VK_EXT_sample_locations,
it checks real extension/feature support, adds VK_EXT_extended_dynamic_state3
and a feature struct enabling **only** SampleLocationsEnable, and forwards the
result to the driver. It does not request unsupported ColorWriteMask. Caller
features/pNext remain unchanged. An existing state3 structure is preserved;
if it does not enable SampleLocationsEnable, this explicit profile fails rather
than rewriting it. Missing native support fails explicitly. No no-op command,
NULL suppression, feature removal, graphics emulation or extension advertisement
is introduced. This is an explicitly logged additive device-negotiation
exception to the otherwise strict profile, not observational instrumentation.

Signed VeRantyx commit `d72bdbe` enables that profile automatically for MoltenVK
when using the broad state3 fallback. `--sample-locations-compat enabled|disabled`
permits explicit comparisons; default `auto` leaves KosmicKrisp unchanged. Both
the original request and forwarded extension/pNext chain are logged, and the
manifest records the effective choice. Compare drivers with explicit matching
policies. The constrained host has been rebuilt; installed Monado is unchanged.

Actual native MoltenVK tests reproduce a NULL device proc address with the
profile off. With it on, the real command resolves and records VK_TRUE followed
by VK_FALSE, submits and completes. Tests also verify unchanged caller create
info, existing feature chains and persistent rejection of unsupported
ColorWriteMask. The existing strict fake-driver regression still passes.
Launcher dry-runs cover all runtime-directory precedence cases, automatic driver
selection and explicit profile overrides; all pass. Focused evidence is in
`alyx-valve-lua-results/sample-locations-fourth-run/`.

```sh
cd /private/tmp/alyx-klepton
KL_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.1.dylib \
VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
  sh tools/check_vulkan_sample_locations.sh
```

The next user run must confirm this resolves the actual engine call and expose
any subsequent failures. It remains a startup checkpoint, not a first-frame or
stereo-correctness result. FDM remains unimplemented and disabled in observed
Alyx device requests.

## Fifth user result: profile guard skipped; actual renderer replay now passes

The next user run faults at the same sample-location call. Its manifest correctly
sets KL_VK_SAMPLE_LOCATIONS_COMPAT=1, but its request contains neither
VK_EXT_sample_locations nor VK_EXT_extended_dynamic_state3. The engine's broad
state3-disable option removes both. The first profile required sample_locations
in the incoming list and silently skipped activation. No compatibility-enable
message or added extension/feature appeared in that run. The previous standalone
fixture retained sample_locations, so it failed to model this prerequisite.
That fixture's success did not establish that the profile activated in Alyx.

Signed Klepton correction `829f1ec` applies the explicit profile even when both
extensions are omitted, after checking real support for both and the feature.
It reserves space for the portability addition plus both extension additions.
It still preserves every caller-requested feature/structure and rejects missing
support or unsupported caller features. The native regression now starts with
neither extension, and passes command resolution, record/submit/completion and
unchanged-request/unsupported-feature checks.

Signed VeRantyx test commit `1b8cb5d` adds a closer reproduction, using the owner's
actual renderer and exactly this run's captured core features, four feature
structures,20 extension entries and queue request. The harness checks the known
renderer SHA256 before using private entry RVAs. It loads/relocates the renderer
with constructors disabled and executes **only its generated Vulkan dispatch
loaders**, then reads the actual function slot. It does not run Source2Main,
Steam authentication, OpenXR or a headset session. Original and cloned renderer
hashes remain unchanged after the tests. Private-memory ABI translation remains
necessary; no game-specific Lua/video logic patch is linked or enabled.

With compatibility off, device creation passes but the actual renderer slot is
NULL, reproducing the failed startup prerequisite. With compatibility on, the
actual slot matches the native driver's function, and calls through it record
VK_FALSE/VK_TRUE/VK_FALSE, submit and complete. The exact captured caller
structures remain byte-for-byte unchanged. The existing strict fake-driver and
launcher policy/environment regressions pass. Evidence is in
`alyx-valve-lua-results/sample-locations-fifth-run/`.

Reproduction after building the `valve-vulkan-dispatch` target in the isolated
constrained host directory (use profile0/expected0 for the negative case):

```sh
KL_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.1.dylib \
VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
KL_VK_SAMPLE_LOCATIONS_COMPAT=1 \
XR_RUNTIME_JSON=/private/tmp/disabled-openxr-dispatch-test.json \
  /private/tmp/alyx-constrained-host-build/valve-vulkan-dispatch \
  /private/tmp/alyx-native-guest-libs-ready 1
```

The host is rebuilt with the corrected profile; the launcher's default profile
selection is unchanged. A sixth user run is needed for the full engine path.
The dispatch-loader reproduction establishes the previously NULL function slot
and native command behaviour; it does not establish game-frame correctness.

## Next user test

Native macOS Steam must be running and signed in to the account that owns Alyx.
Use the normal PS VR2/Monado setup. From the isolated VeRantyx checkout:

```sh
cd /private/tmp/alyx-verantyx
python3 NativeARM/scripts/launch_xrizer.py \
  --root /private/tmp/alyx-native-game-root \
  --libs /private/tmp/alyx-native-guest-libs-ready \
  --output /private/tmp/alyx-monado-sixth \
  --runtime-json /Users/nickkennedy/Code/monado-2/build/arm64/openxr_monado-dev.json \
  --timeout 120
```

This selects MoltenVK by default and uses the existing runtime JSON without
editing it or the service. The output directory must be new; change its name on
repeat runs. The wrapper stops its own process group after120seconds, and Ctrl-C
also stops that group. No `-noasserts`/`-nopassiveasserts` flags are supplied.

Report whether it reaches the menu, whether both eyes display correctly, and
any visible/stated error. Logs and selected-driver census are under
`/private/tmp/alyx-monado-sixth/backend/`; launch metadata also hashes the host,
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
