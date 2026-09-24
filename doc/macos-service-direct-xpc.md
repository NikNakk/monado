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
one-time cross-process handoff. The first different PID to retrieve it becomes
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
an existing legacy token claimable is effectively a no-op there.

### Per-client ownership

Registry tokens are scoped to the application process that owns them.

- The XPC endpoint obtains the publisher/retriever PID from
  `NSXPCConnection.processIdentifier`.
- Ordinary Monado IPC already receives the application's PID in
  `instance_describe_client` and stores it in `ics->client_state.pid`.
- A server-side texture import is accepted only when the token's XPC owner PID
  matches that Unix IPC client PID.
- A client can retrieve a service-created shared event only when its XPC PID
  matches the PID for which the service published the event.
- Discard operations are owner checked as well.

This gives separate OpenXR applications independent Metal-resource namespaces
without adding a new OpenXR or Monado IPC protocol field. It is the first
resource-isolation step needed for a persistent launcher plus temporary VR apps.

PID reuse is not relied upon as token identity: tokens retain random bits and
must also match the stored owner. On ordinary Monado IPC teardown the service
drops pending Metal-XPC entries after the last live IPC client for that PID has
disconnected.

The standalone broker target is retained temporarily for comparison and
fallback testing.

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

This `/tmp` registration is for development and is not intended to survive a
reboot. A later installation step should use a stable installed executable and
persistent per-user LaunchAgent registration.

Remove the development job with:

```sh
build-dir/src/xrt/targets/service/monado-service-xpc-control bootout
```

To return to the old broker during development:

```sh
build-dir/src/xrt/ipc/monado-ipc-metal-xpc-broker bootstrap
```

## Validation

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
- install a reboot-persistent LaunchAgent using a stable installed executable;
- define idle-shutdown / explicit `Quit VR` policy.
