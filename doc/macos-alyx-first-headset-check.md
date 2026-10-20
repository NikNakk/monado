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

## Sixth user result: import guard stops the font stack

The corrected sample-location profile activates and Alyx passes the previous
NULL call. The run exits3, about2.05seconds after launch, when loading the
Pango/FreeType font stack: missing GLib/GObject/GIO, FriBidi, compression, PNG and
Expat dependencies leave56 unresolved FreeType imports. The safety guard refuses
constructors. This is an explicit import failure, not an observed GPU fault.
Command-buffer purge warnings (2577 allocated, zero currently available) occur
earlier and remain a separate unresolved observation; no first game frame is
claimed. The total run time is a startup observation, not a frame benchmark.

Twelve public Debian ARM64 packages are now extracted into
`/private/tmp/alyx-font-runtime`, with HTTPS package-index size/SHA256 verification,
archives and copyright notices retained. Nothing is installed. The expanded
allowlist in the existing fetch script records exact versions/URLs/hashes in
`font-stack-sixth-run/dependency-manifest.json`. A new flat search directory
`/private/tmp/alyx-native-guest-libs-fonts` has162 names, preserving all previous
inputs and adding the new runtime. The old directory remains untouched.
Twenty-one font-stack libraries now pass relocation diagnostics with zero
unresolved imports or refused ABI rewrites.

Constructor probing then exposed the existing FreeType/HarfBuzz dependency
cycle:29 imports bind only after all members are mapped. Immediate validation
and constructors ran too early. Signed Klepton `66cf7a0` adds opt-in
KL_LOAD_DEFER_INIT=1: map the recursive group, rebind missing imports, run all
new-image validation hooks, then dependency-order constructors. Original hook
load paths/aliases are preserved. A Lua regression initially caught alias-path
changes from canonicalization; that was corrected before committing. Existing
non-opt-in loader behaviour is preserved. Missing-import checks are retained.

Signed VeRantyx `c05f415` enables this ordering in the constrained host, provides
an owned cyclic-ELF fixture and real font-layout test, and writes private
Fontconfig configuration/cache per launch. System macOS fonts are read in place;
no fonts or game binaries are copied/modified by this preparation step. The
existing private game clone remains the execution root. No installed Monado
runtime, service, OpenXR JSON or Steam authentication behaviour is changed.

Actual local checks, with no headset session:

- Owned AArch64 cyclic libraries: both validation hooks see resolved imports
  before either constructor; constructors call across the cycle and return77;
  repeated loading does not reinitialize.
- Real font constructors: all21 libraries load with zero unresolved imports,
  and the probe exits0 with zero libc fallback-stub calls.
- Real Pango/FreeType/HarfBuzz/Fontconfig layout: English83x15pixels and mixed
  Greek/Arabic71x16pixels, zero unknown glyphs, zero libc fallback-stub calls.
  This verifies layout execution, not headset typography or game font assets.
- Valve Lua allocator regression under the new ordering: all previous allocation,
  teardown, mremap-fallback and ENOMEM/recovery checks pass; five input hashes
  remain unchanged and libc reports zero fallback-stub calls.
- Engine constructor probe with the expanded library directory passes, with
  zero unresolved imports and libc fallback-stub calls. Launcher environment/
  driver-profile dry-run regression passes.

The constrained host is rebuilt. Use the new library directory in the next
command. The full engine must still confirm progression past fonts and expose
any subsequent rendering/synchronization failure. Focused evidence is in
`alyx-valve-lua-results/font-stack-sixth-run/`; no game code/disassembly is included.

To reproduce preparation in fresh output directories, use the existing
`fetch_lua_runtime.py --packages` interface with the12 names from its retained
manifest, then `prepare_guest_dir.py` with the GNU, UUID and font runtime trees
as inputs. Build `check-loader-cycle` and `valve-font-stack` targets in the
isolated constrained host build to reproduce the additional tests. The font
layout fixture needs FONTCONFIG_FILE pointing at a configuration containing
`/System/Library/Fonts` and an isolated cachedir; the launcher now writes that
configuration automatically.

## Seventh user result: Panorama OpenSSL imports and whole-tree dependency audit

Font-stack loading now succeeds. The next engine stop is
libpanorama_text_pango.so: missing libssl.so.3/libcrypto.so.3 leave39 unresolved
imports and the guard refuses constructors. The log also identifies missing
libatomic.so.1 in GLib, whose imported functions happened to bind through the
existing shim. No guard has been weakened.

Signed VeRantyx `1b7d0ac` expands the hash-verified fetch allowlist and adds a
static whole-input dependency audit plus a headless Panorama/runtime fixture.
Auditing every unique ELF input also found Ogg and ICU67 gaps. An additional
transitive OpenSSL dependency, Zstandard, was found by the expanded audit.
Five packages are now extracted into additional temporary runtime trees:

- libssl3t64 `3.5.7-1~deb13u2` (libssl.so.3 and libcrypto.so.3).
- libatomic1 `14.2.0-19`, libogg0 `1.3.5-3+b2`.
- libicu67 `67.1-7`, from the official Debian bullseye ARM64 index, preserving
  the exact .so.67 ABI required by the bundled HarfBuzz ICU module. No .so.76
  rename/substitution is used. The fetch script now has explicit --suite support.
- libzstd1 `1.5.7+dfsg-1` for OpenSSL's compression dependency.

Manifests retain exact package URLs, SHA256/size checks and copyright/archive
locations. No package installation or game binary change occurs. The new flat
directory `/private/tmp/alyx-native-guest-libs-runtime` contains193 names for106
unique ELF files. All previous directories remain available.

The final DT_NEEDED audit across **all106 prepared ELF inputs** reports zero
missing non-shim libraries. Expected absent files are classified using the
explicit shim list parsed from the tested Klepton source. This is a packaging
check; it does not establish every import ABI, future dlopen name or game path.
The audit and manifests are retained in `panorama-seventh-run/`.

Actual local runtime checks pass without Source2Main, Steam, network or OpenXR:

- Panorama text module relocation: nine libraries, zero unresolved imports and
  refused ABI rewrites.
- Constructors for libpanorama_text_pango, libharfbuzz-icu, libvorbisfile and
  libpanorama pass under the constrained import/ABI guards and deferred ordering.
- Real guest OpenSSL SHA256 of "abc" matches the known answer; RAND_bytes returns
  success; TLS_method/SSL_CTX_new/SSL_new and object teardown pass. This is object
  lifecycle coverage, not a TLS handshake or authentication test.
- HarfBuzz ICU67 Unicode-functions integration initializes; Vorbis dependencies
  load. Panorama's V8 dependencies pass constructor loading, without claiming
  that JavaScript JIT or gameplay is tested.
- Font-layout regression with the expanded runtime remains83x15/71x16pixels,
  zero unknown glyphs and zero libc fallback-stub calls. The Panorama fixture
  likewise reports zero libc fallback-stub calls. Launcher dry-run regression
  and the next real-command dry-run pass.

No host/runtime code change or rebuild was needed for the new libraries; the
existing rebuilt constrained host is used. The test executable is built by the
`valve-panorama-runtime` Makefile target. Reproduce the static audit with:

```sh
python3 /private/tmp/alyx-verantyx/NativeARM/scripts/audit_guest_dependencies.py \
  --libs /private/tmp/alyx-native-guest-libs-runtime \
  --klepton /private/tmp/alyx-klepton \
  --output /private/tmp/alyx-dependency-audit-repeat.json
```

The output JSON must be new. Continue using the new library directory below.
Full-engine progression, first-frame/stereo correctness and the earlier
command-buffer purge warnings remain unverified or unresolved. No installed
Monado/runtime/Steam authentication change or FDM implementation is included.

## Eighth user result: worker-thread viewport dispatch fault

The eighth run passes dependency checks and progresses into a Vulkan worker.
It faults with SIGSEGV at PC0, renderer return VA0x26d374 (about1.81seconds after
launch). The dispatch slot0xa52788, assigned by the renderer's generated loader
at0x2cddc0, is `vkCmdSetViewportWithCountEXT`. Fontconfig's earlier warnings are
not the identified fatal event; correctness of game-specific font configuration
still needs visual validation. The recorded exit245 is the backend Python
process's modulo256 representation of a -11 child return, not a Vulkan VkResult.
The earlier null sample-location slot is now resolved.

The broad engine state3-disable option also removes EDS1 from the incoming
request, despite the renderer using viewport/scissor dynamic commands.
Klepton signed commit `44985ec` extends the **explicit** compatibility profile:
query real support and add VK_EXT_extended_dynamic_state plus extendedDynamicState
alongside the existing supported sample-location state. It reserves extension
space for all additions, avoids duplicate structs, preserves caller chains/bits,
and fails explicitly on missing support or incompatible existing feature structs.
It does not enable unsupported ColorWriteMask or advertise a new driver feature.
The historical KL_VK_SAMPLE_LOCATIONS_COMPAT setting now controls both supported
state groups needed by this engine fallback. Default non-profile strict dispatch
remains unchanged.

VeRantyx signed test commit `9651574` expands the hash-checked actual-renderer
harness to all12 EDS1 command slots. The eighth-run original device request
matches its fifth-run capture exactly for extensions, features and queues.
With compatibility off, all12 slots are NULL. With it on, every slot equals the
native driver's function. Commands through those real slots record viewport and
scissor counts1/2/1, culling/front-face/topology, depth/stencil states and vertex
buffer binding with real allocated memory, then submit and complete. The existing
sample-location commands remain covered. This exercises recording/submission;
it does not test a graphics pipeline consuming those states or a rendered image.

The separate native regression verifies existing EDS1/EDS3 chains are preserved,
false requested enable fields are not rewritten, and unsupported ColorWriteMask
requests still fail. The unchanged strict fake-driver and launcher dry-run tests
pass. Focused evidence is in `alyx-valve-lua-results/viewport-eighth-run/`.
The constrained host is rebuilt and the ninth-command dry-run is ready. Full
engine progression, first-frame/stereo correctness, game font mappings and the
earlier command-buffer purge warnings remain unverified or unresolved.

## Ninth user result: xrizer overlay pipeline sample count

The ninth run reaches xrizer's skybox/overlay texture submission path. The stack
passes OverlayMan::set_skybox, Overlay::set_texture and VulkanData's overlay copy
before panicking at create_graphics_pipelines (old line822), with
ERROR_INITIALIZATION_FAILED. The run lasts about5.54seconds and the child abort
is encoded as exit250 by the Python backend; this is not a Vulkan result code.
A scene image or visible headset frame is still not established.

The overlay builder used ash::vk::PipelineMultisampleStateCreateInfo::default(),
whose required rasterizationSamples field is zero. Zero is not a valid sample
count. The render pass already uses a proper SampleCountFlags value derived from
the source/target sample count. Signed xrizer `8417a1d` sets pipeline rasterization
samples to that same value and logs overlay source/target formats and sample
count when creating the pipeline. This fixes a generic invalid pipeline state,
without altering game shaders, Vulkan feature requests or Monado.

A new ignored macOS native-GPU test invokes the **actual private overlay builder
and real embedded shaders**, selecting an explicit ICD through the existing
loader helper. Before the fix, it reproduces the same initialization failure.
After the fix it creates RGBA UNORM→sRGB and BGRA UNORM→sRGB single-sample
pipelines successfully on both MoltenVK1.4.2 and the existing Mesa/KosmicKrisp
build. The test creates no OpenXR instance, game process or headset session.
It verifies pipeline/shader creation, not a draw, image correctness, MSAA
sampling or presentation. The observed test durations are not performance
benchmarks. KosmicKrisp's missing standard Metal-object interop still blocks the
Monado zero-copy path.

The normal xrizer unit suite reports83 pass,1 explicitly ignored native GPU test,
and3 existing input tests filtered exactly as in macOS branch CI. The GPU test
was then executed explicitly on both drivers; two existing unused-stage-function
warnings remain. The existing default-probe path did not cover this shader
pipeline, which is why its earlier stereo-submit result did not catch the bug.
No further runtime compositor or texture copy is introduced.

The isolated debug library is rebuilt with
`XRIZER_VERSION=8417a1d cargo build --offline -p xrizer`. Its SHA256 differs from
the ninth-run library and is recorded beside source revision/build command in
`overlay-ninth-run/xrizer-build-identity.json`. The previous isolated binary is
saved at `/private/tmp/xrizer-before-overlay-fix.dylib`; no binary is committed.
The installed Monado/runtime JSON is unchanged. The next launcher dry-run hashes
the rebuilt library automatically.

Native GPU reproduction (outside the restrictive GPU sandbox):

```sh
cd /private/tmp/xrizer-dual-vulkan
XRIZER_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.1.dylib \
VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \
  cargo test --offline --lib native_overlay_pipeline -- --ignored --nocapture
```

For KosmicKrisp, substitute the retained build-tree ICD
`/private/tmp/alyx-mesa-build/src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json`.
Full-engine user confirmation, overlay/stereo image correctness, multisample
source handling, game-specific Fontconfig warnings and earlier command-buffer
purge warnings remain unverified or unresolved. No substantial FDM work begins.

## Tenth run: sound, fatal allocation and platform VM adapter

The user reports sound before failure. The engine prints an allocation failure
for4096bytes and then faults at tier0+0x6c040. Read-only inspection identifies
an intentional NULL store of0xdeadbeef in its fatal OOM handler. The Panorama
caller is at+0x339e68, following the virtual-commit failure path; the same error
label is also reachable on arena-capacity exhaustion. The log alone does not
provide the failed syscall errno or prove physical RAM exhaustion. Exit245
encodes SIGSEGV through the backend wrapper.

A headless test of the owner's unmodified tier0 exports reproduces a concrete
ABI mismatch: reserve64KiB, commit4KiB at the base succeeds, and commits at
base+4096/+8192/+12288 return NULL with EINVAL. macOS host pages are16KiB;
Panorama uses a hardcoded4KiB default commit granularity. A16KiB commit/write/
read/release control succeeds. No engine/gameplay entry point, Steam or headset
is involved in this test.

Signed VeRantyx `b8b3ccf` adds a constrained-host platform VM adapter, tracking
only reservations obtained through Plat_VirtualAlloc and interposing its
commit/free/protect exports. It combines the logical guest subpage protection
bits for each host page, applies real mprotect operations, keeps neighbouring
data/access alive on partial decommit, and forgets allocations only after a
successful whole-reservation release. Invalid ranges, partial releases and
executable protection changes on tracked reservations are refused. Special
flags/max-address paths and untracked operations retain their original handling.
No generic libc mprotect rounding, game instruction edit, allocator-success
stub, Monado change or additional GPU copy is introduced.

The important limit is protection granularity: one live4KiB subpage can make
its16KiB host page accessible, so individual4KiB guard faults and mixed subpage
protections cannot be faithfully represented. This is logged at startup. The
adapter is a targeted platform compatibility implementation, not general4KiB
virtual-memory emulation or evidence of successful Alyx gameplay.

The adapted real-tier0 test passes successive4KiB commits, data preservation,
decommit/recommit, cross-host-page ranges, host read-only/PROT_NONE queries,
invalid ranges, executable refusal and rounded reservation lengths. The
Panorama dependency fixture passes with zero stubs and verifies its actual ELF
PLT allocation/free imports bind to the adapter. The Lua stress/GC/ENOMEM
recovery fixture passes with unchanged input hashes; launcher regression passes.
Source/binary identity, sanitized fatal excerpt and logs are in
[vm-tenth-run](alyx-valve-lua-results/vm-tenth-run/README.md).
The isolated host is rebuilt; full-engine confirmation is the next user test.

The tenth log separately reports Steam is not logged in during certificate
acquisition. Native Steam ownership/authentication remains unverified. The
warning has not been bypassed; account identifiers/full logs are not committed.
The repeated unsupported stage-override warnings also remain unresolved.

## Eleventh run: first successful simulated-HMD gameplay

On20October2026 the user confirms Valve intro playback, menu access, loading
an existing save and entering the level, using Monado's **simulated HMD**. This
is the first functional native ARM64 Alyx→Klepton→xrizer→Monado gameplay
checkpoint. It does not validate PS VR2 display/input, sustained gameplay,
performance parity or FDM. The user reports substantially lower frame rate
than the best Wine runs and a low-RAM warning at the menu; no FPS/frame-time
numbers or matched Wine configuration were captured.

The tested VeRantyx host is `b8b3ccf`, Klepton `44985ec`, xrizer `8417a1d`
(debug library); source/binary hashes match the tenth-run VM-adapter identity.
The selected driver is MoltenVK1.4.2 through the common Vulkan loader. The log
records first compositor submission, save restoration and map loading. The
platform VM adapter translates subpage commits and the user passes the previous
fatal allocation point. No fragment-density-map or fragment-shading-rate
extension appears in the requested/enabled device extension list. Entry-point
lookups alone are not evidence of using a rendering feature.

The native launcher inherited Klepton's Android-oriented6144MiB budget through
synthetic /proc/meminfo/sysinfo. A direct host query reports25769803776bytes
(24576MiB/24GiB). Signed VeRantyx `74396b5` makes this launcher set the existing
KL_MEM_TOTAL_MB override from host RAM by default; `--memory-mb N` permits a
recorded explicit override. Klepton retains its host cap and process-memory
pressure accounting. Its generic default and all installed runtimes are
unchanged. This changes reported RAM, not physical capacity or guaranteed free
memory; disappearance of the UI warning requires the next user run.

Launcher inert-file regression checks pass (including explicit forwarding and
invalid-value rejection). A real automatic-RAM dry-run records24576MiB and
creates no output directory or engine process. The source patch and sanitized
checkpoint facts are in [gameplay-eleventh-run](alyx-valve-lua-results/gameplay-eleventh-run/README.md).
No full engine log or account identifier is committed.

Performance remains unmeasured. The run uses a debug xrizer library, synchronous
pipeline creation, diagnostic logging, and a simulated-HMD recommendation of
896×1007 per eye at90Hz. The log contains323 repeated unsupported stage-override
warnings. These are profiling candidates, not established causes of the Wine
comparison. Next compare identical scene/resolution/quality, warmed caches,
CPU/GPU frame times and thermal/run order with a separately retained optimized
build. Keep this working binary as a baseline. No substantial FDM implementation
begins at this checkpoint.

## Twelfth run: RAM warning removed; optimized comparison prepared

The user reports improved performance and no RAM warning. The manifest and
synthetic guest memory report both confirm24576MiB. The run reaches first-frame
submission without a fatal OOM log. This validates the RAM-reporting change;
performance improvement remains qualitative. It does not establish a RAM-only
performance cause, because cache/scene/run-order effects are uncontrolled.

The user also correctly recalls the Wine GPU timing fix. Local Wine xrizer has
cb2986a shared history plus3fffac0 D3D11 queries; the native Vulkan branch still
has older synthetic GPU values. Reuse the shared history/validity logic when
adding Vulkan queries. Full findings, tested revisions, retained binary hashes
and limitations are in [performance-preparation](alyx-valve-lua-results/performance-preparation/README.md).

An isolated optional API-wall trace and optimized build are ready.83 unit tests
and the explicit optimized MoltenVK overlay test pass. The original successful
debug library and host remain byte-identical; default library selection is
unchanged. No GPU timing claim or feedback behavior change is made by this
candidate, and no additional compositor/copy is introduced.

## Thirteenth run: optimized stage fallback panic; wrappers hid the cause

The user reports a launcher crash. The backend manifest records a32.04-second
SIGABRT (child-6); first-frame submission preceded the crash. xrizer panics on
"Eye0 has not been submitted" after recreating a stage swapchain for a backend
that cannot render stages. That recreation cleared both submitted eyes after
stereo readiness had already been computed; the fallback used the stale value.
A real frame-controller regression reproduces the exact panic before the fix.
This logic also exists in the original debug build; optimization is not proven
to be the trigger.

Signed performance xrizer2b73755 checks stage support before mutating swapchains,
logs unsupported stages once per controller, and rechecks eye availability
after a supported stage attempt. Only the existing D3D11 stage renderer opts in.
Vulkan retains the application/skybox fallback without losing its scene images.
A second regression confirms that supported-but-failed stage recreation cannot
reuse discarded eyes. Both pass in optimized and debug tests; full unit suite
is85pass,1ignored native-GPU test and3known input skips.

Both Python wrappers then encountered invalid UTF-8 in the crash diagnostics,
obscuring the panic and losing the outer final manifest. Backend a8feb75/5c2b9c0
and VeRantyx aa6ea3f use replacement decoding for display; original log bytes
remain intact. The outer manifest is now also written before launch. A bounded
inert-fixture test runs both real wrappers with a non-UTF-8 byte and child exit7:
raw bytes, final status and both manifests are preserved. Launcher regression
passes. [Focused evidence](alyx-valve-lua-results/stage-thirteenth-run/README.md)
includes no full game log or account identifiers.

The fixed optimized candidate and matching instrumented debug build are retained
separately. Original successful debug library/host are unchanged. Further user
validation is needed; no measured performance improvement is claimed.

## Durable workspace (20 October 2026)

The user correctly flagged that the active experiment was under/private/tmp.
Linked Monado/original-xrizer worktree commits already lived in permanent Git
repositories, but standalone clones, dependencies, binaries and the prepared
game root could be lost when temporary storage is cleared. The complete local
experiment is now preserved at `/Users/nickkennedy/Code/alyx-native`.

Six independent Git mirrors and checkouts retain available local history and
shallow boundaries with no object alternates. Builds, extracted dependencies,
prepared game files/saves and private run logs were copied using APFS clones.
741 temporary-path symlinks were relocated, with no unmapped links remaining;
all193 active guest library names and six critical binary hashes match the
originals. The relocated VM test, default/optimized launcher dry runs and both
driver-only censuses pass. No full-game test has yet used the permanent paths.
See [the durable workspace record](alyx-valve-lua-results/durable-workspace/README.md).
Original temporary paths and the installed Monado runtime remain untouched.
Use the permanent sources for further work; old build metadata may retain debug
source paths and should be regenerated when rebuilding from the new sources.

## Next user test: optimized xrizer

Keep native Steam signed in and the current simulated-HMD setup. Run:

```sh
~/Code/alyx-native/run-alyx --optimized --api-trace \
  --output ~/Code/alyx-native/runs/next-test --timeout 180
```

Load the same save/scene, retain graphics settings and remain stationary for
30seconds after loading. Note the approximate elapsed time at level entry.
The launcher retains the automatic24GiB budget and common MoltenVK loader.
It stops only its process group after180seconds; use a new output directory
for each repeat. This is still a user-run gameplay check, not an unattended
benchmark. Omit --optimized to return to the original working library.

For a matched instrumented debug comparison, use
/Users/nickkennedy/Code/alyx-native/workspace/xrizer-perf-target/debug/libxrizer.dylib and a new output directory.
API CSV analysis (optionally select a steady-state window after loading):

```sh
python3 ~/Code/alyx-native/workspace/alyx-verantyx/NativeARM/scripts/summarize_api_trace.py \
  ~/Code/alyx-native/runs/next-test/xrizer-api.csv
```

The resulting median/p95/p99 describe API wall duration and pose-call cadence.
They are neither GPU execution nor presentation latency. Do not use the current
native GetFrameTiming GPU fields as measured data; those remain placeholders.

## Still unverified / next milestone

Alyx now creates a real Vulkan device and reaches overlay submission, with
requested device features/extensions traced in the earlier checkpoints. No
actual FDM enable is verified; user-confirmed level rendering uses the simulated
HMD. Standalone driver and
pipeline tests remain separate evidence. Stereo/depth images,
sustained gameplay, PS VR2 display/input, nonzero signals/unwind/raw
syscalls and headset frame timing remain unverified. Intro video playback and
loading a saved level are now user-confirmed on the simulated HMD. A reached SVC marker needs
a specific implementation or trap-semantics analysis; do not change it to NOP.

KosmicKrisp still lacks the VK_EXT_metal_objects path required by the present
Monado sharing architecture. Its standalone device tests pass, but end-to-end
OpenXR use is blocked. The next milestone is a measured MoltenVK gameplay baseline plus an
observed Alyx foveation trace, then a minimal reproduction of the actual API
operations. The prior Metal rasterization-rate-map/FDM assessment remains valid;
no substantial FDM implementation has begun.
