<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Projection-only workload census — 2026-10-13

**The measured extra-pass target is native UE's identity color-scale/bias flag.**
Native OpenBrush, Wine Hyperbolica and the sampled SteamVR Home workload already select the
single-projection distortion fast path. The existing projection-plus-quads
optimization does not apply to these measured nonempty submissions.

## Diagnostic and capture method

`PSVR2_TIMING_TRACE=1` now writes `composition.csv`: one frame summary (index -1)
and every native layer, including type, flags, projection view count, chroma,
foveation, first-view source size and color scale/bias. The summary records the
existing fast-path selection and all applicable rejection gates. Rendering
selection is unchanged. Diagnostic code is retained in `3601baab5`; the captures
used that code before commit on base `6825797ab` (the binary reports the base tag). The [analyzer](../scripts/macos/analyze-projection-census.py)
rejects incomplete lists and inconsistent selection, trims each compositor
instance separately, and labels short traces as insufficient duration.

New captures use the checkout's diagnostic runtime via temporary launchd
registrations, without replacing the installed runtime or LaunchAgent. The
installed profile sets GPU priority **0**; captures preserve it in both service
and hosted client, and logs confirm foreground queue readback 0. Background
readback stays 2, as already documented. Game Mode is not independently verified.
Raw captures remain in `/tmp`, not Git. Counts below are native compositor
submissions, including reuse of application frames, not distinct game frames.

## Observations

All table counts exclude empty submissions and trim two seconds from each end.
These are static layer inventories, not performance A/B or worn visual tests.

| Client | Nonempty compositor submissions | Layers and flags | Fast-path result |
| --- | ---: | --- | --- |
| Native UE `MonadoMacTest`, hosted compositor | 1,632 | One projection; `0x10` color-scale/bias; source 2800×2856; no depth, foveation or chroma key | Every frame rejected solely for color-scale/bias. Every flagged value is identity: scale (1,1,1,1), bias (0,0,0,0). |
| Native Unity/OpenBrush, service compositor | 1,003 | One projection; `0x4` unpremultiplied-alpha; source 2800×2856 | Every nonempty frame eligible and selected. |
| Wine OpenXR Hyperbolica, service compositor | 5,158 | One projection; `0x6` source-alpha plus unpremultiplied-alpha; source 2800×2856 | Every nonempty frame eligible and selected. |
| SteamVR Home, service compositor | 14,133 | One projection; flags 0; source settles at 2019×2058 after initial sizes | Every nonempty frame eligible and selected. Dashboard/multiple-projection content was not sampled in this run. |

UE, Unity, Wine Hyperbolica and SteamVR record 1,917 / 1,396 / 4,857 / 14,497
actual physical presentation callbacks respectively. Counts establish a live presentation surface, not image
quality or motion-to-photon latency. A short UE startup attempt had only two
nonempty frames and no physical callbacks; it is superseded by the longer census.
Temporary helpers needed adjustments for on-demand launchd startup, absent
client-side trace flushing in service mode and Windows process names. Unity and
SteamVR inventories were validated from complete traces closed during service
teardown; those helper runs are not timing acceptance captures. Hyperbolica
likewise needed teardown for its older client trace, which does not acknowledge
flush requests; the service flush completed and its full census validates. The pre-existing
Steam process was preserved and census-owned VR processes/services were stopped.

Hyperbolica is installed under `~/Windows/prefix/drive_c/Program Files (x86)/Steam/
steamapps/common/Hyperbolica/Hyperbolica.exe`. It was launched directly with
`startXR -force-d3d11`, the existing CrossOver/DXMT Wine tree, and wineopenxr;
logs confirm a four-image, two-slice D3D11-to-Metal zero-copy swapchain. The first
attempt omitted `startXR` and stayed in desktop mode: one empty compositor
submission, no physical callbacks, explicitly insufficient duration. It is
superseded by the VR census, not evidence of headset sleep.

SteamVR and Hyperbolica use the existing x86_64 native bridge client at
`6f96e49ee` against diagnostic service base `6825797ab` plus this census change,
with `IPC_IGNORE_VERSION=1`. The IPC sources and `xrt_compositor.h` are unchanged
between those tags; the census adds no wire changes. Installed runtime files,
Steam configuration and prefix registration were not replaced.

The old Underture/InMind copies were not present under `~/Windows`; they are not
freshly measured here. Earlier SteamVR logs also sample two projections with
dashboard content; that path still needs a current inventory before specialization.

Local captures: `/tmp/monado-ue-layer-census-long-20261013`,
`/tmp/monado-unity-layer-census-20261013`,
`/tmp/monado-steamvr-layer-census-20261013`,
`/tmp/monado-hyperbolica-xr-layer-census-20261013`. These temporary files are not durable
project evidence; the observations and limitations above are the retained record.

## Next optimization gate

Test whether a macOS identity scale/bias attachment can use the already existing
projection fast path. It must preserve opaque/alpha semantics and image quality:
removing the intermediate pass changes filtering/quantization, so identity color
values alone do not establish byte-equivalent output. Require matched source/pose
checks, a representative UE image comparison and priority-0 hardware controls
before any promotion. No fast-path eligibility change is included in this census.
For Unity, Hyperbolica and the sampled Home path, focus on distortion/timewarp cost or GPU
scheduling; they already avoid the layer pass. Do not apply the quad specialization
or report its synthetic gains as benefits for these clients.

Software validation: full arm64 build and all 73 CTests pass; changed x86_64
compositor/compute-test targets build. Five Python analyzer tests cover identity
flags, multiple projections, incomplete lists, selection disagreement and short
traces. The final writer is also checked with a bounded projection-only offscreen
submission. No Linux CI or new game-performance improvement is claimed.
