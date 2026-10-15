<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Initial current-base WSI headset checks — 15 October 2026

> **Superseded by the [fixes and rerun results](macos-wsi-headset-fixes.md).**
> The failed captures below are preserved as the original evidence.

**Result: fixed-rate WSI presentation and hosted visibility work in these short
checks, but refresh switching is not ready. Visual acceptance remains pending.**
The user explicitly authorized automated headset checks, superseding the earlier
handover's user-operated-only boundary for this run.

Source: Monado-2 `experiment/macos-wsi-current-native`, base
`f076c87ed8715e00a66c58e6bec7ad43b5ad19b1`, plus the uncommitted review fixes.
All runs use `build/review/wsi-current-native-fresh`, the same service, runtime
and client binaries that passed the review validation. Each manifest records
binary hashes, exact environment, tracked diff and all new source bytes.
No production runtime was installed or replaced.

## Short fixed-rate and hosted checks

The headset was initially 4000×2040 at 90 Hz. Tests used a light 24-instance
native Metal application, one GPU pass, six seconds after session start,
three output images, and Metal API validation. Native uses the current
CADisplayLink default and 8 ms minimum presentation duration. The runner now
has `--native-display-link ca|cv` (default `ca`) so historical CV conditions
remain explicit. WSI conditions exercise GOOGLE pacing, CV phase, and CV phase
with calibration separately. These are functional pilots, not benchmarks.

| Condition | 90 Hz | 120 Hz |
| --- | --- | --- |
| Native | Application exited 0; GPU-obligation timeout near shutdown | Exited 0; no runtime errors logged |
| Generic WSI / GOOGLE | Exited 0; feedback and application-source frames recorded | Same |
| Generic WSI / CV | Exited 0; feedback and application-source frames recorded | Same |
| Generic WSI / calibrated CV | Exited 0; feedback and application-source frames recorded | Same |
| Hosted calibrated WSI | Exited 0; hidden→pending→visible→hidden | Same |

All six service-owned WSI runs and both hosted runs have positive presentation
feedback and application-source records. Native at 90 Hz reported
`Semaphore waiting failed: value=2136 result=2` and
`Client GPU obligation failed: frame=535 value=2136 result=2` before disconnect.
A zero application exit does not clear that failure. Its shutdown association
is an observation, not an established cause.

The two hosted runs use separate service lifetimes. They validate visibility
acknowledgement and teardown, not multiple sessions or clients on one service,
and do not establish Game Mode behavior.

## Refresh requests: failures remain

Both initial pilots verified a 90 Hz request before `xrBeginSession` in 0.000 s,
and rejected unsupported 73 Hz without changing the current mode.

- Native completed the first 90→120 switch, then stalled before completing the
  return to 90. The app could not exit within the runner's 15-second deadline.
  A separate reproduction stalled before the first switch completed, while
  the compositor continued presenting at 90 Hz. Neither is a passing cycle.
- Generic GOOGLE WSI completed 90→120, then logged
  `WSI physical refresh 120.000 -> 60.000 Hz` on the return request. The app
  rejected the observed mode and the target subsequently reported the selected
  display inactive. This does not establish that the physical headset actually
  entered a 60 Hz mode: the transient CoreGraphics/display-identity handling
  needs investigation.

The native diagnostic samples show the IPC request spending time in the
present-worker dispatch-group drain and CoreGraphics/SkyLight mode configuration;
the compositor continues working. Samples also traverse display-link recreation.
They narrow the blocking path but do not establish one permanent deadlock or
prove a pacer reset caused it. Stacks are retained in `native-refresh-stack`.

The runner restored the initial mode and original LaunchAgent registration
following both failures. CV/calibrated refresh cycles remain unvalidated;
the first failed condition stopped that matrix. No automatic sleep/reconnect
recovery is claimed.

## Passthrough: camera pixels pass; overlay proof is incomplete

Three six-second calibrated WSI pilots completed and the applications exited 0:
service overlay at 90 Hz, service camera-only at 120 Hz, and hosted overlay at
90 Hz. Each recorded a completed readback over **6,104 samples**, with
`nonblack=5788`, `bad_alpha=0`, `bad_opaque=0`, `bad_gray=0`.

The verifier ran before any opaque application pixels were sampled:
`opaque=0` in all three readbacks. These results establish camera-output checks,
not application-pixel preservation in the overlay. The one-shot verifier must
also check an actual application-overlay frame before that gate can pass.
Native calibration/rotation parity remains absent from this WSI bridge.

The camera-only pilot initially returned `VK_ERROR_INITIALIZATION_FAILED` from
composition. The target logged that the acquired image was presented after
restoration, and then composed **575 frames** with successful pixel readback.
This exercises one recoverable startup failure without exhausting the swapchain;
it does not validate every export/Metal/device-loss failure path. The service
overlay and hosted overlay composed 306 and 311 frames respectively.

## Evidence and limits

Seventeen application sessions were attempted across these groups. Every
manifest reports successful display/job restoration. A final independent check
confirms mode `67`, 90 Hz, 4000×2040; the original installed LaunchAgent is
registered and idle, the experiment job is absent, and no experiment processes
remain. See `final-restoration.json`.

Local captures and `functional-summary.json` are under:

`build/review/wsi-headset-current-20261015/`

Groups: `baseline`, `hosted`, `passthrough-overlay`, `passthrough-only`,
`passthrough-hosted`, `refresh`, `refresh-wsi`, and native diagnostic captures.
Main runner logs are `build/review/wsi-headset-*.log`. The summary retains errors
instead of equating process success with test success.

This build has `XRT_FEATURE_MACOS_TIMING_DIAGNOSTICS=OFF`. It records presentation,
frame-pipeline and application-source traces, but not the full renderer
`reprojection` trace required by the strict performance analyzer. That analyzer
rejects these captures; `baseline/strict-analysis.json` preserves each rejection.
No latency, motion-to-photon or throughput superiority claim follows. Tracking
freshness, optical output, subjective smoothness, same-service repeated sessions,
Game Mode, sleep and disconnect/reconnect remain separate checks.
