# UE Garbage Collection Safe Points Plan

Summary: Replace business-level GC deferral with explicit safe-point and lifetime contracts.

Last reviewed: 2026-11-10

Status: Completed
Completed: 2026-11-10

## Current Status

Implementation and validation complete: 32 ordinary test targets, the all build,
and one bounded normal startup/shutdown smoke passed. This follows the completed
[request and execution alignment](UEAlignedGarbageCollection.md).

The audit found two internal registration/default-construction rollback collectors;
both now mark and request, with tests checking retirement before host collection.
Remaining synchronous sites are frame end, shutdown, module-default draining,
package unload/removal and offline asset tools. They require owner-established
safe points. Existing subsystem/history reference collectors and editor-session
strong ownership are retained; no new blanket roots are introduced.

## Goal

Remove `FGarbageCollectionDeferralScope`, its counter and query API after making
collection timing the responsibility of synchronous collector callers. Business
callbacks request collection; they do not execute it or reenter a host safe point.
References retain ordinary objects across safe points. Explicit garbage marking
still invalidates objects; business reentry and teardown state machines remain.

UE 5.8 references are local `UnrealEngine.cpp`, `GarbageCollection.cpp`,
`SubsystemCollection.cpp` and `EditorTransaction.cpp` under
`E:\Programs\Epic Games\UE_5.8\Engine\Source`. UE GC synchronization locks are
not equivalent to Durin's single-game-thread business deferral counter. Do not
introduce dummy locks, loading flags or a replacement business scope counter.

## Implementation Stages

### Stage 0: Establish safe-point and lifetime contracts

- [x] Audit synchronous collection and ownership across Engine, Sandbox and RoadWeaver.
- [x] Document callback requests, explicit collection boundaries, borrowed storage,
  logical destruction and ownership across collection.
- [x] Identify any internal cleanup collection that must become a request.

### Stage 1: Remove business deferral and migrate consumers

Depends on Stage 0.

- [x] Remove scopes, counter and query; preserve game-thread checks, collector
  reentry rejection and destruction readiness.
- [x] Keep initialization, rollback, transaction, World and editor state guards.
- [x] Migrate tests from automatic refusal inside business callbacks to requests
  serviced after complete operations; cover collector reentry independently.
- [x] Update implemented contracts and remove stale deferral claims.

### Stage 2: Validate and accept

Depends on Stage 1. Follow the [build](../Agents/BuildAndRun.md) and
[test](../Agents/Testing.md) workflows.

- [x] Run affected-selection discovery and relevant CoreDObject, Engine, World,
  editor, package and material-query lifetime tests.
- [x] Build `all`: removing shared API and scope members changes foundational
  headers and public class layouts beyond reliable bounded consumer coverage.
- [x] Run one bounded normal startup/shutdown smoke for host drain coverage.
- [x] Validate changed documentation, all plans and the final diff.
- [x] Record final evidence and commit with plan/stage provenance.

## Validation Evidence

Validation uses `Win64-Debug-DurinEditor`, profile `windows-msvc-x64`, against
base `00c7e1585` plus this plan's working changes. MSVC runs outside the sandbox
because its sandbox-only C1902 failure was already established. Logs remain in
`Build/.agent-state/logs/`.

- Affected discovery identified CoreDObject, Engine and DurinEd consumers. The
  bounded set `@domain=reflection+transaction+world+editor-operation+garbage-collection+viewport+asset-package+material-runtime,kind=contract+feature`
  covered 23 targets. Its 22 non-material targets passed in
  `20261110-192933-006276-42696-ctest.log`. After migrating material callback tests
  and accepting an already-collected parent package in fixture cleanup,
  `MaterialRuntimeTests` passed all 95 cases in
  `20261110-193144-057098-36904-MaterialRuntimeTests.log`.
- `@domain=gameplay+road-graph+road-scene` passed all five targets, including
  SandboxGameplayTests and RoadEditorIntegrationTests, in
  `20261110-193239-127848-18824-ctest.log`.
- The all build passed in `20261110-193448-772643-17328-cmake.log` after
  fixing explicit lifecycle-header dependencies in LevelEditor and DurinPython.
  `@domain=python` then passed all four integration targets in
  `20261110-193603-004568-39468-ctest.log`.
- `FLaunchStartupCharacterizationTests.BoundedTickExitUsesNormalApplicationShutdown`
  passed in characterization mode in `20261110-193644-702344-37164-ctest.log`.
  It uses project-browser startup and exits through normal shutdown after two ticks.
- Changed-document validation, all-plan validation and `git diff --check` passed.
- Recursive Collect death coverage and recursive Try/request preservation remain
  in CoreObjectTests; construction failure tests now assert logical failure and
  pending requests before explicit host collection. Business reentry, exception
  cleanup, reference ownership and finish-destroy readiness coverage passed.
- Production and test searches across all three workspace projects found no
  remaining deferral class, query or counter references. Historical completed
  plans retain their original decision record.

Passing results are reused for unchanged source, fixtures and environment.
The material fixture correction does not alter other targets. GPU qualification
and the full startup characterization suite are outside this plan's gates.
