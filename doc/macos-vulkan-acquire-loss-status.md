<!-- Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0 -->

# Vulkan acquire loss status — 2026-10-15

The isolated `codex/macos-vulkan-acquire-loss-status` branch, based on
`cf6bedeb2`, implements the fix at `7a494b87e` and removes OpenXR event dispatch from the macOS Vulkan acquire wait.
The user requested headset checks on 2026-10-15. The tested branch was then
fast-forwarded locally into `macos-upstream-clean` at `cd9294f8c`. Matching ARM64
service/runtime and Wine x64 runtime are installed as `v25.1.0-2314-gcd9294f8c`;
the [installation manifest](macos-graphics-integration-results/local-install-20261015.json)
records paths, hashes and the previous-installation backup. Existing LaunchAgent
plists are byte-for-byte preserved. Both loader probes pass (61 ARM64 / 57 x64
extensions). No remote push has occurred. Headset visual acceptance is pending
the user observations below.

## Behavior and ownership

Previously, a 50 ms image-wait timeout called `oxr_session_poll`. An application
thread running `xrPollEvent` could concurrently drain events and mutate the same
OpenXR session state. The instance event queue mutex does not protect these
session mutations.

The wait now queries sticky loss through `xrt_session_get_loss_status`, without
consuming or dispatching events. `b_session` records loss when the LOST event is
published, under its existing event mutex. Consuming the event does not clear
loss. LOSS_PENDING alone does not cancel acquisition. All OpenXR `has_lost`
accesses use the existing portable atomic helpers. The 50 ms cancellation checks
and existing IPC-error-to-session-loss mapping remain intact.

Service clients use a read-only IPC query. Its command is appended after all 129
existing command IDs; shared-memory layout is unchanged. Hosted clients query
local GPU loss, ignore loss of the unused remote headless compositor, and still
propagate service transport failure. Unsupported session implementations return
`XRT_ERROR_NOT_IMPLEMENTED`. Service and client must be deployed together.

There is no extra GPU wait, image copy or compositor change. A stalled acquire
performs an IPC status request after each timeout; it uses the existing serialized
IPC transport. Healthy acquisition does not make this request. This addresses
the added acquire/event-poller race; it is not a general proof that every OpenXR
session state access supports arbitrary concurrent application calls. Destruction
still requires the existing lifetime discipline.

## Regression evidence

- `tests_session_loss_status`: sticky/non-consuming loss and concurrent ordered
  publication, event polling and status observation; 1019 assertions pass.
- `tests_oxr_vk_acquire_loss`: the actual Vulkan acquire implementation cancels
  after another thread publishes and consumes LOST, without dispatching OpenXR
  events or entering the image barrier; six assertions pass. Linking the same
  test against the baseline acquire implementation fails specifically because
  it dispatches events (one dispatch instead of zero).
- Hosted-session tests preserve queued local loss, ignore unused remote loss,
  and propagate transport failure.
- ThreadSanitizer passes for both focused regressions, instrumenting the actual
  `b_session.c`, test code, and (for acquire) `oxr_swapchain_vk.c`. Other linked
  runtime components were not instrumented; this is not a full-runtime audit.
- ARM64 and x64 builds pass. Rosetta x64 loader negotiation and extension
  enumeration pass (57 extensions), without contacting the installed service.
- Simulated-HMD Vulkan service and hosted fault/replacement suites pass with
  runtime defaults and diagnostic readbacks disabled. Normal fault recovery can
  observe loss in `xrWaitFrame`; the focused acquire test separately proves the
  stalled-acquire branch. These are not PS VR2 visual acceptance runs.

The full macOS suite passes 80/80 (47.88 seconds); default Ubuntu CI passes
49/49 (329.86 seconds). Retained logs in
`macos-graphics-integration-results/` have SHA256 values:

- `polling-fix-final-ctest.log`: `01cebea520f48c5a86300375134994dd4cf8307bd2dcb6ee5cd9038ef933a996`
- `polling-fix-tsan.log`: `cf5a1566e26e507057fd932374e824a198ac6485b3ca287036257b3a2a9606c1`
- `polling-fix-acquire-test.log`: `2c710b33b2f1df7af5c4cd5a571e451c6e8d798e5ab1348d2a55f451caae05db`
- `polling-fix-negative-control.log`: `78b89a43bf199fe98199c827f232eef004e356241bfaba80e4823bd465bcc932`
- `polling-fix-acquire-tsan.log`: `2717bacfed03d3d4353b49e21039b40799b6f97c36b1ec50275678822777e731`


- `polling-fix-linux-ctest.log`: `8eb0c53dfc7e15d8d5904f32a5eddc78ae76e9bec3356d810e80a7df69724187`

## Reproduction and remaining gate

Use the macOS build recipe in `macos-port.md`. Enable
`XRT_MACOS_SERVICE_GPU_TESTS=ON` for service GPU tests. Run:

```sh
cmake --build <build> --parallel
ctest --test-dir <build> --output-on-failure
ctest --test-dir <build> -R 'tests_(session_loss_status|oxr_vk_acquire_loss|ipc_local_session)' --output-on-failure
python3 scripts/macos/run-vulkan-metal-textures.py --build <build> --output <results>/service --suite --runtime-defaults --no-readbacks
python3 scripts/macos/run-vulkan-metal-textures.py --build <build> --output <results>/hosted --suite --runtime-defaults --no-readbacks --hosted
```

Linux validation uses the unmodified default CI configuration on Ubuntu 24.04.
The test hardware is an Apple M5 MacBook Air (Mac17,4), 24 GB, macOS 26.7.1
(25G241), Xcode 27.0 (27A266a), Apple Clang 21.0.0, CMake 4.4.3, Ninja 1.13.2,
MoltenVK 1.4.2 and Vulkan headers/loader 1.4.357.0. The ARM64 build uses
RelWithDebInfo, timing diagnostics ON, service GPU tests ON and Perfetto OFF.
Ubuntu packages: GCC `4:13.2.0-7ubuntu1`, CMake `3.28.3-1build7`, Ninja
`1.11.1-2`, Ceres `2.2.0+dfsg-4.1ubuntu2`, Eigen `3.4.0-4build0.1`,
Vulkan `1.3.275.0-1build1` and glslang `15.1.0-2~ubuntu0.24.04.2`. Linux validation runs in an
amd64 Ubuntu container, not as a runtime dependency.

Matching ARM64/service and x64 clients are now deployed for the user-operated
four-case service/hosted `hello_xr` and stereo-array headset checks. The [publication gate](macos-graphics-integration-review.md#installed-build-headset-gate-before-publication)
remains pending; a future installation changes its tested revision. The requested installation is complete; no push has occurred. Source is original BSL-1.0 work;
no external project code was adapted.

## Installed headset checks — 2026-10-15

At the user's request all four checks were launched against the installed runtime
and launchd service at `cd9294f8c` (`v25.1.0-2314-gcd9294f8c`). `hello_xr`
ran for 45 seconds in each mode and shut down through its normal stdin command.
Each stereo-array check submitted 3600 projection frames and exited zero. Both
hosted runs explicitly log client-process composition; the hosted array logs
`remote=0`, two layers and the Metal shared-event frame handoff. The sharing
switches were unset and source/presentation diagnostic readbacks disabled.
This is installed-service testing, not the isolated simulated-HMD launcher.

The user reports that the stereo array test looked fine, but `hello_xr` head
tracking seemed off. The mode and nature of that symptom are awaiting
clarification. **The publication gate remains open.** Successful process exits
and array colours do not establish correct head tracking, depth or frame pacing.
The physical display was already configured at 90 Hz (~89.91 Hz measured);
no display refresh change was requested or applied. No pose, prediction or
presentation source changed between the prior installed `84b3749c2` and this
build; the polling fix is the only runtime source delta. This narrows the
investigation but does not rule out an indirect graphics timing effect.

The unmodified SDK `hello_xr` source is clean at
`3ed64d0f9bb680f24b80a085091e5c8fab38f7b7`, built Release. The first array launch
exited before rendering because the temporary launcher lacked
`MONADO_OPENXR_RUNTIME_PATH`; after setting it to the installed runtime, both
array runs completed. The temporary launcher and all raw logs remain under
`build/arm64/headset-gate-20261015/` (launcher copy below); the
[durable run manifest](macos-graphics-integration-results/headset-checks-20261015.json)
records commands, timestamps, exit codes and log hashes. The normal ARM64
installation build recovered an incomplete Ninja log and emitted existing
external Perfetto variadic-macro warnings; both architecture builds completed.

### Tracking symptom clarification

The user localizes the symptom to the first, service-composited `hello_xr`:
the head-locked cube and distant cubes looked fine, while the nearby world-locked
large cube made occasional large jumps and juddered with tiny head movements.
They did not observe the first hosted scene. A 45-second hosted repeat exits
cleanly; its visual result is pending.

The existing raw SLAM receipt trace for the service run contains ten consecutive
sample steps above 10 cm, with a maximum 30.97 cm in 33.396 ms. These positions
are recorded from headset data before prediction/rendering. The largest samples
have driver relation flags 63 on both sides; that is not independent hardware
confidence. The first hosted run has no steps over 10 cm (maximum 2.06 mm), and
the observed-repeat trace likewise has none (maximum 10.47 mm). These are
unmatched head motions at different times, so they do not prove a compositor-mode
cause or a hardware fault. They establish raw positional discontinuities in the
run the user found problematic, warranting tracking-state investigation before
attributing the effect to Vulkan synchronization. No tracking fix or visual
acceptance is claimed. The [analysis record](macos-graphics-integration-results/headset-tracking-20261015.json)
retains trace paths, hashes, method and largest steps.
