# Structured Task Scopes Plan

Summary: Add explicit bounded owner scopes that close task admission, propagate cancellation, expose descendant diagnostics, and quiesce without implicit destructor waits.

Last reviewed: 2026-08-08

Status: Archived
Completed: 2026-08-08

## Current Status

All stages are complete. Structured scopes now cover bounded admission,
drain-to-cancel escalation, cooperative cancellation, post-propagation
quiescence, Worker helping, GameThread deferred wait rejection, scheduler-wide
weak scope tracking and shutdown closure, bounded diagnostics, and numeric
profiler correlation. AssetImport request work and source-image thumbnail decode
work now use explicit owner scopes. Qualification, lasting runtime contracts,
profiling parity, lifecycle smoke, and M4 roadmap closure are complete.

The completed [Task System Audit Remediation](TaskSystemAuditRemediation.md)
established `FTaskSchedulerConfig`, the default 16,384-node process capacity,
the final post-propagation terminal hook, and non-nested deep snapshot locking
used by Stage 1. The frozen scope contract remains valid with the integration
clarifications below.

The frozen contract uses a move-only `FTaskScope` controller and a
copyable `FTaskScopeToken`, appends explicit scope selection to existing option
types, forbids scoped descendants from silently changing owner scopes, and
balances each accepted node at `FTaskScheduler::Submit` and the end of
`FTaskStateData::FinishTerminalPublication`. Close-and-drain may escalate to
close-and-cancel, scope destruction never waits, and bounded diagnostics retain
at most 64 nonterminal task snapshots.

AssetImport uses one request scope while retaining its owner/provider admission,
mailbox, latest-wins selection, and explicit result taking. The source-image
thumbnail cache uses one cache-lifetime scope, closes result publication,
cancels the scope, and explicitly quiesces outstanding decode tasks before
releasing asynchronous cache state. No Core API, admission-race, lock-order,
GameThread, or pilot-boundary question remains open.

## Goal

Give a subsystem owner one explicit scope for admitting related task roots and
descendants, closing future work, requesting cooperative cancellation, and
waiting for bounded quiescence at an owner-controlled lifecycle boundary. Scope
state and diagnostics must compose with the existing process scheduler,
owner/category attribution, Worker and GameThread targets, typed fan-in, and
unique-result ownership without creating another scheduler lifetime.

## Scope

- A move-safe Core scope handle/state with explicit open and closed admission.
- Deterministic association for scoped roots, inherited descendants,
  continuations, typed fan-in, unique-result sinks, and parallel-for chunks.
- Explicit close-and-drain and close-and-cancel operations with observable
  quiescence and wrong-thread/wrong-lifetime rejection.
- Bounded scope diagnostics for admission, active descendants, terminal
  outcomes, attribution, and close/quiescence state.
- Integration with scheduler shutdown and the GameThread deferred pump without
  hidden waits or executor ownership inversion.
- Production pilots for AssetImport and source-image thumbnails that preserve
  their mailbox, cache, request serial, and render/RHI policies.

## Non-Goals

- Creating a scheduler, worker pool, native thread, IO executor, priority
  policy, work stealing, fiber, coroutine runtime, or serialized lane.
- Blocking from a scope destructor or treating RAII destruction as owner
  shutdown.
- Making cancellation preemptive or promising interruption of blocking platform
  and third-party calls.
- Replacing task attribution, subsystem mailboxes, latest-wins policy,
  GameThread pumping, or render/RHI command ownership.
- Persisting scope ids, retaining completed task history, or accepting dynamic
  per-request names as scope identity.
- General task-group result aggregation, `WhenAny`, or multi-stage unique-result
  production.

## Design Decisions and Invariants

### Ownership and lifetime

- A scope is an owner-controlled admission and quiescence boundary inside one
  running process scheduler lifetime; it never owns or restarts that scheduler.
- Scope destruction is non-blocking. An owner explicitly closes and quiesces at
  a lifecycle point where its required executors can still make progress.
- Scope state outlives public handle copies only while accepted descendants or
  explicit observers retain it. Completed descendant records are not retained.
- Attribution remains the cost identity. A scope may summarize existing
  attribution but does not replace owner/category tokens or create unbounded
  profiler names.

### Admission and descendants

- Admission has one linearizable close boundary. A root or descendant either
  participates before close and is counted exactly once, or is rejected without
  becoming hidden scope work.
- Descendants inherit their executing task's scope unless an explicit public
  rule selected in Stage 0 permits another scope. Additional prerequisites do
  not merge scope ownership.
- Continuations, typed fan-in, unique sinks, and parallel-for chunks follow the
  same selected scope as ordinary task nodes. No callback runs inline to evade
  admission.
- Scope bookkeeping never holds a scope lock while moving or destroying user
  callables, resolving labels, invoking profiler code, or entering scheduler and
  executor queues in an inverted order.

### Close, cancellation, and quiescence

- Close prevents future scoped admission. Cancel additionally requests
  cooperative cancellation of accepted descendants; drain preserves their
  existing execution semantics.
- Quiescence means every accepted descendant and terminal propagation step has
  left the scope. Worker-queue idle and zero root handles are insufficient.
- GameThread cannot wait for scoped `GameThreadDeferred` work through ordinary
  task waits. The engine shutdown coordinator remains the only generic
  pump-until-quiescent path; subsystem operations must use an explicitly safe
  owner boundary selected in Stage 0.
- Scope cancellation never changes task terminal precedence: callback failure
  remains failure, running cancellation waits for callable return, and terminal
  publication remains single-winner.

### Bounds and diagnostics

- Scope diagnostics use fixed counters/gauges and bounded nonterminal snapshots;
  they retain no completed task history, raw samples, or dynamic labels.
- Scope active counts reconcile with its accepted and terminal outcomes under
  admission/close/cancel races. Scheduler owner/category aggregates remain the
  authoritative process-wide cost totals.
- Scheduler shutdown closes every live scope before executor teardown and leaves
  no admitted scope work nonterminal.

## Current Foundations and Gaps

- Task admission already has a linearizable scheduler close boundary and one
  winning terminal transition.
- Executing roots inherit task attribution, while continuations and typed
  composition inherit their primary predecessor; the same sites can carry a
  frozen scope association after Stage 0 selects the public contract.
- Scheduler diagnostics already track bounded nonterminal task snapshots and
  fixed owner/category aggregates without completed history.
- Cross-executor shutdown already closes root admission, drains Worker and
  GameThread graphs, and rejects recursive GameThread pumping.
- AssetImport owns explicit close/cancel/wait/drain operations. The thumbnail
  cache owns request rejection and result-publication closure but intentionally
  detaches in-flight decode work through weak state.
- Core has no owner scope object, descendant counter, scoped close race, or
  scope-specific quiescence diagnostic.

## Stage 0 Frozen Contract

### Public types and source compatibility

- `FTaskScope` is the move-only, default-constructible owner controller.
  `CreateTaskScope()` returns a valid controller only while the process task
  scheduler is running. The controller exposes `IsValid()`, `GetToken()`,
  `Close(ETaskScopeCloseMode)`, `Wait()`, `WaitFor(double TimeoutSeconds)`, and
  `GetDiagnostics()`. Copy construction and copy assignment are deleted; move
  transfers the one controller reference without changing scope state.
- `FTaskScopeToken` is an opaque, default-constructible, copyable, movable, and
  equality-comparable association token. It retains the scope state for later
  admission but exposes no close or wait operation. A default token means no
  explicit scope. Tokens are process-local and are not reused across scheduler
  lifetimes.
- `ETaskScopeCloseMode` contains `Drain` and `Cancel`.
  `ETaskScopeCloseResult` distinguishes `Closed`, `EscalatedToCancel`,
  `AlreadyClosed`, and `Invalid`. `ETaskScopeWaitResult` distinguishes
  `Quiescent`, `TimedOut`, `ScopeOpen`, `UnsupportedThread`, and `Invalid`.
  `Wait()` is the unbounded form; it still returns a result so rejected waits
  are observable rather than assertions with side effects.
- `ETaskScopeState` contains `Invalid`, `Open`, `ClosingDrain`,
  `ClosingCancel`, `QuiescentDrain`, and `QuiescentCancel`.
  `FTaskScopeDiagnostics` reports this state, a monotonically assigned
  `ScopeId`, accepted/rejected/succeeded/failed/canceled counts, current and
  peak active nodes, a nonterminal snapshot vector, and its truncation count.
- `FTaskDiagnostics` appends the selected `ScopeId`, with zero meaning
  unscoped. The existing enqueue, execution, and terminal profiler annotations
  carry the same numeric id. Tracy-disabled adapters remain fixed-width no-ops;
  scope ids never create source locations, plot names, or dynamic labels.
- A `FTaskScopeToken Scope` field is appended after `Attribution` in
  `FTaskLaunchOptions`, `FTaskContinuationOptions`, and `FParallelForOptions`.
  Existing defaults and positional aggregate initialization therefore remain
  source compatible. Existing APIs gain no required overload and unscoped
  callers retain current scheduling, cancellation, and diagnostic behavior.
- V1 exposes no dynamic scope name, deadline, result aggregate, or scope
  options structure. Task owner/category attribution remains the only label and
  cost identity; a scope id is bounded process-local correlation only.

### Scope selection and descendant ownership

Scope selection is resolved once before scheduler admission:

1. A continuation, typed fan-in node, or unique-result sink uses its primary
   predecessor's scope. For typed fan-in, the primary predecessor is tuple
   position zero, matching current parent and attribution selection.
2. A root launched while a scoped task is executing in the same scheduler
   inherits the executing task's scope.
3. Otherwise an explicit non-default option token selects the scope; a default
   token creates an unscoped node.

An explicit token matching the inherited scope is accepted. An explicit
different token on a scoped descendant is rejected before node creation; V1
does not permit reparenting or detachment that could make owner quiescence miss
work. Additional prerequisites may belong to other scopes but never select,
merge, retain, close, or cancel those scopes. Invalid and cross-lifetime tokens
reject deterministically.

Every scheduled `ParallelForCancelable` worker chunk receives the selected
scope in its `FTaskLaunchOptions`. The synchronous caller chunk remains in its
caller's current execution context and creates no synthetic task node or scope
charge. An external caller may supply an explicit scope for worker chunks. If
scope close rejects any worker chunk, the existing group cancellation path wins
and the operation returns `Canceled`; nested serial fallback creates no scoped
node. `ParallelForOperationCount` remains attribution-only and is not duplicated
as scope task admission.

### Admission, close, and terminal balancing

- `FTaskScheduler::Submit` validates scheduler lifetime, prerequisites, the
  authoritative process-capacity slot, and scope lifetime, then calls the
  selected scope's `TryAdmit` while holding the scheduler mutex. Scope
  acceptance does not create a second process-capacity tracker. `TryAdmit` and
  `Close` serialize on the scope mutex: either admission increments
  accepted/active exactly once before node creation, or close wins and the
  callable remains outside the node and scope. Construction rollback releases
  both provisional charges.
- Scope association is stored in `FTaskStateData`. Construction rollback before
  insertion releases the provisional scope charge; after insertion there is no
  admission rollback. A rejected request carrying a valid scope increments its
  scope rejection counter without incrementing accepted or active counts.
- `FTaskStateData::FinishTerminalPublication` is the sole scope release site.
  It opens the terminal-publication barrier after the completion hook, notifies
  all direct dependents, then releases the scope immediately before the
  existing final `FTaskScheduler::OnTaskTerminal` capacity/accounting hook.
  Scope and scheduler locks are acquired separately, so a zero scope count
  cannot become visible before terminal propagation has left the scope. The
  winning terminal state supplies exactly one succeeded, failed, or canceled
  count.
- `Close(Drain)` changes `Open` to `ClosingDrain` and rejects all later
  admission. `Close(Cancel)` changes `Open` to `ClosingCancel`, snapshots active
  task states, releases the scope lock, and requests ordinary cooperative
  cancellation. `Close(Cancel)` may atomically escalate `ClosingDrain` while
  work remains; drain never weakens an existing cancel close. Once quiescent,
  later closes return `AlreadyClosed`. Repeated and concurrent closes are
  idempotent under this ordering.
- A closed scope becomes quiescent when active reaches zero. Open scopes with
  zero active nodes are idle, not quiescent, and `Wait` returns `ScopeOpen`.
  Running cancellation remains active until the callable returns and terminal
  propagation completes. Failure continues to win over a cancellation request.
- Destroying the last `FTaskScope` controller while open performs a
  non-blocking cancel close and increments the scheduler's abandoned-open-scope
  diagnostic. It never waits or destroys user callables under the destructor.
  This is a safety fallback, not a successful owner lifecycle boundary.

### Locks, waiting, and scheduler shutdown

- Admission may nest the global scheduler lifetime mutex, scheduler mutex, and
  scope mutex in that order; it never enters a task-state mutex while the scope
  mutex is held. Terminal publication leaves the task-state mutex before the
  scope release and final scheduler hook, and those two locks are not nested.
  Deep diagnostics pin scheduler lifetime under the global mutex, pin task
  cohorts under the scheduler or scope mutex, then release those locks before
  resolving labels or querying task diagnostics. Scope close follows the same
  copy-then-release rule before cancellation or callable destruction. No scope
  operation enters an executor queue while holding the scope mutex.
- Scope waits are supported only after close. A non-GameThread external caller
  blocks on the scope condition variable. A Worker in the same scheduler uses
  the existing eligible-task helping path. GameThread rejects a wait while any
  scoped `GameThreadDeferred` node is nonterminal; it never pumps the deferred
  queue from `FTaskScope::Wait`.
- `ShutdownTaskSystem` remains the only generic GameThread pump-until-quiescent
  coordinator. It closes live scopes in the selected shutdown mode after root
  admission closes and before executor teardown, pumps accepted deferred work
  under the existing recursion guard, and requires every scope active count to
  reach zero before uninstalling the GameThread adapter.
- `ShutdownTaskScheduler` applies the same scope close mode for worker-only
  lifetimes. Scheduler restart never revives a token: a token from the prior
  lifetime is invalid even if its scope was already quiescent.

### Bounded diagnostics

- Each scope owns fixed counters and gauges plus weak references to its active
  nodes. `GetDiagnostics()` returns at most the 64 lowest task ids as
  `FTaskDiagnostics`; `NonterminalSnapshotTruncationCount` reports omitted
  active nodes. Completed task records, raw timing samples, scope names, and
  per-request labels are never retained.
- Scheduler diagnostics add live, open, nonquiescent, and abandoned-open scope
  counts plus total scope admission rejections. Live-scope registry entries are
  weak and are compacted only during explicit creation, snapshot, and shutdown
  operations; task admission does not grow completed-scope history.
- Scope accepted equals succeeded plus failed plus canceled plus active.
  Scope counters do not replace or alter owner/category aggregates; scoped and
  unscoped task totals continue to reconcile through the existing scheduler
  accounting sites.

### Production pilot boundaries

- AssetImport creates one request scope before launching its unique producer,
  stores the controller in `FAsyncImportRequestState`, and supplies its token on
  the producer. The outcome sink inherits that scope while retaining explicit
  `AssetImport/PublishPlan` attribution. Normal publisher completion closes the
  request scope in drain mode; rejection, supersession, owner/provider closure,
  and explicit cancel close it in cancel mode. Existing request tables,
  cancellation source, mailbox notices, latest-wins selection, provider leases,
  and result taking remain subsystem-owned. Matching drain waits on scopes
  rather than treating the publisher handle as descendant-complete evidence.
- `FSourceImageThumbnailCache::FImpl` owns one scope for its lifetime and
  supplies its token to every decode launch. `Shutdown()` first rejects new
  requests and closes `bAcceptingResults` under the async-state mutex, then
  releases that mutex, closes the scope in cancel mode, and explicitly waits
  for decode quiescence before clearing entries and releasing `AsyncState` and
  its disk cache. Decode remains Worker work; upload throttling, serial checks,
  weak result publication, and render/RHI commands remain cache-owned.
- Both pilots wait only for `AnyWorker` work and hold no mailbox, cache,
  provider, object, render, RHI, or async-state lock during scope wait. Neither
  pilot pumps GameThread deferred work or moves policy into Core.

### Qualification baseline and Stage 1 working set

The no-scope baseline is the verified M3 final cohort from completion commit
`97e046e9`, preset `Win64-Debug-DurinEditor-Tests`, Tracy off, target
`CoreConcurrencyTests`, five-run median rule. Its medians are 15,371,100 ns for
128 copyable callables, 15,009,200 ns for 128 move-only callables, 8,110,000 ns
for 32 shared 64 KiB transfers, 8,337,600 ns for 32 unique transfers,
22,195,800 ns deferred admission, 9,759,800 ns deferred pump, 20,251,606 ns
average deferred residency, 207 ms for the four-fixture process, and 112 ms for
the diagnostic snapshot fixture. Stage 4 investigates any no-scope median
regression above 10 percent using the identical cohort.

Stage 1 adds a paired qualification fixture with 128 no-op roots and a
root/child/continuation/fan-in cohort in one open-then-drained scope. It records
five scoped and unscoped samples in the same process; median scoped overhead
above 15 percent or any unreconciled scope count blocks Stage 2. This is the
first scoped implementation baseline, not a promise of release latency.

The Stage 1 write set is:

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Tests/Native/CoreTests/Private/ThreadingTests.cpp`

`LaunchEngineLoop.cpp` and `Profiling.h` are Stage 2 shutdown and profiler
integration files. The two pilot implementations and their existing test files
remain Stage 3 work. No direct dependency outside this staged working set was
found.

## Implementation Stages

### Stage 0: Freeze scope contract and baselines

Dependencies: completed M2 and M3 milestones; M3 completion commit recorded by
the Task Owner Diagnostics handoff.

- [x] Inventory root, child, continuation, fan-in, unique sink, parallel-for,
  cancellation, terminal, scheduler shutdown, and GameThread pump sites.
- [x] Freeze public type and member names, move/copy traits, option placement,
  scope association precedence, invalid/closed admission result, and source
  compatibility expectations.
- [x] Freeze close/drain/cancel/quiescence state transitions and the exact rule
  for descendants racing owner close.
- [x] Select bounded diagnostics, overflow behavior if any, lock order, and
  scheduler-shutdown integration sites.
- [x] Define the AssetImport and thumbnail pilot boundaries, including whether
  thumbnail decode is drained or remains safely detached.
- [x] Record the verified no-scope baseline, freeze the paired scoped
  qualification cohort, and record the Stage 1 working set.

#### Acceptance Gate

- No API, lifecycle, admission-race, GameThread, boundedness, or pilot ownership
  decision remains unresolved for Core implementation.
- Every scope count has one named admission and terminal balancing site.

#### Stage 0 Handoff

- Baseline commit: `97e046e9` (`feat(tasks): add bounded owner diagnostics`).
  M2 and M3 are complete; no code change follows that baseline in this
  checkout.
- Working set: Stage 1 writes only `Task.h`, `Task.cpp`, and
  `ThreadingTests.cpp`. `LaunchEngineLoop.cpp` and `Profiling.h` join in Stage
  2; AssetImport, thumbnail cache, and their tests join in Stage 3.
- Key symbols: `FTaskScheduler::Submit` owns the admission winner;
  `FTaskStateData::FinishTerminalPublication` owns the balanced release after
  dependent notification; `GCurrentTaskState` supplies root inheritance;
  `Private::LaunchContinuationTask` supplies primary-predecessor inheritance;
  `ParallelForCancelable` forwards scope selection to worker chunks; and
  `ShutdownTaskSystem` retains sole GameThread pump ownership.
- Decisions: controller/token ownership, exact selection precedence,
  cross-scope rejection, close escalation, destructor fallback, 64-entry
  diagnostic bound, lock order, wait restrictions, and both pilot boundaries
  are frozen in `Stage 0 Frozen Contract`.
- Open questions: none for Stage 1. The first valid scoped timing baseline is
  produced by the Stage 1 paired fixture because no scoped API exists at this
  baseline.
- Validation: targeted symbol inspection agreed with the M3 handoff and the
  five-file initial working set. The M3 completion cohort supplies the no-scope
  baseline; the all-plan validator and `git diff --check` passed for this
  planning update.

### Stage 1: Add bounded scope admission and inheritance

Dependencies: Stage 0 frozen contract; completed
[Task System Audit Remediation](TaskSystemAuditRemediation.md). Complete.

- [x] Implement scope state and public handles/options without changing default
  behavior for unscoped callers.
- [x] Associate roots and inherited descendants across continuations, typed
  fan-in, unique sinks, and parallel-for according to the frozen precedence.
- [x] Linearize close against admission and reject post-close work without
  retaining callables or task nodes in the scope.
- [x] Add compile-time and focused tests for traits, defaults, explicit scope
  selection, inheritance, cross-scope prerequisites, and close races.

#### Acceptance Gate

- Every accepted scoped node is charged exactly once and every post-close
  admission is rejected deterministically.
- Existing unscoped task APIs and scheduling behavior remain source compatible.

#### Stage 1 Handoff

- Baseline commit: `c2535eea` (`docs(asset): activate DAST v4 wire contract
  plan`). The task-system prerequisite is completion commit `21019339`
  (`feat(tasks): remediate task system audit findings`); intervening commits do
  not change the Stage 1 Core working set.
- Working set: Stage 1 changed `Task.h`, `Task.cpp`, `ThreadingTests.cpp`, and
  this plan. Stage 2 continues in those files and adds `Profiling.h` and
  `LaunchEngineLoop.cpp` for profiler and shutdown integration.
- Key symbols: `FTaskScope` owns the one non-copyable controller reference;
  `FTaskScopeToken` carries copyable association; `FTaskScheduler::Submit`
  resolves root inheritance or the already-selected primary-predecessor scope,
  reserves process capacity, then calls `FTaskScopeState::TryAdmit` under the
  scheduler mutex; `FTaskStateData::FinishTerminalPublication` releases the
  scope after dependent notification and before `OnTaskTerminal`.
- Decisions: a scoped executing root forbids explicit reparenting; continuation,
  typed fan-in, and unique sinks select their primary predecessor before
  admission; parallel-for forwards the selected token only to scheduled Worker
  chunks. Scope construction rollback balances provisional capacity and scope
  charges, and close/admission races reconcile accepted, rejected, terminal,
  active, and peak counts.
- Open questions: none for Stage 2. Worker helping, GameThread deferred wait
  rejection, scheduler live-scope registry/shutdown closure, abandoned-open
  diagnostics, profiler scope ids, and the full concurrent diagnostic suite
  remain explicitly owned by Stage 2.
- Validation: `FTaskScope*` passed 4/4; task-focused tests passed 58/58; full
  `CoreConcurrencyTests` passed 113/113 on
  `Win64-Debug-DurinEditor-Tests`. The five-run paired qualification recorded
  7,673,100 ns unscoped and 7,804,300 ns scoped medians (about 1.7 percent
  overhead), below the frozen 15 percent threshold, with all scope counts
  reconciled.

### Stage 2: Implement cancellation, quiescence, and diagnostics

Dependencies: Stage 1 stable scope association.

- [x] Add explicit drain/cancel close modes and bounded wait/quiescence APIs at
  supported thread boundaries.
- [x] Propagate cooperative cancellation without changing terminal precedence or
  blocking destruction.
- [x] Add bounded scope counters, gauges, nonterminal snapshots, and scheduler
  shutdown closure.
- [x] Append numeric scope correlation to per-task diagnostics and the existing
  Tracy/no-op task profiler adapters without adding dynamic cardinality.
- [x] Add races for admission versus close, child launch versus cancel, terminal
  publication, handle release, scheduler shutdown, and concurrent diagnostics.

#### Acceptance Gate

- Scope counts reach zero only after all accepted descendants and terminal
  propagation are complete, including failure and cancellation races.
- Wrong-thread and GameThread deferred waits reject rather than deadlock.

#### Stage 2 Handoff

- Baseline commit: `62a66263` (`feat(tasks): add bounded task scope admission`).
- Working set: Stage 2 changed `Task.h`, `Task.cpp`, `Profiling.h`,
  `ThreadingTests.cpp`, and this plan. No `LaunchEngineLoop.cpp` edit was needed:
  its existing shutdown calls flow through the now scope-aware
  `ShutdownTaskSystem` and `ShutdownTaskScheduler` entrypoints.
- Key symbols: `FTaskScheduler::CreateScope`, `RegisterScope`,
  `CloseLiveScopes`, and `SnapshotScopeCounts` own weak registry lifecycle;
  `FTaskScopeState::WaitFor` blocks externally, helps from a Worker waiting on a
  different scope, and rejects self-scope, rendering-thread, and scoped
  GameThread-deferred waits; `AbandonOpen` performs the controller destructor's
  non-blocking cancel fallback. `FTaskSchedulerDiagnostics` exposes live, open,
  nonquiescent, abandoned-open, and scope-rejection totals.
- Decisions: nonquiescent counts every live scope not yet in a quiescent state,
  including idle open scopes. Scope registry entries remain weak and compact
  only during creation, diagnostics, and shutdown. Scheduler shutdown closes
  all pinned live scopes after root admission closes and before executor
  teardown. Profiler enqueue, execute, and terminal events append only the
  numeric scope id; the disabled adapters remain fixed-signature no-ops.
- Open questions: none for Stage 3. The production migrations retain their
  existing mailbox, cache serial, result-taking, and render/RHI policies.
- Validation: `FTaskScopeTests.*` passed 9/9 and full
  `CoreConcurrencyTests` passed 119/119 on
  `Win64-Debug-DurinEditor-Tests`. The focused assertion side-effect audit
  returned no new invocation; its four reported findings are pre-existing
  scanner/allowlist entries shifted by this plan's source edits. The repository
  wide audit remains blocked by unrelated stale allowlist entries outside this
  working set.

### Stage 3: Migrate production owners

Dependencies: Stage 2 lifecycle semantics.

- [x] Migrate AssetImport owner/provider shutdown to a scope while retaining its
  request table, mailbox, unique result sink, latest-wins policy, and explicit
  result taking.
- [x] Migrate the selected source-image thumbnail boundary while retaining cache
  serial validation, decode/upload throttling, weak publication, and render/RHI
  ownership.
- [x] Add focused owner shutdown, late publication, cancellation, destruction,
  and restart tests for both pilots.
- [x] Capture diagnostics proving each owner closes admission and reaches its
  selected quiescence/detachment contract.

#### Acceptance Gate

- Both production owners shut down without hidden work, post-close publication,
  GameThread pumping inversion, or policy migration into Core.

#### Stage 3 Handoff

- Baseline commit: `5428947b` (`feat(tasks): complete structured scope
  lifecycle`).
- Working set: Stage 3 changed `AsyncImport.cpp`,
  `SourceImageThumbnailCache.cpp`, their existing `AssetImportCoreTests.cpp` and
  `SourceImageThumbnailTests.cpp` pilot suites, and this plan. Core scope APIs
  required no change.
- Key symbols: `FAsyncImportRequestState::Scope` owns one request controller;
  `FAsyncImportCoordinator::Launch`, `RequestCancellation`, and
  `CancelAndDrainMatching` own scoped admission, cancellation publication, and
  quiescence. `FSourceImageThumbnailCache::FImpl::TaskScope` owns every decode;
  `FSourceImageThumbnailCache::Shutdown` closes result publication before
  cancel-and-wait, then clears entries and releases `AsyncState`.
- Decisions: AssetImport publishes its subsystem-owned cancellation result and
  mailbox notice before cancel-closing the scope, so cancellation of the result
  continuation cannot strand a pending handle. Normal publication drain-closes
  the scope. Matching drains wait on the scope without holding coordinator or
  request locks. Thumbnail shutdown holds the async-state mutex only while
  closing result admission; scope wait and render/RHI resource release remain
  outside that mutex and on their existing owner paths. Quiescence is proven by
  zero open and nonquiescent scope counts; a terminal queue node may briefly
  retain an already-quiescent live scope reference.
- Open questions: none for Stage 4. Qualification owns full native, full `all`,
  profiling parity, lifecycle smoke, stable documentation, and roadmap closure.
- Validation: `FAssetImportCoreTests.*` passed 12/12, full
  `AssetImportCoreTests` passed 24/24, `FSourceImageThumbnailTests.*` passed
  15/15, and full `ThumbnailTests` passed 61/61 on
  `Win64-Debug-DurinEditor-Tests`. The focused assertion side-effect audit over
  both implementations and pilot suites reported 0 invocations and 0 findings.

### Stage 4: Qualify, document, and close M4

Dependencies: Stage 3 production evidence.

- [x] Compare post-change qualification medians with Stage 0 and disposition any
  regression above the frozen threshold.
- [x] Run focused Core and pilot suites, full native aggregate, complete `all`
  builds, and hidden-window lifecycle smoke through the repository contract.
- [x] Validate profiling-disabled and profiling-enabled builds and correlate
  bounded scope diagnostics without dynamic profiler cardinality.
- [x] Move stable scope rules into the CPU Task System and runtime lifecycle
  documentation.
- [x] Record M4 completion and M6 serialized-lane evidence disposition in the
  roadmap, then complete the final handoff and plan validation.

#### Acceptance Gate

- M4 roadmap exit criteria pass for two production owners under normal,
  cancellation, saturation, and shutdown loads.
- Scope lifetime is explicit, bounded, behavior-neutral for unscoped callers,
  and independent of scheduler ownership.

#### Stage 4 Handoff

- Baseline commit: `78f53a2f` (`feat(tasks): migrate production owners to
  scopes`). The lasting documentation working set is `TaskSystem.md`,
  `RuntimeLifecycle.md`, `Profiling.md`, `TaskSystemEvolution.md`, and this
  plan. Qualification also added one isolation fix to
  `ReflectionTypeTests.cpp`: the recursive-CDO fixture now registers its
  persistent dependency class before measuring rollback-owned object count.
- Qualification: the frozen four-fixture five-run medians were 7,649,300 ns
  copyable, 7,845,000 ns move-only, 4,101,100 ns shared transfer, 4,329,000 ns
  unique transfer, 11,408,300 ns deferred admission, 5,194,000 ns deferred
  pump, 10,390,641 ns average deferred residency, 54 ms process total, and 6 ms
  diagnostic snapshot. They changed -50.24%, -47.73%, -49.43%, -48.08%,
  -48.60%, -46.78%, -48.69%, -73.91%, and -94.64% from the Stage 0 frozen
  medians, so no 10 percent regression investigation remained. Across five
  paired scope runs, the median-of-medians was 8,384,500 ns unscoped and
  8,471,400 ns scoped, about 1.04 percent overhead and below the frozen 15
  percent threshold; every run reconciled its scope counts.
- Key decisions: stable scope association, close, cancellation, quiescence,
  waiting, shutdown, and bounded diagnostics now live in the CPU Task System
  contract. Runtime lifecycle documents owner publication-before-close and
  lock-free wait ordering for both pilots. Profiling carries only a numeric
  scope id in existing task events; scope values create no dynamic zone, plot,
  source-location, or registry cardinality.
- Roadmap disposition: M4 is complete. M6 remains deferred because AssetImport
  and source-image thumbnails do not provide two owners needing reusable
  ordered non-affine Worker execution beyond scopes, prerequisites, and their
  existing domain queues. M5 remains deferred under its existing unmet blocking
  occupancy and platform-cancellation evidence gate.
- Open questions: none. Full Core and both pilot suites passed; the full native
  aggregate and direct-lifecycle phase passed after the fixture isolation fix.
  Tracy-disabled `Win64-Debug-DurinEditor-Tests` and Tracy-enabled
  `Win64-Release-DurinEditor-Profiling` completed `all` builds. Both presets
  passed hidden-window scheduler lifecycle smoke through normal engine exit,
  with matching completed, failed, canceled, rejected, long-wait, and retained-
  handle diagnostics. The focused assertion side-effect audit reported zero
  invocations and findings, the all-plan validator passed, and `git diff
  --check` was clean.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Root/descendant -> scope | Frozen explicit and inherited precedence selects one scope across every task form. |
| Admission -> close | Exactly one side wins; post-close work is rejected without hidden descendants. |
| Scope cancel -> terminal | Waiting, queued, and running work preserves cooperative cancellation and failure precedence. |
| Terminal -> quiescence | Active counts release once after terminal hooks and dependent propagation. |
| GameThread target -> owner wait | Unsupported waits reject; selected owner and engine shutdown paths keep pumping ownership explicit. |
| Scope -> diagnostics | Counts and bounded nonterminal snapshots reconcile without completed history. |
| Scheduler shutdown -> scopes | Admission closes before executor teardown and all scoped work becomes terminal. |
| Production owner -> policy | AssetImport mailbox and thumbnail cache/render policy remain subsystem-owned. |

## Definition of Done

- Explicit owner scopes close admission, cancel or drain descendants, and reach
  observable quiescence without destructor blocking.
- Scope association covers all task forms and preserves unscoped compatibility.
- Diagnostics remain bounded and reconcile with task and owner/category totals.
- Two production owners validate independent shutdown contracts.
- Required tests, builds, profiling configurations, lifecycle smoke, lasting
  documentation, roadmap update, and plan validation pass.

## Deferred Follow-ups

- A bounded IO executor remains M5 and requires named blocking-occupancy and
  platform cancellation evidence.
- Worker-backed serialized lanes remain M6 and require at least two owners whose
  ordering cannot be expressed cleanly with scopes and prerequisites.
- Scope result aggregation, deadlines, multi-stage unique results, fibers,
  coroutines, priorities, and work stealing require separate evidence and plans.

## Related Documentation

- [Task System Evolution Roadmap](../../../Roadmaps/Archive/2026-08/TaskSystemEvolution.md)
- [CPU Task System](../../../Runtime/Core/TaskSystem.md)
- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [Task Owner Diagnostics](TaskOwnerDiagnostics.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Implementation Plan Rules](../../AGENTS.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Source/Editor/AssetImportCore/Private/AsyncImport.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SourceImageThumbnailCache.cpp`
- `Engine/Tests/Native/CoreTests/Private/ThreadingTests.cpp`
