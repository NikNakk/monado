<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Native Alyx: process file limits and desktop controllers — 21 October 2026

Both the user's clean KosmicKrisp run and MoltenVK control failed to load the
startup world. This is separate from the custom-resolve rendering defect.
The user then confirmed that correcting the resource limit restored startup;
after restoring desktop controllers and disabling custom resolve, the menu and
usual saved level work. Intro video playback remains unverified by the user.

## Evidence and reproduction

- The login session reports a soft maxfiles limit of 256 and unlimited hard
  limit. The agent's command runner instead inherits a soft limit of 1,048,575.
  Earlier automated startups therefore did not reproduce the terminal failure.
- `kk-custom-resolve-visual-1` records 13,149 UDP socket allocation failures with
  errno 24 (EMFILE), followed by failure to open `maps/startup/world.vrman_c`
  and `NETWORK_DISCONNECT_CREATE_SERVER_FAILED`. The Steam relay configuration
  was eventually downloaded; its transient HTTP 504 does not explain the later
  local allocation failures. The user's MoltenVK control also fails to load
  the world, without logging those UDP allocation failures.
- An owned MoltenVK launch with the parent soft limit explicitly set to 256
  (`mvk-low-fd-repro-1`) reproduces both world/server failure and 5,436 errno-24
  socket failures. Live `lsof` numeric-descriptor counting shows exactly 256
  open descriptors, highest descriptor 255. No filenames are collected.
- With the helper correction, the same 256-limit parent launches MoltenVK and
  clean KosmicKrisp successfully (`mvk-fixed-fd-1`, `kk-fixed-fd-1`): each runs
  for 65 seconds, ending at its deliberate timeout; neither logs errno 24 or
  startup-world/server failure. The corrected MoltenVK process is observed at
  338 descriptors, beyond the old limit.

## Correction

The paired service runner now raises only its own process soft RLIMIT_NOFILE
to at least 8,192, subject to the existing hard limit. The client inherits it
at exec. A higher existing soft limit is preserved. The hard limit, login
session, installed LaunchAgent and system configuration are untouched.
Actual before/after values and any failure to raise the limit are recorded in
the companion `*-monado/identity.json` as `client_open_file_limits`.

Three real-OS tests in disposable subprocesses verify 256 → 8,192, preservation
of an existing 16,384 limit and respect for a 512 hard cap. The existing three
read-only video-settings snapshot tests still pass. Raising the limit is a
launcher compatibility correction; it is not evidence about long-term resource
retention or a performance improvement.

## Desktop input profile

The next user-visible launch rendered but had no controllers. Its service log
contained only Simulated HMD, whereas earlier working runs (`kk-alyx-3/4`)
contained Qwerty Left Controller and Qwerty Right Controller. The automated
launch lacked the launching shell's Qwerty setting.

The paired runner now defaults `QWERTY_ENABLE=1` in desktop mode and zero in
offscreen mode, while preserving an explicit caller override. It records the
effective value in identity.json. `kk-custom-resolve-controllers-1` confirms
both controllers in the service inventory. The user reports hands/menu but no
background with custom resolve enabled: that visual path remains broken despite
the independently proven fixed-depth correction.

The subsequent `kk-clean-driver-fallback-1` retains the clean driver, controllers
and corrected file limit, but uses Alyx's own custom-resolve disable switch.
The user confirms the main menu and usual saved level work; intro video playback
is still uncertain. Do not claim custom resolve or video playback is fixed.

For this known working clean-driver configuration:

```sh
~/Code/alyx-native/run-alyx-kosmickrisp-clean \
  --output ~/Code/alyx-native/runs/kk-clean-gameplay-1 --timeout 600 \
  -- +vr_fidelity_level_auto 0 +vr_fidelity_level 0
```

The original KK fallback and MoltenVK default remain available. The experimental
`run-alyx-kosmickrisp-custom-resolve` deliberately enables the still-broken path;
use the clean fallback launcher above for gameplay. Click the Monado window to
capture input, press R to bring the controllers in front, and hold left
Control/Option to focus the left/right controller. See
[desktop simulation](macos-desktop-simulation.md) for controls.

Next rendering work should reproduce the remaining custom resolve failure with
Alyx's color/depth input reads, exact formats and command-buffer usage. The
current passing owned tests prove the dropped fixed-depth bug only, not the
whole game resolve shader or presentation path.
