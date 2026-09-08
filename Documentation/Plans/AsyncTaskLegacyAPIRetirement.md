# Async Task Legacy API Retirement Plan

Summary: Establish a LaunchTask API that accepts valid work during normal operation and returns TTask directly, migrate callers and tests, and retire legacy task semantics while preserving result ownership and owner lifetimes.

Last reviewed: 2026-09-08

Status: Active
Completed:

## Current Status

Stage 0 is complete. Stage 1 is the next implementation stage; no runtime API
or scheduling behavior has changed yet.
The review baseline is `3e18b1eee21db02aae857750f9d9f45dcff3eda4`.
The execution checkout was audited at `7618e71a6707f747dfbf587d2667c215665c8ae0`.
Contrary to the initial inventory, its source-image thumbnail decoder still
uses legacy void launch and a result mailbox. Production consumers still
use legacy launch functions, shared result handles, and outcome continuations.
The selected API contract was revised on 2026-09-08: ordinary callers submit
through `Tasks::LaunchTask` and receive `TTask<T>` directly. They do not handle
scheduler admission or declare scheduler payload budgets. This supersedes the
earlier requirement to migrate business callers to `TrySpawn` and an outer
`TTaskAdmission` result. Queueing and owner resource policy must implement this
contract before consumer migration; renaming the existing function is insufficient.
The Stage 0 handoff below records the corrected inventory, selected signatures,
resource policy, test mapping, and configured validation targets. All runtime
acceptance, migration, and removal gates remain open.

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

- [x] Recheck the callers above and inventory legacy API use in project source, tools, tests, and templates. Record removal, migration, and kernel-retention lists.
- [x] Map generation/coalescing, prerequisites, borrowed scopes, blocking I/O, result-byte estimates, wait restrictions, and diagnostics. Identify where each missing capability belongs.
- [x] Fix the direct-return signatures for LaunchTask and all ordinary composition/source/child construction; inventory the justified internal checked-admission consumers and the final namespace/name mapping.
- [x] Specify pending-work storage, manager throttling, overload diagnostics, allocation failure, submission/closure ordering, and non-check-build lifecycle diagnostics. Map existing scheduler and deferred capacity limits to the selected acceptance policy.
- [x] Map legacy tests to retained behavior coverage, especially unique claims, fan-in failure precedence, cancellation, capacity rejection, module unloading, and deferred shutdown.
- [x] Query configured test targets and record the smallest validation selections for later stages; do not infer executable targets from source directory names.

Completion condition: every legacy API has a disposition and every consumer has
an ownership/terminal-state mapping. An unresolved mapping blocks removal of the
corresponding interface. Queueing, shutdown, and overload policy must be concrete
before implementing the public acceptance promise.

#### Stage 0 Handoff: Audited Inventory And Selected Contract

Audit date: 2026-09-08. Tracked-file searches covered the checkout, including
source, tools, tests, and templates, using the launch, outcome, consumption,
typed-handle, and `TrySpawn` symbol families. No additional tool or template
callers were found. This is design evidence, not runtime validation.

| Owner / audited files | Result and terminal-state migration | Resource and lifetime boundary |
| --- | --- | --- |
| ContentBrowserModel `.cpp` / `.h` | Use unique snapshot tasks; take only after success; clear loading and report failures/cancellation in the owner pump | Keep one items task and one directory task, latest pending items request, directory request set, navigation/mount/catalog revisions, generations, borrowed module scope, and destructor cancel/wait |
| SourceReferenceIndex `.cpp` | Explicitly share the snapshot task; publish an immutable result owner only for the current generation | Keep one service build, inspection limit, registry revision and generation checks; reset building markers for every terminal state |
| EnvironmentLightingBuild `.cpp` | Unique face/row values replace shared handles containing mutable shared pointers; collect only after successful waits | Keep the fixed six irradiance, six prefilter and sixteen LUT jobs; retain all submitted jobs until drained before propagating failure; preserve Worker execution |
| AssetCompatibilityAudit `.cpp` and model fields | Share the terminal summary and use `ThenCompleted` on GameThreadDeferred; inspect success before result access | Preserve serial mailbox, weak publication lifetime, generation token, cancellation identity, and `CancelAndDrain`; owner shutdown settles Running even if the observer never executes |
| MaterialCompileLifecycle `.cpp` | Direct void task, existing owner mailbox and terminal polling | Preserve request/consumer/concurrent-flight limits, request/result/retained-program budgets, coalesced consumers, module scope and GameThread publication |
| StaticMeshCompilingManager `.cpp` | Direct void task, owner record polling and result delivery | Preserve 32 records, 512 MiB request / 1 GiB total reservations, latest-owner rules, cancellation and scope drain before record release |
| CookedMeshLoadManager `.cpp` | Direct void build task; retain I/O request polling and completion mailbox | Preserve configured concurrent requests, in-flight/pending/completion byte budgets, pending promotion, identity checks, and stop/cancel/drain ordering |
| TextureCompilingManager `.cpp` | Remove public admission branch and scheduler byte declarations from the typed task | Preserve MaxWorkers, pending queues/count limit, InFlightByteBudget, oversized-alone rule, owner pump, terminal polling and borrowed scope |
| PackageResource `.cpp` | `Bind` accepts a direct task and explicitly shares it; transforms use direct `ThenCompleted`; remove rejected-sharing/producer bookkeeping only after all dependent ownership is replaced | Retain copied request facade, binding/retirement synchronization, shared package lifetime, cancellation callback, read-range and retained-package budgets; BlockingIO remains the read executor |
| SourceImageThumbnailCache `.cpp` | Replace void mailbox-only decode ownership with retained unique decode-result tasks and owner terminal polling | Preserve four decodes, two uploads/frame, 64 MiB cache budget, visibility priority, disk-cache policy, serials, borrowed module scope, and render upload ownership |
| TaskSchedulerLifecycleSmoke `.cpp` | Direct void tasks and composition for prerequisites; use the internal boundary only for the deliberate close probe | Preserve cross-executor shutdown and wait diagnostics; do not launch a public task after closure |

Thumbnail cleanup needs an actual behavior fix: `ActiveDecodeCount` currently
decrements only when `DecodedResults` contains a result. An exception or
pre-execution cancellation can leave loading state and a reservation behind.
Stage 2 must retire each retained decode task once on every terminal state,
including stale serials, and must not also decrement through the old mailbox.
This correction supersedes the initial claim that thumbnails were already typed.

Production execution policy remains unchanged: existing Worker filesystem work
stays Worker in this retirement; only the package read path already using
BlockingIO keeps that selection. Moving additional workloads to I/O is outside
this plan. Scheduler queueing must not change ParallelFor thresholds, batching,
priority selection, nested serial execution, or executor-specific helping.

The complete additional test/fixture inventory is `ThreadingTests.cpp`,
`TaskCompositionTests.cpp`, `AsyncOperationGroupTests.cpp`,
`ThreadingQualificationTests.cpp`, `DynamicUnloadFixtureModule.cpp`,
`AsyncTaskPilotLifecycleTests.cpp`, `BulkDataTests.cpp`,
`MaterialCompileLifecycleTests.cpp`, `SourceImageThumbnailTests.cpp`,
`WorldSubsystemTests.cpp`, and `RenderResourceLifecycleTests.cpp`.
Their owning modules retain capture fences, failed/canceled-result checks,
and module lease checks when their launch fixtures migrate.

Removal covers global `LaunchTask` / `LaunchCancelableTask` (void and typed),
`LaunchUniqueTask`, `LaunchUniqueCancelableTask`, `ConsumeThen`,
`ConsumeThenOutcome`, old `Then` / `ThenOutcome` / `WhenAll` / `WhenAllOutcome`,
`TTaskHandle<T>`, `TTaskResultState<T>`, `FTaskOutcome<T>`,
`FUniqueTaskOutcome<T>`, and `TTaskAggregateOutcome`. Remove their converters,
friend declarations and `Private::LaunchCancelableTaskWithCompletion` /
`Private::LaunchContinuationTask` wrappers after callers disappear.
Retain `FTaskHandle`, `TUniqueTaskHandle`, `TUniqueTaskResultState`, native scope,
cancellation, generation/coalescing, attribution, runtime access, and result
claim machinery used by composition. Retain WaitTask/WaitAll/CancelTask,
ParallelFor, scheduler lifecycle, deferred pumping, and module drain primitives.
Public `TrySpawn`, `TrySpawnChild`, admission-returning Share/composition/source
creation, `FTaskGroup::TryCreate`, and admission-returning inner callback support
are also removed, without compatibility aliases.

Selected signatures below use existing result deduction and `TTaskValue<void>`
(`monostate`) rules. `Options` means `const FTaskExecutionOptions&`; `F` is
forwarded. All symbols are in `Durin::Tasks` unless qualified otherwise.

| Construction / composition | Direct result |
| --- | --- |
| `LaunchTask(FTaskGroup&, ETaskExecutor, Options, F&&)` | `TTask<T>`; callable accepts no arguments or `FTaskContext&` |
| `FTaskContext::LaunchChild(ETaskExecutor, Options, F&&)` | `TTask<T>`; invocation-scoped parent authority |
| `FTaskGroup()` / `FTaskGroup(FTaskScopeToken)` | Owned group / borrowed open scope; no checked public factory |
| `TCompletionSource<T>::Create(FTaskGroup&, Options)` | `TCompletionSource<T>`; unknown external requirements by default |
| `Share(TTask<T>&&)` | `TSharedTask<T>`; unique input is relinquished |
| `Then(TTask<T>&&, ETaskExecutor, Options, F&&)` | `TTask<U>`; consumes success; optional context precedes value |
| `Then(const TSharedTask<T>&, ETaskExecutor, Options, F&&)` | `TTask<U>`; immutable successful input |
| `ThenCompleted(const TSharedTask<T>&, ETaskExecutor, Options, F&&)` | `TTask<U>`; callback receives the shared task in any terminal state |
| `ThenAsync(TTask<T>&&, ETaskExecutor, Options, F&&)` returning unique inner | `TTask<U>`; callback returns `TTask<U>` |
| Same with shared inner | `TTask<std::shared_ptr<const TTaskValue<U>>>`; cancellation stays local to observer |
| `WhenAll(vector<TTask<T>>&&, Executor = Worker, Options = {})` | `TTask<vector<TTaskValue<T>>>` |
| `WhenAll(tuple<TTask<Ts>...>&&, Executor = Worker, Options = {})` | `TTask<tuple<TTaskValue<Ts>...>>` |
| `WhenAll(const vector<TSharedTask<T>>&, Executor = Worker, Options = {})` | `TTask<vector<shared_ptr<const TTaskValue<T>>>>` |

Keep completion-source `TakeTask` and `TrySetValue` / `TrySetFailure` /
`TrySetCanceled`: single-publication races are distinct from admission.
Scope-token source construction and the known-requirements flag become Detail
operations for ThenAsync and empty fan-in. Empty fan-in uses the current
scheduler lifetime and produces an immediately successful empty result.
Then rejects task-returning callbacks; use ThenAsync to flatten them.

Add `Prerequisites` as completion observations, `GenerationToken` and
`CoalescingKey` to execution options, translating them to existing kernel
dependencies and deferred behavior. Continuations inherit their primary scope;
cross-scope dependencies retain their own lifetimes without changing ownership.
Preserve debug name, priority, attribution and cancellation. Remove capture and
result byte estimates from these public options; measure scheduler-owned node,
callable, dependency-array and inline result storage internally. Do not claim
that these measurements include heap payloads behind pointers or containers.

The final checked-admission allowlist is deliberately narrow:

- `Durin::Private::TryLaunchCancelableTaskWithCompletion` and
  `TryLaunchContinuationTask` retain structured errors and strict validation.
  Composition's internal construction may share this kernel with a private
  accepted-work policy; ordinary clients cannot select checked capacity policy.
- `ParallelForCancelable` in `Task.cpp` uses checked root launch for its bounded
  infrastructure chunks. Preserve cancellation on a later rejection and WaitAll
  of every previously accepted chunk before returning or destroying captures.
- Core admission/allocation/closure tests and the deliberate scheduler-close
  probes in Launch smoke and RenderResourceLifecycleTests exercise that boundary.
  Ordinary fixture setup uses LaunchTask. No Engine/Editor feature, package
  adapter, or module-unload fixture gets a checked-admission exemption.

Accepted storage and overload policy for Stage 1:

1. Publish every valid ordinary node into scheduler-owned lifetime/scope
   tracking, including dependency-waiting and external-source nodes. Existing
   worker priority queues retain ready work. Pending nodes are separately
   counted from running bodies; worker and I/O thread counts remain hard active
   compute limits. Do not gate a dependency or a helped child behind a slot
   held by its waiting parent.
2. `MaxNonterminalTasks` (16,384) and `MaxBlockingIOTasks` (128) remain hard
   checked-kernel reservation limits and become overload thresholds for ordinary
   accepted nodes. Pending metadata grows with accepted work; it is not a fixed
   ring that can reject or overwrite. Preserve owner request/payload budgets
   listed above. Package payload allocation remains inside the read callable,
   with retained-result lifetime owned by its request facade and consumers.
3. Deferred work retains scheduler-owned entries until pumping or explicit
   cancellation. Keep priority/FIFO, generations, coalescing and frame item/time
   budgets. The old 1,024-entry, 8 MiB total and 1 MiB per-entry limits remain
   checked-path limits; ordinary retained-storage crossings produce overload
   diagnostics, never dispatch failure. Public entries charge actual scheduler
   storage, not guessed transitive result payload. Large domain results stay in
   owner records/mailboxes with their existing limits. Do not add a second
   hidden queue excluded from shutdown, diagnostics or scope drain.
4. Expose current/peak pending count and scheduler-owned bytes per executor,
   threshold-crossing counts and current/peak running bodies. Use fixed counters
   and rate-limited threshold transition diagnostics, with existing bounded
   attribution. Include accepted work beyond thresholds in all gauges and
   shutdown snapshots. These are storage measurements, not total process RSS.
5. Allocation failure in ordinary construction, graph registration, result
   storage creation, or dispatch is fatal through the repository's always-on
   `requiref` failure path, outside internal locks after rollback as applicable.
   Do not convert scheduler allocation failure into callable failure. Checked
   infrastructure retains recoverable allocation errors. Exceptions thrown by
   the user body still produce task failure; cancellation still produces a
   terminal canceled task. No retry loop, inline fallback or capacity wait.
6. Scheduler acceptance linearizes under its Submit mutex while accepting is
   true; scope acceptance linearizes at `TryAdmit` under the scope mutex. Retain
   submission participation until registration is complete, including rollback.
   A cancellation racing registration is reconciled by BindTask. Public
   submissions losing either close race diagnose a lifecycle violation through
   `requiref` in Debug, Release and Shipping; do not return an invalid/failed
   surrogate task. Checked race probes may observe structured rejection.
7. Preserve the counted-child exception: group Drain excludes external roots
   but a live counted parent may launch children until it retires. Cancel close
   and global scheduler close exclude new children. Already accepted dispatch
   remains enabled during drain. Owners stop producers, then close/cancel/drain,
   and retain captures and code leases through terminal hooks and result-owner
   release. Borrowing a scope does not transfer module drain authority.

Stage 1 stress uses deliberately small thresholds plus a separate bounded run
above the former default 16,384 nonterminal and 1,024 deferred limits. Hold
workers behind an event while submitting roots, dependents, sources and fan-in;
release them and verify progress. Include single-worker and single-I/O-worker
child waits, deferred affinity, canceled pending nodes and drain/cancel shutdown.
Record node counts, current/peak pending and active storage and running bodies,
plus process peak memory when available; after release require zero live
reservations/queues, distinguishing intentionally retained terminal handles.
Memory evidence must identify workload size and exclude a performance-equivalence
claim. Add public lifecycle death tests and keep checked close-race tests to
verify both sides without terminating the test host.

| Existing coverage | Retained behavior / replacement |
| --- | --- |
| FUniqueTaskTests and move-only callable tests | Unique take/Then, explicit Share, exactly-once destruction, throwing sink, canceled/dropped result; internal claim rollback and duplicate-claim diagnostics |
| FTaskFanInTests and composition fan-in tests | Unique tuple/vector order; shared duplicate inputs and pinned lifetimes; failure before cancellation, lowest failing input index in the new API; all-terminal observation through shared ThenCompleted observers |
| FTaskAdmissionTests and allocation-injection cases | Structured rejection and rollback only at the internal boundary; remove legacy validation-bypass expectations; fatal public allocation/lifecycle contracts need isolated death tests |
| FTaskCapacityTests, BlockingIOIsBoundedAndDoesNotOccupyCpuWorkers, deferred count/payload tests | Separate checked rejection from public above-threshold acceptance, bounded running bodies, deferred pump progress and pending-storage accounting |
| Scope close races, DrainAdmitsOnlyLiveParentChildrenAndJoinIncludesThem, AsyncContextSpawnsChildDuringDrain | Preserve counted descendants, cancellation identity, submission publication ordering and join semantics; public post-close behavior is diagnostic |
| ThenAsync, dynamic dependency and wait tests | Unique forwarding, shared local cancellation, cycle/unknown/deferred wait restrictions; remove admission-returning callbacks, retain real execution failure and abandonment |
| Deferred generation/coalescing and shutdown suites | Same stale/superseded terminal reasons, pump affinity, time/item budgets, cancel/drain and no callbacks under locks |
| AsyncOperationGroupTests and DynamicUnloadFixtureModule | Borrowed scope, partial internal submission drain, callable/result/module lease fences and external source lifetime |
| Owner integration fixtures | No stale publications, stuck loading, leaked reservations or skipped owner cleanup when the body/observer fails or is canceled |

Configured registry queries ran with `test list task`, `core`, `async`,
`content`, `asset`, `material`, `resource`, `world`, and `render`, plus
`test explain CoreConcurrencyTests`, on `Win64-Debug-DurinEditor`.
`test list threading` had no match; directory names are not selections.
The selected lanes are:

- Stage 1: `.\DevTool.bat test CoreConcurrencyTests` for implementation iteration.
- Owner coverage discovered in the registry: ContentBrowserWorkflowTests,
  EditorAssetWorkflowTests, ThumbnailTests, TextureTests, MaterialTests,
  StaticMeshTests, CookedMeshLoadingTests, AssetPackageTests, WorldTests,
  RenderContractTests and AsyncTaskPilotLifecycleTests. Use `test affected`
  for the changed set and explicitly include the async integration target when
  its behavior changes. Source ownership for BulkDataTests and
  RenderResourceLifecycleTests was confirmed in their CMake registrations.
- Compile CoreConcurrencyQualificationTests and DynamicDllUnloadQualificationTests
  / DynamicDllUnloadFailureQualificationTests after their fixtures migrate;
  run the dynamic-unload lifecycle selections to prove changed module ownership.
  No new CPU timing policy or GPU performance gate is selected by this audit.
- Final validation includes affected tests, changed integration/lifetime lanes,
  a full `all` compile covering Launch and tools, and documentation validation.
  No native tests or builds have run as part of this documentation-only stage.

Stage 0 validation: `.\DevTool.bat doc validate --scope changed` passed for
the plan; `git diff --check` passed. Runtime failure scenarios and saturation
measurements have been mapped, not executed. Every audited runtime and fixture
caller remains for Stages 1-4.

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
- [ ] Migrate legacy thumbnail decoding with terminal polling, and move Texture compilation and package composition away from public admission handling and scheduler byte estimates; retain domain memory and concurrency policies.
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
