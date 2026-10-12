<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Outstanding GPU synchronization headset tests

Hardware runs are user-operated. Record the commit, launch settings, observed
behavior and output directory. Wear the headset for the visual checks: recorded
presentation callbacks alone do not establish a stable view. Close other USB
owners and stop an older Monado service before a private native fixture claims
the headset. Use a new output directory per test.

## 1. Native three versus four swapchain images

From `~/Code/monado-2`, after the normal native incremental build:

```sh
python3 scripts/macos/run-three-image-comparison.py \
  --headset --frames 2400 --output /tmp/monado-three-image-headset-new
```

This runs GPU checks followed by four–three–three–four timing runs. Look for a
stable panel, first still and then with gentle head motion; record black frames,
checkerboards, judder and stalls in each mode. The fixture rejects an absent
headset rather than accepting offscreen results. See the
[full procedure and offscreen baseline](macos-three-image-comparison.md).

## 2. SteamVR fixed-allocation copy versus three-image zero-copy

Fully stop SteamVR between trials, and ensure the installed service has loaded
the compatible updated binary. Use the usual hardware launcher:

```sh
MWXR_STEAMVR_ZERO_COPY=0 MWXR_STEAMVR_THREE_IMAGES=0 ~/Windows/run-mwxr.zsh

# After stopping VR and the previous session:
MWXR_STEAMVR_ZERO_COPY=1 MWXR_STEAMVR_THREE_IMAGES=1 ~/Windows/run-mwxr.zsh
```

Use the same Home/dashboard scene and comparable still/moving-head windows.
For the second run, verify `zero-copy (first image ...)` in the driver's log
and `Three-image compatibility` in Monado's service log. Flags alone do not
prove the chosen transport. Record Home/dashboard behavior, black frames,
checkerboards, stalls and any crashes separately. The zeroCopy setting persists;
the three-image flag is launch-specific. Follow the
[detailed SteamVR procedure](macos-psvr2-timing-diagnostics.md#three-image-steamvr-compatibility-prepared-2026-10-10).
The [backend GPU test](macos-steamvr-resolve-copy-test.md) now proves that the
tested format can remove two copies per stereo frame; this hardware trial must
validate the actual SteamVR deployment and workload.

## Other remaining hardware coverage

The previous [native synchronization timing comparison](macos-gpu-sync-headset-comparison.md)
has no headset-worn visual judgement. The tests above provide a new current-build
visual record, but do not retrospectively validate its older captures.
Chromium ANGLE/Dawn video, pause/buffering/recovery on the physical display,
hosted Wine/Game Mode full-rate presentation, 90 Hz and Wine client failure
recovery remain separate coverage gaps in the
[M5 limits](macos-gpu-sync-cross-stack.md#exit-criteria-and-practical-limits).
There is not yet a validated single hardware runner for those cases. The
headless M3–M5 scripts/results must not be relabelled as headset validation.
