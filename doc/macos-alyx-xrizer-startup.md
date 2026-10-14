<!--
Copyright 2026, Nick Kennedy
SPDX-License-Identifier: BSL-1.0
-->

# Alyx xrizer startup deadlock — 2026-10-14

The user reports Alyx launches through SteamVR but appears to exit through
xrizer. The direct run `~/Windows/logs/alyx-xrizer-20261014-095145/alyx.log`
initializes wineopenxr, detects PS VR2/Sense, begins the session, decodes the
startup DDS stage and completes map loading. At 09:51:59.312 its final message
is `Creating real backend for texture type DirectX`. The game console ends
at `Submitting first frame to the compositor`.

## Confirmed blockage

At approximately 10:01 the original macOS PID 75256 is still alive. Wine PID
3932 (`0x0f5c`) has two blocked threads, captured with WineDbg and detached
without terminating the game:

- Thread `0x0aa0`: `GetDigitalActionData` → `state_from_bindings` → Rust
  `RwLock::read_contended`, waiting at address `0x896b7380`.
- Thread `0x0980`: `Submit` → `initialize_real_session` → `restart_session`
  → `RwLock::write_contended`, waiting on the same lock's writer notification
  word at `0x896b7384`.

The installed DLL exactly matches the local release build (SHA-1
`485d46d3aa83be744f520b85c8d10de64da3086f`). Its COFF symbols map return offsets
`0x2e4cf` and `0x26c8d7` to those input/restart functions respectively.

`GetDigitalActionData` already holds `session_data`'s read guard while calling
`state_from_bindings`, which takes another read guard. Once the render thread
queues a writer for the first graphics-session replacement, Rust's lock blocks
new readers behind it. Input cannot finish its nested read; restart cannot
obtain the writer until input releases its first read. This is an xrizer lock
bug, before session teardown, rather than a demonstrated Monado graphics fault.

The same nested helper is used by scalar analog queries. The correction passes
the existing `&SessionData` through both binding helpers, including left/right
queries, so the entire query uses one session generation and one read guard.
No production Monado or Wine bridge changes are required.

## Local correction and validation

Source changes are in `~/Code/xrizer`, based on `655d448044e48f7a7c65c0bb58a44d2177795114`
on `windows-openxr-d3d11`; the pre-existing Cargo.lock modification is preserved.
The Windows fake-runtime test path now chooses Vulkan, because its fake runtime
does not expose D3D11; production Windows temporary sessions retain D3D11.

`binding_query_with_session_restart_pending` holds a session read guard while
another thread attempts restart, then queries both/unrestricted hand bindings.
A 50 ms scheduling interval allows the writer to queue. With the original
nested acquisition restored, it hangs until a five-second subprocess deadline.
With the fix it passes ten consecutive runs. The corrected full Windows unit suite reports
82 passed, 3 failed, 2 ignored; the same three input failures occur in the
baseline (81 passed, 3 failed, 2 ignored, new regression excluded):
`action_origins_match_pose_and_skeleton_hands`,
`non_hand_restriction_activates_only_secondary_set_for_hands`, and
`restricted_action_set_activates_secondary_set_for_other_hand`.

Raw test logs and the Rosetta-limited native stack sample are retained in
ignored `doc/results/alyx-xrizer-20261014/`. The native sample alone cannot
resolve the Windows stacks and is not the basis for the lock diagnosis.

A fresh user-operated Alyx launch remains necessary to validate startup and
subsequent rendering/input. The old deadlocked process cannot load the fix.
The log also contains duplicate MoltenVK-class and controller-property/binding
warnings; their independent effect after this deadlock is not established.

The installed corrected `vrclient_x64.dll` has SHA-256
`1b655ff0794fe7203eb3e4d536005fb0c758d077f00e3495c6f19c13f7aff7a0`.
Its original is backed up at
`~/Windows/backups/xrizer-pre-session-lock-20261014.dll`. Installation used an
atomic rename, preserving the old mapped file for the still-running game.


## Committed cleanup and subsequent gameplay

The historical audit and current follow-up are committed in the adjacent xrizer
checkout as `e469039`, at `docs/ALYX_CHANGE_REVIEW.md` and
`docs/ALYX_REVIEW_FOLLOWUP.md`. The implementation commits `40099f0` through
`cb2986a` pin dependencies, repair singleton/mailbox/session locking, remove
behavioral experiments and fake controllers, fix input fixtures, isolate stage
rendering, add asynchronous bounded stage loading, validate D3D11 formats, and
replace fabricated timings with measured CPU/GPU history. Frame-path runtime
errors now latch failures and stop image reuse; automatic session recovery and
inherited initialization/teardown error handling remain incomplete.

Claude's subsequent xrizer commits `15e9926`, `c6ff3f9` and `1eb76cd` correct
refresh-rate units, HMD naming and successful float-property error reporting.
A fresh Windows/Wine build of the combined code passes 97 tests (2 ignored),
and the restart-deadlock regression passes 10/10. The cleanup's Linux validation
passes 96 unit and 2 integration tests; strict Clippy passes on both platforms.
Full Miri is not established: ARM64 hits an unsupported NEON intrinsic and an
attempted x64 run lacks cross-target bindgen headers.

The current installed runtime DLL matches the combined release build at
`1eb76cd`, SHA-256
`f82c751f0bce49952298380df472f34ceca7480dddec712c4c3d484803fd87d2`.
The earlier installed session-lock-only DLL/hash above is historical.

Subsequent user-operated Alyx gameplay and auto-fidelity measurements are recorded
in [the ownership/pacing follow-up](macos-steamvr-keyed-ownership-followup.md),
including 45% scale at 120 Hz with combined xrizer `1eb76cd`. This supersedes the
initial pending-startup status: the game reaches play. Remaining long frame gaps
occur inside the application frame and are consistent with DXMT backpressure;
that pacing issue remains under investigation. The launcher's default still
sets manual fidelity; later hardware runs explicitly enabled auto-fidelity.
The removed projection-switch export is committed in macos-wine-xr `ecfb660`.
