<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Wine/DXMT GPU synchronization: M4, 2026-10-12

**M4's six exit criteria pass for the synthetic Windows/DXMT → native Vulkan
fixture.** This extends the M1 transport and fault contract across genuine
Windows x86-64 code. It does not validate SteamVR, Wine OpenXR presentation,
production service recovery from a Wine client, or physical headset output.
Those remain separate from this headless milestone.

Implementation: `492f9d030` (shared protocol, external producer, matched native
upload control, failed-start cleanup), `b10eda942` (Windows producer and runners).
Chromium M3 cleanup is recorded in [the M3 ledger](macos-gpu-sync-chromium.md).
No additional production Monado, Wine or DXMT synchronization changes were
needed in this run.
The pre-existing SteamVR/Wine/DXMT working trees were preserved.

## Actual producer and consumer

The test executable is PE32+ x86-64, running under the existing CrossOver-built
runtime (`wine --version`: `wine-11.0`). The native test unixlib is Mach-O x86-64,
running inside that Wine process under Rosetta. It creates three IOSurface-backed
64×64 BGRA8 textures and shared events, transports IOSurface IDs and XPC event
handles, and receives the existing binary commands. **It never writes pixels.**

The Windows DLL requires DXMT's actual `IDXMTNativeDevice2`, imports those exact
native allocations into D3D11, writes through `UpdateSubresource`, signals the
imported `ID3D11Fence`, and flushes the D3D11 context. Native allocation followed
by D3D11 wrapping replaces the retired Basalt producer's texture-export route;
the Windows process still performs every tested producer write and normal
completion signal. The installed matching D3D11/DXGI/WineMetal binaries are
copied beside the private executable; the existing runtime and Steam prefix are
not changed. Artifact hashes are recorded, rather than claiming the binaries
exactly match the dirty DXMT checkout (`d116ce4` plus unrelated local changes).

The ARM64 consumer is Monado's existing `tests_macos_gpu_sync` Vulkan/MoltenVK
fixture. It imports the surfaces and event, requires observed CPU readiness,
checks the imported Vulkan timeline value, and keeps the already-satisfied
TRANSFER-stage semaphore wait. It validates every GPU-readback pixel against
frame, image and producer identity before authorizing reuse. An unsatisfied
obligation never reaches that GPU submission. This tests the transport/ordering
contract; it does not rerun production compositor sampling or XPC service
quarantine policy, already covered by M1/M2.

The multi-queue case joins a D3D11-signalled internal event into a second native
Metal queue before signalling the exported event. Its negative control signals
on that second queue before the D3D11 write, and must detect incorrect pixels.
The trusted in-flight gate uses D3D11 `Wait` before the write. DXMT does not expose
its Metal command-buffer status to this interface: the Wine receipt explicitly
marks that status unavailable, and termination checks an outstanding exported
fence after D3D11 submission. It does not claim a sampled Metal buffer status.

All native helper/consumer timestamps use the same Darwin steady clock; no
Windows QPC timestamps are mixed into the durations. JSONL remains the shared
M1 result/assertion schema. The existing production Perfetto backend is
unchanged; these standalone fixtures do not claim a production cross-stack
Perfetto capture.

## Executed correctness results

- Original native compute producer: **16/16 scenarios PASS**. Final committed
  harness CTest rerun also passes **16/16** in **6.84 s**.
- Native CPU-pattern/upload control: **16/16 scenarios PASS**.
- Corrected Windows/DXMT producer: **32/32 runs PASS**, all sixteen scenarios
  twice, twelve frames per ordinary case. Fault cases intentionally stop early.
- Matched normal transfers: **4×720 frames PASS**, including repeated reuse of
  all three images and exact GPU-readback patterns.

The sixteen names are `normal`, `delayed`, `missing`, `future`, `nonmonotonic`,
`exit-before`, `exit-after`, `consumer-delay`, `multi-queue`, `broken-queue`,
`reuse`, `idle`, `cancel-callback`, `exit-inflight`, `consumer-gpu-delay`, and
`concurrent-producers`. Concurrent producers have distinct native process IDs
and independently verified frame patterns. The broken-queue test passes by
detecting corruption, not by treating the deliberately broken ordering as safe.
Missing/future signals and producer failure refuse unverified GPU work; actual
consumer GPU delay excludes reuse until completion.

Initial runs exposed test packaging/startup problems and two harness mismatches:
the non-monotonic rejection receipt needed the native zero-commit marker, and
the first producer's command-read deadline needed to cover a second Wine
startup. The first matrix was **27/32**, with those failures retained in its raw
logs; it is not counted as passing. The corrected matrix passes all 32. Startup
has a finite 60 s Wine deadline, the producer's initial command wait is 65 s,
and the outer runner bounds each case to 160 s. Native activation keeps its
original 5 s deadline. Failed activation now closes descriptors and removes
its launchd registration instead of leaving a restartable test service.

Correctness matrices ran during the incremental Chromium build and include
thermal state 1. Their timings are not used for performance conclusions.

## Matched low-load timing

A = native Metal CPU-pattern/upload, B = Windows D3D11/DXMT upload. Both generate
the same pattern and perform GPU uploads; comparing against only the native
compute shader would confound producer workload. Allocation, D3D11 translation,
queueing and transport implementations still differ, so the result is an
observed stack cost, not an isolated cost for Wine or one synchronization call.

After the browser build finished, A–B–B–A used the same M5 GPU, 64×64 BGRA8,
three images, 720 frames per run, 8,333 µs intervals and a 6,000 µs readiness
margin. Preflight/start/end thermal state was **Nominal (0)** in every run;
continuous thermal sampling was not collected. AC power, battery charged,
Low Power Mode off; Game Mode was not requested. Frequency/power counters were
not collected. The native consumer and its readback/empty-wait controls are
identical in all four runs.

| Run | Readiness p50 µs | Vulkan submit p50/p95 µs | GPU-completed handoff p50 µs | Missed initial cutoff |
| --- | ---: | ---: | ---: | ---: |
| A1 native | 669.416 | 59.084 / 77.125 | 1411.334 | 0/720 |
| B1 Wine | 802.750 | 63.000 / 85.500 | 1233.083 | 5/720 |
| B2 Wine | 806.500 | 64.583 / 90.542 | 1231.459 | 5/720 |
| A2 native | 671.375 | 65.667 / 86.416 | 1372.000 | 1/720 |

Readiness is command obligation to CPU-observed completion, including pattern
generation, upload, transport and scheduling. Its Wine median is about **134 µs
higher** averaged across the two pairs. The complete handoff medians move in the
opposite direction; this does not establish a causal improvement or physical
presentation latency. Missed initial cutoffs defer the selected presentation
target and retain ownership; every frame completes correctly.

Median empty already-signalled wait submissions cost 15.417–16.041 µs, versus
14.667–15.375 µs without a wait: the difference between each run's medians is
0.625–0.750 µs. The active copy/readback submit is a different workload. Four
short samples do not establish a statistical tail non-regression guarantee.

## Exit criteria and limits

| Criterion | Result | Evidence |
| --- | --- | --- |
| M4.1 Genuine Windows producer | PASS | PE32+ x86-64; actual D3D11 device and required DXMT interface; Windows writes/signals. |
| M4.2 Native object transport | PASS | Exact imported IOSurface allocations, XPC shared-event handles and matching GPU registry. |
| M4.3 Readiness/memory ordering | PASS | CPU gate, imported Vulkan timeline verification, retained already-signalled wait and exact GPU pixel checks. |
| M4.4 Applicable native faults | PASS | All sixteen scenarios twice, including negative queue control and corrected rejection protocol. |
| M4.5 Failure/reuse | PASS | Pending/completed producer termination, consumer GPU delay, callback retirement, repeated image reuse and concurrent attribution. |
| M4.6 Measured overhead | PASS | Nominal-thermal A–B–B–A, matched upload control, all 2,880 transfers checked. |

These are guarantees within the tested synthetic contract. Production Wine
OpenXR/service failure recovery, physical presentation, SteamVR keyed mutexes,
extended cross-stack stress, 90 Hz, and pathological GPU/driver resets remain
unverified here. The prior native device-loss startup remains an unresolved
observation. M5 should carry these limits forward rather than treating this
fixture as evidence for those routes.

## Reproduction and raw evidence

Use the existing checkouts; no DXMT or Wine rebuild/deployment is required:

```sh
cmake --build build/arm64 --target tests_macos_gpu_sync --parallel 4
GPU_SYNC_WINE_BUILD=/tmp/m4-wine-gpu-sync \
GPU_SYNC_WINE_RUNTIME="$HOME/Code/wine-crossover/runtime" \
GPU_SYNC_WINE_SOURCE="$HOME/Code/wine-crossover/sources/wine" \
GPU_SYNC_DXMT_SOURCE="$HOME/Code/dxmt" \
zsh scripts/macos/build-wine-gpu-sync.zsh
python3 scripts/macos/run-wine-gpu-sync.py \
  --artifacts /tmp/m4-wine-gpu-sync \
  --wine-runtime "$HOME/Code/wine-crossover/runtime" \
  --output /tmp/m4-wine-corrected-matrix --frames 12 --repeats 2
```

The runner uses a private prefix under the artifact directory and stops only
that prefix's Wine server. Both artifacts and raw results must stay outside the
repository. GPU/XPC/launchd tests require ordinary host access, not the restricted
tool sandbox. For the matched native upload path, set
`GPU_SYNC_PRODUCER_UPLOAD=1`; the default remains the original compute producer.
For a direct Wine run after prefix initialization, also set
`GPU_SYNC_WINE_LAUNCHER=/tmp/m4-wine-gpu-sync/launch.zsh`, then run
`build/arm64/tests/tests_macos_gpu_sync normal 720`. Alternating native/Wine/Wine/
native with identical options reproduces the comparison; check thermal state
with `build/arm64/tests/tests_macos_service_gpu_sync telemetry` first.

Local raw evidence (untracked): `/tmp/m4-native-regression`,
`/tmp/m4-native-upload-regression`, `/tmp/m4-wine-matrix` (initial failures),
`/tmp/m4-wine-corrected-matrix` (32/32), and `/tmp/m4-matched-comparison`.
The corrected matrix records Wine version and PE/native/DXMT artifact hashes;
the comparison records the native probe hash, workload and power state. No raw
recordings or generated binaries are committed.
