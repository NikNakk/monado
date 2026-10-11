<!-- Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0 -->
# Native macOS GPU synchronization: M1–M2 completion, 2026-10-11

This report supersedes the remaining native synchronization criteria in the
[first checkpoint](macos-gpu-sync-first-run.md),
[service follow-up](macos-gpu-sync-service-tests.md), and
[headset comparison](macos-gpu-sync-headset-comparison.md).
Chromium, Wine/DXMT and wider presentation tuning remain outside this run.

Implementation commits: **`bcd3a4458`**, **`7d8bf13d7`**, **`99bd947ec`**, on `codex/macos-gpu-sync-harness`.
Existing unrelated integration edits remain uncommitted and are preserved.
A separate build of the scoped production/index variants succeeded, without
relying on the unrelated multi-compositor modifications. The full integration
checkout is the tested configuration; its exact diff and source variants are
recorded with the evidence.

Evidence: [macos-gpu-sync-completion-20261011](results/macos-gpu-sync-completion-20261011/).

## What changed to close the gaps

**Readiness policy.** Producer GPU readiness must be observed by predicted
presentation minus **6 ms**, configurable through
`XRT_MACOS_FRAME_READINESS_MARGIN_US` (0–100000 µs). A completed frame that misses
that cutoff remains owned and can become eligible on a subsequent tick. The
previous delivered frame remains available for reprojection. The deterministic
policy test executes production scheduling and verifies the frame identity
survives deferral. Linux scheduling is unchanged.

The producer contract remains write-before-signal on a monotonic event, with
explicit queue joins when writes and signalling use different queues. The
consumer must observe the selected expected value on the CPU, then preserve
that imported Vulkan wait for GPU memory ordering. It must observe consumer
completion before granting image reuse. A producer that signals before writing
violates the contract: the broken-queue fixture detects corrupt pixels; Monado
cannot infer missing producer queue ordering from the signal alone.

An outstanding producer obligation still expires after **500 ms** by default,
measured from submission, rather than from idle time. Failed/timed-out waits
never mark the frame GPU complete. Worker shutdown checks cancellation while
waiting for a scheduled slot; completed-slot backpressure also has a finite
500 ms replacement deadline. Replacement of an older completed slot does not
pretend that a missing producer finished.

**Pre-allocation budgets.** Each client has a default **512 MiB / 64 swapchain**
budget; the service has a **1 GiB / 256 swapchain** tracked-storage bound.
Admission reserves conservative image storage before create/import, including
supplied native storage sizes and checked dimensions/overflow. Successful
allocation reconciles the reservation against Vulkan memory requirements and
transfers the charge to the tracker. Failed allocation rolls back. Quarantine
keeps the client's charge and budget object alive after client destruction;
only observed producer and consumer completion permits reclamation/refund.
Admission polls reclaimable quarantines too, allowing replacement after the
last healthy client has ended. Object generations have unique log IDs so
allocator address reuse cannot confuse recovery evidence.

`XRT_MACOS_GPU_CLIENT_MB` accepts 1–1024, default 512;
`XRT_MACOS_GPU_CLIENT_OBJECTS` and `XRT_MACOS_GPU_GLOBAL_OBJECTS` accept 1–256,
default 64/256. These are bounded policy controls, not synchronization bypasses.
A client's quota cannot be raised beyond the global bound. Safe completed
objects queued for the ordinary garbage collector are no longer quarantined;
these limits govern admitted tracked storage, not all compositor/system memory.

**Teardown and failure ownership.** Client destruction stops its worker and
quarantines unresolved producer/consumer references without waiting for GPU
completion. A client-supplied unlimited consumer wait is capped at 500 ms and returns
timeout without releasing ownership. Known-completed native images skip the redundant device-idle wait
when garbage-collected. Final compositor shutdown stops display-link and
passthrough inputs, retires pending presentation jobs inside a bounded drain,
and includes the CADisplayLink thread join in the shared 500 ms deadline for
presentation/callback groups and 500 ms for the
last renderer fence. If completion is unproven, the entire compositor/device,
GPU children and callback state are retained until process exit. Destroying the
Vulkan device while those child resources remain referenced is forbidden.
Failed Vulkan/Metal presentation completion retains its source image rather
than authorizing reuse, and forces final retention.

This does not promise bounded driver calls or recovery from a physically
wedged GPU. Existing driver-level device/queue destruction and normal-frame
fence operations remain driver-dependent. Client-controlled *unverified future*
signals never reach those GPU dependencies. The macOS physical and debug-image
targets are non-deferred; the generic deferred-surface lifecycle is not exercised
by this port. Permanent hardware device loss is tested through Vulkan mocks,
not by crashing/resetting the real GPU.

## Validation

- Final native integration suite: **70/70 PASS**, 55.81 s, after the last
  teardown change. Final scoped policy/lifetime/service checks also passed
  **5/5**, 27.78 s.
- New real-GPU in-flight producer exit, delayed consumer GPU completion and
  two concurrent producers: **30/30 repeated runs PASS**.
- Sustained concurrent GPU stress: **24,000 transfers PASS in 92.165 s**,
  two producers, 1024×1024 textures, 64 repeated GPU copies per transfer,
  all pixel patterns/producer attribution verified before reuse.
  Thermal state was nominal (0) throughout; actual thermal throttling was not
  observed. No CPU/GPU frequency or privileged power counters were collected.
- Readiness-cutoff, producer/consumer lifetime, allocation overflow/caps and
  mocked device-loss checks pass. Device loss never authorizes reuse or free.
- Real service missing signal: session loss **525.293 ms**, 24 fresh healthy
  latches during the fault, 50 replacement latches, retained failed storage,
  one service start. Disconnect: replacement first frame **197.137 ms**,
  11 fresh healthy latches during recovery, 55 replacement latches.
- Real service budget exhaustion: per-client cap 2, global cap 8, seven stalled
  clients retained; four pre-import rejections, 319 healthy fresh latches during
  failures, 52 replacement latches after the healthy client ended. One service
  start, peak eight tracked objects, no missing producer value submitted.

The in-flight exit fixture submits real Metal work behind a separate trusted
test gate, verifies GPU completion is still pending, kills the producer, and
releases that gate. The consumer either observes the real producer signal and
checks GPU pixels, or refuses consumption and retains resources until exit.
It never invents a producer completion. The consumer-delay fixture submits
real Vulkan work behind a trusted test-only semaphore, verifies zero-time fence
polling is incomplete, and refuses reuse until an independent bounded release
and actual fence completion. These trusted fixture gates are not production
client dependencies.

Two early large-texture stress attempts failed because launchd producers did
not inherit the requested texture size. Passing that setting explicitly in the
producer plist fixed the harness; corrected stress and repeated tests pass.
Failures and the corrected results are retained.

Final-headset attempts are **NOT VERIFIED**: the first reported the PS VR2
display asleep and returned only zero physical presentation timestamps; after
an activity wake, macOS no longer listed the PS VR2. The runner refused to count
these as successful physical presentation. The installed service was restored.
The runner now rejects offscreen fallback immediately when `--headset` is
requested. After the user powered on the headset, it was detected at 4000×2040 / 120 Hz,
but all 242 presentation timestamps remained zero. A read-only
`CGSessionCopyCurrentDictionary` check confirmed the Mac was locked. Unlock
the Mac before repeating physical checks; no authentication bypass was attempted.
Earlier physical missing/disconnect recovery evidence remains valid for its
recorded source versions. No headset-worn visual judgement has been obtained.

## Performance and interpretation

The prior native satisfied-wait controls measured +0.499 / +0.541 µs
(+3.42% / +3.74%) median CPU submission overhead, with no shared-texture read in
the no-wait control. The original production offscreen A–B–B–A comparison
measured +2.5 µs (+1.67%) median Vulkan CPU submission cost; the mandatory
already-satisfied wait and checks preserve cross-API memory ordering.

The final-policy A–B–B–A comparison uses `fca1f399d` plus the same initial
integration overrides as A, and `bcd3a4458` plus those integration changes as B.
Eleven production source/header files differ; full variants, source hashes,
working diff and build logs are recorded in `offscreen-comparison`. Four runs
submit 1800 normal frames each (~30 s), simulated 60 Hz HMD, 1280×720 real Vulkan
output and the same 64×64 Metal quad. Thirty warmup frames are excluded.
Readbacks/per-image logs are disabled, tracing is fully buffered and identical,
Game Mode is not requested. Thermal state remains nominal and Low Power Mode
is off in every run; startup power/background-load snapshots are retained.

Microseconds, p50 / p95 / p99 / max:

| Metric | A0 | B0 | B1 | A1 |
|---|---|---|---|---|
| xrEndFrame | 133 / 190 / 243 / 513 | 134 / 203 / 263 / 504 | 133 / 200 / 254 / 503 | 133 / 198 / 240 / 412 |
| Readiness observation | 476 / 625 / 755 / 911 | 459 / 624 / 759 / 982 | 451 / 620 / 748 / 941 | 446 / 618 / 765 / 966 |
| Vulkan CPU submission | 113 / 160 / 194 / 256 | 114 / 155 / 192 / 244 | 116 / 159 / 188 / 280 | 114 / 155 / 189 / 246 |
| Entry to fresh latch | 14263 / 16393 / 17024 / 17322 | 14493 / 16387 / 17111 / 17402 | 14563 / 16353 / 17045 / 17286 | 14494 / 16384 / 17096 / 17287 |

All runs deliver 59.995–60.007 client Hz. Averaging per-run medians, Vulkan
submission rises 113.5→115 µs, **+1.5 µs (+1.32%)**; xrEndFrame 133→133.5 µs,
**+0.5 µs (+0.38%)**; readiness 461→455 µs, **−6 µs (−1.30%)**;
entry-to-latch 14378.5→14528 µs, **+149.5 µs (+1.04%)**.
These small observed differences include scheduling noise and do not isolate
one check's causal cost. They are proportionate to the explicit readiness and
ownership checks. Allocation quotas and final drains are outside normal frame
submission; no blocking wait was added to that path. The final finite consumer
wait cap changes the maximum requested timeout, not the completed normal path.

The earlier matched physical comparison remains separately reported: throughput
119.578→119.526 Hz (**−0.052 Hz / −0.044%**), averaged median entry-to-display
27.35→26.98 ms (**−0.37 ms / −1.35%**), and gaps >12.5 ms 33/14128→39/14124
(**0.234%→0.276%, +0.042 percentage points**). Maximum gap 25.028→33.367 ms
(**+8.339 ms / +33.3%**). Rare-gap causality remains unresolved under varying
background activity. That comparison predates the final policy changes and is
not presented as their physical measurement. Final physical A–B–B–A awaits the
Mac being unlocked; offscreen latch timing is not physical display latency.

M2.10 requires baseline/modified measurements and quantitative reporting and
justification of regressions. Earlier reports used NOT VERIFIED for the stronger
claim of strict tail non-regression. That stronger claim remains unverified;
it is not a prerequisite for completing the stated measurement criterion.
Measured CPU costs and physical gaps are reported, rather than represented as
proof of universal performance equivalence. The default 6 ms cutoff deliberately
trades a possible later delivery tick for a bounded readiness decision; its
ownership and ordering protections must not be removed to recover unsafe latency.

## Exit criteria

| Criterion | Status | Evidence / qualification |
|---|---|---|
| M1.1 | PASS | Genuine Metal producer processes and Vulkan consumer GPU reads. |
| M1.2 | PASS | Exact observed expected producer value before consuming submission. |
| M1.3 | PASS | Mandatory imported, already-signalled Vulkan timeline wait. |
| M1.4 | PASS | GPU-written full patterns and producer attribution verified. |
| M1.5 | PASS | Missing/future values excluded; delayed completion safely deferred. |
| M1.6 | PASS | Real delayed consumer GPU fence prevents reuse; independent lifetime tests. |
| M1.7 | PASS | In-flight process kill, real late callback, finite timeout retention; permanent GPU loss mocked. |
| M1.8 | PASS | Structured deterministic cases, 30 repeats, sustained concurrent stress. |
| M1.9 | PASS | CTest, XPC/IOSurface fixtures, production scheduler/renderer reused. |
| M1.10 | PASS | Native latency and paired satisfied-wait overhead recorded. |
| M2.1 | PASS | Real service Metal OpenXR → Vulkan GPU rendering. |
| M2.2 | PASS | Unverified future client values never admitted as compositor GPU waits. |
| M2.3 | PASS | Errors/timeouts never authorize GPU completion or image reuse. |
| M2.4 | PASS | No added frame-loop blocking; finite readiness/backpressure/drain failures. |
| M2.5 | PASS | Producer plus consumer ownership retained, including pending renderer claims. |
| M2.6 | PASS | Bounded client worker teardown, asynchronous quarantine, whole-device retention on failed final drain. |
| M2.7 | PASS | Per-client/global pre-import reservations, actual-size charges, real service cap exhaustion. |
| M2.8 | PASS | Replacement renders without restarting the service, including after cap exhaustion. |
| M2.9 | PASS | Healthy concurrent GPU composition through faults; earlier physical presentation evidence retained. |
| M2.10 | PASS | Baseline/modified normal-path costs and physical tail differences reported; strict tail equivalence unproven. |
| M2.11 | PASS | Available deterministic native/mock/service tests pass; hardware limitations explicitly listed. |
| M2.12 | PASS | Protocol, scoped commits, raw traces, budgets, recovery and measurements documented. |

Additional validation: final-code physical presentation **NOT VERIFIED** until
the Mac is unlocked; moving-head comfort **NOT VERIFIED**; real permanent GPU
loss **SKIP**; actual thermally throttled execution **NOT VERIFIED**; Linux CI
**NOT RUN** locally. These limits are not substituted with mock/offscreen claims.

## Reproduction and next milestones

Run `cmake --build build/arm64 --parallel`, then
`ctest --test-dir build/arm64 --output-on-failure` with normal host GPU/XPC access.
The default suite includes the native GPU faults, production readiness policy
and real headset-free service missing/disconnect/budget tests.

```sh
python3 scripts/macos/run-gpu-sync.py --output <new-directory> \
  --scenario exit-inflight --scenario consumer-gpu-delay \
  --scenario concurrent-producers --repeats 10 --frames 24 --keep-awake
GPU_SYNC_TEXTURE_SIZE=1024 GPU_SYNC_COPY_REPEATS=64 \
  python3 scripts/macos/run-gpu-sync.py --output <new-directory> \
  --scenario concurrent-producers --frames 12000 --timeout 240 --keep-awake
python3 scripts/macos/run-gpu-sync-service.py --budget-suite --output <new-directory>
```

For headset tests, ensure the display is awake/listed and exclusive USB ownership,
then temporarily stop the installed service and restore it in a `finally` block:
`run-gpu-sync-service.py --headset --suite --fault missing|disconnect`.
The recorded orchestration includes restoration evidence. Benchmarks disable
readback/per-image logging, use identical buffered tracing/configuration,
record power/background load/thermal state, and alternate A–B–B–A. Do not compare
sustained stress latency with nominal low-load latency.

Stop native milestone implementation here. M3 should test Chromium's real
SharedImage/Dawn/ANGLE ordering, pause/buffering and restart through this validated
service contract. M4 should then extend the D3D11/DXMT producer and test x86-64/Wine
imports, queue joins and exit recovery. Both require separate authorization.
Keep moving-head headset judgement, heavier applications, client-hosted Game Mode
and general presentation tuning as separately measured follow-ups.
