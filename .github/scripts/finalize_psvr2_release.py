#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: str, old: str, new: str, description: str) -> None:
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{description}: expected exactly one match, found {count}")
    p.write_text(text.replace(old, new, 1))


# Proven cadence default: newest-frame drawable-slot mode owns stale replacement.
replace_once(
    "src/xrt/compositor/main/comp_window_macos_latest.m",
    'DEBUG_GET_ONCE_BOOL_OPTION(macos_present_stale_substitute, "XRT_MACOS_PRESENT_STALE_SUBSTITUTE", true)',
    'DEBUG_GET_ONCE_BOOL_OPTION(macos_present_stale_substitute, "XRT_MACOS_PRESENT_STALE_SUBSTITUTE", false)',
    "stale-substitute default",
)

# Keep the real late-render wait in normal builds, while compiling CSV-only tracing out.
renderer = Path("src/xrt/compositor/main/comp_renderer.c")
text = renderer.read_text()
feature = "XRT_FEATURE_MACOS_TIMING_DIAGNOSTICS"
timing_option = 'DEBUG_GET_ONCE_BOOL_OPTION(comp_psvr2_timing_trace, "PSVR2_TIMING_TRACE", false)'
if text.count(timing_option) != 1:
    raise SystemExit(f"renderer timing option: expected one match, found {text.count(timing_option)}")
text = text.replace(timing_option, f"#ifdef {feature}\n{timing_option}\n#endif", 1)
open_anchor = "static void\nrenderer_late_render_trace_open(struct comp_renderer *r)"
wait_anchor = "static void\nrenderer_late_render_wait(struct comp_renderer *r)"
frame_anchor = "static void\nrenderer_late_render_trace_frame(struct comp_renderer *r)"
renderer_end_anchor = "\n#endif\n\nstatic void\nrenderer_wait_queue_idle(struct comp_renderer *r)"
for anchor in (open_anchor, wait_anchor, frame_anchor, renderer_end_anchor):
    if text.count(anchor) != 1:
        raise SystemExit(f"renderer anchor {anchor!r}: expected one match, found {text.count(anchor)}")
text = text.replace(open_anchor, f"#ifdef {feature}\n{open_anchor}", 1)
text = text.replace(wait_anchor, f"#endif\n\n{wait_anchor}", 1)
text = text.replace(frame_anchor, f"#ifdef {feature}\n{frame_anchor}", 1)
text = text.replace(renderer_end_anchor, f"\n#endif\n{renderer_end_anchor}", 1)
renderer.write_text(text)

# Correct persistent-helper usage text.
replace_once(
    "src/xrt/targets/service/macos_xpc_control.m",
    "bootstrap/unbootout are development registration controls using /tmp",
    "bootstrap/bootout are development registration controls using /tmp",
    "XPC helper usage typo",
)

# The OpenXR runtime is a SHARED library on Apple. Install manifests must therefore
# use CMAKE_SHARED_LIBRARY_SUFFIX (.dylib), not the MODULE suffix (.so).
replace_once(
    "cmake/GenerateKhrManifest.cmake",
    '''    set(TARGET_FILENAME
        "${CMAKE_SHARED_MODULE_PREFIX}${_genmanifest_TARGET}${CMAKE_SHARED_MODULE_SUFFIX}"
    )''',
    '''    get_target_property(_genmanifest_TARGET_TYPE "${_genmanifest_TARGET}" TYPE)
    if(_genmanifest_TARGET_TYPE STREQUAL "SHARED_LIBRARY")
        set(_genmanifest_TARGET_PREFIX "${CMAKE_SHARED_LIBRARY_PREFIX}")
        set(_genmanifest_TARGET_SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}")
    else()
        set(_genmanifest_TARGET_PREFIX "${CMAKE_SHARED_MODULE_PREFIX}")
        set(_genmanifest_TARGET_SUFFIX "${CMAKE_SHARED_MODULE_SUFFIX}")
    endif()
    set(TARGET_FILENAME
        "${_genmanifest_TARGET_PREFIX}${_genmanifest_TARGET}${_genmanifest_TARGET_SUFFIX}"
    )''',
    "install manifest target suffix selection",
)

# These values are intentionally used by assertions in debug builds; mark them as
# consumed as well so NDEBUG release builds don't produce migration-related warnings.
replace_once(
    "src/xrt/auxiliary/tracking/t_dead_reckoning.c",
    '''\t\tif (using_accel) {
\t\t\tassert(got && gyro_ts == accel_ts && "Failure getting synced gyro and accel samples");
\t\t}
\t\tassert(ts >= base_rel_ts && "Accessing imu sample that is older than latest SLAM pose");''',
    '''\t\tif (using_accel) {
\t\t\tassert(got && gyro_ts == accel_ts && "Failure getting synced gyro and accel samples");
\t\t}
\t\t(void)got;
\t\tassert(ts >= base_rel_ts && "Accessing imu sample that is older than latest SLAM pose");''',
    "dead-reckoning release warning",
)
replace_once(
    "src/xrt/drivers/psvr2/psvr2.c",
    '''\t\tret = libusb_cancel_transfer(xfer);                                                                    \\
\t\tassert(ret == 0 || ret == LIBUSB_ERROR_NOT_FOUND);                                                     \\
\t}''',
    '''\t\tret = libusb_cancel_transfer(xfer);                                                                    \\
\t\tassert(ret == 0 || ret == LIBUSB_ERROR_NOT_FOUND);                                                     \\
\t\t(void)ret;                                                                                              \\
\t}''',
    "PSVR2 transfer-cancel release warning",
)

# Bring the architecture document up to the implementation that is already present.
doc = Path("doc/macos-service-direct-xpc.md")
text = doc.read_text()
old = '''PID reuse is not relied upon as token identity: tokens retain random bits and
must also match the stored owner. Cleanup of any token stranded by a client
crash is still a follow-on item.'''
new = '''PID reuse is not relied upon as token identity: tokens retain random bits and
must also match the stored owner. When the last ordinary IPC connection for an
application PID closes, the service discards any texture or shared-event tokens
still owned by that PID, covering normal exit and client crashes.'''
if text.count(old) != 1:
    raise SystemExit("ownership-cleanup documentation anchor not found exactly once")
text = text.replace(old, new, 1)

old = '''This `/tmp` registration is for development and is not intended to survive a
reboot. A later installation step should use a stable installed executable and
persistent per-user LaunchAgent registration.'''
new = '''This `/tmp` registration is for development and is not intended to survive a
reboot. Use the persistent installation mode below for normal use.'''
if text.count(old) != 1:
    raise SystemExit("development-registration documentation anchor not found exactly once")
text = text.replace(old, new, 1)

validation_anchor = "## Validation\n"
persistent_section = '''## Persistent per-user installation

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

'''
if text.count(validation_anchor) != 1:
    raise SystemExit("validation documentation anchor not found exactly once")
text = text.replace(validation_anchor, persistent_section + validation_anchor, 1)

old = '''The next steps are:

- clean stranded registry entries when a Unix IPC client dies unexpectedly;
- keep a persistent launcher/home application while foreground applications
  come and go;
- exercise Monado's existing multi-client active/focused application switching;
- remove the legacy standalone broker once the direct path has enough soak time;
- install a reboot-persistent LaunchAgent using a stable installed executable;
- define idle-shutdown / explicit `Quit VR` policy.'''
new = '''The next steps are:

- keep a persistent launcher/home application while foreground applications
  come and go;
- exercise Monado's existing multi-client active/focused application switching;
- remove the legacy standalone broker once the direct path has enough soak time;
- refine the user-facing `Quit VR` policy on top of the implemented idle and
  display-loss shutdown lifecycle.'''
if text.count(old) != 1:
    raise SystemExit("follow-on documentation block not found exactly once")
text = text.replace(old, new, 1)
doc.write_text(text)
