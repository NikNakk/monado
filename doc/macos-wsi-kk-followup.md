<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->

# WSI with MVK and KK — 24 October 2026

This follow-up makes WSI the focus of the Alyx workspace's compositor comparison.
The previous native-presenter numbers in `macos-kk-compositor-followup.md` are not
WSI measurements. No installed runtime change or upstream submission is included.

## Capability correction

The macOS WSI target previously rejected every driver without
`VK_GOOGLE_display_timing`. KK provides the Metal surface and Vulkan swapchain
extensions, but not GOOGLE display timing, so it failed before creating a usable
compositor. This is recorded as `wsi-kk-graphics-default-a` in the parent Alyx
workspace. Its early client exit is not a successful performance run.

The generic swapchain helper already supports a fallback pacer and avoids
GOOGLE calls when the extension is absent. The macOS service target now permits
that path. It retains CVDisplayLink phase pacing, explicitly disables present-
offset calibration and logs that actual presentation feedback is unavailable.
The refresh-duration query is also gated on the enabled extension.

Hosted WSI still requires real first-presentation acknowledgement. Adaptive WSI
requires presentation feedback for its correction model. Both remain rejected
without GOOGLE timing; a successful queue-present return must not masquerade as
a presentation callback. A future standard present-ID/wait completion adapter
could address hosted acknowledgement, but is not implemented by this change.

This removes a portability restriction in the service target. It does not add
GOOGLE timing to KK, invent actual-present timestamps, prove display cadence,
change source-image reuse or make KK conformant.

## Comparisons

The parent workspace keeps `runs/wsi-kk/check-game.py`, its matrix, raw captures,
analysis and hashes. Both the service and client receive the explicit selector
`XRT_MACOS_PRESENTER=wsi`. `XRT_COMPOSITOR_COMPUTE` is also explicit:

- Graphics: distortion renders directly into FIFO swapchain images. This is
  the current WSI default without passthrough and the initial upstream focus.
- Compute: distortion writes private images, followed by a fragment output pass
  into the swapchain. Passthrough is disabled for these comparisons.

Alyx stays on the same KK binary with FDM and native 2x MSAA. Scene, fidelity,
logical eye dimensions and scripted input stay fixed. Main comparisons use
service composition and default queue priorities on both drivers; private Apple
priority setters are not needed. The first MVK priority-0 pilot is retained but
excluded from those comparisons. The actual desktop swapchain is 3078x1568;
headset-sized recommendations do not turn the window into the 4000x2040 panel.

The graphics runs use KK/MVK/MVK/KK order. Compute runs are initial per-driver
compatibility/measurement checks, not a repeated performance conclusion.
GPU durations come from the existing calibrated compositor brackets. Compute's
bracket excludes its subsequent fragment output pass, so these durations cannot
rank complete graphics-versus-compute pipeline cost.

MVK can report actual presentation feedback; KK cannot on this snapshot.
Compare successful submissions and rendering duration where available, and keep
actual presentation cadence/source-age metrics explicitly unavailable for KK.
CVDisplayLink cadence alone is not image presentation cadence.

All Alyx runs retain the previously identified startup framebuffer-input pipeline
failure after programmable-resolve/stitching fallback. They are diagnostic
captures, not visual acceptance or a clean end-to-end benchmark. Offscreen
renderer pixel tests were already validated in the preceding follow-up; the
known two KK host-event gate failures remain outstanding.

## Upstream boundaries

Prefer the Metal surface, ordinary Vulkan swapchain and generic pacer as the
small initial target. Preserve the no-display-timing service fallback. Keep
remote CALayer hosting, private queue-priority policy and PS VR2 camera/calibration
adapters outside that base requirement. The camera/output rendering operations
and their pixel/lifetime tests are portable Vulkan work and can be reviewed
separately from the platform/source policy.

Do not revive the rejected native direct-drawable or extra-buffering experiments:
WSI already owns its FIFO acquisition/presentation policy, and the earlier work
showed that isolated copy savings could worsen physical cadence.

## Hardware validation

`kk-tests/run-monado-kk-headset-comparison` in the parent Alyx workspace now
selects `--presenter wsi` and `--renderer graphics` explicitly by default.
Use `--renderer compute` for the private-output path, or `--presenter native
--renderer compute` for the previous native control. Default queue priority is
`-1` (leave unchanged). Each trial needs a fresh output directory; `--dry-run`
prints its full selected environment without starting a service.

These captures do not validate the previous moving-head shimmer issue, gaze,
final stereo image quality or PS VR2 cadence. Per AGENTS.md, hardware runs remain
user-operated. Native runtime defaults are not promoted or changed here.

The initial default-buffer captures expose another driver policy difference:
MVK returns two swapchain images, while KK returns three. Additional MVK runs
request three images with `XRT_COMPOSITOR_PREFERRED_IMAGE_COUNT=3` to compare
matching actual buffering. The raw default-buffer measurements are retained and
identified separately. The hardware launcher accepts `--image-count 3` for this
control; it does not change the runtime's image-count default.

Four independent synthetic OpenXR checks submit 60 projection frames each with
Metal API validation: graphics and compute on MVK and KK, three actual swapchain
images in every case. All complete with no recorded Metal assertion or device
loss. They use the no-USB desktop build and no Alyx shaders, so the known Alyx
startup pipeline failure cannot mask WSI initialization/rendering validation.
The desktop selector suite and headset-build selector suite each pass all five
existing cases. These checks do not replace the outstanding moving-head test.

For a future hosted completion adapter, see the Khronos
[vkWaitForPresentKHR contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitForPresentKHR.html).
Completion can acknowledge presentation without fabricating GOOGLE timestamps;
it has no required precise timing relationship to the first displayed pixel.
A completion callback is therefore distinct from calibration/actual-time feedback.
That adapter is not part of this patch.

## Final sampled GPU results

With three actual swapchain images on both drivers, graphics run medians are 0.294 ms, 0.277 ms on MVK and 0.417 ms, 0.410 ms on KK. The corresponding initial compute brackets are 0.667 and 1.077 ms, excluding the output fragment pass. Every selected window contains 600 valid single-projection frames and successful WSI submissions.
