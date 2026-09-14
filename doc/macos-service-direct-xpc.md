# macOS direct service XPC activation

This branch moves the macOS Metal XPC endpoint from the standalone
`monado-ipc-metal-xpc-broker` process into `monado-service` and adds a first
on-demand launchd activation path.

## Goal

Keep Monado's existing Unix-socket IPC as the primary protocol while using a
small launchd/XPC control and Metal-object side channel on macOS:

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

## First implementation milestone

The direct XPC endpoint deliberately implements the same token-based protocol
as the previous standalone broker. Existing texture and shared-event call sites
therefore do not need to change in the first milestone.

This means `monado-service` can currently make an XPC request back to its own
in-process endpoint when the server side resolves a texture token or publishes
a shared event. That is intentional scaffolding: it proves direct service
ownership and launchd activation without simultaneously changing the Metal
resource protocol. A later cleanup can make server-side accesses use the local
store directly and leave XPC only for cross-process client-to-service transfer.

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

## Suggested validation

Build the service, launchd helper, and OpenXR runtime from this branch. Register
the direct service with the helper, make sure no manually started
`monado-service` remains, then start a known-working OpenXR application without
starting the service by hand.

Expected client-side sequence when cold-starting is:

```text
initial Unix socket connection fails
Monado service socket is unavailable; asking launchd to activate the macOS service
Monado launchd XPC activation ready
Connected to launchd-activated Monado service
```

The launchd service log should include:

```text
Monado service is hosting Metal XPC endpoint 'org.freedesktop.monado.metal-ipc' directly
```

After basic OpenXR startup succeeds, verify a Metal-array application such as
the known-working Godot/Unity path. Existing Metal XPC texture/event logging
should remain present even though the standalone broker process is no longer
running.

## Deliberately not done yet

This milestone does not yet:

- remove the legacy broker target;
- eliminate the service-to-self XPC hop;
- pair XPC connections explicitly with individual Unix IPC clients;
- implement launcher/home-shell policy;
- automatically start a launcher when the last foreground VR app exits;
- install a reboot-persistent LaunchAgent;
- add idle-shutdown policy.

Those are follow-on steps after the direct endpoint and cold-start path have
been validated on macOS.
