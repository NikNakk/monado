<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Preparing the macOS fork for upstream review

This integration branch combines hardware, platform, IPC, compositor and
experimental OpenXR work. Submit independently buildable review units against
upstream, with optional features disabled by default. The current sync base is
`045931d12`; update the base in `scripts/check-contribution-style.py` when the
next upstream sync is reviewed.

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
6. Wine and scheduler diagnostics as optional development tooling. Describe the
   authenticated transport and its same-user trust boundary explicitly.

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
Linux and macOS; Windows runs native tests, including the authenticated TCP
handshake and synchronous pipe shutdown. Android API 26/NDK r26d builds cover
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

Verify native socket peer identities, authenticate the optional Wine TCP bridge,
preserve live socket endpoints, and bound pending Metal/IOSurface XPC resources.
Retire the external-broker runtime override that bypassed native ownership checks.

### Platform and contribution validation — `doc`

Add contribution formatting/license checks, Android native builds for both
upstream ABIs, and macOS version coverage with a remote-layer lifecycle smoke test.
