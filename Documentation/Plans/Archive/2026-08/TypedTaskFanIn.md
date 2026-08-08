# Typed Task Fan-In Plan

Summary: Add deterministic heterogeneous shared-result fan-in to the existing bounded task graph without changing unique-result or executor ownership.

Last reviewed: 2026-08-07

Status: Archived
Completed: 2026-08-07

## Current Status

All stages are complete from the M1 baseline commit `34c8218e`; the Stage 1/2
implementation baseline is `49b8eca2`. `WhenAll` and `WhenAllOutcome` compose
one heterogeneous shared-result tuple through the existing
aggregate-prerequisite node. Focused Core coverage proves success, failure,
cancellation, duplicate, lifetime, rejection, move-only callback, and
GameThread pump paths. Stable Task System documentation owns the lasting
contract, the M2 roadmap exit gate is closed, the full native aggregate and
`all` build pass, and the hidden-window scheduler lifecycle smoke exits cleanly.

## Goal

Allow one continuation node to consume a heterogeneous, non-empty tuple of
immutable shared typed results without manually capturing handles or looking up
each result. Provide a completion-edge variant that exposes every predecessor
outcome plus one deterministic aggregate classification.

## Scope

- Heterogeneous shared typed `WhenAll` and `WhenAllOutcome` composition.
- Value and outcome callback signatures, result lifetime, input ordering, and
  aggregate failure/cancellation precedence.
- Worker and `GameThreadDeferred` continuation targets through the existing
  bounded admission and shutdown paths.
- Compile-time, race, failure, cancellation, destruction, and lifecycle tests.
- Stable runtime documentation and roadmap evidence for M2 completion.

## Non-Goals

- `WhenAny`, racing tasks, or cancellation of losing work.
- Unique-result fan-in or general multi-stage unique-result production.
- Homogeneous range fan-in, dynamic result type erasure, or empty fan-in.
- Structured task scopes, owner attribution, new executors, priorities, or
  scheduler policy changes.
- Replacing `Then`, `ThenOutcome`, explicit prerequisites, or domain mailboxes.

## Design Decisions and Invariants

### Public API

The supported input is a non-empty `std::tuple<TTaskHandle<Ts>...>`. Every
`Ts` is a non-void shared typed result. Inputs are copied into the continuation,
so releasing caller handles after successful registration cannot release result
storage before callback completion.

```cpp
template<typename... Ts, typename F>
auto WhenAll(
    const std::tuple<TTaskHandle<Ts>...>& Predecessors,
    const char* Name,
    F&& Function,
    const FTaskContinuationOptions& Options = {});

template<typename... Ts, typename F>
auto WhenAllOutcome(
    const std::tuple<TTaskHandle<Ts>...>& Predecessors,
    const char* Name,
    F&& Function,
    const FTaskContinuationOptions& Options = {});
```

`WhenAll` requires a callback invocable as `F&(const Ts&...)`. It creates one
success-dependent graph node and therefore runs only when every typed input and
every additional prerequisite succeeds. Its return follows the existing
continuation convention: exact `void` yields `FTaskHandle`; an object value
yields `TTaskHandle<U>`.

`WhenAllOutcome` requires a callback invocable as
`F&(TTaskAggregateOutcome<Ts...>)`. It creates one completion-dependent node,
runs after all typed inputs and additional prerequisites are terminal, and has
the same return convention.

The tuple overload is the sole M2 entry point. No variadic-handle overload is
added because it makes the trailing name, callable, and options ambiguous and
does not provide a stable object that can be passed between helper layers.

### Aggregate outcome

```cpp
template<typename... Ts>
struct TTaskAggregateOutcome
{
    std::tuple<FTaskOutcome<Ts>...> Outcomes;
    std::string Diagnostic;
    uint64 BlockingTaskId = 0;
    ETaskState State = ETaskState::Invalid;
    ETaskTerminalReason Reason = ETaskTerminalReason::None;
};
```

- `Outcomes` preserves tuple position exactly, including repeated handles.
- Each element owns the same immutable shared result view and diagnostic data
  as the corresponding single-predecessor `ThenOutcome` callback.
- The aggregate is `Succeeded` only when every typed predecessor succeeded.
- Otherwise a failed predecessor wins over a canceled predecessor. Among
  predecessors with the winning state, the smallest nonzero task id wins.
- `BlockingTaskId`, `Diagnostic`, and `Reason` are copied from that selected
  predecessor. Invalid inputs do not create a continuation, so `Invalid` is
  only the default value of a directly constructed aggregate.
- Additional prerequisites in `Options` are scheduling gates and are not added
  to `Outcomes` or its aggregate classification.

This rule matches the scheduler's existing aggregate success-edge selection,
but the public outcome is computed from terminal snapshots in a deterministic
tuple-independent way rather than exposing completion arrival order.

### Graph, ownership, and failure

- One fan-in call creates exactly one continuation node. It uses the first
  typed handle as the existing primary predecessor and supplies the remainder
  through `FTaskContinuationOptions::Prerequisites`; scheduler deduplication
  prevents duplicate graph edges while callback tuple positions remain intact.
- Registration rejects an invalid primary/input handle, a scheduler mismatch,
  closed admission, or invalid additional prerequisite through existing
  continuation admission. No callback runs for a rejected registration.
- A success-edge fan-in canceled by a predecessor preserves the existing
  deterministic dependency diagnostic and direct blocking task id.
- Callback exceptions remain `CallbackFailure`. A callback result uses existing
  immutable shared result publication and terminal-publication barriers.
- Move-only callbacks are accepted. Captured handles and callback state are
  destroyed exactly once on success, rejection, cancellation, or shutdown and
  are never moved or destroyed under a queue mutex.
- Named targets never execute inline. `GameThreadDeferred` admission retains
  its existing count and declared-payload bounds; shared input results do not
  become newly declared unique retained-result bytes.

## Current Foundations and Gaps

- `LaunchContinuationTask` already builds one deduplicated aggregate
  prerequisite set and supports success or completion dependency kinds.
- Aggregate success-edge terminal selection already prefers failure over
  cancellation and then the smallest task id.
- `TTaskHandle<T>` retains immutable result state and `FTaskOutcome<T>` exposes
  a stable terminal snapshot.
- `LaunchContinuationResult` already provides move-only callback ownership and
  shared typed result publication for arbitrary continuation results.
- The missing layer is tuple validation, typed callback expansion, aggregate
  outcome construction, and focused contract coverage.

## Implementation Stages

### Stage 0: Freeze the typed aggregate contract

Dependencies: completed M1 Move-Only Tasks and Consuming Results milestone.

- [x] Select tuple-only heterogeneous shared inputs and exclude empty, void,
  unique, range, and `WhenAny` composition.
- [x] Freeze `WhenAll` and `WhenAllOutcome` signatures and callback forms.
- [x] Freeze result lifetime, duplicate-input, additional-prerequisite, target,
  rejection, and destruction behavior.
- [x] Freeze aggregate failure/cancellation precedence and selected diagnostic.
- [x] Record the existing scheduler path and bounded implementation working set.

#### Acceptance Gate

- Later stages have one unambiguous API and outcome model, with no scheduler or
  executor redesign required.
- The Task System Evolution roadmap links the active M2 plan.

#### Stage 0 Handoff

- Baseline commit: `34c8218e`.
- Working set: `Task.h`, `Task.cpp`, Core `ThreadingTests.cpp`, stable Task
  System documentation, and the Task System Evolution roadmap.
- Key symbols: `TTaskHandle<T>`, `FTaskOutcome<T>`,
  `Private::LaunchContinuationResult`, `Private::LaunchContinuationTask`, and
  `FTaskStateData::OnPrerequisiteTerminal`.
- Decision: build M2 as a header-only typed adapter over the existing aggregate
  graph node; do not add scheduler state or another callback queue.
- Open questions: none blocking Stage 1.
- Validation: plan structure and all-plan validation are required in the Stage
  0 commit; no build is required for documentation-only work.

### Stage 1: Implement typed fan-in and compile-time constraints

Dependencies: Stage 0 frozen contract.

- [x] Add `TTaskAggregateOutcome<Ts...>` and private tuple helpers.
- [x] Add constrained `WhenAll` and `WhenAllOutcome` APIs with move-only
  callback support and existing continuation return behavior.
- [x] Route every typed input through existing scheduler validation before the
  callback can run; the non-empty constraint makes primary indexing defined.
- [x] Preserve tuple positions while graph prerequisite edges are deduplicated.
- [x] Add compile-time fixtures for accepted callbacks and rejected empty,
  void, unique, wrong-argument, reference-result, and non-invocable forms.

#### Acceptance Gate

- Core compiles with the exact public surface and all compile-time fixtures.
- Existing `Then`, `ThenOutcome`, shared handles, and unique sinks remain source
  compatible.

#### Stage 1 Handoff

- Baseline commit: Stage 0 commit `4e84fd1b`.
- Working set: Core `Task.h` and `ThreadingTests.cpp`; no scheduler `.cpp` state
  changed.
- Key symbols and decisions: `TTaskAggregateOutcome`,
  `MakeFanInContinuationOptions`, `MakeTaskAggregateOutcome`, `WhenAll`, and
  `WhenAllOutcome`; the first tuple element is the primary predecessor and the
  whole tuple is supplied for existing edge deduplication.
- Open questions: none blocking Stage 2.
- Validation: the focused continuation suite passes 9/9, and compile-time
  fixtures build with the CoreConcurrencyTests target.

### Stage 2: Prove deterministic runtime and lifecycle behavior

Dependencies: Stage 1.

- [x] Test heterogeneous success, void/value callback returns, input order,
  repeated handles, and caller handle release.
- [x] Test success-edge failure/cancellation propagation and callback skipping.
- [x] Test outcome-edge all-success, mixed failure/cancellation, stable
  smallest-task-id selection, and result presence only on success.
- [x] Test completion races, invalid and mixed-scheduler rejection, explicit
  cancellation, shutdown destruction, and move-only callback destruction.
- [x] Test Worker and manual-pump `GameThreadDeferred` targets with bounded
  admission and no inline execution.

#### Acceptance Gate

- Focused Core tests pass repeatedly with deterministic aggregate snapshots and
  no callback/result lifetime leak.
- Existing Core concurrency and task lifecycle suites remain green.

#### Stage 2 Handoff

- Baseline commit: Stage 0 commit `4e84fd1b`; Stage 1 and Stage 2 share the next
  implementation commit because the public templates and their runtime
  instantiations form one compile boundary.
- Working set: Core `Task.h` and `ThreadingTests.cpp`.
- Key symbols and decisions: repeated tuple positions remain visible to the
  callback while prerequisite diagnostics deduplicate them; outcome selection
  ignores tuple order and chooses failure, then cancellation, then task id.
- Open questions: none blocking Stage 3.
- Validation: the new fan-in suite passes 3/3, the deferred fan-in test passes,
  the continuation suite passes 9/9, and the full CoreConcurrencyTests
  aggregate passes 98/98. One earlier pre-fixture run observed a transient
  failure in an existing move-only timing test; the complete post-change rerun
  passed and no fan-in assertion was involved.

### Stage 3: Qualify, document, and close M2

Dependencies: Stage 2.

- [x] Add shared typed fan-in rules and selection guidance to the stable CPU
  Task System contract.
- [x] Record M2 completion evidence and open the M4 composition side of its
  entry gate in the Task System Evolution roadmap.
- [x] Run the focused Core suite, full required aggregate, full `all` build,
  hidden-window lifecycle smoke, and changed/all-plan documentation validation
  through the repository build contract.
- [x] Record the final handoff, set this plan to Completed, and preserve M3 as
  the next required independent milestone.

#### Acceptance Gate

- Shared typed fan-in is stable, documented, and validated across both current
  executors and cross-executor shutdown.
- M2 exit criteria in the roadmap are satisfied without adding M3/M4 behavior.

#### Stage 3 Handoff

- Baseline commit: Stage 1/2 commit `49b8eca2`.
- Working set: stable CPU Task System documentation, Task System Evolution
  roadmap, and this plan; implementation remains confined to the Stage 1/2
  working set.
- Key symbols and decisions: `WhenAll`, `WhenAllOutcome`, and
  `TTaskAggregateOutcome` are the complete M2 public surface. M3 owner
  diagnostics is the next required milestone; M4 has only its composition side
  open and remains blocked on M3 plus production-owner evidence.
- Open questions: none. `WhenAny`, unique aggregation, and dynamic range fan-in
  remain explicitly deferred.
- Validation: focused fan-in tests pass 3/3, the deferred fan-in test passes,
  the continuation suite passes 9/9, and CoreConcurrencyTests passes 98/98.
  The complete native aggregate passes on final rerun, the complete `all` build
  passes, and DurinEditor exits successfully after three hidden-window ticks
  with `--task-scheduler-lifecycle-smoke`. The first full native run had two
  unrelated concurrent integration failures; both passed alone before the
  clean full rerun. Changed-document and all-plan validation pass after the
  final lifecycle update.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Typed tuple -> success callback | Heterogeneous results arrive as `const T&...` in tuple order exactly once. |
| Terminal tuple -> outcome callback | Every input snapshot is stable and aggregate selection is failure, then cancellation, then smallest task id. |
| Handle release -> callback | Captured fan-in handles retain all result states through callback completion. |
| Duplicate tuple positions -> graph | Callback positions repeat while diagnostics contain one edge per task id. |
| Invalid admission -> ownership | No callback runs and move-only captures are destroyed exactly once. |
| Worker -> GameThread target | Deferred fan-in never runs inline and obeys existing bounded pump/shutdown policy. |
| Compatibility | Existing void/shared/unique task APIs and their focused tests remain unchanged. |

## Definition of Done

- `WhenAll` and `WhenAllOutcome` satisfy every Stage 0 invariant.
- Compile-time and runtime coverage proves deterministic fan-in, terminal
  precedence, lifetime, rejection, race, and shutdown behavior.
- Stable runtime documentation owns the implemented contract.
- M2 is marked complete in this plan and the Task System Evolution roadmap.
- Required build, test, smoke, and documentation validation pass.

## Deferred Follow-ups

- `WhenAny` and loser cancellation require a separate ownership and fairness
  contract.
- Unique-result aggregation remains deferred until a concrete multi-owner
  workflow can prove consumption and rollback semantics.
- Homogeneous dynamic range fan-in can be planned if a production caller needs
  runtime-sized composition.
- Structured scopes remain M4 and require M3 owner attribution in addition to
  this milestone.

## Related Documentation

- [Task System Evolution Roadmap](../../../Roadmaps/Archive/2026-08/TaskSystemEvolution.md)
- [CPU Task System](../../../Runtime/Core/TaskSystem.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Implementation Plan Rules](../../AGENTS.md)
- [Move-Only Tasks and Consuming Results](MoveOnlyTasksAndConsumingResults.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Tests/Native/CoreTests/Private/ThreadingTests.cpp`
