<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# ARM64 Alyx constrained-host follow-up — 20 October 2026

A Lua callback replacement and a **preflight-only constrained build** are now
implemented in the isolated VeRantyx checkout. This is not a successful game
launch or a completed runnable constrained mode. The native thread fixture
also found a reason not to declare the existing video patch redundant.

## Source and changes

- VeRantyx `codex/native-xrizer-vulkan`: `63bec84`, after xrizer adapter `4c354a6`,
  based on native `38c02b9492c9148f6a4ca86dc607ce52aa6f7403`.
- Klepton unchanged at `debda5f`, after prerequisite `67ef51f`, upstream `ce74582`.
- xrizer unchanged at `4c5dc14`; no Monado runtime code changed in this follow-up.
- Reconstructable signed patch:
  `experiments/alyx-vulkan/patches/verantyx-constrained-host.patch`.

`kl_lua_builtin.c` interposes `lua_VALVE_OVERRIDE_MMAP` and calls the original
image definition with five null callback pointers. Static evidence for the
setter and its fallback is in the [read-only audit](macos-alyx-arm64-static-audit.md).
The component has no instruction scan, executable-page write, protection change
or max-address override. It resolves the setter afresh and aborts on a missing
image, missing definition or recursive binding.

`HOST_MODE=constrained` excludes the legacy Lua and video text-patch source
objects. A `.host-mode` output marker prevents subsequent mode mixing. The
**unconditional guest-execution gate** precedes guest loading and runtime setup:
only `--constrained-check` succeeds; probes and gameplay return status 2.
Klepton's generic patches and Vulkan substitutions remain linked but unreachable
through this build. They have not been made safe merely by adding the gate.
`legacy` remains the build default and retains its existing game workarounds.

The existing kernel thread query is factored into `kl_thread_probe.c` for native
testing; it now requires a complete `proc_threadinfo` result rather than accepting
any positive byte count. It still describes kernel-thread existence, not guest
thread lifetime. No guest pthread registry is implemented yet.

## Actual tests

Both native ARM64 xrizer host variants compile and link, using separate outputs:
`/private/tmp/alyx-constrained-host-build` and `/private/tmp/alyx-native-host-build`.
The full build has the existing `tmpnam` deprecation warning in `kl_glibc.c`.

| Check | Result and scope |
| --- | --- |
| Native Lua setter fixture | Pass: five null callbacks, repeated invocation; missing image, symbol and recursive binding each abort. Owned fake image/setter only. |
| Kernel-thread fixture | Pass: self and live worker found; terminated Mach thread and invalid IDs give ESRCH. |
| Joined pthread must report exited | **Failed initial hypothesis:** query still returns 0 after join on this Mac. macOS can retain the kernel thread. Final fixture records this limitation explicitly. |
| Constrained CLI and symbols | Pass: preflight success; probe/game rejection; `nm` excludes both legacy patch-install symbols. |
| Output mode mismatch | Pass: rebuilding constrained output as legacy rejects with status 2. |
| Read-only depot scan | Pass: all 15 inspected ELF hashes unchanged during scan; complete inventory equals the preceding audit. |

Evidence is in `doc/alyx-constrained-host-results/`. The Lua fixture does **not**
validate Valve Lua's mmap/mremap allocation, growth/shrink, address-range failure,
page alignment or teardown. Nor does it prove actual ELF PLT binding with the
game: source inspection establishes the path, while fixture binding is mocked.
No game binaries, constructors or translated game images were executed.
No primary checkout, installed runtime, Steam data, saves or game files changed.

## Reproduction

From `/private/tmp/alyx-verantyx`:

```sh
make -C NativeARM/host KL=/private/tmp/alyx-klepton \
  OUT=/private/tmp/alyx-constrained-host-build HOST_MODE=constrained \
  VR_RUNTIME=xrizer -j6
make -C NativeARM/host KL=/private/tmp/alyx-klepton \
  OUT=/private/tmp/alyx-constrained-host-build HOST_MODE=constrained \
  VR_RUNTIME=xrizer check-lua-builtin check-thread-probe
python3 NativeARM/host/tests/check_constrained.py \
  /private/tmp/alyx-constrained-host-build/alyx_host
```

Use a fresh directory when first changing mode: old build directories predate
the marker. These commands exercise native fixtures/preflight, not gameplay.
The existing dual-ICD commands and previous driver results remain in the
[dual-Vulkan report](macos-alyx-kosmickrisp.md). They have not been rebenchmarked.

## Next implementation milestone

1. Track **guest** pthread creation, return and explicit exit, rather than
   inferring guest liveness solely from a Darwin kernel thread. Review tgkill's
   target process and nonzero-signal errno handling. Re-test the video profiler
   path before concluding its text patch can be removed.
2. Validate actual Lua fallback lifecycle, including the current ENOSYS mremap
   path, and page-size handling outside Lua. The native setter fixture alone
   is insufficient to remove the launch gate.
3. Replace broad raw-SVC NOP handling with explicit supported syscall
   translation/refusal and establish the allowed TLS/x18 ABI translation boundary.
4. Add and test a Vulkan mode that preserves requested features/extensions and
   reports unsupported functions rather than clamping, dropping or returning
   synthetic success. Preserve the intentional synthetic desktop WSI separately.
5. Only then remove the preflight gate in a reviewed change, assemble an isolated
   content root, and collect the first actual Alyx API/foveation trace.

The earlier FDM assessment is unchanged: CPU-authored maps and per-eye offsets
are promising, but Metal sampling semantics/reconstruction still require proof.
KosmicKrisp remains blocked at Monado interoperability by missing
`VK_EXT_metal_objects`; this follow-up does not resolve it. No FDM extension or
accelerated prototype was implemented or advertised.
