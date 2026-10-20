# Native ARM64 Alyx workspace

Preserved on 20 October 2026 at `/Users/nickkennedy/Code/alyx-native`.
Use this workspace for further work; `/private/tmp` is no longer the canonical
location. The original temporary setup was left intact.

## Launch

Keep native macOS Steam running and signed in, and use the existing Monado
setup. The default launcher selects the original user-confirmed debug xrizer:

```sh
~/Code/alyx-native/run-alyx \
  --output ~/Code/alyx-native/runs/new-debug-run --timeout 180
```

The optimized candidate includes the tested stage-fallback fix, but still needs
full-game confirmation. Its optional trace measures API wall time, not GPU time:

```sh
~/Code/alyx-native/run-alyx --optimized --api-trace \
  --output ~/Code/alyx-native/runs/next-test --timeout 180
```

Each output directory must be new. The launcher reports host RAM automatically,
keeps MoltenVK as default, and records the selected library hashes/driver.
It does not modify the installed Monado service or original Steam game files.

## Contents

- `workspace/alyx-verantyx`: constrained native host and launch/analysis tools.
- `workspace/alyx-klepton`: Linux ABI and Vulkan compatibility implementation.
- `workspace/xrizer-dual-vulkan`: original working native xrizer source/build.
- `workspace/xrizer-performance`: isolated stage-fix/optional API tracing source.
- `workspace/xrizer-perf-target`: matching optimized/debug candidate builds.
- `workspace/monado-alyx-dual-vulkan`: investigation documents and source patches.
- `workspace/alyx-mesa` and `workspace/alyx-mesa-build`: experimental driver.
- `workspace/alyx-native-game-root`: private prepared game clone and saves.
- `workspace/alyx-native-guest-libs-runtime`: active guest library links; all
  temporary dependencies were copied and links relocated into this workspace.
- Other `workspace/alyx-*` / `workspace/xrizer-*`: dependencies, extracted packages,
  tools, old run logs and reproduction results, retained for investigation.
- `git-backups/*.git`: independent bare Git mirrors of all six repositories.
  These preserve locally available history, including shallow boundaries where
  upstream clones were shallow. They use no temporary object alternates.
- `preservation.json`: source revisions, copied paths and SHA256 checks.
- `validation-*`: relocation checks. Driver-only checks do not prove gameplay.

The private game root, dependency packages and logs are outside Git. Git mirrors
preserve source history separately from those data files. APFS copy-on-write was
used for the large data/build copies. Copies have separate directory entries;
changing or removing the old temporary paths does not remove this workspace.

## Validation and dependencies

Six Git mirrors/checkouts passed connectivity checks with no object alternates.
741 symlinks were relocated; no unmapped temporary symlink remains. All active
library aliases resolve. SHA256 checks match the originals for 193 guest library
names and six critical launch/driver binaries. The relocated real-tier0 VM test
passes. Default/optimized launcher dry runs use permanent paths. Relocated
MoltenVK and KosmicKrisp driver censuses both identify Apple M5 successfully.
A full game run from the new permanent paths is the remaining validation step.
KosmicKrisp still lacks the Metal-object interoperability needed by this Monado
path; the census is not evidence that it can run the game end to end.

Homebrew Vulkan dependencies, the native OpenXR SDK loader, the installed Monado
runtime and native Steam remain in their existing permanent locations. They are
external prerequisites; the launcher does not replace or install them. Old build
metadata/debug source references can mention `/private/tmp`; rebuild from the
preserved sources with explicit permanent build/output paths when changing code.

Example candidate rebuild:

```sh
cd ~/Code/alyx-native/workspace/xrizer-performance
CARGO_TARGET_DIR=~/Code/alyx-native/workspace/xrizer-perf-target \
  XRIZER_VERSION=$(git rev-parse --short HEAD) cargo build --offline --release -p xrizer
```

`preserve-workspace.py` records the one-time preservation procedure; it is not an
incremental backup command. Further development should be committed in these
permanent checkouts and their local mirrors refreshed when needed.
