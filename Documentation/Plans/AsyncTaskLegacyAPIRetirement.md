# Async Task Legacy API Retirement Plan

Summary: Establish a LaunchTask API that accepts valid work during normal operation and returns TTask directly, migrate callers and tests, and retire legacy task semantics while preserving result ownership and owner lifetimes.

Last reviewed: 2026-09-08

Status: Active
Completed:

## Current Status

The migration and removal approach is selected; implementation has not started.
The review baseline is `3e18b1eee21db02aae857750f9d9f45dcff3eda4`.
Thumbnail decoding already uses typed tasks; that fix does not establish that
migration of all asynchronous interfaces is complete. Production consumers still
use legacy launch functions, shared result handles, and outcome continuations.
The selected API contract was revised on 2026-09-08: ordinary callers submit
through `Tasks::LaunchTask` and receive `TTask<T>` directly. They do not handle
scheduler admission or declare scheduler payload budgets. This supersedes the
earlier requirement to migrate business callers to `TrySpawn` and an outer
`TTaskAdmission` result. Queueing and owner resource policy must implement this
contract before consumer migration; renaming the existing function is insufficient.
This revision changes the plan only; no implementation stage has started.

## Goal

Business code submits valid work through `Durin::Tasks::LaunchTask`, receives a
`TTask<T>` (`TTask<void>` for void work), and handles execution results and
business-relevant cancellation. Scheduler capacity and bookkeeping do not become
routine per-call error branches. Preserve explicit unique/shared result ownership
and remove the parallel legacy launch/outcome semantics. The final `LaunchTask`
name denotes the new contract, not a compatibility wrapper around the old API.

## Scope And Decisions

- Include the Core task API, remaining Engine/Editor callers, Launch smoke
  diagnostics, related tools and tests, and legacy interface descriptions in
  the [task system contract](../Runtime/Core/TaskSystem.md).
- Limit scheduler changes to those needed for the acceptance and queueing contract;
  do not replace the scheduling algorithm, unify rendering-thread commands, migrate services
  into Subsystems, or change asset formats, build algorithms, or cache policy.
- Use `Tasks::LaunchTask`, `Then`, `ThenAsync`, `ThenCompleted`, and `WhenAll`, with
  `TTask<T>`/`TSharedTask<T>` results. Sharing is explicit; failed or canceled
  tasks must not access successful results.
- Ordinary construction and composition return task handles directly, without
  an outer admission result. Valid roots, continuations, fan-in, completion
  sources, and child submissions must not reintroduce routine capacity rejection
  through another public entry point. Stage 0 fixes their exact signatures.
- Success, failure, and cancellation must settle owner state and release
  reservations. Mandatory cleanup remains at the owner lifecycle boundary and
  cannot depend solely on an optional continuation running.
- Preserve module scopes, code leases, cancellation identity, request serials,
  generations, latest-wins behavior, and GameThread publication boundaries. Where
  composition lacks equivalent behavior, add the minimum required capability or
  equivalent owner control before migrating the consumer. Removing an API must
  not discard these constraints.
- Preserve cleanup after partial infrastructure submission failure: already accepted child
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

## Submission And Resource Contract

- A valid submission while its scheduler and owner scope are open is accepted.
  Busy workers or ordinary scheduler saturation queue work rather than return an
  invalid handle, a failed task disguising admission rejection, or an admission
  error to business code. Acceptance is distinct from execution success.
- Acceptance must not be implemented as an assertion around `TrySpawn`, a retry
  loop, blocking the GameThread until capacity frees, or running the callable
  inline and changing its executor/reentrancy contract. Accepted deferred work
  remains scheduled until executed or explicitly canceled during retirement.
- Task storage and queue accounting belong to the scheduler. Large payload
  limits, active-compute budgets, request coalescing, and producer throttling
  belong to the resource-owning managers. Pending requests may wait in an owner
  queue before task creation. Ordinary call sites do not estimate captured or
  retained result bytes merely to obtain a valid task.
- This is not an unlimited-memory guarantee. Stage 0 must define pending-work
  storage, overload diagnostics, and the repository-consistent fatal allocation
  failure policy. Genuine allocation failure is not a routine recoverable
  admission branch in every business caller. Do not silently drop work or remove
  all resource budgets to claim acceptance.
- Owners stop producing work before closing scopes, then cancel or drain already
  accepted work while captures and module code remain alive. Submitting after
  closure is a lifecycle violation, diagnosed at the boundary; invalid parameters
  and dependencies are programming errors constrained by types or assertions.
  Define the submission/closure linearization point and behavior in non-check
  builds. Already accepted work must reach a terminal state.
- Retain checked admission only behind an internal `TryLaunchTask` or equivalent
  kernel operation for explicitly identified infrastructure that genuinely needs
  recoverable rejection. Keep its structured errors and partial-submission
  cleanup. Stage 0 records the bounded consumer list and justification; ordinary
  feature code must not use this escape hatch to avoid the new contract.

## Initial Migration Inventory

These findings describe the review baseline. Recheck all version-controlled
source, tools, and tests before implementation.

| Category | Baseline consumers or location | Disposition |
| --- | --- | --- |
| Legacy unique construction/consumption | `LaunchUniqueTask`, `LaunchUniqueCancelableTask`, `ConsumeThen`, `ConsumeThenOutcome`; only test callers found | Migrate meaningful ownership, exception, and capacity coverage to the new API, then remove the entry points |
| Legacy aggregate outcomes | `WhenAllOutcome`, `TTaskAggregateOutcome`; only test callers found | Distinguish successful aggregation from all-terminal-state observation; preserve failure selection and input ownership coverage before removal |
| Legacy typed launch | ContentBrowserModel, SourceReferenceIndex, EnvironmentLightingBuild | Adopt the new LaunchTask contract with explicit unique/shared results |
| Legacy terminal continuations | AssetCompatibilityAudit uses `ThenOutcome`/`FTaskOutcome` | Migrate while preserving generation/serial filtering, cancellation, and owner cleanup |
| Legacy void launch | MaterialCompileLifecycle, StaticMeshCompilingManager, CookedMeshLoadManager, TaskSchedulerLifecycleSmoke | Use new construction entry points while preserving owner polling, scopes, and shutdown order |
| Legacy supporting types and helpers | `TTaskHandle<T>`, `TTaskResultState<T>`, `FTaskOutcome<T>`, old `Then`/`WhenAll` overloads and conversion helpers | Remove after the last consumer and dependency disappear; do not add forwarding aliases |
| Compatibility execution paths | `Private::Launch*` wrappers and paths disabling `bValidateConstruction` | Remove after entry-point migration; preserve legitimate differences still needed by the kernel |
| Current composition admission surface | `Tasks::TrySpawn` and admission-returning composition/source/child construction | Establish direct task returns for ordinary callers; isolate justified checked admission internally |

## Implementation Stages

### Stage 0: Fix The API Inventory And Behavior Mapping

- [ ] Recheck the callers above and inventory legacy API use in project source, tools, tests, and templates. Record removal, migration, and kernel-retention lists.
- [ ] Map generation/coalescing, prerequisites, borrowed scopes, blocking I/O, result-byte estimates, wait restrictions, and diagnostics. Identify where each missing capability belongs.
- [ ] Fix the direct-return signatures for LaunchTask and all ordinary composition/source/child construction; inventory the justified internal checked-admission consumers and the final namespace/name mapping.
- [ ] Specify pending-work storage, manager throttling, overload diagnostics, allocation failure, submission/closure ordering, and non-check-build lifecycle diagnostics. Map existing scheduler and deferred capacity limits to the selected acceptance policy.
- [ ] Map legacy tests to retained behavior coverage, especially unique claims, fan-in failure precedence, cancellation, capacity rejection, module unloading, and deferred shutdown.
- [ ] Query configured test targets and record the smallest validation selections for later stages; do not infer executable targets from source directory names.

Completion condition: every legacy API has a disposition and every consumer has
an ownership/terminal-state mapping. An unresolved mapping blocks removal of the
corresponding interface. Queueing, shutdown, and overload policy must be concrete
before implementing the public acceptance promise.

### Stage 1: Establish The Public Acceptance Contract

Depends on Stage 0; complete before migrating production callers.

- [ ] Implement Tasks::LaunchTask with a direct TTask result, and apply the same ordinary submission policy to composition, completion-source creation, and child tasks.
- [ ] Implement accepted-work queueing and deferred scheduling under saturation without inline execution, busy retries, or GameThread capacity waits.
- [ ] Move scheduler byte declarations/accounting out of ordinary call sites; preserve manager-owned compute/payload limits and bounded active resource use, with explicit pending-request policy.
- [ ] Isolate checked admission for the justified infrastructure consumers. Implement scope closure ordering and diagnostic behavior without abandoning accepted tasks or releasing module leases prematurely.
- [ ] Prove acceptance above the previous nonterminal/deferred limits, eventual completion and cancellation, declared executor affinity, dependency progress, and submission/closure races. Record pending storage and peak-memory behavior under bounded stress.

Completion condition: valid ordinary submissions return usable tasks and continue
to make progress under saturation; callers do not see scheduler admission details.
Resource pressure and shutdown have tested owners and explicit behavior.

### Stage 2: Migrate Production Tasks And Owner Cleanup

Depends on Stage 1. Add the minimum capabilities required by the mapping before
migrating consumers in groups organized by owner.

- [ ] Migrate typed launches in ContentBrowserModel, SourceReferenceIndex, and EnvironmentLightingBuild to direct-return LaunchTask; preserve fan-out result lifetimes and owner-level load control.
- [ ] Migrate AssetCompatibilityAudit while preserving generation/serial invalidation, owner shutdown, and terminal publication; ensure cleanup still occurs when a continuation does not run.
- [ ] Migrate void launches in the material, StaticMesh, and CookedMesh managers, preserving accounting, result delivery, cancellation, module draining, and resource publication order.
- [ ] Migrate already typed thumbnail, Texture compilation, and other composition consumers away from public admission handling and scheduler byte estimates; retain domain memory and concurrency policies.
- [ ] Add or adjust coverage for saturation, exceptions, cancellation, stale results, owner shutdown, and module unloading; run the relevant behavior checks. Retain rejection tests only for the identified internal boundary.

Completion condition: production consumers no longer use retired entry points;
there are no stuck loading/pending states, leaked reservations, stale
publications, or premature releases of captured objects.

### Stage 3: Migrate Tests And Startup Diagnostics

Depends on Stage 2. Tests may migrate alongside their consumers, but all gates
in this stage must still be satisfied.

- [ ] Migrate TaskSchedulerLifecycleSmoke while preserving shutdown, failed dependencies, and wait restrictions; replace ordinary capacity-rejection expectations with acceptance/progress checks and test lifecycle violations at their explicit boundary.
- [ ] Migrate legacy calls in ThreadingTests, TaskCompositionTests, and related integration/qualification fixtures.
- [ ] Consolidate duplicate coverage against the revised Stage 0 matrix. Replace public capacity-rejection tests with saturation, pending-work, and progress tests; retain structured rejection coverage for internal checked admission and real execution-failure coverage.
- [ ] Define boundaries for tests that still exercise the kernel directly; do not restore public legacy wrappers solely for testing.

Completion condition: no tests or tools consume retired entry points; coverage
proves behavior rather than continued existence of legacy types.

### Stage 4: Remove Legacy APIs And Compatibility Branches

Depends on Stage 3.

- [ ] Remove legacy unique/typed launch, consumption, and outcome/aggregation APIs, together with states, helpers, friends, and includes used only by them.
- [ ] Remove legacy `Private::Launch*` wrappers and compatibility paths that disable construction validation; check for unused parameters and redundant result models.
- [ ] Retain internal handles, result storage, and scheduler primitives actually used by the new framework. The final Tasks::LaunchTask is the authoritative new implementation, not a legacy alias; do not retain TrySpawn as a second public synonym or restore global compatibility entry points.
- [ ] Audit fully qualified symbols and signatures, not the LaunchTask spelling alone. Verify zero legacy overload/type references, no admission wrappers or scheduler-byte boilerplate in ordinary consumers, and an exact match to the retained internal consumer inventory.

Completion condition: business callers have only the selected new model; no
execution semantics or isolated implementations remain solely for legacy calls.

### Stage 5: Update Contracts And Complete Validation

Depends on Stage 4.

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
new GPU performance runs or application-hosted tests. The acceptance-policy
change requires bounded CPU saturation and dependency-progress tests, with
pending/active memory and queue-drain evidence. Stage 0 selects the corresponding
configured targets. If executor selection, parallel execution policy, or memory
budgets change further, add the relevant CPU qualification gates explicitly and
record the reason. Do not claim performance equivalence from correctness alone.

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
