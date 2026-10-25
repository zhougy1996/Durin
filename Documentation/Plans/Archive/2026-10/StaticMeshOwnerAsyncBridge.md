# StaticMesh Owner Async Bridge Plan

Summary: Migrate StaticMesh render compilation owners to asynchronous derived-data sessions.

Last reviewed: 2026-10-12

Status: Archived
Completed: 2026-10-12

## Current Status

All stages are complete. The plan was saved and validated before source changes.
Render owners use asynchronous DDC dispatch and retain their reservation across
I/O. The owner pump schedules CPU consumption only after callback retirement;
publication and explicit finish retain owner-thread reconciliation. Lasting
behavior is recorded in [Asset Compilation](../../../Runtime/Assets/Build/AssetCompilation.md#staticmesh-completion)
and [Derived Data Build](../../../Runtime/Assets/Build/DerivedDataBuild.md#family-boundaries).

Validation on `Win64-Debug-DurinEditor`:

- Final `test StaticMeshTests --report`: all 169 cases passed, covering authored
  builds, DDC, replacement, cancellation, reconciliation, Cook and independent
  collision lifecycle. The mailbox barrier now drives CPU continuations; building
  barriers retain their original terminal-notification timing.
- `test AsyncTaskPilotLifecycleTests --report`: passed with two Core workers.
  New mesh coverage verifies retained slots and bytes during slow reads, available
  Core workers, cancellation, slow persistence, DDC callback retirement, CPU
  mailbox retirement, selected render finish, warm hits without another source
  read, supersession, and combined Mesh/Texture2D/Cube Core-first shutdown.
  Three BlockingIO workers allow one physical read per family during shutdown;
  additional mesh requests remain admitted or queued and all cancel exactly once.
- Final `test "@module=derived-data-cache" --report`: all five targets passed,
  including the lifecycle regression on final code and registration metadata.
- `test AssetCompilingManagerTests --report`: passed.
- `build --target all`: passed, covering Engine, Sandbox, RoadWeaver and native
  target compilation. Log: `Build/.agent-state/logs/20261012-024419-700809-75736-cmake.log`.
- Changed-document validation, all-plan validation and diff whitespace checks passed.

Validation uses bounded CPU suites. GPU behavior, cache keys and output schemas
are unchanged. Synchronous Build, detached import, Cook and independent physics
cooking retain their existing boundaries.

## Goal

Release Core workers while render requests await cache I/O, preserve owner
reservations and reconciliation, and publish prepared render data on GameThread.
Keep synchronous Build, import and Cook boundaries and independent collision
compilation unchanged.

## Selected Decisions

- Use a persistent task-backed render session and a request owner per admitted
  render record, with captured builder version, priority and cancellation.
- Share validation, policy and output consumption with the synchronous wrapper.
- DDC callbacks only publish detached completion data. After callback retirement,
  the owner pump dispatches output assembly and ray preparation to a Core worker.
  Finish pumps this continuation without waiting from a worker.
- Retain admission slots, byte reservations and detached inputs through I/O and
  CPU continuation retirement; cancel and drain suspended requests on shutdown.
- Preserve keys, schemas, collision independence and owner-thread reconciliation.

## Implementation Stages

### Stage 0: Record the StaticMesh bridge

- [x] Inspect render dispatch, mailbox and synchronous consumer boundaries.
- [x] Save and validate this plan before changing source.

### Stage 1: Migrate StaticMesh owner consumption

- [x] Share render validation, request policy and output preparation.
- [x] Bridge owner dispatch, cancellation and CPU continuation retirement.
- [x] Update lasting asset and derived-data contracts.

### Stage 2: Validate and deliver

- [x] Add two-worker regressions for slow reads/writes, cancellation, warm hits,
  selected finish, supersession and shutdown.
- [x] Pass relevant StaticMesh, owner lifecycle and DDC suites and the all build
  following [build](../../../Agents/BuildAndRun.md) and [testing](../../../Agents/Testing.md) guidance.
- [x] Validate documentation and all plans, record evidence and commit.
