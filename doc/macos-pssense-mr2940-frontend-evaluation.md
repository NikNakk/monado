# PS Sense: upstream's optical front end (MR 2940) against M1, and recording for evaluation

Branch `claude/pssense-mr2940-evaluation`, on top of `codex/pssense-upstream-fusion-evaluation`. The upstream side is
on `claude/pssense-upstream-frontend-replay`, which branches from `macos-upstream-sync-2026-10`. That branch is upstream
`main` with MR 2940 merged into this fork.

This note covers the optical front end. The fusion back ends are in `doc/macos-pssense-upstream-fusion-evaluation.md`.

## Status (1 October 2026)

- **The tooling is built and checked end to end on synthetic recordings with exact ground truth.** The upstream
  tracker runs on a recording in its own tree. Its poses are scored beside M1's by one evaluator.
- **All 27 usable Mac recordings have been run through it.** See "Results on recordings". The earlier conclusion
  holds and is firmer: M1's poses are more consistent than upstream's on every neutral measure, at about 1/70 of the
  cost.
- **Upstream solves more exposures than the replay's M1 loop: 63% against 56%.** M1's coverage gate is not the cause.
  It accounts for 3–4% of the exposures only upstream solved. About half of the gap is the replay's M1 loop losing
  lock in fast motion, which the shipped tracker with the EKF prior does not. The rest is mostly the right controller
  failing M1's RMS limits, and poses in which one front end has fitted the wrong controller.
- **No change to M1's coverage gate is proposed.** Two follow-ups are, under "Proposed follow-ups". The second is not
  implemented.
- **The first follow-up is measured.** See "The LED model's fit". The right controller is not a special case. Both
  rings fit the cameras better about 1% larger than the model, and a correction fitted on one day's sessions carries
  over to the next day's: in the shipped path the right controller gains 10% more poses and its RMS falls from 0.53 px
  to 0.38 px. The driver applies the correction with `PSSENSE_LED_CORRECTION=1`. It is off by default and has not
  been run on the headset.
- **The recording format now carries what this evaluation lacked** (packet 5 extension records, below). New
  recordings need no extra steps beyond the static markers, which are optional.

## Results on recordings (1 October 2026)

Commits tested:

- `claude/pssense-mr2940-evaluation` at `8f9402395` (`constellation_replay`);
- `claude/pssense-upstream-frontend-replay` at `047371d0f` (`constellation_upstream_replay`).

Both were built RelWithDebInfo from clean trees and run on an Apple M5, with the commands under "Running it on the Mac
recordings", over the sessions in `~/Code/psvr2-datasets/sessions/` from `20260924-233215` on. The outputs are in
`~/Code/psvr2-datasets/experiments/20261001-mr2940-frontend/`. The 14 earlier sessions have no `constellation.ctd`. Of
the 31 that were run:

- two have no `constellation.ctd` either (`20260925-002058` and `20260926-014455`, both aborted starts);
- two have no solves by either front end (`20260925-213220` and `20260925-213249`, the handoff tests);
- 27 are reported here. Only the last two carry IMU samples, so only they have gyro metrics.

No recording has annotated static intervals or ground truth.

### The two sessions with IMU data

Values are left / right controller.

`20260926-010135-imu-capture` (5394 exposures, 90 s):

| | M1 | upstream (MR 2940) |
|---|---|---|
| solved, % of exposures | 67.4 / 37.4 | 79.3 / 52.0 |
| solved, % of exposures either solved | 83.1 / 68.9 | 97.7 / 95.7 |
| own reprojection RMS, px, p50 | 0.405 / 0.529 | 0.137 / 0.258 |
| evaluator inliers, p50 | 22 / 21 | 21 / 20 |
| evaluator RMS, px, p50 | 0.412 / 0.528 | 0.623 / 1.103 |
| evaluator RMS, px, p95 | 0.633 / 0.866 | 1.081 / 1.847 |
| unsupported poses | 37 (1.02%) / 45 (2.23%) | 103 (2.41%) / 169 (6.02%) |
| rotation vs gyro, deg, p50 | 0.182 / 0.113 | 0.376 / 0.211 |
| rotation vs gyro, deg, p95 | 0.737 / 0.613 | 3.427 / 3.402 |
| rotation jumps (> 10°) | 0 / 0 | 74 / 59 |
| position jumps (> 30 mm) | 0 / 0 | 81 / 77 |
| step while still, mm, RMS | 1.99 / 2.03 | 33.34 / 646.01 |
| step while still, mm, p95 | 3.82 / 3.66 | 10.73 / 200.87 |
| step while still, deg, RMS | 0.50 / 0.51 | 9.31 / 28.32 |
| solved by this front end only | 99 / 125 | 738 / 912 |
| cost per exposure, µs, mean / p95 / max | 135 / 509 / 1578 | 9366 / 35903 / 162768 |
| exposures costing more than the 16.7 ms interval | 0.0% | 21.4% |

Where both solved, the poses differ by 2.55 / 2.56 mm and 0.81 / 0.76° at p50, and by 9.82 / 62.07 mm and
3.31 / 53.45° at p95.

`20260926-014505-filter-live` (5393 exposures, 90 s):

| | M1 | upstream (MR 2940) |
|---|---|---|
| solved, % of exposures | 36.9 / 74.5 | 43.1 / 84.3 |
| solved, % of exposures either solved | 81.0 / 85.1 | 94.6 / 96.2 |
| own reprojection RMS, px, p50 | 0.386 / 0.470 | 0.123 / 0.216 |
| evaluator inliers, p50 | 23 / 23 | 23 / 22 |
| evaluator RMS, px, p50 | 0.395 / 0.469 | 0.638 / 0.838 |
| evaluator RMS, px, p95 | 0.852 / 0.767 | 1.433 / 1.609 |
| unsupported poses | 2 (0.10%) / 13 (0.32%) | 80 (3.44%) / 145 (3.19%) |
| rotation vs gyro, deg, p50 | 0.120 / 0.106 | 0.260 / 0.220 |
| rotation vs gyro, deg, p95 | 1.188 / 0.496 | 2.259 / 1.785 |
| rotation jumps (> 10°) | 0 / 0 | 24 / 28 |
| position jumps (> 30 mm) | 0 / 0 | 28 / 47 |
| step while still, mm, RMS | 2.47 / 0.82 | 2.44 / 39.99 |
| step while still, mm, p95 | 6.22 / 1.97 | 5.27 / 12.17 |
| step while still, deg, RMS | 0.88 / 0.33 | 1.26 / 10.47 |
| solved by this front end only | 132 / 180 | 468 / 705 |
| cost per exposure, µs, mean / p95 / max | 114 / 383 / 1278 | 8543 / 31078 / 82890 |
| exposures costing more than the 16.7 ms interval | 0.0% | 22.4% |

Where both solved, the poses differ by 2.50 / 2.36 mm and 0.74 / 0.73° at p50, and by 12.15 / 4.27 mm and
8.53 / 2.11° at p95.

### All 27 sessions

Each cell is M1 / upstream. `L` is device 0 and `R` is device 1. Cost and the share over the interval are per session,
for all devices.

| session | ctrl | solved % | only M1 / only up | eval RMS px p50 | eval RMS px p95 | unsupported % | position jumps | both: mm / deg p50 | cost µs mean | up > 16.7 ms |
|---|---|---|---|---|---|---|---|---|---|---|
| 0924-2332 replay-check-left | L | 53.4 / 61.6 | 20 / 167 | 0.59 / 1.17 | 0.80 / 1.80 | 0.7 / 0.7 | 0 / 1 | 1.96 / 1.52 | 64 / 3295 | 0.3% |
| 0924-2336 replay-both-static | L | 75.9 / 75.9 | 0 / 0 | 0.30 / 0.37 | 0.35 / 0.58 | 0.0 / 0.0 | 0 / 0 | 0.82 / 0.93 | 70 / 6417 | 0.1% |
|  | R | 52.3 / 52.3 | 0 / 0 | 0.56 / 0.64 | 0.67 / 1.27 | 0.0 / 0.0 | 0 / 0 | 0.92 / 1.59 |  |  |
| 0924-2339 both-static-right-first | L | 48.1 / 47.5 | 19 / 4 | 0.30 / 0.51 | 0.37 / 1.07 | 2.5 / 2.2 | 0 / 0 | 1.00 / 1.29 | 59 / 10548 | 25.7% |
|  | R | 58.2 / 58.0 | 27 / 20 | 0.52 / 0.77 | 0.63 / 1.19 | 1.3 / 2.1 | 0 / 6 | 1.13 / 1.60 |  |  |
| 0924-2340 replay-left-slow | L | 80.5 / 80.8 | 0 / 10 | 0.50 / 0.69 | 0.75 / 1.38 | 0.1 / 0.6 | 0 / 0 | 1.09 / 0.80 | 59 / 636 | 0.0% |
| 0924-2344 replay-both-grip | L | 13.8 / 27.2 | 30 / 632 | 0.42 / 1.44 | 0.77 / 1.64 | 1.9 / 0.6 | 0 / 0 | 1.68 / 0.64 | 109 / 3928 | 5.1% |
|  | R | 81.0 / 86.9 | 6 / 274 | 0.68 / 1.04 | 1.00 / 1.59 | 0.0 / 0.1 | 0 / 0 | 1.71 / 0.89 |  |  |
| 0924-2346 replay-left-fast | L | 37.3 / 38.5 | 23 / 66 | 0.49 / 0.62 | 0.70 / 1.20 | 2.5 / 5.4 | 0 / 5 | 1.28 / 0.99 | 32 / 796 | 0.6% |
| 0925-0000 replay-both-grip | L | 59.6 / 68.5 | 76 / 476 | 0.46 / 0.73 | 0.84 / 1.69 | 2.4 / 1.2 | 0 / 54 | 2.57 / 0.78 | 105 / 7646 | 18.3% |
|  | R | 44.6 / 52.5 | 143 / 501 | 0.66 / 0.92 | 0.93 / 1.59 | 0.2 / 2.3 | 0 / 221 | 2.13 / 0.88 |  |  |
| 0925-0002 replay-left-fast | L | 75.3 / 76.0 | 17 / 45 | 0.45 / 0.70 | 0.86 / 1.48 | 0.3 / 0.7 | 0 / 7 | 2.28 / 0.93 | 53 / 4933 | 7.6% |
|  | R | 0.0 / 14.3 | 0 / 516 | – / 1.44 | – / 1.94 | 0.0 / 75.2 | 0 / 30 | – / – |  |  |
| 0925-0004 replay-left-fast-2 | L | 71.6 / 73.9 | 50 / 132 | 0.50 / 0.72 | 0.75 / 1.29 | 1.0 / 1.9 | 0 / 5 | 2.43 / 1.00 | 53 / 1259 | 1.3% |
| 0925-0012 joint-both-grip | L | 66.9 / 67.4 | 10 / 34 | 0.40 / 0.60 | 0.59 / 1.19 | 0.5 / 1.4 | 0 / 18 | 1.52 / 0.69 | 106 / 7377 | 16.5% |
|  | R | 59.3 / 65.4 | 24 / 299 | 0.61 / 1.04 | 0.92 / 1.67 | 0.0 / 0.5 | 0 / 26 | 1.66 / 0.82 |  |  |
| 0925-0015 joint-both-grip-2 | L | 62.8 / 67.9 | 1 / 231 | 0.50 / 0.79 | 0.71 / 1.60 | 0.0 / 2.9 | 0 / 24 | 2.78 / 0.93 | 106 / 6270 | 13.2% |
|  | R | 19.3 / 35.5 | 96 / 823 | 0.55 / 1.36 | 0.90 / 1.76 | 0.3 / 2.0 | 0 / 30 | 2.61 / 0.72 |  |  |
| 0925-0021 joint-both-grip-3 | L | 49.0 / 53.2 | 217 / 406 | 0.48 / 0.88 | 0.78 / 1.71 | 11.2 / 0.5 | 0 / 8 | 2.53 / 0.90 | 97 / 9004 | 22.4% |
|  | R | 70.5 / 75.6 | 153 / 382 | 0.73 / 0.96 | 0.98 / 1.59 | 0.5 / 4.6 | 0 / 12 | 2.05 / 0.81 |  |  |
| 0925-0833 joint-both-grip-newcal | L | 35.9 / 36.7 | 101 / 133 | 0.48 / 0.90 | 0.75 / 1.58 | 0.1 / 2.1 | 0 / 7 | 3.41 / 0.79 | 89 / 6886 | 14.9% |
|  | R | 79.2 / 83.4 | 0 / 173 | 0.56 / 1.08 | 0.81 / 1.72 | 0.0 / 0.3 | 0 / 0 | 2.81 / 0.64 |  |  |
| 0925-2047 joint-both-ledblobs | L | 26.5 / 27.4 | 19 / 49 | 0.66 / 1.22 | 0.89 / 1.78 | 0.0 / 0.0 | 0 / 0 | 2.69 / 0.75 | 122 / 9285 | 25.4% |
|  | R | 68.7 / 79.7 | 0 / 354 | 0.61 / 1.16 | 0.92 / 1.84 | 1.6 / 2.9 | 0 / 4 | 2.31 / 0.65 |  |  |
| 0925-2054 joint-both-ledblobs-rel | L | 56.5 / 68.8 | 47 / 596 | 0.59 / 1.03 | 0.88 / 1.71 | 0.0 / 1.5 | 0 / 5 | 2.05 / 0.65 | 182 / 12379 | 29.1% |
|  | R | 63.7 / 73.6 | 145 / 589 | 0.66 / 1.38 | 0.92 / 1.83 | 0.2 / 2.6 | 0 / 18 | 2.75 / 0.67 |  |  |
| 0925-2059 joint-both-coverage | L | 1.6 / 0.0 | 72 / 2 | 0.67 / 1.75 | 0.70 / 1.75 | 0.0 / 100.0 | 0 / 0 | – / – | 150 / 5640 | 10.3% |
|  | R | 62.8 / 78.8 | 8 / 726 | 0.48 / 1.08 | 0.88 / 1.80 | 0.0 / 0.9 | 0 / 2 | 2.55 / 0.68 |  |  |
| 0925-2126 joint-both-coverage | L | 50.3 / 50.4 | 72 / 75 | 0.51 / 0.90 | 0.73 / 1.47 | 0.0 / 0.3 | 0 / 2 | 2.36 / 0.76 | 104 / 7174 | 13.9% |
|  | R | 80.7 / 85.1 | 16 / 213 | 0.54 / 1.15 | 0.84 / 1.74 | 0.2 / 0.5 | 0 / 1 | 2.92 / 0.89 |  |  |
| 0925-2325 rotation-sweep | L | 70.5 / 75.4 | 69 / 334 | 0.63 / 1.08 | 0.94 / 1.74 | 0.0 / 0.9 | 0 / 7 | 2.03 / 1.17 | 135 / 12525 | 26.8% |
|  | R | 77.5 / 81.3 | 128 / 337 | 0.54 / 1.11 | 0.86 / 1.79 | 0.1 / 2.3 | 0 / 33 | 2.85 / 1.09 |  |  |
| 0925-2341 joint-both-hinted | L | 68.8 / 72.0 | 248 / 395 | 0.43 / 0.66 | 0.78 / 1.29 | 0.2 / 1.5 | 0 / 25 | 2.14 / 0.67 | 120 / 12750 | 29.8% |
|  | R | 75.4 / 86.9 | 67 / 585 | 0.54 / 1.06 | 0.80 / 1.81 | 0.0 / 1.0 | 0 / 28 | 3.03 / 0.70 |  |  |
| 0925-2346 head-motion | L | 38.3 / 43.9 | 445 / 749 | 0.40 / 0.57 | 0.77 / 1.53 | 0.4 / 3.3 | 0 / 92 | 2.84 / 0.95 | 124 / 7815 | 16.7% |
|  | R | 62.5 / 87.6 | 22 / 1378 | 0.44 / 0.69 | 0.72 / 1.44 | 0.5 / 1.1 | 0 / 48 | 3.04 / 1.13 |  |  |
| 0926-0009 head-from-camera0 | L | 63.3 / 66.4 | 108 / 272 | 0.44 / 0.62 | 0.60 / 0.88 | 1.2 / 0.9 | 6 / 205 | 4.19 / 1.03 | 114 / 9021 | 21.4% |
|  | R | 60.9 / 69.7 | 111 / 589 | 0.46 / 0.66 | 0.77 / 1.23 | 7.7 / 12.4 | 1 / 277 | 3.95 / 1.38 |  |  |
| 0926-0022 world-head-motion | L | 56.6 / 69.2 | 305 / 983 | 0.44 / 0.83 | 0.67 / 1.97 | 4.5 / 2.5 | 0 / 82 | 4.15 / 1.05 | 102 / 7699 | 17.6% |
|  | R | 28.8 / 39.8 | 240 / 832 | 0.49 / 0.84 | 0.82 / 1.97 | 0.6 / 4.3 | 0 / 102 | 3.32 / 1.09 |  |  |
| 0926-0034 world-gyro-bias | L | 26.6 / 33.5 | 78 / 450 | 0.47 / 0.70 | 0.73 / 1.36 | 0.9 / 2.8 | 0 / 119 | 4.28 / 1.00 | 116 / 8067 | 20.1% |
|  | R | 66.5 / 79.6 | 17 / 720 | 0.48 / 0.79 | 0.81 / 1.74 | 0.6 / 2.0 | 0 / 74 | 2.83 / 0.98 |  |  |
| 0926-0044 world-steady-probes | L | 72.0 / 87.1 | 78 / 891 | 0.42 / 0.65 | 0.59 / 1.19 | 0.1 / 2.0 | 0 / 77 | 2.92 / 0.80 | 97 / 7251 | 17.8% |
|  | R | 36.9 / 39.7 | 346 / 494 | 0.53 / 0.92 | 0.80 / 1.69 | 0.4 / 3.4 | 0 / 102 | 2.78 / 0.78 |  |  |
| 0926-0048 world-confirm | L | 65.5 / 71.5 | 22 / 345 | 0.47 / 0.71 | 0.79 / 1.64 | 0.1 / 0.6 | 0 / 33 | 2.58 / 0.78 | 170 / 7076 | 15.6% |
|  | R | 82.3 / 92.8 | 23 / 586 | 0.53 / 0.98 | 0.89 / 1.82 | 0.3 / 1.5 | 0 / 31 | 2.90 / 0.83 |  |  |
| 0926-0101 imu-capture | L | 67.4 / 79.3 | 99 / 738 | 0.41 / 0.62 | 0.63 / 1.08 | 1.0 / 2.4 | 0 / 81 | 2.55 / 0.81 | 135 / 9366 | 21.4% |
|  | R | 37.4 / 52.0 | 125 / 912 | 0.53 / 1.10 | 0.87 / 1.85 | 2.2 / 6.0 | 0 / 77 | 2.56 / 0.76 |  |  |
| 0926-0145 filter-live | L | 36.9 / 43.1 | 132 / 468 | 0.40 / 0.64 | 0.85 / 1.43 | 0.1 / 3.4 | 0 / 28 | 2.50 / 0.74 | 114 / 8543 | 22.4% |
|  | R | 74.5 / 84.3 | 180 / 705 | 0.47 / 0.84 | 0.77 / 1.61 | 0.3 / 3.2 | 0 / 47 | 2.36 / 0.73 |  |  |

Pooled over the 27 sessions (223,262 controller-exposures):

| | M1 | upstream (MR 2940) |
|---|---|---|
| solved | 124,233 (55.6%) | 140,689 (63.0%) |
| solved by this front end only | 4,235 | 20,691 |
| unsupported poses | 1,175 (0.95%) | 3,323 (2.36%) |
| position jumps (> 30 mm) | 7 | 2,054 |
| cost per exposure, µs, mean, range over sessions | 32–182 | 636–12,750 |
| exposures over the 16.7 ms interval, range over sessions | 0% | 0–29.8% |

### What the tables show

- **Upstream is not as accurate as M1 on real data.** The neutral measures favour M1:
  - evaluator RMS is lower for M1 in every session and controller. At p50 it is 0.40–0.53 px for M1 and 0.62–1.10 px
    for upstream in the IMU sessions;
  - M1's rotation residual against the gyro is about half of upstream's at p50 (0.11–0.18° against 0.21–0.38°), and a
    half to a fifth at p95 (0.5–1.2° against 1.8–3.4°);
  - M1 has no rotation jumps and no position jumps in the IMU sessions. Upstream has 185 and 233;
  - upstream's step while still is dominated by outliers for three of the four controllers: 33–646 mm RMS against
    M1's 0.8–2.0 mm. For the fourth the two are equal (2.4 mm against 2.5 mm);
  - M1's unsupported share is lower pooled (0.95% against 2.36%), though not in every session.
- **The evaluator's RMS favours a joint solve by construction.** It scores a pose in every camera, which is what M1
  minimises and upstream does not. The gyro residual, the jumps and the still steps do not depend on that, and they
  agree with it.
- **Where both solve, they agree** to about 2.5 mm and 0.75° at p50, the same as on synthetic data.
- **Upstream costs about 70× more.** The median ratio of mean cost over the sessions is 70. In most two-controller
  sessions, 13–30% of exposures cost upstream more than the frame interval. M1's worst p95 in any session is 1 ms.
- **Upstream solves more exposures**: 63.0% against 55.6% pooled, and 64.7% against 54.1% in the IMU sessions. The
  next section looks at where.

### The exposures only upstream solved

The IMU sessions have 2,823 exposures that only upstream solved, out of 21,574 controller-exposures. Upstream's poses
there are mostly supported: 87% overall, with a gyro residual p50 of 0.2–0.3°. That alone would point to M1's gates.
The per-exposure M1 output (`constellation_replay --m1 --csv`) says which gate, and it is not coverage.

Each exposure is put in the first class that fits:

| what M1 did | exposures | share | upstream supported | upstream eval RMS px p50 | gyro deg p50 / p95 | shipped path, same pose |
|---|---|---|---|---|---|---|
| upstream's pose is on the other controller, and upstream has the wrong device | 268 | 9% | 51% | 1.10 | 1.96 / 12.89 | 0 |
| upstream's pose is on the other controller's M1 pose, and M1 has the wrong device | 187 | 7% | 90% | 0.62 | 0.45 / 11.84 | 134 |
| two devices at one place, not resolved | 292 | 10% | 98% | 0.59 | 0.33 / 3.95 | 172 |
| tracking solve rejected on coverage alone | 87 | 3% | 91% | 0.90 | 0.28 / 6.41 | 53 |
| tracking solve rejected on RMS, outliers or match count, with or without coverage | 774 | 27% | 96% | 0.78 | 0.18 / 1.98 | 635 |
| tracking solve found no matches | 307 | 11% | 95% | 1.03 | 0.22 / 1.46 | 258 |
| bootstrap found upstream's pose and rejected it | 506 | 18% | 93% | 1.60 | 0.19 / 1.68 | 66 |
| bootstrap found a different pose and rejected it | 84 | 3% | 77% | 0.44 | 0.26 / 35.10 | 44 |
| bootstrap found no candidate | 318 | 11% | 68% | 0.69 | 0.42 / 59.00 | 111 |
| total | 2,823 | | | | | 1,473 |

"Same pose" means within 15 mm of upstream's. "Shipped path" is `constellation_replay --tracker-filter`: the
live joint tracker with the EKF's prediction as its prior. Over all 27 sessions the shares are 9 / 3 / 9 / 4 / 17 / 5 /
28 / 3 / 21%, of 20,691 exposures.

**Coverage gate.** Coverage is the only failed gate in 87 tracking solves (3%), and 858 (4%) over all sessions.
Counting bootstrap attempts outside the collisions too, there are 115. In those, M1's rejected pose had a coverage of
0.35–0.68 at p50 per controller, well under the 0.80 limit, and it matched upstream's pose (within 15 mm and 5°) in
31. The synthetic result, where merged blobs put correct poses at 0.70–0.79 coverage, does not appear in the
recordings.

**Fast motion (about 40%).** The replay's M1 loop starts each solve from the last pose, with the IMU orientation and no
position prediction. Its share of upstream's solves falls with upstream's step from the previous exposure:

| upstream step, mm per exposure | 0–4 | 4–8 | 8–12 | 12–16 | 16–20 | over 20 |
|---|---|---|---|---|---|---|
| also solved by M1, IMU sessions | 95% | 89% | 82% | 69% | 35% | 29% |
| also solved by M1, all sessions | 92% | 89% | 83% | 68% | 42% | 35% |

16 mm per exposure is about 1 m/s. Upstream's step is 16.5 mm at p50 in the exposures where M1's tracking solve
failed, against 5.0 mm where both solved. M1 then fails three more times, drops to bootstrap and re-acquires: 208 of
the 522 runs of only-upstream exposures are exactly four exposures long, and hold 832 of the 2,823. M1's rejected
tracking poses sit 25–28 mm from upstream's at p50, with an RMS over 1 px and a coverage of 0.2–0.4. They are correctly
rejected: M1 had lost the pose.

The live tracker does not start from the last pose. It uses the driver's predicted position
(`t_constellation_tracker_joint.cpp`). Replayed with the EKF prior, it solves 946 of the 1,168 exposures in the three
tracking classes at upstream's pose, and 1,473 of the 2,823 in all. Its poses there are 2.3–3.2 mm from upstream's at
p50, the same as where M1 and upstream both solve. Per controller, in poses:

| | M1 loop | shipped path (`--tracker-filter`) | upstream |
|---|---|---|---|
| `010135` left / right | 3638 / 2020 | 4259 / 1973 | 4277 / 2807 |
| `014505` left / right | 1989 / 4020 | 2030 / 4222 | 2325 / 4545 |
| total, % of controller-exposures | 54.1% | 57.9% | 64.7% |

The shipped path's poses were not scored by the evaluator. The front-end comparison only scores M1 and imported
records.

**The right controller's RMS (18%).** In 506 exposures M1's bootstrap found upstream's pose (within 15 mm and 5°;
2.5–2.8 mm apart at p50) and rejected it. 499 of them are the right controller. The candidates have full coverage (0.8
or more in 495) and 19–22 matches over four cameras. They fail on RMS: 1.04 px at p50, against a bootstrap limit of
0.8 px and a tracking limit of 1.0 px. 216 are at or under 1.0 px and 399 at or under 1.2 px. The residual is spread
over all four cameras (0.86–1.15 px each), so the camera-dropout retry does not apply. Upstream's poses there score
1.6 px with the evaluator, and agree with the gyro (0.19° at p50). Over all sessions this is the largest class (5,824
exposures, 28%), and 4,290 of them are the right controller. The right controller also fits worse where M1 does solve:
0.47–0.53 px against the left's 0.40–0.41 px.

**Two devices at one place (26%).** In 747 exposures, upstream's pose is within 60 mm of the other controller's pose.
Real separations never fall under 190 mm in these sessions, so one of the two has the wrong device. The left and right
rings are near mirror images, and each model fits the other ring at 0.6–1.3 px. Where both front ends agree on a
controller's position (within 20 mm of each other) less than 0.3 s away, that settles which one is wrong:

| wrong-device poses | M1 | upstream |
|---|---|---|
| IMU sessions | 190 of 11,667 (1.6%) | 286 of 13,954 (2.0%) |
| all sessions | 699 of 124,233 (0.6%) | 2,079 of 140,689 (1.5%) |
| longest run, IMU sessions | 17 exposures (0.28 s) | 58 exposures (0.97 s) |

These are lower bounds. The unresolved cases are not counted. Two examples:

- `010135`, 46.38–46.65 s. The left controller moves at about 1.1 m/s. M1 loses it, and M1's right model bootstraps
  onto the left ring and follows it for 17 exposures at 0.4–0.95 px. Upstream tracks the left controller throughout.
- `014505`, 24.19–25.14 s. Both controllers are still and 220 mm apart. Upstream puts the right controller on the left
  ring at about 1.0 px for a second, about 13 mm from its own left pose.

**The evaluator does not see wrong-device poses.** It supports 88% of them, because a mirror-image fit puts most LEDs
within its 3 px gate. They are also smooth, so the jump counts miss them. The "unsupported" figures above therefore
understate both front ends' wrong poses. The replay's M1 loop has neither of the live tracker's defences against this:
the bootstrap contest between devices, and the three confirming solves before a new track's poses are used.

**Bunching.** The only-upstream exposures come in runs. The commonest length is four exposures, and 47% of the
exposures are in runs of more than ten. They are not tied to the controllers being close together: the controllers
were never closer than 190 mm. They follow fast motion, stretches where the right controller fits poorly, and
wrong-device episodes.

**The rest (14%).** Where M1's bootstrap found nothing or something else, upstream's poses are its weakest: 9–10
inliers at p50, 68–77% supported, and a gyro p95 of 35–59°. Outside the collisions, upstream had a pose from only one
camera in 56% of the only-upstream exposures, against 25% where both solved. M1's bootstrap needs two cameras.

Other things the tables show:

- M1 never solves the right controller in `0925-0002`. Upstream reports 516 right-controller poses there, 75% of them
  unsupported.
- M1's unsupported share reaches 11.2% for the left controller in `0925-0021` and 7.7% for the right in `0926-0009`.
  These were not examined. 33 of the left controller's poses in `0925-0021` are attributed wrong-device poses.

### Conclusion

The earlier conclusion is confirmed, and stronger than "similar accuracy". On the recordings, M1's poses are more
consistent with the gyro, steadier when still and free of jumps, at about 1/70 of upstream's cost. Upstream's
deterministic replay overruns the frame interval on a fifth of exposures.

Neither condition that would change the conclusion is met:

- Upstream is not as accurate as M1 on real data.
- M1 does miss exposures that upstream solves with supported poses: 13% of controller-exposures in the IMU sessions.
  But the coverage gate explains 3% of those. About half are solved by the tracker that ships, and at least 9% are
  upstream's own wrong-device poses.

There is no case for adopting upstream's front end, and none for relaxing the coverage gate.

### Proposed follow-ups

Neither is implemented.

1. **Find why the right controller fits at about 1 px.** This is the one real coverage loss the comparison found that
   the shipped path does not recover: 440 of the 506 exposures in the IMU sessions, and the largest class over all
   sessions. Raising the bootstrap RMS limit from 0.8 px to the tracking limit of 1.0 px would recover 216 of the 506.
   But that limit is what keeps mirror-image fits out (0.80–0.96 px), and the wrong-device counts above say the risk
   is real. Fixing the fit comes first. It is measured in "The LED model's fit" below.
2. **Score the shipped path in the comparison.** `--compare-frontend` scores the bare M1 loop, which has no position
   prediction, no bootstrap contest and no confirming solves. Half of its gap to upstream and all of its wrong-device
   poses may be artefacts of that. The comparison should also score `--tracker-filter`'s poses, and the evaluator
   should flag two devices within 60 mm of each other, which it cannot see today.

The per-exposure analysis behind this section is in the experiment directory, under `analysis/`: the scripts, the
`--m1 --csv` outputs and the `--tracker` / `--tracker-filter` outputs.

## The LED model's fit (1 October 2026)

This follows up the right controller's RMS. Two options were added to `constellation_replay`:

- `--residuals-csv OUT.csv` writes one row per correspondence of every M1 solve, accepted or not: LED, camera, blob,
  projection, pixel residual, range, facing angle, and the residual as a displacement of the LED in the model frame
  (`err_*_mm`, perpendicular to the view ray `ray_*`).
- `--led-offsets OFFSETS.csv` moves LEDs of the recorded models before any replay mode runs. Rows are
  `device,led,dx_mm,dy_mm,dz_mm` in the recorded model frame, which is the header's with y and z negated.

### What the residuals show

Over the 27 sessions, accepted solves hold 1.27 million left and 1.37 million right correspondences. The RMS is
0.49 px left and 0.57 px right. It rises with the facing angle for both (left 0.41 px head on to 0.55 px at 75–90°,
right 0.47 to 0.64 px), and no single camera or LED stands out.

Each LED's position error was fitted from those residuals: the least-squares displacement over all its views. One
pass gives 0.1–0.4 mm per LED for the left controller and 0.2–0.65 mm for the right, where they point outward. A
single pass underestimates, because the pose solve absorbs part of the error. So the fit was iterated: replay with the corrected
model, refit, remove the rigid part, repeat until the steps fall under 0.1 mm.

Three groups of sessions were fitted separately:

| fit | sessions | calibration | RMS before → after, px, L / R | scale it implies, L / R |
|---|---|---|---|---|
| A | 7, 25 Sep 20:47 to 26 Sep 00:09 | combined | 0.544 → 0.423 / 0.566 → 0.359 | +1.18% / +1.33% |
| B | 6, 26 Sep 00:22 to 01:45 | combined | 0.463 → 0.424 / 0.542 → 0.406 | +0.40% / +0.81% |
| C | 3, 25 Sep 00:12 to 00:21 | old (13 Sep) | 0.473 → 0.443 / 0.636 → 0.565 | −0.62% / +0.33% |

Fit A's offsets, in mm, in the recorded model frame:

| LED | left model | left offset | right model | right offset |
|---|---|---|---|---|
| 0 | (-12.1, +55.2, -38.2) | (-0.51, +0.85, -0.49) | (+12.1, +55.2, -38.2) | (+0.04, +0.65, -0.60) |
| 1 | (-33.1, +55.2, -20.2) | (-0.56, +0.61, +0.52) | (+33.1, +55.2, -20.2) | (+0.48, +0.82, +0.51) |
| 2 | (-53.7, +31.0, -28.4) | (-0.98, +0.68, -0.43) | (+53.7, +31.0, -28.4) | (+1.13, +0.80, -0.41) |
| 3 | (-67.0, +10.5, -8.7) | (-0.97, +0.67, -0.08) | (+67.0, +10.5, -8.7) | (+1.25, +0.21, +0.23) |
| 4 | (-63.5, -24.6, -4.2) | (-1.27, -0.21, -0.12) | (+63.5, -24.6, -4.2) | (+1.18, -0.37, -0.06) |
| 5 | (-47.8, -45.0, -18.5) | (-0.91, -0.50, -0.13) | (+47.8, -45.0, -18.5) | (+0.76, -0.45, -0.35) |
| 6 | (+45.5, +27.0, -43.0) | (+0.85, -0.09, -0.46) | (-45.5, +27.0, -43.0) | (-0.90, +0.03, -0.68) |
| 7 | (+59.5, +14.0, -28.5) | (+0.54, +0.25, -0.19) | (-59.5, +14.0, -28.5) | (-0.74, +0.57, -0.17) |
| 8 | (+61.1, -14.6, -24.7) | (+0.80, +0.04, -0.20) | (-61.1, -14.6, -24.7) | (-0.92, +0.19, -0.11) |
| 9 | (+51.0, -27.3, -36.2) | (+0.97, -0.48, -0.31) | (-51.0, -27.3, -36.2) | (-0.89, -0.68, -0.28) |
| 10 | (+48.5, -43.0, -18.8) | (+0.68, -0.04, +0.10) | (-48.5, -43.0, -18.8) | (-0.73, -0.13, +0.08) |
| 11 | (+30.6, -53.4, -29.4) | (+0.60, -0.54, -0.04) | (-30.6, -53.4, -29.4) | (-0.33, -0.63, -0.25) |
| 12 | (+18.5, -63.9, -11.2) | (+0.20, -0.15, +0.29) | (-18.5, -63.9, -11.2) | (-0.15, -0.31, +0.14) |
| 13 | (-2.7, -64.3, -22.7) | (+0.12, -0.51, +0.05) | (+2.7, -64.3, -22.7) | (+0.12, -0.54, -0.20) |
| 14 | (-29.1, -13.4, +60.7) | (-0.13, +0.07, +0.61) | (+29.1, -13.4, +60.7) | (+0.25, +0.04, +0.76) |
| 15 | (+1.1, -13.4, +67.3) | (+0.15, -0.17, +0.50) | (-1.1, -13.4, +67.3) | (-0.11, -0.02, +0.74) |
| 16 | (+31.8, -13.4, +59.3) | (+0.42, -0.47, +0.37) | (-31.8, -13.4, +59.3) | (-0.43, -0.17, +0.66) |

- **The right controller is not a special case.** The two controllers were fitted independently, and their offsets
  are mirror images of each other to 0.32 mm RMS, against an offset size of 0.9–1.0 mm RMS. The left ring has the
  same error. It shows less in the left's RMS on the 26 Sep sessions (0.46 px against the right's 0.54 px).
- **The offsets point outward.** They fit a uniform scale of +1.2 to +1.3%, or a shift of 0.8–0.9 mm along each LED's
  normal, about equally well (0.39–0.45 mm and 0.34–0.41 mm left over). The ring's normals are nearly radial, so the
  recordings cannot tell the two apart.
- **The size is not stable.** Fit B, on the same calibration, gives +0.4 to +0.8%. Fit C, on the old calibration, gives
  about zero and did not converge. A fixed error in the LED model would give the same answer each time. So part of
  this is the rig calibration, or depends on where the controllers were.
- **It is not the board's print scale.** The ChArUco squares were measured at 40.00 mm (`doc/macos-port.md`), assuming
  the mode-4 captures used the same board.

### Does a correction carry over?

Each correction was applied to sessions it was not fitted on, in the shipped path (`--tracker-filter`). The table
gives poses pushed, and the median over sessions of the RMS p50, as left / right.

26 Sep sessions (fit B's own; held out from fit A):

| model | pushed | RMS px p50 | RMS px p95 |
|---|---|---|---|
| as recorded | 17417 / 17116 | 0.424 / 0.526 | 0.618 / 0.800 |
| fit A offsets | 17696 / 18837 | 0.396 / 0.383 | 0.548 / 0.620 |
| fit B offsets | 17532 / 18833 | 0.403 / 0.413 | 0.581 / 0.657 |
| scale +1.0% | 17657 / 18586 | 0.423 / 0.446 | 0.583 / 0.674 |
| scale +1.3% | 17581 / 18690 | 0.428 / 0.418 | 0.573 / 0.633 |
| 0.9 mm along the normals | 17604 / 18669 | 0.432 / 0.450 | 0.581 / 0.674 |

25 Sep evening sessions (fit A's own; held out from fit B):

| model | pushed | RMS px p50 | RMS px p95 |
|---|---|---|---|
| as recorded | 17005 / 22633 | 0.504 / 0.525 | 0.778 / 0.820 |
| fit A offsets | 18137 / 24438 | 0.355 / 0.301 | 0.627 / 0.574 |
| fit B offsets | 17623 / 24187 | 0.425 / 0.363 | 0.684 / 0.678 |
| scale +1.0% | 18025 / 24199 | 0.425 / 0.380 | 0.680 / 0.652 |
| scale +1.3% | 18067 / 24276 | 0.383 / 0.372 | 0.679 / 0.607 |
| 0.9 mm along the normals | 18079 / 24281 | 0.378 / 0.372 | 0.664 / 0.598 |

- **Fit A's offsets are the best correction on both groups**, including the one they were not fitted on. Held out, the
  right controller gains 10% more poses (17116 → 18837) and its RMS p50 falls from 0.53 px to 0.38 px. The left gains
  2%, which is inside the replay's count noise (about 5%, see `doc/pssense-optical-tracking.md`), and its RMS falls
  from 0.42 px to 0.40 px.
- **A plain scale or normal shift gets most of the right controller's gain**, and less of the RMS.
- In `010135`, the right controller goes from 1973 to 2438 poses and from 0.53 px to 0.31 px. Upstream had 2807.
- On `20260925-083326`, recorded with the 25 Sep calibration alone, fit A takes the RMS from 0.48 / 0.57 px to
  0.32 / 0.26 px, with the counts unchanged.
- **On the old calibration it is worse.** In `20260925-002110`, fit A takes the left's RMS from 0.47 px to 0.68 px and
  the right's poses from 3342 to 2649. The correction belongs with the combined calibration.

Not checked: whether a better-fitting model changes the wrong-device poses, and the corrected poses' gyro and
stillness metrics.

### In the driver, opt-in

`PSSENSE_LED_CORRECTION=1` adds fit A's offsets to both controllers' models before they go to the tracker. The table
is `src/xrt/drivers/pssense/pssense_led_correction.h`. `pssense_led_model.h`, which is upstream's file with Sony's
data, is unchanged. The corrected model was checked against the one replayed above: all 34 LEDs agree to 0.0001 mm.
A recording made with the variable set carries the corrected model, so it replays without `--led-offsets`.

Limits:

- it is fitted on one pair of controllers and one rig calibration, and it would have to be refitted if the calibration
  changes;
- the cause is not settled, so it may be compensating a rig error rather than correcting the LEDs;
- it has not been run on the headset.

**Hardware test.** Use the combined calibration (`20260926-charuco-mode4-combined-head.json`) and the same settings
for each pair of runs:

1. The same moves twice, with `PSSENSE_LED_CORRECTION=0` and then `=1`. `run.log` should show `LED_CORRECTION side=L`
   and `side=R` in the second. Compare the poses pushed and the reprojection RMS per controller, the right one above
   all, and replay each recording through `--tracker-filter`.
2. Both controllers resting at two or three separations measured with a ruler, 30 s each, with the correction on and
   Create-button static markers. The measured separation gives the rig's scale a reference that does not depend on
   the LED model, which settles the cause: if the tracked separation is right with the correction, the model was
   small; if it is about 1% long, the rig is.

Record the results here, with the commit tested.

The scripts and the fitted offsets are in the experiment directory, under `analysis/led/`.

## Synthetic results

The recordings were made by `constellation_synth`:

- two Sense rings, four 508×508 KB4 cameras at 60 Hz;
- 0.2 px blob noise, 5% blob drops, merged blobs, three background lights;
- 3 s still periods every 10 s;
- IMU at 66 Hz.

Results are given as left / right controller.

`base` is seed 1, 20 s, speed 1.0:

| | M1 | upstream (MR 2940) |
|---|---|---|
| solved, % of exposures | 83.8 / 79.5 | 100 / 100 |
| position error vs truth, mm, p50 / p95 | 0.62 / 1.62, 0.61 / 1.59 | 1.79 / 5.42, 1.66 / 4.34 |
| orientation error vs truth, deg, p50 / p95 | 0.38 / 0.75, 0.36 / 0.80 | 0.97 / 2.50, 0.93 / 2.23 |
| wrong poses (> 30 mm or > 10°) | 0 / 0 | 0 / 2 (37° max) |
| evaluator reprojection, px, p50 | 0.264 / 0.263 | 0.334 / 0.318 |
| rotation vs gyro, deg, p50 / p95 | 0.26 / 0.71, 0.25 / 0.71 | 0.94 / 3.37, 0.84 / 2.65 |
| step while still (gyro), mm RMS | 1.12 / 1.32 | 4.80 / 3.18 |
| spread in annotated still intervals, mm | 0.75 / 1.27 | 2.94 / 2.14 |
| cost per exposure, µs, mean / p95 | 776 / 1774 | 7272 / 27080 |
| exposures costing more than the 16.7 ms interval | 0% | 17.3% |

`fast` is seed 2, 30 s, speed 1.5:

| | M1 | upstream (MR 2940) |
|---|---|---|
| solved, % of exposures | 85.0 / 84.8 | 99.9 / 99.8 |
| position error, mm, p50 / p95 | 0.59 / 1.58, 0.65 / 1.70 | 1.53 / 4.59, 1.67 / 4.85 |
| orientation error, deg, p50 / p95 | 0.36 / 0.73, 0.38 / 0.77 | 0.88 / 2.28, 0.89 / 2.24 |
| wrong poses | 1 (180 mm, 34°) / 0 | 2 (366 mm, 45°) / 0 |
| cost per exposure, µs, mean / p95 | 721 / 1664 | 9317 / 32654 |
| exposures costing more than 16.7 ms | 0% | 20.8% |

Costs are single-threaded on a 2.1 GHz Xeon. Both trees were built RelWithDebInfo against the same Ceres.

What this shows, on synthetic data only:

- **Upstream's poses are 2–3× less accurate.** Its position and orientation errors are larger, its rotation agrees
  less with the gyro, and it moves more while the controller is still. It solves each camera on its own, and the
  record keeps the camera with the most matched blobs. M1 solves all cameras jointly. It also uses the IMU orientation
  as a prior when tracking, which upstream's tracker does not. Upstream's own RMS (0.19 px) is lower than M1's (0.26
  px), but the common evaluator puts it at 0.32–0.33 px against M1's 0.26 px. One camera's fit is tighter than the
  pose really is.
- **Upstream solves more exposures.** M1 rejects 15–20% of exposures in which the pose is in fact correct. The cause
  is its coverage gate: at least 80% of the LEDs it predicts as visible must be matched. The synthetic merges and drops
  push those exposures to 0.70–0.79 coverage. With drops off, M1 still misses about 10%, from merges alone. Real
  recordings behave differently: after the coverage-margin fix, M1 solved about every lit exposure of `234059` (see
  `doc/pssense-optical-tracking.md`). The recordings do not show this gap: coverage alone rejects 3–4% of the
  exposures only upstream solved (see "The exposures only upstream solved").
- **Both front ends make rare wrong poses.** The consistency metrics did not flag M1's single wrong pose in `fast`
  (0 rotation jumps), but the evaluator's "unsupported" count did. Ground truth found it directly.
- **Upstream costs about 10× more on average and 20× more at p95.** In deterministic mode, 17–21% of exposures take
  longer than the frame interval. Live, upstream runs the slow correspondence search on a separate thread and skips
  frames while it is busy. Its live coverage would therefore be lower than the replay's, and its CPU use about the
  same.

The synthetic recordings model blob noise, drops, merges and background light. They do not model:

- LED timing or brightness;
- self-occlusion by the ring;
- the hand;
- calibration error;
- SLAM error.

They test the pipeline and give a first comparison. They are not a substitute for the recordings.

## How the comparison works

### Upstream side: `constellation_upstream_replay`

It runs in upstream's tree, because the two trees' constellation code shares symbol and type names and cannot be
linked together. It feeds a recording's camera frames through upstream's `t_constellation_tracker` in timestamp order,
and writes a records CSV (`# constellation frontend records v1`):

- `R` rows: every pose pushed, with its camera, matched blob count and RMS (OpenCV convention);
- `M` rows: the blob-to-LED matches behind each pose;
- `F` rows: the time spent on each camera frame.

Choices, made to give upstream every advantage the live tracker would have:

- **Deterministic, single-threaded mode.** Every frame gets the fast path and, if that fails, the slow search, so
  coverage is an upper bound.
- **Blob labels are carried between frames by blob id**, as `t_rift_blobwatch` does live. Recorded blob ids come from
  the same blobwatch, and recorded labels are stripped.
- **The tracking source of each controller is the history of upstream's own pushed poses**, as the Sense driver's
  constellation pose is. Upstream's tracker ignores IMU samples, so none are fed to it.
- **The Sense match parameters are upstream's** (`DEFAULT_MATCH_PARAMETERS`, 7 LEDs without a prior, 5 with one).
  `--min-leds-without-prior` and `--min-leds-with-prior` override them.

### Our side: `constellation_replay --compare-frontend`

`--compare-frontend NAME=RECORDS.csv` imports a records file. It may be given more than once. M1 always runs. Each
exposure keeps the pose of the camera with the most matched blobs, then the lowest RMS. Its correspondences are the
matches of every camera that produced a pose.

Every pose is then scored by the same evaluator, which uses nothing from either front end:

- **Coverage:** exposures solved, as a share of all exposures and of those any front end solved. With ground truth,
  also as a share of those where the true pose puts at least 4 LEDs on blobs.
- **Reprojection:** the device's visible LEDs are projected into every camera at the pose and paired one-to-one with
  the nearest blob within 3 px. The report gives inliers, their RMS, and the found/predicted ratio. A pose is
  "unsupported" when it has fewer than 4 inliers or under half the predicted LEDs are found.
- **Gyro consistency:** the rotation angle between consecutive poses (at most 60 ms apart) against the integrated gyro
  angle. The angle does not depend on the IMU-to-model or world alignment, so neither needs estimating. A residual
  above 10° counts as a rotation jump.
- **Position jumps:** misses of more than 30 mm against a constant-velocity prediction from the two previous poses.
- **Stillness:** pose-to-pose steps while the gyro reads under 0.05 rad/s for 50 ms either side. Also the spread of
  poses inside annotated static intervals, skipping 0.5 s at each end.
- **Ground truth**, when recorded: position and orientation error, and the count of poses more than 30 mm or 10° off.
- **Agreement:** the difference between front ends where both solved, and where only one did.
- **Cost:** front-end time per exposure, all devices, and the share of exposures over the exposure interval.

`--compare-out OUT.csv` writes one row per scored pose. `--fusion-compare --fusion-frontend NAME` runs the EKF and
sliding-window fusion on an imported front end's poses instead of M1's. This is checked on the synthetic recordings,
where both paths accept upstream's records. Fusion results on synthetic data are not reported here.

## Running it on the Mac recordings

Build both trees. Upstream's tracker needs Ceres (`brew install ceres-solver`).

```sh
git fetch origin claude/pssense-upstream-frontend-replay claude/pssense-mr2940-evaluation
git worktree add ../monado-upreplay origin/claude/pssense-upstream-frontend-replay
cmake -S ../monado-upreplay -B ../monado-upreplay/build-rel -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build ../monado-upreplay/build-rel --target constellation_upstream_replay
cmake --build build-sense-rel --target constellation_replay constellation_synth
```

Then, for each recording:

```sh
UP=../monado-upreplay/build-rel/src/xrt/tracking/constellation/constellation_upstream_replay
REPLAY=build-sense-rel/src/xrt/tracking/constellation/constellation_replay
OUT=~/Code/psvr2-datasets/experiments/20261001-mr2940-frontend
mkdir -p $OUT
for S in ~/Code/psvr2-datasets/sessions/{20260926-010135-imu-capture,20260926-014505-filter-live}; do
  N=$(basename $S)
  $UP $S/constellation.ctd $OUT/$N-upstream.csv > $OUT/$N-upstream.log 2>&1
  $REPLAY $S/constellation.ctd --compare-frontend upstream=$OUT/$N-upstream.csv \
    --compare-out $OUT/$N-compare.csv > $OUT/$N-compare.log
done
```

Any recording with a `constellation.ctd` works. Those without IMU packets have no gyro metrics. The upstream replay
has no `--calibration` override, so leave it off `$REPLAY` too. Both front ends must see the same camera poses.

To check the pipeline on the Mac first:

```sh
build-sense-rel/src/xrt/tracking/constellation/constellation_synth $OUT/synth.ctd --duration 20
```

Then run the two commands above on `$OUT/synth.ctd`. The results should be close to the `base` table.

**What would change the conclusion:**

- Upstream's poses on real data as accurate as M1's (the evaluator's RMS and the gyro residual are the
  ground-truth-free measures).
- M1 missing many exposures that upstream solves with supported poses ("only upstream" in the agreement line, with
  upstream's unsupported share low).

The first would mean the synthetic accuracy gap does not carry over. The second would point to M1's coverage gate.
Neither would justify upstream's cost.

## The dataset format

### Distortion model codes

The dataset wrote `t_camera_distortion_model` as its raw value. Upstream commit `9e06aa432` inserted
`T_DISTORTION_PINHOLE` at the front of that enum. As a result, a KB4 recording made here read as `RADTAN_14` in
upstream's tree. Upstream's tracker then ran about 20× slower and found under half the poses. The format now stores fixed codes in the
enum's original order:

| code | model |
|---|---|
| 0 | RADTAN_5 |
| 1 | RADTAN_8 |
| 2 | RADTAN_14 |
| 3 | KB4 |
| 4 | WMR |
| 5 | PINHOLE |
| 6 | RIFT_CV1 |

Every existing recording reads unchanged, and both trees now agree. Files written by upstream `main`'s own recorder
still use upstream's shifted values, and would read wrongly here. The same fix belongs upstream.

### Extension records (packet 5)

Packet 5 is `{u32 kind, u32 length, payload}`. A reader skips kinds it does not know, and ignores bytes past the end
of the fields it knows. Fields can therefore be appended to a kind without breaking older readers. Readers from before
packet 5 stop at the first one, after reading everything before it.

| kind | record | written by |
|---|---|---|
| 1 | session info: JSON with tool, git tag, calibration, duration, world frame, capture directory, notes, device ids, and every `PSVR2_`/`PSSENSE_`/`CONSTELLATION_` variable | `psvr2-constellation` at start |
| 2 | sync event `{device, host_ns, kind, value[3]}`: clock snap, gyro bias, LED lock, lost, scan, phase move | Sense driver |
| 3 | IMU timing `{device, host_ns, device_ns, clock_offset_ns, applied_gyro_bias[3]}`, beside each IMU sample | Sense driver |
| 4 | head pose `{timestamp_ns, flags, T_world_head (XR), source_ns, interpolated / exact}` at each exposure | `psvr2-constellation`, world frame only |
| 5 | ground truth `{device, timestamp_ns, T_world_device (XR), sigmas, flags}` | `constellation_synth`; a future fixture or mocap |
| 6 | annotation `{device, host_ns, text}` | `psvr2-constellation` (static markers) |

`constellation_replay DATASET.ctd` summarises all of these: the session JSON, counts per kind, head-pose age against
the newest SLAM pose, and the annotations. `tests_constellation_dataset` covers the round trip, unknown kinds and
truncation.

This replaces the "smallest compatible extension" proposed in the fusion evaluation. The sync events replace parsing
`run.log`. The IMU timing records hold the device clock and the bias the driver subtracted. The head poses record
their age.

## Recording for future evaluations

Use `scripts/psvr2_sense_session.sh` as before. For recordings meant for evaluation:

- **Record in world frame** (`PSVR2_CONSTELLATION_WORLD=1`). Head poses and their age are recorded only then.
- **Mark still intervals.** Press a controller's Create button when it is resting on something fixed, and again
  before picking it up. Each press toggles `static_begin` / `static_end` for that controller, and the CLI echoes it.
  The replay then scores spread and drift against a pose that is known to be constant. This is the closest thing to
  ground truth without a fixture. A few 5–10 s rests, near and far, at different orientations, are enough.
- **Describe the session.** The script's NOTE argument now goes into the recording's session record too (through
  `PSVR2_CONSTELLATION_NOTES`), so a `.ctd` copied elsewhere still says what was done.
- **Include the hard cases on purpose:**
  - both controllers close together, which causes merged blobs;
  - fast rotations;
  - controllers far from the headset;
  - a background light in view.

  These are where the front ends differ.
- **Keep runs short and separate.** Several 30–60 s sessions are easier to compare than one long one.

## Commits

On `claude/pssense-mr2940-evaluation`:

- `t/constellation: extension records for timing, head pose and ground truth`
- `t/constellation: store distortion models as fixed dataset codes`
- `t/constellation: synthetic dataset generator with ground truth`
- `t/constellation: score optical front ends side by side in the replay`
- `t/constellation: per-correspondence residuals and LED offsets in the replay`
- `d/pssense: opt-in measured correction to the LED models`

On `claude/pssense-upstream-frontend-replay`:

- `t/constellation: replay datasets through the tracker for front-end comparison`. It also brings over the dataset
  format, with the extension records and the fixed codes.
