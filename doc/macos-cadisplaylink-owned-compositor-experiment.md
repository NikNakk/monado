<!--
Copyright 2026, Nick Kennedy

SPDX-License-Identifier: BSL-1.0
-->

# CADisplayLink-owned compositor: failed experiment

**Decision, 2026-10-03:** reject and remove the tested CADisplayLink-owned
rendering variants. Include this record when integrating the current work into
`macos-upstream-clean`. Retain ordinary CADisplayLink pacing and client-hosted
compositing; the experiment did not reject either of those architectures.

## What was tested

Work on `codex/cadisplaylink-compositor` used base `2276cfba9` plus an
uncommitted experiment patch saved with the captures. The branch ref alone
does not contain or identify the tested implementation.

An immediate callback variant changed rendering phase and performed badly.
It was replaced by a phase-matched variant that ran the compositor body on
the CADisplayLink callback thread, including the ordinary pacing wait. A
further revision prepared work off-thread and dispatched a short rendering
section onto the CA run loop after the callback returned.

Each final variant was compared with ordinary CA pacing using five static
headset runs per mode, alternating control and experiment. Runs used the
MonadoMacTest UE project in `-game -vr` mode, client-hosted compositing,
shared-event release waits and the then-current buffered tracing. Game Mode
was not independently confirmed for these automated recordings. Source
delivery varied across runs, and the fixed A/B order limits causal attribution.
These tests preceded the later source/target-pose correction.

## Results and decision

| Comparison, mean across five runs per mode | Ordinary CA pacing | Experiment |
| --- | --- | --- |
| Callback-body physical cadence | 108.96 Hz | 102.03 Hz |
| Callback-body intervals over 12 ms | 10.47% | 17.47% |
| Deferred CA physical cadence | 119.36 Hz | 119.14 Hz |
| Deferred CA intervals over 12 ms | 0.369% | 0.542% |

The callback-body variant had worse cadence overall, despite an improvement
in one latency-tail statistic. Long callback bodies also delayed the link.
The deferred revision kept callbacks short, but all five experimental runs
had more intervals over 12 ms than their adjacent controls, with a longer
typical pose-to-display latency tail. It also rendered more iterations without
adding displayed frames. Neither tested implementation justified its added
scheduling complexity.

This is a failed experiment for the tested designs and workloads, not proof
that every possible CA-thread implementation must lose. The later pose fix
does not constitute a retest or validation of these removed modes.

## What is carried into integration

- Keep timestamp-only CA pacing and the ordinary compositor thread.
- Keep client-hosted compositing for Game Mode, asynchronous drawable
  acquisition, late target-pose queries, timewarp and GPU/shared-event
  synchronization.
- Keep passive `ca_callback.csv` and `renderer_stage.csv` diagnostics, along
  with the measurements and limitations above.
- Remove `XRT_MACOS_CA_COMPOSITOR`, callback/deferred rendering dispatch,
  prepared-view caching, generic dispatcher hooks and dispatch-only tests.
  Those rejected runtime modes are absent from the current source.

Detailed run tables, capture identifiers and methods remain in the
[callback-body results](macos-psvr2-timing-diagnostics.md#five-alternating-pairs-with-fully-buffered-traces--2026-10-03)
and [deferred revision](macos-psvr2-timing-diagnostics.md#deferred-run-loop-results-and-retirement). The
[toggle ledger](macos-env-toggles.md) records retirement of the option.

This record and its detailed evidence ledgers are included with the
`macos-upstream-clean` integration. Its previous remote head, `08619006b`,
did not contain this experiment record. The failed runtime implementations
remain removed.
