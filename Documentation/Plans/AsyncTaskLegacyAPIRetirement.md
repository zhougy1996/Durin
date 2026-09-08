# Async Task Legacy API Retirement Plan

Summary: Migrate remaining production callers and tests to the Tasks composition API, then remove legacy task entry points, result models, and compatibility branches while preserving the shared scheduler kernel and owner lifecycle semantics.

Last reviewed: 2026-09-08

Status: Active
Completed:

## Current Status

The migration and removal approach is selected; implementation has not started.
The review baseline is `3e18b1eee21db02aae857750f9d9f45dcff3eda4`.
Thumbnail decoding already uses typed tasks; that fix does not establish that
migration of all asynchronous interfaces is complete. Production consumers still
use legacy launch functions, shared result handles, and outcome continuations.
This plan covers the remaining migration, preservation of test coverage, and
legacy API removal. Creating this plan does not start its implementation stages.

## Goal

Business code constructs tasks and composes dependencies through `Durin::Tasks`,
handling admission, terminal state, failure information, and explicit result
ownership. Completion removes the parallel legacy launch/outcome API available
to business callers and the deferred construction checks retained for its
compatibility. The scheduler kernel still used by the new API remains intact.

## Scope And Decisions

- Include the Core task API, remaining Engine/Editor callers, Launch smoke
  diagnostics, related tools and tests, and legacy interface descriptions in
  the [task system contract](../Runtime/Core/TaskSystem.md).
- Do not rewrite the scheduler, unify rendering-thread commands, migrate services
  into Subsystems, or change asset formats, build algorithms, or cache policy.
- Use `Tasks::TrySpawn`, `Then`, `ThenAsync`, `ThenCompleted`, and `WhenAll`, with
  `TTask<T>`/`TSharedTask<T>` results. Sharing is explicit; failed or canceled
  tasks must not access successful results.
- Handle admission failure at the call site. Success, failure, cancellation, and
  admission rejection must all settle owner state and release reservations.
  Mandatory cleanup must not depend solely on a continuation that can itself be
  canceled or rejected.
- Preserve module scopes, code leases, cancellation identity, request serials,
  generations, latest-wins behavior, and GameThread publication boundaries. Where
  composition lacks equivalent behavior, add the minimum required capability or
  equivalent owner control before migrating the consumer. Removing an API must
  not discard these constraints.
- Preserve cleanup after partial submission failure: already accepted child
  tasks remain owned and are drained; a later admission failure must not release
  their captured objects or module code prematurely.
- `FTaskHandle`, `TUniqueTaskHandle`, scopes, result storage, and underlying
  `Private::TryLaunch*` operations are reused by the new framework. Do not remove
  them based on their names. Limit internal visibility changes to those justified
  by the dependency audit.
- Do not classify `WaitTask`, `CancelTask`, underlying ParallelFor operations,
  scheduler startup/shutdown, or the deferred pump as legacy merely because they
  are older. Stage 0 fixes their retained boundaries. Migration must not silently
  change existing CPU execution policy.

## Initial Migration Inventory

These findings describe the review baseline. Recheck all version-controlled
source, tools, and tests before implementation.

| Category | Baseline consumers or location | Disposition |
| --- | --- | --- |
| Legacy unique construction/consumption | `LaunchUniqueTask`, `LaunchUniqueCancelableTask`, `ConsumeThen`, `ConsumeThenOutcome`; only test callers found | Migrate meaningful ownership, exception, and capacity coverage to the new API, then remove the entry points |
| Legacy aggregate outcomes | `WhenAllOutcome`, `TTaskAggregateOutcome`; only test callers found | Distinguish successful aggregation from all-terminal-state observation; preserve failure selection and input ownership coverage before removal |
| Legacy typed launch | ContentBrowserModel, SourceReferenceIndex, EnvironmentLightingBuild | Adopt explicit unique/shared results and admission handling |
| Legacy terminal continuations | AssetCompatibilityAudit uses `ThenOutcome`/`FTaskOutcome` | Migrate while preserving generation/serial filtering, cancellation, and owner cleanup |
| Legacy void launch | MaterialCompileLifecycle, StaticMeshCompilingManager, CookedMeshLoadManager, TaskSchedulerLifecycleSmoke | Use new construction entry points while preserving owner polling, scopes, and shutdown order |
| Legacy supporting types and helpers | `TTaskHandle<T>`, `TTaskResultState<T>`, `FTaskOutcome<T>`, old `Then`/`WhenAll` overloads and conversion helpers | Remove after the last consumer and dependency disappear; do not add forwarding aliases |
| Compatibility execution paths | `Private::Launch*` wrappers and paths disabling `bValidateConstruction` | Remove after entry-point migration; preserve legitimate differences still needed by the kernel |

## Implementation Stages

### Stage 0: Fix The API Inventory And Behavior Mapping

- [ ] Recheck the callers above and inventory legacy API use in project source, tools, tests, and templates. Record removal, migration, and kernel-retention lists.
- [ ] Map generation/coalescing, prerequisites, borrowed scopes, blocking I/O, result-byte estimates, wait restrictions, and diagnostics. Identify where each missing capability belongs.
- [ ] Map legacy tests to retained behavior coverage, especially unique claims, fan-in failure precedence, cancellation, capacity rejection, module unloading, and deferred shutdown.
- [ ] Query configured test targets and record the smallest validation selections for later stages; do not infer executable targets from source directory names.

Completion condition: every legacy API has a disposition and every consumer has
an ownership/terminal-state mapping. An unresolved mapping blocks removal of the
corresponding interface.

### Stage 1: Migrate Production Tasks And Owner Cleanup

Depends on Stage 0. Add the minimum capabilities required by the mapping before
migrating consumers in groups organized by owner.

- [ ] Migrate typed launches in ContentBrowserModel, SourceReferenceIndex, and EnvironmentLightingBuild; handle partial fan-out admission failure and result lifetimes.
- [ ] Migrate AssetCompatibilityAudit while preserving generation/serial invalidation, owner shutdown, and terminal publication; ensure cleanup still occurs when a continuation does not run.
- [ ] Migrate void launches in the material, StaticMesh, and CookedMesh managers, preserving accounting, result delivery, cancellation, module draining, and resource publication order.
- [ ] Review the already migrated thumbnail and Texture compilation paths; change them only where a contract gap is found.
- [ ] Add or adjust failure coverage for rejection, exceptions, cancellation, stale results, owner shutdown, and module unloading; run the relevant behavior checks.

Completion condition: production consumers no longer use retired entry points;
there are no stuck loading/pending states, leaked reservations, stale
publications, or premature releases of captured objects.

### Stage 2: Migrate Tests And Startup Diagnostics

Depends on Stage 1. Tests may migrate alongside their consumers, but all gates
in this stage must still be satisfied.

- [ ] Migrate TaskSchedulerLifecycleSmoke while preserving observations of shutdown, failed dependencies, wait restrictions, and rejection diagnostics.
- [ ] Migrate legacy calls in ThreadingTests, TaskCompositionTests, and related integration/qualification fixtures.
- [ ] Consolidate duplicate coverage while preserving the Stage 0 behavior matrix. Replace legacy dispatch-time rejection tests with admission-time rejection coverage and retain coverage of actual dispatch failures where needed.
- [ ] Define boundaries for tests that still exercise the kernel directly; do not restore public legacy wrappers solely for testing.

Completion condition: no tests or tools consume retired entry points; coverage
proves behavior rather than continued existence of legacy types.

### Stage 3: Remove Legacy APIs And Compatibility Branches

Depends on Stage 2.

- [ ] Remove legacy unique/typed launch, consumption, and outcome/aggregation APIs, together with states, helpers, friends, and includes used only by them.
- [ ] Remove legacy `Private::Launch*` wrappers and compatibility paths that disable construction validation; check for unused parameters and redundant result models.
- [ ] Retain internal handles, result storage, and scheduler primitives actually used by the new framework. Do not substitute same-name aliases, deprecated wrappers, or new global forwarding entry points for removal.
- [ ] Search version-controlled source for zero references to retired symbols and verify that retained kernel dependencies match the inventory.

Completion condition: business callers have only the selected new model; no
execution semantics or isolated implementations remain solely for legacy calls.

### Stage 4: Update Contracts And Complete Validation

Depends on Stage 3.

- [ ] Update TaskSystem, affected AssetCompilation and editor asynchronous contracts, and active examples; do not mechanically rewrite historical archives.
- [ ] Run affected validation for the change set and integration tests covering changed behavior; record actual commands, selections, results, and limitations.
- [ ] Compile related tools, Launch smoke diagnostics, and qualification targets whose fixtures migrated. Fixture changes alone do not require GPU performance qualification.
- [ ] Review the complete diff from the baseline to the final implementation, verify production/test entry-point removal, failure coverage, and documentation consistency, then complete this plan.

Completion condition: all required checks pass, the removal audit finds no
residual legacy use, and lasting contracts reside in their owning documents.
Required gates that have not run remain incomplete.

## Validation And Handoff

Follow the [build and run workflow](../Agents/BuildAndRun.md),
[testing workflow](../Agents/Testing.md), and
[documentation workflow](../Agents/Documentation.md). This plan does not require
new GPU performance runs or application-hosted tests. If executor selection,
parallel execution policy, or memory budgets actually change, add the relevant
CPU qualification gates explicitly and record the reason.

For each stage, record passed failure scenarios, test evidence, and remaining
callers. Compilation alone does not establish lifecycle correctness.
Implementation commits update this plan and carry accurate Plan/Stage trailers;
check off tasks only after their acceptance conditions pass.

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Public/Threading/TaskComposition.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserModel.cpp`
- `Engine/Source/Editor/DurinEd/Private/Source/SourceReferenceIndex.cpp`
- `Engine/Source/Editor/MainFrame/Private/AssetCompatibilityAudit.cpp`
- `Engine/Source/Runtime/Engine/Private/EnvironmentLighting/EnvironmentLightingBuild.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialCompileLifecycle.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshCompilingManager.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/CookedMeshLoadManager.cpp`
- `Engine/Source/Runtime/Launch/Private/Diagnostics/TaskSchedulerLifecycleSmoke.cpp`
