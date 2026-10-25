<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Strict Vulkan boundary and raw-SVC guard — 20 October 2026

The isolated Klepton path now has opt-in strict graphics dispatch and a
conservative raw-syscall execution guard. Both Vulkan drivers initialize and
create devices through the strict boundary on Apple M5. **Alyx is not launched**;
the allowed private-memory instruction-translation boundary needs clarification.
These tests do not establish game compatibility or remove the Metal interop
blocker for KosmicKrisp/OpenXR.

## Revisions and changes

- Klepton strict graphics: `f2b0c23`; raw-SVC policy: `66e30eb`, after thread
  prototype `e7c22d9`. Branch `codex/alyx-dual-vulkan`.
- VeRantyx fixture integration: `e97ae52`, after thread tests `0659a70`.
  Branch `codex/native-xrizer-vulkan`.
- MoltenVK 1.4.2, Homebrew Vulkan loader 1.4.363.0; unmodified KosmicKrisp
  Mesa `0913fce718feb08571529f1ca97ebf05d1df634a` (27.0.0-devel).
- xrizer remains `4c5dc14`; no Monado runtime change.
- Signed reconstructable patches: `klepton-strict-vulkan-svc.patch` and
  `verantyx-svc-guard.patch` under `experiments/alyx-vulkan/patches/`.

`KL_VK_STRICT=1`, latched on first use, preserves requested API version,
layers, features/pNext and unsupported extension names. It does not clamp/retry
failed device creation, substitute a physical device, enable optional Metal
extensions or return successful stubs for missing functions. Graphics, shader,
render-pass and capability commands bypass the legacy repair/emulation wrappers.
Missing entry points return NULL. More than 64 extension names are preserved.

The explicit exceptions remain synthetic desktop WSI and required Vulkan
portability handling. Implemented synthetic surface-extension names are removed
before native instance creation; advertised portability enumeration/subset
requirements are added. This is not a claim of full Vulkan loader/WSI conformance.
The small amount of device tracking needed by synthetic swapchains is retained
and cleaned on strict device destruction. Legacy mode remains the default.

`KL_SVC_POLICY=reject` scans file-backed executable ELF LOAD bytes before guest
mapping/relocation and refuses SVC candidates with any immediate. It rejects
bad scanned ranges/headers, invalid policy strings and the pretranslated
Mach-O path. It deliberately can reject data inside executable segments. It
neither NOPs candidates nor translates syscalls. The default legacy behaviour
is unchanged. Unlike strict Vulkan, this experimental setting is read per load;
a future runnable policy must keep it stable. The unconditional constrained-host
launch gate remains intact.

## Actual results

| Check | MoltenVK | KosmicKrisp |
| --- | --- | --- |
| Strict vkCreateInstance | Pass | Pass |
| Strict vkCreateDevice | Pass | Pass |
| Unsupported geometryShader request | FEATURE_NOT_PRESENT; requested true flag unchanged | Same |
| Base FDM / dynamic FDM query | false / false | false / false |
| Missing device function | NULL | NULL |
| Driver identity | ID14, MoltenVK 1.4.2 | ID28, Mesa27 git0913fce718 |
| Physical device identity | Apple M5, vendor0x106b, device0x1a07020a | Apple M5, vendor0x106b, device0x64 |
| Reported API in this 1.2-instance test | 1.2.357 | 1.4.363 |
| Enumerated device extension count | 131 | 145 |

The full driver/version/extension census is in `doc/alyx-strict-vulkan-results/`.
Sandboxed Metal access initially failed (MoltenVK INCOMPATIBLE_DRIVER and no
KosmicKrisp device); the successful GPU tests ran outside that sandbox with
explicit ICDs. No game, headset or Monado service was involved. These are
initialization/capability checks, not timing benchmarks.

The owned fake dispatch library proves forwarding of 71 requested names
(including one synthetic WSI name and a missing name), layers/API version,
base/FDM features and pNext without mutation. It verifies a single failing
CreateDevice call, no null-device substitution, real graphics function pointer
forwarding, proper NULL for missing functions, and instance-layer error
propagation. The fake backend is test-owned, not an installed ICD.

Owned minimal ELF fixtures pass for SVC #0/nonzero immediate, malformed program
headers/segments, truncated headers, invalid policy and pretranslated-image
refusal. No ELF instructions or constructors execute. All nine managed-thread
cases plus 100 fast threads still pass. The constrained CLI/symbol check passes,
both host variants compile/link, and the default MoltenVK initialization probe
passes. The existing host tmpnam deprecation warning remains.

## Reproduction

From `/private/tmp/alyx-klepton`:

```sh
sh tools/check_vulkan_strict.sh
KL_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.1.dylib \
VK_DRIVER_FILES=/opt/homebrew/Cellar/molten-vk/1.4.2/etc/vulkan/icd.d/MoltenVK_icd.json \
sh tools/check_vulkan_strict_real.sh
KL_VULKAN_LIBRARY=/opt/homebrew/lib/libvulkan.1.dylib \
VK_DRIVER_FILES=/private/tmp/alyx-mesa-build/src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json \
sh tools/check_vulkan_strict_real.sh
```

Host fixtures:

```sh
make -C /private/tmp/alyx-verantyx/NativeARM/host \
  KL=/private/tmp/alyx-klepton OUT=/private/tmp/alyx-constrained-host-build \
  HOST_MODE=constrained VR_RUNTIME=xrizer -j6 all check-svc-guard check-guest-threads
```

## Input needed before running Valve code

The original requirement forbids modifying game binaries. Klepton's Linux-to-
Darwin path changes TLS/x18/CPU instructions in private loaded memory (or in a
separately generated image), even when the installed game files stay unchanged.
We have not interpreted that as permission to execute modified guest instructions.
The question now pending is whether **necessary ABI instruction translation in
private memory** is allowed, with installed game files unchanged and the
Alyx-specific Lua/video logic patches still disabled.

If permitted, continue with narrowly scoped syscall translation/refusal and
actual Valve Lua fallback lifecycle validation, using isolated writable state
and no installed runtime changes. Do not merely remove the launch gate: raw-SVC
sites, guest unwind/cleanup ABI, nonzero signals and remaining host page-size
behaviour still need review. A first game/headset run then needs the user.

If no game instruction may change even in memory, this Klepton route does not
currently meet the constraint; a different ABI strategy or supported native
build is needed. Broad SVC NOPs and the old game-specific branch patches are
not justified by either answer.

The previous FDM feasibility conclusions and KosmicKrisp
`VK_EXT_metal_objects` interoperability blocker remain unchanged. No FDM code,
game files, installed runtime or primary checkout was changed in this step.

## Authorization and execution follow-up

The user has now authorized necessary private-memory ABI translation.
[Actual Valve Lua tests and a bounded headless engine run](macos-alyx-first-headset-check.md)
pass their defined checkpoints; the first user headset test is prepared.
The input request above records the previous checkpoint and is resolved.
