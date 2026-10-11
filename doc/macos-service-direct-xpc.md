<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# macOS direct service XPC activation

This work moves the macOS Metal XPC endpoint from the standalone
`monado-ipc-metal-xpc-broker` process into `monado-service` and adds on-demand
launchd activation.

## Architecture

Monado's existing Unix-socket IPC remains the primary protocol. XPC is a small
macOS-specific control and Metal-object side channel:

```text
OpenXR application
        |
        v
Monado client library
        |\
        | \-- XPC: activation + Metal shared handles
        |      org.freedesktop.monado.metal-ipc
        |
        +---- Unix socket: normal Monado IPC
                   |
                   v
             monado-service
```

If `monado-service` is already running, the client connects to the Unix socket
exactly as before and does not use XPC for activation. If the socket connection
fails, the macOS service build asks launchd for the XPC Mach service. A
launchd-managed `monado-service` is then started on demand. The XPC activation
reply is sent only after the service's Unix socket is listening, after which the
client retries the ordinary connection.

## Metal resource transport

The external XPC protocol remains token based. An OpenXR client publishes
`MTLSharedTextureHandle` objects through XPC and passes only the compact token
through ordinary Monado IPC. Service-created `MTLSharedEventHandle` objects use
the reverse direction.

The service no longer connects through XPC to its own Mach endpoint. Server-side
texture consumption and shared-event publication access the in-process registry
directly, while genuine cross-process client/service transfer still uses XPC.

```text
client process                         monado-service
--------------                         --------------
MTLTexture
   |
   +-- MTLSharedTextureHandle --XPC--> in-process registry
   |
   +-- token ----------------Unix IPC-----------------> local registry lookup
                                                   |
                                                   +--> MTLTexture recreation

service MTLSharedEvent
   |
   +--> local registry -- token via Unix IPC --> client
                                   |
                                   +-- XPC --> MTLSharedEventHandle
```

### External client-to-client texture handoff

The Chromium macOS WebXR port has one additional process boundary: Chromium's
isolated XR process owns the OpenXR session, while Chromium's GPU process owns
the SharedImage/ANGLE context that renders into the texture.

Monado keeps this transport out of Chromium by building a small helper library:

```text
libmonado_metal_xpc_client.dylib
```

Its external ABI is intentionally limited to opaque texture objects/tokens:

```text
monado_metal_xpc_publish_claimable_texture(...)
monado_metal_xpc_take_texture_on_device(...)
monado_metal_xpc_release_texture(...)
```

Ordinary texture tokens are still PID scoped and remain in the legacy
32-bit-compatible namespace (24 random bits). Claimable external texture tokens
use a separate 64-bit namespace with 56 random bits because they are not carried
through the legacy `xrt_image_native` metadata transport.

A publisher may explicitly mark an **external texture** token claimable for a
one-time cross-process handoff. The first different PID that retrieves it becomes
the token owner and the claimable flag is cleared. The token is then PID scoped
again and is consumed when the texture is taken.

The receiving helper accepts an `MTLDevice`. This is important for ANGLE:
`EGL_ANGLE_metal_texture_client_buffer` requires the imported `MTLTexture`
to belong to the exact device backing the receiving EGL display. The XPC helper
therefore recreates the texture with the receiving process's supplied device
rather than assuming `MTLSharedTextureHandle.device` is the required object.

The helper installs to `${CMAKE_INSTALL_PREFIX}/lib`. The current Chromium
development port expects:

```text
/usr/local/lib/libmonado_metal_xpc_client.dylib
```

The old standalone broker also implements the claimable-token method for
development compatibility; because that broker predates PID ownership, marking
an existing external token claimable is effectively a no-op there.

### Per-client ownership and pending-resource limits

The direct listener accepts only XPC connections with the service user's UID.
The publisher/retriever PID comes from `NSXPCConnection.processIdentifier`.
The ordinary Unix socket obtains UID and PID from `getpeereid()` and
`LOCAL_PEERPID`. A separate `peer_pid` field carries this verified identity;
`instance_describe_client` remains application metadata and cannot change it.
Metal/IOSurface imports, shared-event publication and disconnect cleanup use
only the verified identity. Wine TCP clients have no native peer PID and cannot
use PID-scoped native XPC imports; their bootstrap-name transport remains
separate.

The registry admits at most 64 pending tokens and 128 pending images per PID,
with global limits of 1024 tokens and 1024 images. Replacing an existing image
uses its existing slot. A claimable external texture token transfers only after
the requested object and the recipient's quota are validated.

Pending publications expire 60 seconds after creation. Publication, retrieval
and ownership checks prune expired entries, and the service main loop prunes
once per second even when no client makes another request. This also bounds
retention by XPC-only publishers that never open ordinary Monado IPC. Normal
last-IPC-connection cleanup still releases entries sooner. XPC connections are
short-lived during legitimate publish/import handoffs, so their invalidation
alone does not discard pending resources.

The standalone broker probe now shares this registry implementation, including
ownership checks, quotas and expiry. The old
`XRT_MACOS_METAL_XPC_EXTERNAL_BROKER=1` runtime override is retired: service
startup rejects it because routing server-side imports through a separate XPC
connection would bypass the verified native client's identity. Historical A/B
commands below record earlier experiments and no longer describe a supported
runtime configuration.

## Development launchd registration

The Apple service build adds:

```text
build-dir/src/xrt/targets/service/monado-service-xpc-control
```

Register the sibling `monado-service` binary as an on-demand per-user launchd
job with:

```sh
build-dir/src/xrt/targets/service/monado-service-xpc-control bootstrap
```

The helper:

- unloads the old standalone broker job if present;
- unloads an older direct-service job if present;
- writes a development plist under `/tmp`;
- registers launchd label `org.freedesktop.monado.service`;
- advertises Mach service `org.freedesktop.monado.metal-ipc`;
- sets `XRT_NO_STDIN=1`;
- snapshots only relevant runtime environment variables from the bootstrap
  shell (`XRT_`, `PSVR2_`, `IPC_`, `VK_`, `MVK_`, `MOLTENVK_`, `METAL_`,
  `MTL_`, `PATH`, `DYLD_LIBRARY_PATH`, and `DYLD_FRAMEWORK_PATH`);
- directs launchd stdout/stderr to `/tmp/monado-service-launchd.<uid>.*.log`.

If those environment variables change, run `bootstrap` again so the generated
plist is refreshed.

The job uses `ProcessType=Interactive`. An `Adaptive` job plus an XPC
importance lease held by the client was tried and removed on 2026-09-30: Game
Mode backgrounds the service from outside, and no importance boost overrides
that. See [macos-remote-layer-hosting.md](macos-remote-layer-hosting.md).

This `/tmp` registration is for development and is not intended to survive a
reboot. Use the persistent installation mode below for normal use.

Remove the development job with:

```sh
build-dir/src/xrt/targets/service/monado-service-xpc-control bootout
```

To return to the old broker during development:

```sh
build-dir/src/xrt/ipc/monado-ipc-metal-xpc-broker bootstrap
```

## Persistent per-user installation

For normal use, install Monado to a stable prefix so `monado-service` and
`monado-service-xpc-control` remain side by side, then register the service:

```sh
cmake --install build-dir
installed-prefix/bin/monado-service-xpc-control install
```

`install` writes `~/Library/LaunchAgents/org.freedesktop.monado.service.plist`,
registers it in the current per-user launchd domain, and keeps the same on-demand
Mach service `org.freedesktop.monado.metal-ipc`. Logs go to
`~/Library/Logs/Monado/monado-service.{out,err}.log`.

Unlike development `bootstrap`, persistent installation does **not** snapshot
XRT/PSVR2/Vulkan tuning variables from the invoking shell. The proven runtime
behaviour is supplied by source defaults; the LaunchAgent carries only service
lifecycle settings such as no-stdin, idle exit, and display-loss shutdown. The
LaunchAgent is available again after the next login following logout or reboot.

The plist stores the absolute path of its sibling `monado-service`. If the
installed prefix is moved or replaced at a different path, run `install` again.
Remove the persistent registration with:

```sh
installed-prefix/bin/monado-service-xpc-control uninstall
```

Development `bootstrap` remains useful for A/B testing because it captures the
current shell's relevant runtime environment into a temporary `/tmp` plist.

### Current local installation (2026-10-09)

The native `build/arm64` build at `bc9591f03`, plus the macOS installed-manifest
suffix correction, is installed in `~/.local/monado-psvr2`. The system loader's
`/usr/local/share/openxr/1/active_runtime.json` now points to that prefix's
`share/openxr/1/openxr_monado.json`, replacing Meta XR Simulator. The manifest
uses relative paths to `.dylib` files; the install generator previously used
CMake's macOS MODULE suffix (`.so`) despite these being SHARED libraries.
The correction is Apple-only and preserves the other platforms' filenames.

The existing service LaunchAgent was retained and its executable path changed
to the installed `bin/monado-service`; the generic helper's `install` command
was deliberately not used because it would discard this user's tuning.
All saved environment settings remain, including Sense 6DoF, the absolute
combined-head calibration path, camera streams, timing traces, floor height,
idle exit and display-loss shutdown. Quick lock, headset detector blobs and
Sense reconnect are explicitly set to `1` in this local profile. This does not
enable experimental tracking by default for other installations or Linux.

The companion client-environment LaunchAgent now has a single command selecting
the installed ARM64 manifest and `XRT_MACOS_CLIENT_COMPOSITOR=1` at login.
Its obsolete extra `build-wine` command argument was removed. Already-running
applications and terminals retain their inherited environment; restart them or
update their overrides. Wine launchers must still select the x86-64 manifest.

The previous prefix, both LaunchAgents and original system-link target are
backed up in `build/arm64/local-install-backups/20261009-225526/`.
The full native build and all 49 CTest suites pass (four OS-dependent suites
required an unsandboxed run). With `XR_RUNTIME_JSON` removed, the local SDK
loader selects the system link, loads Monado and enumerates 62 extensions,
including `XR_KHR_metal_enable`. The registered service remains idle/on demand;
this installation check did not initiate a headset run.

The full `cmake --install --prefix` run installed the native runtime and service
but then encountered the unrelated SteamVR driver's hard-coded
`/usr/local/share/steamvr-monado` destination. The corrected OpenXR install
script was rerun independently and succeeded; no SteamVR driver was installed.

#### Wine client mismatch corrected (2026-10-09)

The first Hyperbolica launch after installation failed: the Wine native client
still identified itself as `v25.1.0-2175-gd5eba0543`, while the installed service
was `v25.1.0-2177-gbc9591f03`. The bridge's
`run-mwxr-20261009-230717/steam.log` records `ipc_client_check_git_tag` rejecting
both connection attempts and `XR_ERROR_RUNTIME_UNAVAILABLE` from
`xrCreateInstance`. The service then exited normally with status 0 after its
configured five-second idle timeout; it did not crash.

Both architecture builds were refreshed after the installation-record commit
`805316beb`, and the native runtime, libmonado and service were reinstalled into
the same prefix. Binary inspection confirms the installed service/runtime and
`build/x86_64` client all report `v25.1.0-2178-g805316beb`; system-loader
extension enumeration still passes. Settings and launch registrations are
unchanged. A fresh Hyperbolica launch is required to validate the corrected
pair; no hardware run was initiated during the correction.

Future updates must rebuild both architectures and reinstall the native
service/runtime together. `scripts/macos/rebuild-both` rebuilds the development
trees but does not update this installed prefix. Even documentation commits
change Monado's version tag and can trigger the strict IPC version check.

## Validation

### Chromium system-runtime discovery (2026-10-10)

The local Chromium fork now resolves its selected runtime before entering the
XR sandbox. It honours an explicit `XR_RUNTIME_JSON`, otherwise searches the
architecture-specific and generic registrations in `/usr/local/share/openxr/1`
in loader order. It resolves manifest and library symlinks and relative library
paths, then grants read access to the canonical manifest and read/executable-map
access to the runtime library's directory. No fixed Monado installation prefix
is required. Dependencies outside that directory still need separate scoped
rules; the existing Homebrew dependency allowances remain.

Eight regression tests compile and exercise the actual resolver, and Chromium's
browser integration and policy compile. A Seatbelt probe using the resolver's
system-selected paths loads this installed Monado with `XR_RUNTIME_JSON` unset
and enumerates 62 extensions. Reading the unrelated installed Monado header
remains denied. The development Chromium framework was relinked and installed
in `~/Code/chromium/src/out/mac-webxr/Chromium.app`; restart the browser to use
it. An actual WebXR/headset run remains user validation. This corrects browser
discovery, not the separate end-user installer/dependency packaging work.

Build the service, launchd helper, and OpenXR runtime, bootstrap the direct
service, and make sure no manually started `monado-service` or legacy broker is
running.

A cold-started application should show this client-side sequence:

```text
initial Unix socket connection fails
Monado service socket is unavailable; asking launchd to activate the macOS service
Monado launchd XPC activation ready
Connected to launchd-activated Monado service
```

The service log should include a line similar to:

```text
Monado service is hosting PID-scoped Metal XPC endpoint 'org.freedesktop.monado.metal-ipc' directly
```

For a Metal array swapchain, the service should log direct in-process texture
consumption including the application PID. Shared-event publication should also
include that PID. No `monado-ipc-metal-xpc-broker` process should be required.

After single-application validation, run two supported OpenXR applications in
sequence while keeping `monado-service` alive. A token produced by one process
must never be accepted for the other process.

## Follow-on work

The next steps are:

- keep a persistent launcher/home application while foreground applications
  come and go;
- exercise Monado's existing multi-client active/focused application switching;
- remove the legacy standalone broker once the direct path has enough soak time;
- refine the user-facing `Quit VR` policy on top of the implemented idle and
  display-loss shutdown lifecycle.


## Clean Unix channel build validation (2026-10-01)

The clean branch had retained dormant Unix-channel framing code referring to
removed `ipc_message_channel.frame_reads` / `frame_writes` fields. These
macOS-guarded helpers and branches have been removed. Native Unix IPC uses
its existing plain message/descriptor path; external transport framing belongs
to the external compatibility project. No command IDs, wire schemas, XPC
publication interfaces or Linux paths changed.

From `f1a6b7f98`, with hardware drivers disabled, the clean service,
`ipc_shared` and `monado_metal_xpc_client` build. Shared-memory,
socket-security and thread-shutdown tests pass. The external compatibility
check confirms 136 transitional commands and 18 old schemas unchanged, with
only the generic external semaphore import appended. The external generic
OpenXR host also ran `hello_xr` against this branch's simulated HMD through
standard Metal OpenXR swapchains; hardware pacing comparison remains pending.

The full native client build also exposed a remaining call to the deleted
byte-stream shared-memory snapshot refresh helper, plus its orphaned header
comment. Both are removed: native clients retain their live shared-memory
mapping. The service-based `openxr_monado` target now builds as well. The
contribution checker also required formatting two previously modified IPC
files; that follow-up has no behavior or wire-format changes.
