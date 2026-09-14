# World Timers Plan

Summary: Add play-scoped World timers with a shared gameplay clock and mutation-safe object-bound callbacks.

Last reviewed: 2026-09-14

Status: Completed
Completed: 2026-09-14

## Current Status

Implementation and the [runtime contract](../Runtime/World/WorldTimers.md) are
complete. Validation used the windows-msvc-x64 / Win64-Debug-DurinEditor profile:

- The affected-test selection passed all 83 targets (420.23 seconds including build).
- Final review restricted construction to World, used a Component snapshot for
  destruction cleanup, and closed owner admission before releasing EndPlay
  timer captures. BeginPlay registration and capture cleanup received additional
  regression coverage.
- The final all build passed (26.33 seconds), including workspace consumers.
- After those final changes, complete WorldTests passed all 160 cases across
  27 suites, including 16 timer cases (10.46 seconds including build).
- Changed-document and all-plan validation, plus Git whitespace review, passed.

The broad passing regression results are retained; the final lifecycle changes
were validated by the complete owning WorldTests target and workspace build.
No application-hosted or GPU qualification run was required for this feature.

## Goal

Provide delayed, looping, and next-gameplay-frame callbacks through a mandatory
World-owned native timer manager. Keep timers transient and restricted to one
play lifetime. EndPlay, Level replacement, and shutdown invalidate all timers.

The World snapshots a finite nonnegative time scale at frame entry and supplies
the same scaled delta to gameplay subsystems, Actor/Component Tick, and timers.
Editor/Preview service ticks outside gameplay keep their host delta. Pause
freezes timers; single step admits one frame. Zero scale freezes elapsed time
but permits next-frame callbacks. Reject nonfinite or negative input deltas.

Dispatch after all PostPhysics callbacks within the existing World operation.
New and resumed timers are eligible no earlier than the next gameplay frame.
Looping timers run at most once per frame, skip missed periods while retaining
phase, and require a positive finite interval. Zero delay is asynchronous.
Cancellation is immediate for callbacks not started, including queued entries;
executing callbacks unwind normally. Callback exceptions cancel the offending
timer and propagate, with dispatch state restored by scope cleanup.

Use generation-checked manager/slot identities, a deadline heap, and separate
next-frame/pending queues. Object bindings hold non-retaining FObjectHandle
identities and validate World membership and lifecycle before dispatch. Engine
Actor EndPlay/destruction and Component EndPlay/unregistration clear bindings.
Plain callbacks own their captures and must not capture unguarded object pointers.
Cross-level persistence, wall-clock timers, workers, and serialization are excluded.

## Implementation Stages

### Stage 0: Shared gameplay clock

- [x] Add validated World time scale and snapshot scaled gameplay delta.
- [x] Preserve pause/single-step and non-gameplay subsystem behavior.
- [x] Search all workspace project source/test consumers of the affected API.

Outcome: one gameplay time authority feeds every admitted gameplay tick.

### Stage 1: Timers and lifetime integration

Depends on Stage 0.

- [x] Implement handles, delay/loop/next-frame, cancellation, pause/resume, and queries.
- [x] Integrate frame-tail dispatch and play/World teardown.
- [x] Add safe object bindings and Actor/Component lifecycle cleanup.

Outcome: timers remain deterministic under callback mutation and teardown.

### Stage 2: Contract and validation

Depends on Stage 1.

- [x] Cover timing, invalid input, pause/scale/step, mutation, stale handles,
  object retirement, callback exceptions, and World transition boundaries.
- [x] Publish the implemented [runtime contract](../Runtime/World/WorldTimers.md) and link routing.
- [x] Pass relevant native tests and affected-test selection.
- [x] Complete an all build and changed-document/all-plan validation.
- [x] Review and commit isolated changes with plan/stage provenance.

Follow [build guidance](../Agents/BuildAndRun.md) and
[test guidance](../Agents/Testing.md) for execution and selection.
