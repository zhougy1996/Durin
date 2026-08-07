# Move-Only Tasks and Consuming Results Plan

Summary: Add move-only task callables and a type-distinct unique result consumed by exactly one continuation sink while preserving existing shared task APIs.

Last reviewed: 2026-08-07

Status: Active
Completed:

## Current Status

Stage 0 is active from baseline commit
`68660f0a2b966649f8ca0043f27c19f7f08792e7`; no implementation has started.
The completed continuation foundation publishes typed values through
`shared_ptr<const T>` and ultimately stores task and completion callables in
`std::function`. This supports immutable fan-out but requires copy-constructible
callables and cannot move a large or unique result into one downstream owner.

AsyncImportCore is the selected production pilot. It currently moves one
`FImportPlanResult` into request state, queues a serial notice, scans terminal
handles during drain, and synthesizes results when a worker terminates before
its epilogue. Its owner/provider admission, latest-by-owner replacement,
explicit drain, cancellation, and take-result behavior remain required and are
not replaced by this plan.

This is milestone M1 of the
[Task System Evolution Roadmap](../Roadmaps/TaskSystemEvolution.md). Later typed
fan-in, owner diagnostics, structured scopes, IO execution, and serialized
lanes are outside this plan.

## Goal

Allow a caller to move unique captured state into a task and publish one unique
typed result that exactly one continuation sink can move into its final owner.
The graph must preserve deterministic registration, cancellation, failure,
dispatch rejection, generation, target routing, diagnostics, and shutdown
semantics without changing current void or shared-result behavior.

The production proof migrates only AsyncImportCore's worker-result handoff. It
must simplify result publication while preserving its public handle/status/take
contract and every domain-specific coordinator policy.

## Scope

- A Core-owned C++20-compatible move-only erased callable used internally by
  task nodes and executor queue entries.
- Forwarding launch and continuation templates that accept move-only callable
  captures while retaining current public copyable overloads and aliases.
- `TUniqueTaskHandle<T>` as a move-only, type-distinct handle with status,
  diagnostics, and erased graph-handle access but no shared result observer.
- `LaunchUniqueTask<T>` and `LaunchUniqueCancelableTask<T>` for explicit unique
  result publication.
- `ConsumeThen` and `ConsumeThenOutcome` as one-consumer void sink
  continuations targeting `AnyWorker` or `GameThreadDeferred`.
- A unique result-state claim/consume/destroy state machine and declared
  retained-result byte metadata.
- Deterministic diagnostics for duplicate claims, invalid/foreign handles,
  dispatch rejection, cancellation, stale generation, callback failure, and
  shutdown.
- Core unit/concurrency/lifecycle qualification and one bounded AsyncImportCore
  production migration.
- Lasting contract updates in `Documentation/Runtime/Core/TaskSystem.md` after
  behavior is validated.

## Non-Goals

- Replacing or changing `FTaskHandle`, `TTaskHandle<T>`, `GetResultShared`,
  `LaunchTask`, `Then`, or `ThenOutcome` semantics.
- Inferring unique ownership merely because `T` is move-only or noncopyable.
- Allowing shared observation and unique consumption of the same result.
- Supporting more than one unique consumer or converting a shared result into
  a unique result.
- Returning another unique result from a consuming continuation in this plan;
  consuming callbacks are terminal void sinks.
- Adding `WhenAll`, `WhenAny`, typed aggregate fan-in, structured task scopes,
  deadlines, priorities, or new executors.
- Removing AsyncImportCore's request table, mailbox, owner/provider policy,
  cancellation, explicit drain, or take-result API.
- Migrating source-image thumbnails, material thumbnails, RenderThread, or
  RHIThread work.
- Making move-only callable erasure a general public delegate replacement
  outside the task system.
- Adding custom allocators, coroutine frames, fibers, or work stealing.

## Design Decisions and Invariants

### Compatibility and public vocabulary

- Existing `FTaskFunction`, `FCancelableTaskFunction`, public non-template
  overloads, void handles, and shared typed handles remain source-compatible.
  Existing copyable call sites do not need migration.
- Task entry templates accept invocable move-constructible callables and erase
  them into a task-private move-only function type. Overload resolution must
  continue to select existing void and explicitly typed calls without new
  ambiguity.
- The task-private callable wrapper is noncopyable, noexcept movable, nullable,
  invokes one stored target, and destroys that target exactly once. It is not a
  general event, multicast, reference wrapper, or owning delegate API.
- Scheduler nodes, queued Worker entries, and GameThread deferred entries move
  callback ownership across boundaries. No executor copies a callback after
  successful admission.
- Completion hooks may remain copyable when they capture only shared internal
  state; the public user callable must not be copied to satisfy that boundary.

The additive public vocabulary is conceptually:

```cpp
template<typename T>
class TUniqueTaskHandle;

template<typename T, typename F>
auto LaunchUniqueTask(
    const char* Name,
    F&& Function,
    const FTaskLaunchOptions& Options = {},
    uint64 EstimatedResultBytes = sizeof(T)) -> TUniqueTaskHandle<T>;

template<typename T, typename F>
auto LaunchUniqueCancelableTask(
    const char* Name,
    F&& Function,
    const FTaskLaunchOptions& Options = {},
    uint64 EstimatedResultBytes = sizeof(T)) -> TUniqueTaskHandle<T>;

template<typename T, typename F>
auto ConsumeThen(
    TUniqueTaskHandle<T>&& Predecessor,
    const char* Name,
    F&& Function,
    const FTaskContinuationOptions& Options = {}) -> FTaskHandle;

template<typename T, typename F>
auto ConsumeThenOutcome(
    TUniqueTaskHandle<T>&& Predecessor,
    const char* Name,
    F&& Function,
    const FTaskContinuationOptions& Options = {}) -> FTaskHandle;
```

Stage 0 freezes exact names, parameter placement, constraints, and diagnostic
text before implementation. A changed name must be recorded here rather than
leaving aliases for an unshipped API.

### Unique handle and result ownership

- `TUniqueTaskHandle<T>` is move-only. It exposes the same status and diagnostic
  queries as `TTaskHandle<T>` plus `GetTaskHandle()` for type-erased
  prerequisites. It exposes neither `GetResultShared()` nor a direct take API.
- The caller chooses unique publication explicitly through `LaunchUniqueTask`.
  `LaunchTask<T>` continues to mean immutable shared publication even when `T`
  happens to be move-only.
- A successful unique producer moves exactly one `T` into private pending
  storage and atomically publishes it with `Succeeded`. Failure or cancellation
  destroys pending storage without exposing a value.
- Dropping the last unconsumed unique handle destroys a published result after
  scheduler and continuation owners release it. Diagnostics retain no `T`.
- The erased `FTaskHandle` may outlive the unique handle and remains useful for
  state, wait, prerequisite, and diagnostics, but it grants no result access.

### Consumer registration and invocation

- `ConsumeThen` takes an rvalue unique handle, registers a success dependency,
  and accepts only a callback invocable as `void(T&&)`.
- `ConsumeThenOutcome` takes an rvalue unique handle, registers a completion
  dependency, and accepts only a callback invocable as
  `void(FUniqueTaskOutcome<T>&&)`. The outcome owns an `optional<T>` only when
  the producer succeeded and carries the ordinary task state, reason,
  diagnostic, and erased handle in every case.
- Exactly one consuming continuation may claim the result state. The claim is
  made during graph-node registration, before source completion can publish to
  a competing consumer.
- Registration validation is transactional. Invalid options, foreign lifetime,
  missing callable, or node-construction rejection leave the caller's unique
  handle unclaimed and usable despite the rvalue call expression. A successful
  registration clears the caller-visible handle and transfers the claim to the
  continuation.
- A second registration attempt is rejected deterministically before accepting
  a graph node and increments an explicit duplicate-consumer diagnostic.
- `ConsumeThen` does not run after producer failure/cancellation; its node uses
  existing success-edge propagation. `ConsumeThenOutcome` runs after any
  terminal producer state and receives no value unless that state is
  `Succeeded`.
- The value moves out immediately before user callback invocation. Callback
  failure destroys the moved value during unwinding and terminalizes the
  consumer as `Failed/CallbackFailure`.
- If target dispatch is rejected, generation is stale, work is superseded,
  cancellation wins before invocation, or shutdown cancels the consumer, the
  stored result is destroyed without invoking the callback. There is no hidden
  recovery channel in this first sink contract.

### Thread, queue, and retained-byte policy

- A consuming continuation is always a graph node and follows the selected
  logical executor. `GameThreadDeferred` never invokes inline and only the
  owning pump may move the value into user code.
- `EstimatedResultBytes` includes dynamic storage reachable exclusively from
  the unique value; it is diagnostic/admission metadata rather than an exact
  allocator measurement.
- A `GameThreadDeferred` unique consumer charges the checked sum of its normal
  `EstimatedPayloadBytes` and the producer's declared retained result bytes.
  Overflow, a zero retained-result declaration for a dynamically owning unique
  result, or capacity excess rejects dispatch without bypassing queue bounds.
- Stage 0 defines the zero/default rule for trivially fixed-size values and the
  exact overflow diagnostic. The implementation must not assume `sizeof(T)`
  accounts for heap storage owned by `T`.
- AnyWorker retains current count-bounded scheduler admission. Declared unique
  result bytes still appear in diagnostics so the later owner-metrics plan can
  measure retained storage.
- Worker waiting and GameThread waiting rules do not change. No waiter may
  execute a GameThread-target consumer.

### Cancellation, shutdown, and terminal ordering

- Cancellation is cooperative while a producer or consumer callback runs and
  prevents a not-started callback when it wins. It never moves a result into
  user code after a winning pre-start cancellation.
- Normal cross-executor drain keeps accepted consumer dispatch open after root
  admission closes and pumps GameThread consumers until graph quiescence.
- Cancel shutdown destroys every unclaimed/not-started unique result, requests
  cancellation of running callbacks, and leaves every accepted producer and
  consumer handle terminal.
- Result destruction, terminal completion hooks, dependent notification, and
  executor callbacks run outside task-state locks. User destructors must not be
  invoked while holding scheduler, queue, scope, or subsystem locks.
- Recursive GameThread pump/shutdown protections and adapter-generation checks
  remain unchanged.

### AsyncImportCore pilot boundary

- `AssetImport.PreparePlan` becomes a unique typed producer of
  `FImportPlanResult`. A single AnyWorker consuming sink moves the result into
  `FAsyncImportRequestState`, marks the notice ready, and queues the existing
  serial mailbox notice.
- The request's tracked task becomes the consumer/publisher node so
  `CancelAndDrain` waits through result publication, not merely producer
  completion.
- The consuming outcome path maps producer failure/cancellation into the same
  stable import diagnostics currently synthesized by `Drain()`.
- The coordinator retains request serials, `LatestByOwner`, provider closure,
  explicit mailbox drain, status transitions, cancellation, result take, and
  request removal. The migration must not publish editor state from a Worker.
- `FImportPlanResult` moves exactly once from producer result state into request
  state and once from request state into the caller's `OutResult`; no new full
  result copy is permitted.
- Source-image thumbnail migration remains deferred until the sink contract and
  its per-frame decode/upload ownership are reviewed separately.

## Current Foundations and Gaps

### Foundations

- `FTaskStateData` already owns callback lifetime through accepted, queued,
  running, and terminal states.
- `TTaskResultState<T>` already separates pending storage from successful
  publication and destroys pending values on failure/cancellation.
- Continuations are real graph nodes with success/completion dependency kinds,
  target routing, cancellation, generation, coalescing, and terminal reasons.
- GameThread deferred admission is bounded by count and declared payload bytes
  and has explicit dispatch ownership.
- Cross-executor shutdown already closes root admission while allowing accepted
  continuation dispatch to reach quiescence.
- AsyncImportCore already owns a single move-produced result, request lifetime,
  cancellation, explicit drain, and consuming public Take operation.

### Gaps

- `FTaskFunction`, `FCancelableTaskFunction`, typed launch wrappers, and
  continuation templates require copyable `std::function` targets.
- `Then`/`ThenOutcome` explicitly reject non-copyable callable captures.
- `TTaskResultState<T>` publishes only an immutable shared owner; it has no
  exclusive claim or move-out transition.
- A copyable `TTaskHandle<T>` cannot prove single-consumer ownership and may
  already have external shared result observers.
- Deferred payload accounting currently describes callback captures but does
  not automatically include a unique result retained behind internal state.
- AsyncImportCore manually catches worker errors, writes optional result state,
  queues completion notice, scans terminal handles, and synthesizes missing
  results because the graph cannot currently transfer unique ownership.

## Implementation Stages

### Stage 0: Freeze the move and consumption contract

Dependencies: completed Task Continuations and Thread Dispatch plan; baseline
commit `68660f0a2b966649f8ca0043f27c19f7f08792e7`.

- [ ] Inventory every location that stores, moves, copies, discards, or invokes
  task callbacks and completion functions from public launch through Worker and
  GameThread executors.
- [ ] Freeze the internal move-only erased-callable interface, storage policy,
  allocation behavior, exception/noexcept boundary, null behavior, and
  destruction thread expectations.
- [ ] Freeze exact public unique launch/consume names, concepts/static
  constraints, overload resolution, moved-from handle behavior, and outcome
  structure.
- [ ] Define the unique result state machine for pending, published, claimed,
  consumed, discarded, and terminal storage, including every concurrent
  registration/completion/cancellation ordering.
- [ ] Define transactional registration and duplicate-consumer diagnostics,
  including whether rejected registration preserves the caller's handle and
  how counters are exposed.
- [ ] Freeze retained-result byte declaration, default/zero rules, checked
  addition to GameThread payload admission, and diagnostics.
- [ ] Add compile-time API fixtures for legacy copyable calls, move-only void and
  typed calls, invalid callback signatures, handle copy/move traits, and
  ambiguous overload prevention.
- [ ] Update this plan before implementation if the frozen API differs from the
  conceptual vocabulary above.

#### Acceptance Gate

- Callback and result ownership have one explicit owner at every accepted,
  queued, running, terminal, rejected, and shutdown state.
- Public compatibility, unique handle movement, one-consumer claim, outcome,
  retained-byte, and rejection semantics have no unresolved alternatives.
- Compile-time fixtures demonstrate that legacy calls remain valid and invalid
  shared/unique combinations are unrepresentable or deterministically rejected.

### Stage 1: Move callable ownership through the existing executors

Dependencies: Stage 0 frozen contract.

- [ ] Implement the task-private move-only erased callable and focused tests for
  inline/heap storage as selected, move construction/assignment, null calls,
  exception propagation, and exactly-once destruction.
- [ ] Change task nodes, Worker queue handoff, GameThread deferred queue entries,
  cancellation discard, supersession, dispatch rejection, and shutdown cleanup
  to move callable ownership without copying it.
- [ ] Add constrained forwarding launch and existing shared continuation entry
  points that accept move-only callables while retaining current aliases and
  overloads.
- [ ] Remove V1 copy-constructible assertions only after the internal boundary
  can retain the user callable exactly once.
- [ ] Validate callable destruction thread and reentrant destructor behavior;
  never invoke user destruction under scheduler or queue locks.
- [ ] Add race tests covering cancel-before-claim, executor rejection,
  supersession, stale generation, callback exception, drain/cancel shutdown,
  and dropped handles with move-only captures.

#### Acceptance Gate

- A callable capturing a `unique_ptr` can run as void, shared typed, cancelable,
  Worker continuation, and GameThread deferred continuation work.
- Every accepted or rejected path destroys the capture exactly once, no task or
  executor copies it, and existing copyable-callable tests remain unchanged.
- Core concurrency tests pass with no nonterminal work or retained callable
  storage after shutdown.

### Stage 2: Add unique result handles and consuming sinks

Dependencies: Stage 1 move-only callable boundary.

- [ ] Implement unique pending/published/claimed/consumed/discarded result state
  and move-only `TUniqueTaskHandle<T>` queries.
- [ ] Implement unique launch APIs and require explicit ownership selection
  without changing shared typed launch behavior.
- [ ] Implement transactional `ConsumeThen` and `ConsumeThenOutcome` sink
  registration, success/completion edges, and one-consumer claim.
- [ ] Integrate retained-result byte diagnostics and checked GameThread deferred
  admission accounting.
- [ ] Ensure failure, cancellation, dispatch rejection, stale generation,
  supersession, callback exception, handle release, and both shutdown modes
  destroy stored `T` exactly once outside internal locks.
- [ ] Add focused tests for move-only/non-default-constructible/destructor-
  observing `T`, duplicate claims, rejected registration preservation,
  completion races, moved-from handles, erased prerequisites, and result
  lifetime after producer handle release.
- [ ] Extend scheduler and queue diagnostics only with bounded counters/metadata;
  do not retain result payloads or per-terminal histories.

#### Acceptance Gate

- A unique producer moves one noncopyable value into exactly one Worker or
  GameThread sink with zero value copies and deterministic destruction.
- Shared and unique result modes cannot observe/consume the same storage, and a
  second consumer cannot be accepted.
- All terminal and shutdown paths leave producer and consumer handles terminal,
  queue/scheduler storage empty, and result destructors executed exactly once.

### Stage 3: Migrate the AsyncImportCore result handoff

Dependencies: Stage 2 unique sink API; existing AsyncImportCore tests.

- [ ] Change the import-plan worker to return a unique
  `FImportPlanResult` rather than mutating request state from its callable.
- [ ] Add one AnyWorker consuming outcome sink that moves success into request
  state or publishes stable failure/cancellation state, then queues the existing
  serial notice.
- [ ] Track the publisher sink as the request task so explicit wait/drain covers
  the complete handoff.
- [ ] Remove only worker-epilogue and missing-result synthesis made redundant by
  terminal outcome publication; preserve latest-owner, provider, mailbox,
  cancellation, take, and removal policy.
- [ ] Cover admission rejection at both producer and consumer construction,
  superseded owner requests, provider close, cancellation before/during work,
  callback failure, explicit drain, result take-once, and process shutdown.
- [ ] Verify that no `FImportPlanResult` copy is introduced and provider leases
  remain alive through the same or narrower interval.

#### Acceptance Gate

- Every accepted import request publishes exactly one mailbox notice and one
  stable terminal status, and `TryTakeAsyncImportPlanResult` moves the result at
  most once.
- Latest-by-owner, provider closure, cancellation, drain, and removal behavior
  match the pre-migration contract with fewer manual terminal-state branches.
- No live editor, provider registry, or mutable object state is accessed from
  an unauthorized thread.

### Stage 4: Qualify, document, and close the milestone

Dependencies: Stage 3 production pilot.

- [ ] Measure copyable versus move-only callable admission/execution/destruction
  overhead and shared versus unique result transfer for representative small
  and large payloads in the Agent Debug profile.
- [ ] Saturate GameThread deferred admission with declared unique retained bytes
  and verify count/byte limits, stale drops, cancellation, callback budget, and
  shutdown memory release.
- [ ] Run complete Core and applicable AssetImportCore/editor workflow tests,
  full `all` build, and the hidden-window task lifecycle smoke according to the
  repository build guidance.
- [ ] Move stable callable/result ownership and consuming-sink rules into
  `Documentation/Runtime/Core/TaskSystem.md` and update the roadmap milestone
  status and evidence.
- [ ] Record the source-thumbnail follow-up decision using pilot evidence; do
  not migrate it in this plan.
- [ ] Complete stage handoff, plan lifecycle metadata, and all-plan validation.

#### Acceptance Gate

- Qualification shows no result copy, duplicate consumption, unbounded retained
  bytes, callable leak, or shutdown residue under normal and saturated loads.
- The AsyncImportCore pilot and existing shared-result pilot both pass their
  domain behavior and lifecycle tests.
- Lasting documentation describes when to choose void, shared, or unique result
  tasks without requiring this plan as runtime specification.

Each stage lands as an independent local commit and ends with a compact handoff
recording baseline commit, working set, key symbols/decisions, open questions,
and validation outcome.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Legacy API | Existing void/shared launches and continuations compile and retain current state/result behavior. |
| Move-only callable | Unique captures work across void, cancelable, typed, Worker, and GameThread paths and destruct exactly once. |
| Unique result | Noncopyable/non-default-constructible values publish on success, move once to one sink, and never appear after failure/cancellation. |
| Claim races | Concurrent completion, cancellation, consumer registration, duplicate registration, and handle release have one deterministic owner. |
| Registration rejection | Invalid callable/options/lifetime leave no accepted node or lost value and preserve the source handle according to the frozen contract. |
| Target routing | Worker and GameThread sinks invoke only on the requested executor and never inline for the named GameThread target. |
| Queue bounds | Declared retained result bytes participate in checked deferred admission; saturation cannot hide storage behind a small callback capture. |
| Terminal paths | Failure, cancellation, stale generation, supersession, dispatch rejection, callback exception, and shutdown destroy values/captures exactly once. |
| Waiting | Worker helping and GameThread no-wait rules remain unchanged for unique producers and consumers. |
| Async import | Owner/provider/latest-wins/mailbox/drain/take semantics remain stable with exactly one moved result and notice. |
| Diagnostics | Producer/consumer IDs, target, terminal reason, declared result bytes, duplicate claims, and retained terminal storage are bounded and observable. |
| Performance | Small-callable overhead and large-result transfer are compared with current shared/manual baselines; numbers are qualification data only. |
| Lifecycle | Drain and cancel shutdown leave all accepted handles terminal, all unique values destroyed, and both executors quiescent. |

## Definition of Done

- Existing void and shared-result APIs remain source-compatible and validated.
- Task execution accepts move-only callable captures without artificial shared
  ownership and destroys them exactly once on every path.
- A type-distinct unique producer can feed exactly one void consuming sink on
  Worker or GameThread with deterministic outcome and no result copy.
- Deferred admission accounts for retained unique result bytes.
- AsyncImportCore uses the new handoff while retaining all domain coordinator
  semantics and its move-out public result API.
- Core, editor integration, lifecycle smoke, full build, and plan validation
  pass with recorded stage handoffs.
- Stable rules live in `TaskSystem.md`, and the Task System Evolution Roadmap
  records M1 completion and the next milestone entry state.

## Deferred Follow-ups

- Unique-result continuations that themselves return another unique result.
- Explicit unique-to-shared publication after a consuming transformation.
- Shared typed `WhenAll`/aggregate outcome combinators.
- Owner/category diagnostics and aggregate profiler distributions.
- Structured task scopes.
- Source-image thumbnail migration after its decode/upload/per-frame ownership
  is reviewed against the completed sink contract.
- Evidence-gated IO executor and conditional serialized Worker lanes.
- Move-only callable adoption by `ParallelFor` only if a concrete iteration
  capture requires it and repeated invocation semantics remain explicit.
- Coroutines, fibers, work stealing, Worker priorities, deadlines, RenderThread
  targets, and RHIThread targets.

## Related Documentation

- [Task System Evolution Roadmap](../Roadmaps/TaskSystemEvolution.md)
- [CPU Task System](../Runtime/Core/TaskSystem.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Task Continuations and Thread Dispatch](TaskContinuationsAndThreadDispatch.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Source/Editor/AssetImportCore/Public/AsyncImport.h`
- `Engine/Source/Editor/AssetImportCore/Private/AsyncImport.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/SceneImport.cpp`
- `Engine/Tests/Native/CoreTests/Private/ThreadingTests.cpp`
- `Engine/Tests/Native/EditorAssetWorkflowTests/Private/AsyncImportTests.cpp`
