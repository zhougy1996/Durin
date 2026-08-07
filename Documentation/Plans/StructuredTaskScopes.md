# Structured Task Scopes Plan

Summary: Add explicit bounded owner scopes that close task admission, propagate cancellation, expose descendant diagnostics, and quiesce without implicit destructor waits.

Last reviewed: 2026-08-07

Status: Active
Completed:

## Current Status

Stage 0 is active. M2 typed fan-in and M3 owner diagnostics are complete.
`AssetImport` closes admission and cancels/drains request nodes, while
`SourceImageThumbnail` rejects new requests and closes asynchronous result
publication before releasing cache state. These independent production
shutdown boundaries satisfy the roadmap entry gate. Stage 0 must freeze the
scope API, descendant-admission race, close modes, diagnostic bounds, pilot
migration boundary, and qualification baseline before implementation begins.

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

## Implementation Stages

### Stage 0: Freeze scope contract and baselines

Dependencies: completed M2 and M3 milestones; M3 completion commit recorded by
the Task Owner Diagnostics handoff.

- [ ] Inventory root, child, continuation, fan-in, unique sink, parallel-for,
  cancellation, terminal, scheduler shutdown, and GameThread pump sites.
- [ ] Freeze public type and member names, move/copy traits, option placement,
  scope association precedence, invalid/closed admission result, and source
  compatibility expectations.
- [ ] Freeze close/drain/cancel/quiescence state transitions and the exact rule
  for descendants racing owner close.
- [ ] Select bounded diagnostics, overflow behavior if any, lock order, and
  scheduler-shutdown integration sites.
- [ ] Define the AssetImport and thumbnail pilot boundaries, including whether
  thumbnail decode is drained or remains safely detached.
- [ ] Record focused no-scope and scoped qualification baselines plus the Stage
  1 working set.

#### Acceptance Gate

- No API, lifecycle, admission-race, GameThread, boundedness, or pilot ownership
  decision remains unresolved for Core implementation.
- Every scope count has one named admission and terminal balancing site.

### Stage 1: Add bounded scope admission and inheritance

Dependencies: Stage 0 frozen contract.

- [ ] Implement scope state and public handles/options without changing default
  behavior for unscoped callers.
- [ ] Associate roots and inherited descendants across continuations, typed
  fan-in, unique sinks, and parallel-for according to the frozen precedence.
- [ ] Linearize close against admission and reject post-close work without
  retaining callables or task nodes in the scope.
- [ ] Add compile-time and focused tests for traits, defaults, explicit scope
  selection, inheritance, cross-scope prerequisites, and close races.

#### Acceptance Gate

- Every accepted scoped node is charged exactly once and every post-close
  admission is rejected deterministically.
- Existing unscoped task APIs and scheduling behavior remain source compatible.

### Stage 2: Implement cancellation, quiescence, and diagnostics

Dependencies: Stage 1 stable scope association.

- [ ] Add explicit drain/cancel close modes and bounded wait/quiescence APIs at
  supported thread boundaries.
- [ ] Propagate cooperative cancellation without changing terminal precedence or
  blocking destruction.
- [ ] Add bounded scope counters, gauges, nonterminal snapshots, and scheduler
  shutdown closure.
- [ ] Add races for admission versus close, child launch versus cancel, terminal
  publication, handle release, scheduler shutdown, and concurrent diagnostics.

#### Acceptance Gate

- Scope counts reach zero only after all accepted descendants and terminal
  propagation are complete, including failure and cancellation races.
- Wrong-thread and GameThread deferred waits reject rather than deadlock.

### Stage 3: Migrate production owners

Dependencies: Stage 2 lifecycle semantics.

- [ ] Migrate AssetImport owner/provider shutdown to a scope while retaining its
  request table, mailbox, unique result sink, latest-wins policy, and explicit
  result taking.
- [ ] Migrate the selected source-image thumbnail boundary while retaining cache
  serial validation, decode/upload throttling, weak publication, and render/RHI
  ownership.
- [ ] Add focused owner shutdown, late publication, cancellation, destruction,
  and restart tests for both pilots.
- [ ] Capture diagnostics proving each owner closes admission and reaches its
  selected quiescence/detachment contract.

#### Acceptance Gate

- Both production owners shut down without hidden work, post-close publication,
  GameThread pumping inversion, or policy migration into Core.

### Stage 4: Qualify, document, and close M4

Dependencies: Stage 3 production evidence.

- [ ] Compare post-change qualification medians with Stage 0 and disposition any
  regression above the frozen threshold.
- [ ] Run focused Core and pilot suites, full native aggregate, complete `all`
  builds, and hidden-window lifecycle smoke through the repository contract.
- [ ] Validate profiling-disabled and profiling-enabled builds and correlate
  bounded scope diagnostics without dynamic profiler cardinality.
- [ ] Move stable scope rules into the CPU Task System and runtime lifecycle
  documentation.
- [ ] Record M4 completion and M6 serialized-lane evidence disposition in the
  roadmap, then complete the final handoff and plan validation.

#### Acceptance Gate

- M4 roadmap exit criteria pass for two production owners under normal,
  cancellation, saturation, and shutdown loads.
- Scope lifetime is explicit, bounded, behavior-neutral for unscoped callers,
  and independent of scheduler ownership.

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

- [Task System Evolution Roadmap](../Roadmaps/TaskSystemEvolution.md)
- [CPU Task System](../Runtime/Core/TaskSystem.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Task Owner Diagnostics](TaskOwnerDiagnostics.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Implementation Plan Rules](AGENTS.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Source/Editor/AssetImportCore/Private/AsyncImport.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SourceImageThumbnailCache.cpp`
- `Engine/Tests/Native/CoreTests/Private/ThreadingTests.cpp`
