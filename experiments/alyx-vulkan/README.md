<!-- Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0 -->

# Reproduce the native Alyx Vulkan investigation

Read [the report](../../doc/macos-alyx-kosmickrisp.md) first. These commands test
public-source fixtures only. Native Alyx is unavailable and the existing host's
guest instruction repairs conflict with the no-game-binary-modification rule.
Do not run the game host until that issue is resolved.

## Source reconstruction

The Monado and xrizer changes are local branches in their original repositories:
`codex/alyx-dual-vulkan` and `codex/macos-dual-vulkan`. Public-source clones live
in `/private/tmp/alyx-{klepton,verantyx,mesa}`. Their pins are in the report.
To reconstruct Klepton, check out `ce74582`, apply
`patches/klepton-prerequisites.patch` then `patches/klepton-dual-vulkan.patch`
with `git am`. The prerequisites are VeRantyx's existing patch script output,
kept separate from this experiment. To reconstruct the VeRantyx adapter, check
out native commit `38c02b9492c9148f6a4ca86dc607ce52aa6f7403` and apply
`patches/verantyx-xrizer.patch`. No game files are included.

Build Mesa with its documented dependencies, without installing it:

```sh
python3.14 -m venv /tmp/alyx-mesa-venv-new
/tmp/alyx-mesa-venv-new/bin/pip install meson mako packaging pyyaml
export PATH="/tmp/alyx-mesa-venv-new/bin:/opt/homebrew/opt/llvm/bin:$PATH"
export PKG_CONFIG_PATH=/opt/homebrew/opt/llvm/lib/pkgconfig:/opt/homebrew/lib/pkgconfig
OBJC=/usr/bin/clang meson setup /tmp/mesa-alyx-new /private/tmp/alyx-mesa \
  --buildtype=release -Dplatforms=macos -Dvulkan-drivers=kosmickrisp \
  -Dgallium-drivers= -Dopengl=false -Dzstd=disabled --prefer-static
ninja -C /tmp/mesa-alyx-new -j6
```

The tested build was `/private/tmp/alyx-mesa-build`. A fresh rebuild may use
newer Python dependencies; the original setup/build logs remain under
`/private/tmp/alyx-results`. No archived private interop patch is required.

## Isolated Monado/xrizer submission test

Build Monado on the isolated experiment branch (or Claude's matching legacy fix):

```sh
cmake -S /private/tmp/monado-alyx-dual-vulkan -B /private/tmp/monado-alyx-build \
  -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build /private/tmp/monado-alyx-build --parallel 6
cd /private/tmp/xrizer-dual-vulkan
cargo build --locked
sh tools/macos/build-probe.sh
```

Prepare a client wrapper so nested options are not parsed by Monado's runner:

```sh
cat > /tmp/alyx-vulkan-client.sh <<'SH'
#!/bin/sh
set -eu
set -- --driver "${ALYX_TEST_DRIVER:-moltenvk}"
if [ -n "${ALYX_TEST_ICD:-}" ]; then set -- "$@" --icd "$ALYX_TEST_ICD"; fi
exec python3 /private/tmp/xrizer-dual-vulkan/tools/macos/run-vulkan-backend.py \
  "$@" --output "$ALYX_TEST_OUTPUT" -- \
  /private/tmp/xrizer-dual-vulkan/target/vulkan_submit_probe \
  /private/tmp/xrizer-dual-vulkan/target/debug/libxrizer.dylib 180
SH
chmod +x /tmp/alyx-vulkan-client.sh
export XR_RUNTIME_JSON=/private/tmp/monado-alyx-build/openxr_monado-dev.json
export XRIZER_OPENXR_LOADER=/Users/nickkennedy/Code/gav-psvr2-player-mac/openxr-poc/build/openxr-sdk/src/loader/libopenxr_loader.dylib
# Monado service stays on MoltenVK; only the wrapped client selects the test ICD.
export VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
ALYX_TEST_DRIVER=moltenvk ALYX_TEST_OUTPUT=/tmp/mvk-client-new \
python3 /private/tmp/monado-alyx-dual-vulkan/scripts/macos/run-gpu-sync-service.py \
  --build /private/tmp/monado-alyx-build --output /tmp/mvk-service-new \
  --client /tmp/alyx-vulkan-client.sh
ALYX_TEST_DRIVER=kosmickrisp ALYX_TEST_OUTPUT=/tmp/kk-client-new \
ALYX_TEST_ICD=/private/tmp/alyx-mesa-build/src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json \
python3 /private/tmp/monado-alyx-dual-vulkan/scripts/macos/run-gpu-sync-service.py \
  --build /private/tmp/monado-alyx-build --output /tmp/kk-service-new \
  --client /tmp/alyx-vulkan-client.sh
```

Use fresh output names. MVK passes; KK currently fails extension negotiation.
The runner creates/removes a unique simulated-HMD launchd service and socket;
it does not replace the installed service. Do not use `IPC_IGNORE_VERSION` to
mask a mismatched OpenXR library. These GPU tests need normal host GPU access.

## Other tests and benchmark

Existing Monado tests, with the same isolated build:

```sh
ctest --test-dir /private/tmp/monado-alyx-build --output-on-failure
python3 scripts/macos/run-vulkan-metal-textures.py --build /private/tmp/monado-alyx-build \
  --output /tmp/mvk-import-new --mode import --cross-process
python3 scripts/macos/run-vulkan-metal-textures.py --build /private/tmp/monado-alyx-build \
  --output /tmp/mvk-array-new --frames 60
clang -fobjc-arc -framework Foundation -framework Metal \
  experiments/alyx-vulkan/metal-rate-map.m -o /tmp/alyx-rate-map
/tmp/alyx-rate-map
```

From `driver-benchmark/`:

```sh
for s in fs.vert alu.frag tex.frag; do glslangValidator -V -o "$s.spv" "$s"; done
clang -O2 -I/opt/homebrew/include -L/opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib \
  -lvulkan kkbench.c -o kkbench
clang -framework Foundation thermal-state.m -o thermal-state
python3 sweep.py \
  /private/tmp/alyx-mesa-build/src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json \
  /opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json /tmp/alyx-benchmark-new.jsonl
```

The benchmark is restored from the October 14 archive, adding p99 reporting,
first-observation separation and per-run thermal/process/RSS capture. It does
not purge Metal's cache or guarantee cold starts. Only compare runs with matched
thermal state; record background activity and cool down between heavy cases.
No headset, foveated image, game performance or physical presentation is tested.

The adapter-only check loads xrizer without starting OpenXR or touching a game:

```sh
clang -pthread experiments/alyx-vulkan/host-adapter-smoke.c \
  /private/tmp/alyx-verantyx/NativeARM/host/kl_xrizer.c -o /tmp/alyx-adapter-smoke
XRIZER_VRCLIENT=/private/tmp/xrizer-dual-vulkan/target/debug/libxrizer.dylib \
  /tmp/alyx-adapter-smoke
```

## Read-only ARM64 inventory (20 October)

```sh
python3 experiments/alyx-vulkan/inspect-arm64.py \
  --depot '/Users/nickkennedy/Library/Application Support/Steam/Steam.AppBundle/Steam/Contents/MacOS/steamapps/content/app_546560/depot_546564' \
  --output /tmp/alyx-static-new
```

This only reads ELF files and produces hashes, strings, symbol metadata and
candidate string references. It never loads libraries or translates code.
It hashes the inspected files again afterwards and fails if any changed.
Read the [manual audit](../../doc/macos-alyx-arm64-static-audit.md) for checked
interpretations; candidate addresses alone do not prove execution or control flow.
