<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Chromium runtime sandbox narrowing, 2026-10-12

**The tested Monado and Meta routes now work with Chromium's sandbox enabled.**
Runtime/dependency permissions are exact files, and runtime-specific permissions
no longer apply to every XR service. Physical headset presentation remains a
user-run gate; no hardware or installed Monado/Steam service was restarted here.

## Rules and implementation

Chromium `013d9df8f83` contains the initial narrowing and Claude's accompanying
correctness fixes: failure diagnostics, a 1024-image dependency limit, and
Monado's XDG socket-directory precedence with canonicalisation before socket
creation. Those fixes were retained. Chromium `9b5f9e3e50c` adds an explicit executable
mapping denial, negative policy/parser tests, and matches Monado's default Mach
service when the environment override is empty.

| Resource | XR-specific permission |
| --- | --- |
| Selected manifest and declared non-system libraries | Canonical file reads; executable mapping for individual libraries |
| Unrelated files beside the runtime | No recursive runtime-directory grant |
| Runtime dependency graph | Bounded Mach-O header/load-command reads; code is neither mapped nor executed during browser discovery |
| Monado dynamic loads | Audited SDL3 and Metal helper files, plus their declared dependencies |
| Monado IPC | Only the selected Unix socket and exact Metal Mach-service name; no recursive cache or local TCP grant |
| Meta simulator | Extra resources/data and loopback permissions only when the installed simulator library is selected |
| Simulator resources/data | Required reads/writes, with no automatic executable mapping |

The composed GPU/common policy retains its existing privileges. The explicit
mapping denial preserves its trusted system/Chromium bundle locations and
the exact runtime/dependency list. On this macOS version, the disposable policy
test showed that deny-default alone did not restrict executable mapping;
adding the explicit rule made unrelated mapping queries fail while declared
library queries succeeded. Runtime resource/dynamic-load requirements outside
the declared graph require an audited exception; failure never restores a broad
directory grant. Unresolved non-weak dependencies and malformed images fail
closed with an explanatory browser log.

## Validation

All incremental Chromium builds used `autoninja -C out/mac-webxr chrome`.
The final policy build passed. The subsequent empty-override correction only
affects selecting Monado's default Mach name; the tested private endpoint is
unchanged, and the final binary was rebuilt.

- **19/19 resolver tests pass**, including real Mach-O fixtures, relative/
  absolute and symlink paths, malformed load commands, invalid fat offsets,
  ARM64/x86-64 slice selection and the installed Monado dependency graph.
- **3/3 disposable Seatbelt tests pass**, exercising generic, Monado and Meta
  selection. Actual sibling reads, simulator writes and loopback sockets are
  checked. Policy queries verify executable-map and Mach-service permissions,
  including denial of executable mapping for readable simulator data. Raw ARM64
  mmap was an unsuitable initial control because code-signature registration
  also affects it; that experiment was replaced, not used as passing evidence.
- **Sandboxed ANGLE pixels pass:** all four differing stereo colour/reuse
  windows; three real IOSurface images recycled 839 times; 1676 consumer claims
  and releases balanced; one quarantine/reclaim, none reclaimed with claims.
- **Sandboxed Dawn pixels pass:** all four windows through the actual four-image
  copy transport; 842 array-image acquisitions; 1682 claims/releases balanced;
  two quarantines/reclaims. The separate initial image has one acquisition.
- **Sandboxed ANGLE recovery passes:** all nine pause/buffering/seek/session/
  GPU-decoder replacement phases; independent native client completes 1200
  frames with 1065 fresh latches, including 127 during prolonged buffering and
  87 during crash-to-replacement. All 2658 claims/releases balance; all four
  quarantines reclaim, with no live claims at reclamation.
- **Sandboxed Meta simulator smoke passes:** 1440 XR callbacks in its 20 s
  measured video window after warmup, no recorded JS errors, and 43 matching
  source/runtime diagnostic hash pairs through its non-IOSurface copy fallback.
  These diagnostic hashes are not the Monado exact-eye GPU pixel test.

The ANGLE/Dawn pixel and recovery captures pass the existing strict joined
Perfetto loss, ownership and XR-utility coverage checks. The separate Meta
browser trace has no reported loss/error statistics and records 3319
`GetFrameData` events; it is not run through the Monado composition checker.
Root arguments contain no `--no-sandbox`. The completed captures report
`sandbox: true`; the earlier initial ANGLE run had an obsolete disabled-text
label despite its sandboxed arguments, so the final labelled captures below
are the evidence used here.

Raw evidence remains outside Git:

```text
/tmp/xr-narrow-resolver-final-tests.log
/tmp/xr-policy-tests.log
/tmp/xr-narrow-map-deny-build.log
/tmp/xr-narrow-final-build.log
/tmp/xr-sandbox-narrowing-manifest-20261012.json
/tmp/xr-sandbox-map-angle-20261012
/tmp/xr-sandbox-map-dawn-20261012
/tmp/xr-sandbox-map-recovery-20261012
/tmp/xr-sandbox-map-meta-20261012
```

## Reproduction

Chromium's resolver and policy scripts are under
`~/Code/chromium/src/device/vr/openxr/mac/`. The policy test creates only private
fixture data, disposable sandbox processes and its own loopback listener.
Installed Monado/Meta cases skip explicitly if their binaries are absent.

From the Monado checkout, choose a new output directory per browser run:

```sh
python3 scripts/macos/run-gpu-sync-service.py --perfetto --scale-percentage 20 \
  --client-timeout 150 --output /tmp/new-sandbox-angle \
  --client /usr/bin/env M3_CHROMIUM_SANDBOX=1 M3_CHROMIUM_MODE=pixels \
  M3_CHROMIUM_PRODUCER=angle \
  M3_CHROMIUM_PATH=$HOME/Code/chromium/src/out/mac-webxr/Chromium.app/Contents/MacOS/Chromium \
  M3_CHROMIUM_OUTPUT=/tmp/new-sandbox-angle \
  node scripts/macos/run-chromium-gpu-sync.mjs
python3 scripts/macos/check-chromium-gpu-sync.py /tmp/new-sandbox-angle \
  --processor $HOME/Code/perfetto-sdk-v58.2/tools/mac-arm64/trace_processor_shell
```

Use `M3_CHROMIUM_PRODUCER=dawn` for the second pixel route. Recovery uses
`M3_CHROMIUM_MODE=faults` and the existing `M3_HEALTHY_NATIVE_PATH` fixture.
Monado `a31038c14` adds the sandbox opt-in and accurate summary flag; the
follow-up lets a simulator run label its target using `M3_CHROMIUM_TARGET`.
Existing unsandboxed M3–M5 recordings remain historical evidence.

This closes the isolated-endpoint sandbox coverage gap for the tested builds.
It does not establish sandbox compatibility for every runtime, fresh-install
Monado directory creation, nonstandard dynamic dependencies, physical headset
output, Linux CI or XRMediaBinding sparse layers.
