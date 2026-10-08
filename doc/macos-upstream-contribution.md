<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Preparing the macOS fork for upstream review

This integration branch combines hardware, platform, IPC, compositor and
experimental OpenXR work. Submit independently buildable review units against
upstream, with optional features disabled by default. The current sync base is
`ec188bb13`; update the base in `scripts/check-contribution-style.py` when the
next upstream sync is reviewed.

Upstream submissions are paused as of 2026-10-08 while the first merge request
awaits merge and the contributor learns the review process. Local integration,
CI and evidence maintenance continue; the review units below are future work.

## Review units

1. Generic IPC shutdown ordering and regression tests. This can be reviewed
   independently of the macOS port.
2. Platform foundations: build support, IOKit HID, native time/filesystem helpers
   and the basic macOS presenter. Keep PS VR2 policy changes separate from the
   generic platform interfaces.
3. Metal/IOSurface transport, authenticated native peer identity, bounded XPC
   handoffs and launchd lifecycle. Include rejection, cleanup and ownership tests.
4. Hosted client composition and display ownership, including teardown and
   passthrough sharing. Explain the private QuartzCore dependency and preserve
   explicit unsupported behavior when the interfaces are unavailable.
5. Each opt-in OpenXR extension as its own review unit, with its existing feature
   guards and hardware evidence.
6. Scheduler/timing diagnostics as optional development tooling. Wine/OpenVR
   compatibility, authenticated loopback transport and game-specific policy live
   in the separate `NikNakk/macos-wine-xr` project and are not part of the
   Monado upstream series.

An initial upstream issue should describe the platform architecture and the
private API tradeoff before the larger presentation/hosting changes are proposed.
The fork's evidence ledgers support that discussion; avoid submitting the entire
integration history or generated experiment CSVs as a single merge request.

## Checks

The contribution workflow pins clang-format 23.1.1, cmakelang 0.6.13, codespell
2.4.1 and REUSE 6.2.0. Run:

```sh
python scripts/check-contribution-style.py
reuse lint
```

The style check covers files changed relative to the sync base, including
Objective-C and Objective-C++. It preserves generated CSV line endings and the
whitespace inside vendored patch files. Compiler-warning checks and tests run on
Linux and macOS; Windows runs native tests, including synchronous pipe shutdown. The external
Wine bridge owns and tests its authenticated loopback handshake separately.
Android API 26/NDK r26d builds cover
`armeabi-v7a` and `arm64-v8a`, matching upstream's native configurations.

The macOS context/host smoke test checks runtime compatibility on CI macOS
versions and skips when the private interfaces are absent. It does not establish
compatibility with every macOS release or replace headset testing. Follow
upstream's clang-tidy and documentation-build recommendations for each proposed
review unit.

## Human certification

`CONTRIBUTING.md` requires every submitted commit to have valid human
`Signed-off-by:` trailers, including one matching the author metadata. The current
fork history and the automated follow-up commits have not been certified.
An automated assistant cannot agree to the DCO for a human contributor.

Before submission, the human contributors must review provenance and certify the
prepared patches under DCO-1.1. Keep the integrated branch history intact; prepare
and sign the upstream review series separately. Do not add another person's
sign-off without their explicit certification.

## Draft changelog fragments

After each upstream MR exists, copy the appropriate text below into
`doc/changes/<section>/mr.<actual-number>.md`, as required by
`doc/changes/README.md`. These are drafts, not invented MR references.

### IPC shutdown — `ipc`

Stop client loops and cancel blocking Windows pipe reads before joining during
service shutdown; avoid repeated joins and delayed startup reviving stopped loops.

### macOS IPC hardening — `ipc`

Verify native socket peer identities, preserve live socket endpoints, and bound
pending Metal/IOSurface XPC resources. Expose a small PID-scoped external Metal
handoff ABI without embedding translation-layer transport or policy in Monado.
Retire the external-broker runtime override that bypassed native ownership checks.

### Platform and contribution validation — `doc`

Add contribution formatting/license checks, Android native builds for both
upstream ABIs, and macOS version coverage with a remote-layer lifecycle smoke test.

## Native IPC boundary, 2026-10-02

The external `macos-wine-xr` proxy now terminates shared-memory snapshot chunks,
layer upload chunks, all single-layer and copy-commit submissions, and Metal
bootstrap-name imports. Monado accepts ordinary native shared-memory slot
submissions and PID-scoped XPC token imports. Its nine transitional commands,
handlers, upload staging fields and transitional chunk/payload types are removed.
No texture allocation, copy, blit or rendering path was changed. The native
minimum-frame-period pacing hint remains available to ordinary clients.

Before deletion, a sender audit of both repositories found no native Monado
client/test callers. The remaining Wine senders belong to the external project's
transitional Win64 build framework; its proxy and tests consume that frozen ABI.
The proxy no longer forwards any of those commands. The bridge was committed
first (`2e46673`), then validated against unchanged Monado `7fd7f2835` using a
synthetic HMD, as authorized in place of the unavailable headset run.

Native command IDs are regenerated by name from the selected Monado headers;
the external Wine ABI keeps its IDs. The native command count changes from 137
to 128. Representative old -> new IDs:

| Command | Before | After |
| --- | ---: | ---: |
| `instance_describe_client` | 3 | 2 |
| `compositor_layer_sync` | 24 | 23 |
| `compositor_layer_sync_with_semaphore` | 31 | 24 |
| `swapchain_import_metal` | 104 | 96 |
| `compositor_semaphore_import_metal` | 137 | 128 |

Follow-up cleanup restores the original native Unix packet receive on macOS,
Linux and Android, and removes orphaned bootstrap texture/event reconstruction
helpers and declarations. Windows pipe handling is unchanged.
Surviving native commands and shared-memory/layer layouts are unchanged. Rebuild
native clients with the service after this protocol revision. The proxy pins the
Monado header commit at CMake configuration and logs that commit plus the protocol
SHA-256. Reconfigure/rebuild it against each newly built service revision.

Submission instrumentation now lives in the ordinary native layer-sync handlers,
so the external timing analyzer consumes `ipc_submit` and `ipc_swapchain` files
without retaining compatibility handlers. The trace schema and event names remain
stable. Stale Wine transport toggle rows, the Wine-only auxiliary D3D build branch,
and Wine script/build-directory references in native feature notes are removed.

Validation before deletion: proxy hello_xr exited 0 with Metal validation;
explicit 2D and array-size-2 swapchains imported/acquired/waited/released three
images each. Proxy logs show `path=shared-metal-zero-copy pixel-copies=0 gpu-blits=0`
and `shared-event import path=native-token`. Service logs confirm native texture
imports for `array_size=1` and `array_size=2`. The proxy object continues to call
only the existing resolve/publish helpers; it adds no GPU queue or blit API.

After deletion, the proxy hello_xr and 2D/array probes pass again; the public
`run-generic-openxr.zsh` native host selects `shared-metal-zero-copy` against the
synthetic service and completed 643 frames with Metal validation. All 35 macOS
CTest tests, contribution style and REUSE lint pass. The external bridge's six
configured regression tests pass, including native command-ID shifts and actual
socket/shared-memory transactions. Meta XR Simulator's generic bridge hello_xr
also exited 0 with validation (46 frames), using its existing unshareable-image
GPU-blit fallback. That fallback does not run on the Monado paths.

Physical PS VR2 pacing, visual output and controller regression remain untested.
Simulator success does not establish hardware validation. Linux/Windows/Android
build coverage is provided by the existing CI workflows. The follow-up cleanup
revision `50855002b` passes Linux, Windows, Android, macOS and contribution CI.
The external bridge now checks the selected sharing path and rejects any Monado
swapchain using `gpu-blit`; its GPU pattern probe validates every returned image
for both 2D and array swapchains (four images per Monado swapchain, three per Meta
swapchain), with zero pixel mismatches.

## Remaining transitional pacing policy

The native PID aggregates use upstream `pid_t` again. The external proxy freezes
Wine's `int64_t` client-description/app-state layouts and converts
`instance_describe_client` requests and `system_get_client_info` replies. Native
clients and the service continue to share their platform ABI; rebuild both after
the layout revert. Neither aggregate is embedded in shared memory.

Keep the per-session pacing policy in this integration branch until the
transitional proxy retires, but exclude it from the upstream series:
`xrt_session_info.pacing_flags`, `XRT_SESSION_PACING_USE_MIN_FRAME_PERIOD_BIT`,
`u_pacing`'s `set_use_min_frame_period` vtable entry, its multi-compositor hook and
the OpenVR `.pacing_flags = 0` initializer. Only the external proxy sets the bit;
the native OpenXR host uses ordinary `xrWaitFrame` pacing. Upstream already offers
the global `U_PACING_APP_USE_MIN_FRAME_PERIOD` option. Prepare the upstream slice
without these additions rather than changing proxy pacing in this branch.

## Integration cleanup checks, 2026-10-08

Before publishing the October 7 upstream sync, the local check used the CI-pinned
clang-format 23.1.1, cmakelang 0.6.13, codespell 2.4.1 and REUSE 6.2.0. The style
base is now `ec188bb13`, the merged upstream tip, so upstream-only edits are
not misclassified as fork formatting changes. All 300 changed C/C++/Objective-C
files and 29 CMake files pass without source reformatting. Spelling and diff
whitespace checks also pass. REUSE passes after adding the missing license
header to the floor-calibration note. The existing `.build/native-service-check`
macOS build succeeds without compiler warnings. Linux and macOS CI results for
the published revision remain to be checked; hardware runs remain user-owned.

The first pushed cleanup (`f85849f02`) passed contribution CI. Linux compiled
successfully but failed its warning gate on a macOS-only reconnect option
getter and a C compound literal in the C++ floor-calibration test. The follow-up
guards the getter with `XRT_OS_OSX` and uses ordinary C++ aggregate assignment;
it preserves Linux's disabled reconnect behavior and the strict warning gate.
The macOS rebuild is warning-free and the floor-calibration suite passes.
