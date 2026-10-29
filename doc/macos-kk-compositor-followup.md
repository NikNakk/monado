<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->

# KK compositor follow-up — 24 October 2026

This is an isolated compatibility candidate, not a driver-default change. Keep
MoltenVK as the installed compositor baseline. The Alyx workspace retains raw
logs, driver binaries, regression runners and analysis under
`runs/monado-kk-compat`.

## Private scratch images

`XRT_MACOS_COMPOSITOR_PRIVATE_SCRATCH=1` (default off, macOS only) allocates
scratch images directly with Vulkan device-local memory instead of requesting
exportable native handles. The exported handles only served the scratch-image
debug viewer; production rendering and image lifetime remain Vulkan-owned.
Private scratch images therefore do not appear in that viewer. Application
swapchains, presenter images, shared-event handoff and GPU reuse guards retain
their existing allocation and synchronization paths.

This removes KK's hosted compositor startup failure in `vk_ic_get_handles`:
ordinary KK textures do not have an IOSurface. It also avoids requiring shared
storage for internal images. No bandwidth or GPU-time improvement is claimed
from this allocation change alone.

The same change fixes unchanged non-mutable scratch allocations being recreated:
these images have no mutable-format list, so reuse now compares the actual image
format and the presence of the format list. The lifecycle regression exercises
all four images twice, mutable and non-mutable views, resizing, pixel readback,
unchanged-size reuse and destruction. Metal validation passes with KK/private,
MVK/private and MVK/default. KK/default reproduces the original export failure.

## Timestamps and compatibility gates

The current KK compute bridge used relaxed Metal counter sampling. A six-query
pattern matching Monado's adjacent frame/pass brackets returns zero for some
available queries. Two-query samples with intervening work pass. The independent
`[timestamps]` tests retain both patterns and repeat submissions. Precise Metal
compute sampling fixes the adjacent-query test; this is a KK correctness change,
not a performance optimization. The driver patch lives in the Alyx workspace's
`workspace/kk-fdm/src/kosmickrisp/bridge/mtl_encoder.m`.

The initial full offscreen compositor suite passes all 18 cases on MVK. KK passes
16 and fails two blocked-resource cases: `kk_CmdWaitEvents2` ignores the event
wait, so the host event used by those tests cannot hold back queued work. Keep
these failures visible; do not weaken the tests or remove reuse protection.
These tests do not prove the ordinary native presenter uses Vulkan host events.

KK does not export a traditional Metal command queue through
`VkExportMetalCommandQueueInfoEXT`. Its actual queue is `MTL4CommandQueue`.
Do not cast it to `MTLCommandQueue` merely to enable Monado's priority helper.
A suitable priority mechanism and scheduling comparison remain outstanding.

## Alyx census

Two 90-second desktop hosted runs, with 4000x2040 simulated display size,
fidelity 5, logical 1344x1344 eye targets, foveation and native 2x MSAA, remove
the scratch export failure. All 600 compositor frames sampled 49–59 seconds
from the first compositor record select the single-projection fast path.
Projection-plus-quads specialization therefore cannot improve this sampled scene.
The compositor and application windows use separately recorded time origins.

Producer-readiness `VK_NOT_READY` rejections remain during loading, including
with an MVK service control. Hosted rejections stop before the first game unpause.
The guard intentionally avoids submitting a wait on an unready producer; retain
it. These are separate from scratch allocation failures and GPU device loss.

The first hosted run records 5,211 invalid calibrated GPU intervals. With precise
compute sampling, the next records 5,202 intervals, all ordered and bounded by
the readback time. The sampled compositor median is 0.833 ms. Application GPU
medians are 9.359 and 9.355 ms respectively; one pair does not establish the cost
of precise sampling. Both run at the simulated display's approximately 60 Hz cap.

All three initial desktop captures (the two hosted runs and MVK service control)
also log a background Alyx pipeline creation failure: framebuffer input reads
an invalid attachment format after a programmable-resolve/stitching fallback.
Treat these as diagnostic captures, not final visual acceptance. No headset
motion, final image quality or 120 Hz cadence is established here.

## Next gates

1. Preserve the MVK baseline; run the service compositor with Alyx's KK driver
   fixed, then change only the service ICD. Hosted versus service is a separate
   architecture comparison, not an ICD-only comparison.
2. Resolve/contain host-event incompatibility, queue-priority support and the
   Alyx startup pipeline failure before promoting KK as a general compositor.
3. Hardware validation is user-operated per `AGENTS.md`: compare MSAA coverage
   in motion, final stereo images, display cadence, source-frame age and tails.
4. Profile remaining Alyx fragment work and attachment traffic only against a
   visually accepted baseline. Keep previously rejected direct-output/buffering
   experiments retired unless new evidence changes their premise.

## Service-only controls and queue priority

With Alyx fixed on KK and hosted composition disabled, temporary services on MVK
and KK complete 90-second desktop runs. The selected 49–59-second compositor
windows each contain 600 single-projection fast-path frames with valid GPU
intervals. MVK's sampled median is 0.603 ms; KK's is 1.042 ms. Alyx medians are
9.157 and 9.130 ms respectively. These are single diagnostic controls, not an
ABBA result or a headset comparison. Both retain the startup pipeline failure.

A standalone probe finds both the legacy Metal queue and the Metal 4 queue on
this M5 support the private priority selectors used by Monado. An opt-in in KK's
own queue creation therefore applies priority without misrepresenting its Metal
4 queue through the Vulkan Metal-object export interface:

- `KK_GPU_QUEUE_PRIORITY=-1` (default): leave the queue unchanged.
- Values `0`–`2`: request foreground/background priority if all four selectors
  exist. Invalid values are rejected without invoking setters. This is a private
  Apple interface, runtime checked, not a portable Vulkan priority implementation.
- For an isolated service, pass `KK_GPU_QUEUE_PRIORITY=0` through
  `MONADO_SERVICE_ENV`, not the application's global environment.

The priority-0 service control logs foreground priority 0 (background remains 2)
and a sampled compositor median of 0.774 ms, versus 1.042 ms in the earlier KK
control. MVK remains lower. One run per configuration does not separate scheduling,
thermal and order effects; no production scheduling or latency claim is made.
The application log contains no priority override; only the service is changed.

## User-operated hardware check

The same Monado changes are also built in the separate gaze-fix checkout and
`build/monado-claude-gaze-fix`, preserving commit `93899b7b8`'s gaze correction.
The qwerty-script desktop build deliberately has libusb disabled and must not be
used as the headset service. No installed service is replaced.

From the parent Alyx workspace, inspect the trial before running:

```sh
kk-tests/run-monado-kk-headset-comparison --output runs/headset-msaa-native --aa native-msaa --compositor service-mvk --dry-run
```

Omit `--dry-run` for a user-operated 180-second trial. Compare `--aa
emulated-msaa`, `native-msaa` and `none` on the same compositor, camera, fidelity
and resolution. Then hold AA fixed and compare `--compositor service-mvk`,
`service-kk` and `service-kk-priority`; `hosted-kk` is a separate process-topology
comparison. Every trial needs a new output directory. The runner restores the
original game convars after the game exits and preserves a backup if it cannot
confirm exit. It never claims visual acceptance from successful process exit.

Keep the native presenter and the existing source-reuse protection. Check both
eyes, coverage of thin geometry/foliage, motion, overlays, gaze changes and
final presentation cadence before judging performance. The known startup
framebuffer-input pipeline failure remains an acceptance blocker.

Final headset-capable offscreen regressions: MVK passes all 20 cases (505,990
assertions). KK passes 18 of 20; the same two host-event cases retain seven
failed assertions. Both timestamp patterns and scratch lifecycle pass. No Linux
build or headset run was performed.

The subsequent [WSI study](macos-wsi-kk-followup.md) supersedes the comparison
launcher defaults: it now explicitly selects WSI/graphics and leaves priorities
unchanged. Use `--presenter native --renderer compute` for the native path above.
