# World Operation Boundaries Plan

Summary: Replace subsystem callback depth with World operation boundaries, retained transitions, and deferred garbage collection.

Last reviewed: 2026-09-08

Status: Completed
Completed: 2026-09-08

## Current Status

Implemented and validated. `test affected` passed all 71 selected native test
targets, including CoreObjectTests. A subsequent World-only follow-up preserves
requests to return to the original Level; WorldTests passed all 126 tests after
that change. Added coverage includes collection deferral with automatic GC
disabled, input stop/reentry, transition ownership, and next-request preservation.
Documentation validation passed. Application smoke was not run; no UI behavior
was changed. The contracts live in LevelSystem, WorldSubsystems, and RuntimeLifecycle.

## Goal

Keep World structural changes and physical collection outside active World
operations, independently of whether extensions are input, Actor, Component,
or Subsystem callbacks.

## Decisions

- World operations are game-thread-only and non-reentrant. Public entry points
  reject incompatible synchronous operations or record EndPlay/shutdown intent.
  Private implementations compose transition and shutdown steps.
- Tick completes its registry frame before processing stop requests. Level
  transitions run at the next Tick entry, at most one per entry.
- Pending and active transition targets participate in World GC enumeration.
  Requests submitted by callbacks remain pending for a later Tick.
- CoreDObject provides collection deferral for complete execution regions.
  Requests remain pending until explicit collection or the existing scheduler
  reaches a safe point; leaving a deferral region never triggers collection.
- Existing subsystem registration, dependencies, work gates, and module leases
  remain unchanged. No Play generation or callback-depth replacement is needed.

## Implementation Stages

### Stage 0: Establish and validate operation boundaries

- [x] Implement collection requests and execution-region deferral.
- [x] Replace callback depth with World phases and retained active transitions.
- [x] Cover transition GC, input mutation, recursive entry, and Tick cleanup.
- [x] Update World and runtime contracts; run affected tests and document validation.

Completion requires passing affected native tests and documentation validation.
Follow the repository agent build and testing workflows for command selection.
