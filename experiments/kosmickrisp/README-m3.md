<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Experimental native Vulkan OpenXR path

Read [assessment](../../docs/kosmickrisp-openxr-assessment.md#milestone-3--authorized-experimental-integration-2026-10-14)
for evidence and limits. This is a separate downstream Mesa ABI, not standard
Vulkan external-memory support. Keep it opt-in and out of installed runtime paths.
Do not use Vulkan validation layers with this ABI.

## Build

Use the M3 Monado worktree. The tested dependency environment is recorded in the
assessment's M1/M2 sections and M3 configuration logs: LLVM/libclc 23.1.3,
SPIRV translator 23.1.2, Meson 1.12.0, Ninja 1.13.2, Vulkan headers/loader 1.4.357,
Python 3.14.7 (Mako 1.3.10, MarkupSafe 3.0.4, packaging 25.0, PyYAML 6.0.3).
Mesa requires its documented macOS/Metal toolchain; no OS upgrade was needed on
macOS 26.7.1. Homebrew provides Monado dependencies described in AGENTS.md.

Example with an existing clean Mesa clone and configured toolchain:

```sh
repo=/private/tmp/monado-kosmickrisp-m3
study=/private/tmp/kosmickrisp-study
mesa="$study/mesa"
git -C "$mesa" worktree add -b codex/kk-openxr-reproduction \
  "$study/mesa-reproduction" aac5108bbfa3051f03a71c695b3a6f961ea0fc46
git -C "$study/mesa-reproduction" apply \
  "$repo/experiments/kosmickrisp/mesa-openxr-metal-interop.patch"
export PATH="$study/venv314/bin:/opt/homebrew/opt/llvm/bin:$PATH"
export PKG_CONFIG_PATH=/opt/homebrew/opt/llvm/lib/pkgconfig:/opt/homebrew/lib/pkgconfig
meson setup "$study/build-reproduction" "$study/mesa-reproduction" \
  --buildtype=debugoptimized -Dplatforms=macos -Dvulkan-drivers=kosmickrisp \
  -Dgallium-drivers= -Dopengl=false -Dzstd=disabled --prefer-static
ninja -C "$study/build-reproduction" -j 6
cmake -S "$repo" -B "$study/monado-reproduction" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DXRT_FEATURE_MACOS_KOSMICKRISP=ON \
  -DXRT_MACOS_KOSMICKRISP_INCLUDE_DIR="$study/mesa-reproduction/src/kosmickrisp/bridge"
cmake --build "$study/monado-reproduction" --parallel 6
ctest --test-dir "$study/monado-reproduction" --output-on-failure
```

Apply only `mesa-openxr-metal-interop.patch` to the pristine base; it includes the
texture/event work and supersedes the diagnostic patches. The header is supplied
by the patched Mesa tree. No Monado hard dependency is added with the feature OFF.
The tracked patch SHA256 and exact tested revisions are in the M3 manifest.

Create an explicit MoltenVK ICD in the study directory (adjust library path for
your installation), without changing system ICDs:

```sh
cat > "$study/moltenvk_icd.json" <<'JSON'
{"file_format_version":"1.0.1","ICD":{"library_path":"/opt/homebrew/opt/molten-vk/lib/libMoltenVK.dylib","api_version":"1.2.0"}}
JSON
```

## Simulated service tests

On the original test machine the existing compiled paths are `build-m3` and
`monado-build-m3`; substitute the reproduction paths above on a new checkout.
Arguments below never change installed agents or production manifests. The
runner's parent launches a unique test service and sets KK variables only in its
client. Test logs/CSV go to the selected output directories. macOS sandboxing may
require host GPU, local socket and launchd access.

```sh
python3 "$repo/experiments/kosmickrisp/run-openxr-m3.py" \
  --build "$study/monado-build-m3" \
  --driver "$study/build-m3/src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib" \
  --icd "$study/build-m3/src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json" \
  --molten-icd "$study/moltenvk_icd.json" \
  --output "$study/acceptance-client" --service-output "$study/acceptance-service"
```

Variants: append `--bgra`; use `--mode faults` for suppression/rejection,
concurrent healthy client, killed overlay and replacement. `--bad-library PATH`
can name the old M2 driver to verify a real library without the versioned ABI;
otherwise the missing-library case uses an absent file. Fault mode requires exit
1 for expected rejection and requires session-loss and frame-count log evidence;
a crash or timeout fails the runner. Inspect `client_gpu.csv` for the missing
client's four failures and zero ready/scheduled entries. The kill is a live client
termination, not a proof of killing a stalled GPU command in flight.

`--hosted` requests the existing client compositor. Check the log: a fallback is
not a hosted-path pass. The simulated target rejected hosting in this study.

## hello_xr and the user-operated hardware gate

The official sample source was unmodified at SDK-Source commit
`3ed64d0f9bb680f24b80a085091e5c8fab38f7b7`. The local system JsonCpp target failed
configuration; bundled JsonCpp needed its secure-memory macro defined to satisfy
-Wundef. The tested build was:

```sh
cmake -S "$study/openxr-sdk-source" -B "$study/hello-build-m3" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DBUILD_CONFORMANCE_TESTS=OFF \
  -DBUILD_API_LAYERS=OFF -DXR_USE_GRAPHICS_API_VULKAN=ON \
  -DBUILD_WITH_SYSTEM_JSONCPP=OFF -DCMAKE_CXX_FLAGS=-DJSONCPP_USING_SECURE_MEMORY=0
cmake --build "$study/hello-build-m3" --target hello_xr --parallel 6
```

Append `--mode hello --hello "$study/hello-build-m3/src/tests/hello_xr/hello_xr"`
to the isolated-run command. It renders for eight seconds, sends stdin newline to
quit cleanly, and requires exit 0. Append `--seconds 240` for a four-minute
viewing interval; the service timeout adjusts to allow graceful shutdown. Create
`STOP` in the client output directory to end viewing early through the sample's
normal stdin shutdown. Use fresh output directories for each run. Evidence must include readiness/scheduling and
non-black service output; process exit alone does not prove rendering.

For a **user-operated** PSVR2 trial, first close GAV and stop the ordinary headset
service so USB has one owner. Use fresh output directories and append `--headset`
to the hello command. This launches the existing isolated runner against real
hardware; it was not executed here. Start with service composition, then repeat
with `--hosted` and confirm actual hosted selection. Inspect visual correctness,
head tracking, timing, graceful exit and restart. Record the M3 commit tested in
the port evidence documents. Do not interpret simulated timing as headset pacing.

## Ownership and fallback

`copy_device` returns a retained Metal device; texture/event import retains its
native object and returns a driver-owned Vulkan handle. Vulkan destroy releases
that retain; the existing Metal provider/native compositor owns its independent
references. The adapter verifies library ABI and VkDevice dispatch module identity.
It supports only one Apple GPU, no validation layers, simple UNORM 2D swapchains
and timeline-enabled Vulkan enable2. Unsupported requests return errors rather
than adding copies. Feature OFF and Metal clients retain their working paths.

Pending producer work prevents image reuse. Incomplete barrier objects retire at
swapchain destruction; session teardown drains for at most 500 ms and preserves
unresolved resources. Retention until process exit needs further long-lived-client
budget/lifecycle work before production use. None of these tests establish OpenXR
or Vulkan conformance, driver device-loss recovery, physical Game Mode acceptance,
or a speedup over existing graphics paths.

`--backend moltenvk` runs the baseline driver with KK disabled. The official
hello sample selects different preferred formats and optional depth on that
backend; this is not a format-matched performance comparison. The first physical
baseline aborted on a Depth32Float IOSurface allocation. Keep this failure
separate from KK acceptance and do not claim comparative timing gains.

## Matching colour-only probes and frontend budget

For the depth-free comparison, use `--mode probe --bgra --three-images --hosted`
with `--headset`. Run `--backend moltenvk` and `--backend kosmickrisp` sequentially
in fresh directories, with no concurrent GPU tests. Both probes request Vulkan
1.2, two BGRA8 UNORM 2800x2856 single-layer/single-sample swapchains and three
images per eye on this headset. Descriptor logs verify actual allocation.

The normal driver has no shared-event readiness trace in this Vulkan frontend:
its existing frame commit uses queue idle. The runner therefore checks actual
compositor source consumption, rather than requiring KK semaphore events.
Neither consumption nor a Metal presented-handler invocation proves scanout when
`presented_monotonic_ns` is zero. The final colour-only captures had zero positive
presentation timestamps. Do not substitute callback cadence for scanout cadence.

Summarize the two captures with:

```sh
python3 "$repo/experiments/kosmickrisp/summarize-matched.py" \
  "$study/m3-matched-final-mvk" "$study/m3-matched-final-kk" \
  --output "$study/m3-matched-summary.json"
```

Probe-only runtime queue metrics instrument calls made through the runtime's
Vulkan dispatch: they exclude direct application calls, compositor-own dispatch,
shader compilation outside those calls and whole-process CPU usage. They add
clock reads, and total wall time in a driver call is not pure CPU execution.

Use `--mode budget` with the simulated service (omit `--headset` and `--hosted`).
It pins the runtime module while repeatedly creating/destroying sessions in one
process. Eight deliberately failed sessions retain reservations; session nine is
rejected. A separate healthy process creates/destroys twelve sessions, submitting
six frames each. Assertions require 32 failed obligations, 72 ready/scheduled
values, exactly one budget rejection and no healthy budget exhaustion. This tests
counts and ownership behavior, not an actual GPU device-loss reset.

The experimental driver adapter permits eight active or retained frontends per
process. Safe destroy releases a reservation; unresolved teardown retains it.
At exhaustion, new experimental sessions fail initialization with a diagnostic;
the application must restart after unresolved GPU failures. Existing native
swapchain byte/object quotas remain applicable. This count cap does not define a
complete cross-process byte budget or reclaim unresolved GPU resources.

Actual Game Mode remains a separate user-observed fullscreen application gate.
These headless CLI probes and hosted selection alone do not confirm activation,
service external backgrounding or recovery across Game Mode/menu transitions.
