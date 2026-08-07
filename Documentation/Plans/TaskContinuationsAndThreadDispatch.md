# Task Continuations and Thread Dispatch Plan

Summary: Extend Durin's bounded CPU task system with typed worker continuations and a bounded GameThread deferred executor while preserving subsystem, render, and RHI ownership.

Last reviewed: 2026-08-07

Status: Completed
Completed: 2026-08-07

## Current Status

All four stages are complete. Durin now has immutable typed task results,
success and completion continuations, a bounded and budgeted
`GameThreadDeferred` executor, cross-executor shutdown, and one production
editor pilot. Representative costs and saturation behavior are qualified;
stable behavior and primitive-selection guidance live in the owning runtime
documentation. Async import, thumbnails, RenderThread, and RHIThread retain
their domain queues for the recorded ownership reasons.

Durin currently provides a process-wide bounded worker scheduler with task
prerequisites, cooperative cancellation, dependency propagation, worker-side
helping while waiting, shutdown admission, and diagnostics. The public task
API is void-oriented and has no first-class completion continuation or
execution-target abstraction.

Subsystems currently own their result mailboxes. Rendering and RHI already use
dedicated command queues with their own lifecycle and backpressure rules. There
is no universal GameThread mailbox; the CPU task contract explicitly leaves
game-thread continuation pumping to subsystem-owned synchronization points.

This revision selects a deliberately narrow V1: typed worker continuations and
one `GameThreadDeferred` executor. RenderThread and RHIThread remain domain
queues rather than generic task targets until a production use case and a
non-blocking admission contract justify adapters.

## Goal

Provide an opt-in asynchronous composition model in which:

- a task may publish an owned typed result through immutable shared result
  state;
- a continuation is represented as a new task node with an explicit success or
  completion dependency and a returned handle;
- V1 continuations target `AnyWorker` or `GameThreadDeferred` through logical
  executors rather than native thread IDs;
- `GameThreadDeferred` is backed by one bounded, observable, low-priority queue
  pumped at an engine-owned safe point and by an explicit shutdown pump;
- cancellation, failure, dispatch rejection, supersession, stale-generation
  checks, shutdown, and queue limits remain explicit and observable; and
- existing subsystem mailboxes and render/RHI queues remain valid owners for
  domain-specific sequencing, batching, coalescing, and backpressure.

## Scope

- Define the task continuation contract, typed result ownership, success and
  completion dependency edges, outcome propagation, and V1 executor vocabulary.
- Extend the core task graph without changing the behavior of existing void
  task callers unless they opt into the new API.
- Add a global GameThread deferred-work executor with bounded depth, explicit
  payload estimates, priority, frame budget, cancellation, coalescing, and
  diagnostics.
- Add an engine-owned shutdown coordinator that can make progress on accepted
  GameThread continuations after the regular frame loop has stopped.
- Prove the design with one editor-side pilot that already uses copied inputs,
  value-owned results, cancellation, and a generation/request mailbox.
- Update the CPU task-system contract after the implementation decisions become
  stable.

## Non-Goals

- Replacing every subsystem mailbox with one untyped global queue.
- Moving RenderThread or RHIThread work through the GameThread queue.
- Exposing RenderThread or RHIThread as generic continuation targets in V1.
- Exposing arbitrary `std::thread::id` or native thread handles as task targets.
- Creating a dedicated native thread for every serialized resource; use a pipe
  or serialized task lane when affinity is not required.
- Making low-priority deferred work a guaranteed per-frame completion mechanism
  or a frame-critical synchronization lane.
- Adding fibers, coroutine-backed waits, work stealing, dedicated IO scheduling,
  or RenderGraph integration in the initial implementation.
- Making `Then` a hidden synchronous callback that may run on the completion
  thread when the caller requested a named target.
- Adding a consuming/move-out continuation API in V1; shared immutable result
  observation is the initial fan-out contract.
- Removing existing domain mailboxes merely to demonstrate the generic API.

## Design Decisions and Invariants

### Task graph and continuation semantics

- A continuation is a new task node. A success dependency releases the node
  only after every predecessor succeeds. A completion dependency releases an
  outcome-aware node after every predecessor reaches any terminal state.
  `Then` and `ThenOutcome` return handles so chains and fan-in can be composed
  without storing ad hoc callbacks in subsystem state.
- Existing `FTaskHandle` and void-oriented launch APIs remain source-compatible
  during the initial rollout. `TTaskHandle<T>` owns shared task state containing
  one immutable `T`. A task may move a value into that state, but V1 consumers
  observe it as `const T&` while the shared state is pinned or obtain an
  immutable shared owner. This supports fan-out without copying or consuming
  the predecessor's result and never exposes worker-local storage.
- Existing C++20 `std::function` entry points remain viable because scheduler
  wrappers capture only copyable shared state. Supporting arbitrary move-only
  callables or a single-consumer move-out API is separate follow-up work.
- A successful predecessor enables its continuation. Failure or cancellation
  propagates to a normal success-only continuation without invoking its body.
  The dependent becomes `Canceled` and records the direct blocking predecessor.
  Cleanup and error-reporting paths use `ThenOutcome`, whose completion edge
  passes an owned outcome view and never silently treats failure as success.
- Result access before success, after failure/cancellation, or through an
  invalid handle is rejected deterministically. Result storage is released
  after the scheduler, all handles, and all continuation wrappers release it;
  diagnostics never retain an unbounded result payload.
- Invalid handles, cross-lifetime prerequisites, self-waits, and dependency
  cycles remain rejected deterministically.
- The default target for new CPU work is `AnyWorker`; a named target must be
  selected explicitly. The implementation never infers thread affinity from
  the thread that launched or completed the predecessor.
- A `GameThreadDeferred` continuation is always queued, including when it is
  submitted or released on GameThread. V1 has no inline named-target execution
  and therefore no completion-thread reentrancy optimization.

### Execution targets are logical executors

- `AnyWorker` maps to the current `FTaskScheduler`.
- `GameThreadDeferred` maps to the global `FGameThreadDeferredWorkQueue`, which
  is pumped only by the owning game thread at an engine-approved safe point or
  by the explicit engine shutdown pump.
- The core task graph owns task identity, dependency state, result state, and
  terminal publication. Each executor owns how ready work is admitted, run,
  pumped, and stopped, and reports admission or execution outcome back to the
  graph exactly once.
- Executor availability is explicit. The GameThread adapter is installed by
  the engine lifecycle after the queue is initialized and uninstalled only
  after accepted graph work is terminal. A missing, closing, or generation-
  mismatched adapter rejects dispatch without invoking the continuation.
- A logical target may be implemented by an actual named thread or by a
  configured engine mode in which two roles share a thread. Callers depend on
  the role contract, not a native thread identity.
- RenderThread and RHIThread remain outside `ETaskTarget` in V1. A later adapter
  must live in the owning module, preserve its callable context and payload
  metadata, define non-blocking admission from workers, and have a production
  caller before it is added to the generic vocabulary.

### Global GameThread deferred-work queue

- The queue is an execution adapter for low-urgency GameThread continuations,
  not the owner of every asynchronous result in the process.
- Every entry is bounded by count and an explicit caller-supplied estimated
  payload byte count. The estimate is diagnostic/admission metadata rather than
  an assertion that `std::function` heap allocation can be measured exactly.
  Submission has an observable accepted or rejected outcome and never silently
  grows without limit.
- Entries carry priority and optional coalescing identity. Repeated refresh,
  cache-maintenance, telemetry, or latest-generation work may supersede an
  older not-started entry when the owning caller opts in. The old task becomes
  `Canceled` with a stable `Superseded` diagnostic; the replacement retains its
  own task ID, payload, and handle. Keys include an owner domain and generation
  so unrelated subsystems cannot coalesce each other's work.
- The pump enforces a time/item budget and records queue depth, age, rejected
  work, supersession, execution time, and expired generation counts. It serves
  only deferred work; frame-critical synchronization uses its existing owning
  lifecycle path and cannot consume or reserve this queue's capacity.
- Queue execution never waits for a worker, render, or RHI task while holding a
  subsystem or ownership lock. A continuation that needs a result must receive
  it through its task payload or an immutable shared state.
- Normal drain shutdown closes root task admission while keeping internal
  dispatch for already-accepted graph nodes open, detaches subsystem producers,
  and pumps `GameThreadDeferred` without the normal frame budget until the
  accepted graph is quiescent. Cancel shutdown cancels not-started worker and
  deferred nodes and waits for running bodies. Only then does lifecycle close
  and uninstall the GameThread adapter. Both modes leave every accepted handle
  terminal.
- The engine uses a cross-executor shutdown entry point rather than blocking in
  `ShutdownTaskScheduler(true)` while GameThread work still needs pumping.
  Isolated worker-only callers retain the existing scheduler shutdown behavior.
- UObject/editor model mutation remains owned by GameThread. A queue entry
  must use a documented weak/lifetime token or an owned state object; a raw
  pointer capture is not made safe by queueing.

### Domain mailbox boundary

- Subsystem mailboxes remain responsible for request serials, generation
  checks, latest-wins policy, batching, domain-specific payloads, and result
  streams. A generic continuation may enqueue or drain such a mailbox, but it
  does not erase those semantics.
- The Asset Compatibility Audit remains the first pilot. Its per-package
  records continue to stream through the request-serial mailbox so progress and
  partial results do not regress. The typed task result carries only an owned
  terminal summary, and a `GameThreadDeferred` outcome continuation publishes
  terminal model state after rechecking the request serial. This pilot proves
  typed terminal composition and target routing, not removal of streaming
  mailboxes.
- Render and RHI command queues are not treated as generic mailboxes. Their
  ownership and ordering rules are stronger than the low-priority GameThread
  queue contract.

### Waiting, cancellation, and observability

- Building a dependency graph is preferred to blocking waits. A worker may
  retain the current bounded same-scheduler helping behavior for eligible
  worker tasks, but it may never execute a `GameThreadDeferred` continuation
  while waiting.
- GameThread may not call ordinary `WaitTask` on a `GameThreadDeferred` node.
  Cross-executor shutdown is the only V1 pump-until-quiescent path; it is
  bounded by accepted graph work, closes root admission first, and records long
  callbacks rather than applying the normal frame budget.
- Cancellation is cooperative for running bodies and prevents not-started
  continuations when cancellation wins. A queued `GameThreadDeferred` entry
  checks its token and generation immediately before publication or mutation.
- Parent/continuation IDs, target, queue residency, timestamps, rejection,
  cancellation, stale-drop, and final outcome are included in task diagnostics
  without retaining unbounded terminal payloads.

## Current Foundations and Gaps

| Area | Current foundation | Gap closed by this plan |
| --- | --- | --- |
| Worker execution | Bounded `FTaskScheduler`, prerequisites, cancellation, waits, diagnostics | Typed result handles and first-class continuation nodes |
| Dependency graph | Immutable success prerequisite list and terminal propagation | Explicit success/completion edges, `Then`, `ThenOutcome`, and reusable fan-in |
| GameThread | Existing subsystem-owned drain points only | One bounded deferred executor plus frame and shutdown pumps |
| Rendering | Existing `FRenderThreadCommandPipe` with admission and fences | Remains domain-owned; generic adapter deferred pending a production use case and bounded policy |
| RHI | Existing ordered RHI queue with blocking backpressure | Remains domain-owned; generic adapter deferred pending non-blocking worker admission |
| Mailboxes | Asset import and compatibility-audit mailboxes plus thumbnail/result queues | Clear distinction between generic execution and domain result ownership |
| Lifecycle | Worker scheduler shutdown currently blocks after frame ticking stops | Cross-executor admission, shutdown pumping, quiescence, and adapter teardown ordering |
| Documentation | Generic mailbox and typed results are deferred features in `TaskSystem.md` | Evidence-backed lasting contract after implementation |

## Stage 0 Frozen Contract

This section is implementation input for Stages 1 through 3. It is deliberately
more specific than the lasting task-system documentation, which continues to
describe only implemented behavior until the corresponding stage lands.

### Public API and ownership

The additive public vocabulary is:

```cpp
enum class ETaskTarget : uint8 { AnyWorker, GameThreadDeferred };
enum class ETaskPriority : uint8 { High, Normal, Low };
enum class ETaskDependencyKind : uint8 { Success, Completion };

enum class ETaskTerminalReason : uint8
{
    None,
    DependencyFailed,
    DependencyCanceled,
    CancellationRequested,
    DispatchRejected,
    Superseded,
    StaleGeneration,
    CallbackFailure,
    ShutdownCanceled,
};

struct FTaskCoalescingKey
{
    uint64 OwnerDomain = 0;
    uint64 WorkId = 0;
    uint64 Generation = 0;
};

struct FTaskContinuationOptions
{
    std::span<const FTaskHandle> Prerequisites;
    FTaskCancellationToken CancellationToken;
    FTaskGenerationToken GenerationToken;
    std::optional<FTaskCoalescingKey> CoalescingKey;
    uint64 EstimatedPayloadBytes = 0;
    ETaskTarget Target = ETaskTarget::AnyWorker;
    ETaskPriority Priority = ETaskPriority::Normal;
};
```

`FTaskGenerationSource` owns a shared monotonically increasing generation.
`Capture()` returns an `FTaskGenerationToken` containing the shared state and
observed generation; `Advance()` invalidates older tokens. A default token has
no generation constraint. The deferred executor checks a constrained token
immediately before claiming an entry and publishes `Canceled/StaleGeneration`
without invoking the callback when it no longer matches.

`TTaskHandle<T>` contains an ordinary `FTaskHandle` plus shared typed result
state. It exposes the same task queries, `GetTaskHandle()`, and
`GetResultShared() -> std::shared_ptr<const T>`. Result observation returns an
empty owner for an invalid, nonterminal, failed, or canceled task; it never
blocks and never returns a reference without an owner. `TTaskHandle<T>` is
copyable even when `T` is move-only. There is no `TTaskHandle<void>` in V1;
void work continues to use `FTaskHandle`.

`FTaskOutcome<T>` is an owned snapshot containing the predecessor handle,
terminal state and reason, copied diagnostic, and an immutable shared result
owner only when the predecessor succeeded. `FTaskOutcome<void>` omits the
result owner. Diagnostics add dependency kinds, target, terminal reason,
dispatch and queue timestamps, queue residency, and the direct blocking task
ID; they never copy or retain `T`.

The public operation families are:

- existing `LaunchTask` and `LaunchCancelableTask` overloads remain unchanged;
- `LaunchTask<T>` and `LaunchCancelableTask<T>` accept copyable
  `std::function`-compatible callables returning `T`;
- `Then<T, U>` consumes `const T&`, uses success edges, and returns
  `TTaskHandle<U>` or `FTaskHandle` when its callback returns `void`;
- the void predecessor overload of `Then` invokes a no-argument callback;
- `ThenOutcome<T, U>` consumes an owned `FTaskOutcome<T>` by value, uses
  completion edges, and returns the corresponding typed or void handle; and
- `FTaskContinuationOptions::Prerequisites` adds fan-in predecessors to the
  primary handle. The callback receives the primary result/outcome and may
  capture additional typed handles to inspect their immutable results.

The primary predecessor is automatically included and duplicate task IDs are
deduplicated. Every predecessor must be valid and belong to the current
scheduler lifetime. Empty callables, invalid handles, foreign lifetimes, and a
dependency on the node being constructed reject the operation before a node is
accepted. Rejection returns an invalid handle and increments the scheduler's
rejection diagnostics. The public API cannot construct a cycle because all
edges target already-published older nodes.

Typed launch wrappers allocate shared result state before submission and place
only copyable shared owners inside the existing `std::function` boundary. The
callable may move one `T` into pending result storage. The graph atomically
publishes that storage only when the body returns normally and cancellation has
not won; otherwise it destroys the pending value without making it observable.
The scheduler owns a node and its pending callback until dispatch. An executor
owns the callback from successful admission through invocation or discard.
Scheduler/node owners, typed handles, and outcome wrappers jointly pin the
immutable result, which is destroyed after the last such owner releases it.

### Dependency and terminal state machines

A success edge waits for every predecessor to become terminal so fan-in has a
deterministic aggregate outcome:

```text
Accepted/Waiting
  -> all predecessors terminal
     -> all Succeeded -> dispatch requested
     -> any Failed    -> Canceled/DependencyFailed
     -> else any Canceled -> Canceled/DependencyCanceled
```

When more than one predecessor blocks a success continuation, failure takes
precedence over cancellation and the lowest blocking task ID is recorded. The
callback is never invoked. Existing void tasks launched with
`FTaskLaunchOptions::Prerequisites` retain their current propagation timing;
the aggregate rule applies to new continuation nodes only.

A completion edge never converts a predecessor outcome into the continuation's
own outcome:

```text
Accepted/Waiting -> all predecessors terminal -> dispatch requested
dispatch accepted -> Queued -> Running -> Succeeded | Failed | Canceled
```

The primary predecessor's owned `FTaskOutcome<T>` is created only after all
completion edges settle. Additional predecessor outcomes remain queryable
through captured handles. A callback return publishes its result and
`Succeeded`; a standard or unknown exception publishes
`Failed/CallbackFailure`. Cancellation winning before executor claim publishes
`Canceled/CancellationRequested`; cancellation observed while running remains
cooperative, and normal return after a winning request publishes `Canceled`
without a result. An exception from a running callback still wins over
cancellation and publishes `Failed/CallbackFailure`.

Ready-node dispatch has exactly one terminal report path:

```text
dispatch requested
  -> executor accepts -> executor owns completion report
  -> executor rejects -> Canceled/DispatchRejected
  -> deferred replacement wins -> old node Canceled/Superseded
  -> generation check fails -> Canceled/StaleGeneration
```

A node cannot be invoked inline while a predecessor is publishing terminal
state. In particular, `GameThreadDeferred` always enters its queue even when
released on GameThread. A worker wait may help only `AnyWorker` work; ordinary
GameThread `WaitTask` on a nonterminal `GameThreadDeferred` node is rejected
with a stable diagnostic and does not pump.

### GameThread deferred executor policy

`FGameThreadDeferredWorkQueue` is created by `FEngineLoop::PreInit()` after the
worker scheduler starts and before any engine module may create continuations.
Installing its graph adapter publishes a nonzero, monotonically increasing
adapter generation. An accepted entry binds that generation. A missing,
closing, or mismatched adapter rejects dispatch as
`Canceled/DispatchRejected`; uninstall occurs only after graph quiescence.

V1 configuration constants are:

| Policy | Default |
| --- | --- |
| Maximum queued entries | 1,024 |
| Maximum declared queued payload | 8 MiB |
| Maximum declared payload per entry | 1 MiB |
| Normal frame pump item budget | 64 callbacks |
| Normal frame pump time budget | 1 millisecond |
| Long callback threshold | 2 milliseconds |

A `GameThreadDeferred` continuation must declare
`EstimatedPayloadBytes` in `[1, 1 MiB]`; zero or an over-limit estimate is a
dispatch rejection. Admission atomically reserves both an entry and its
declared bytes. `AnyWorker` ignores payload, priority, generation, and
coalescing fields except that its cancellation token remains effective.

Queues are FIFO within `High`, `Normal`, and `Low`; each pump chooses the oldest
entry from the highest nonempty priority. V1 intentionally provides no
starvation guarantee because all three classes are deferred and bounded.
The time budget is checked after each callback, so one long callback may exceed
the frame budget but is recorded. Count and byte reservations are released
exactly once when an entry is claimed, rejected, superseded, or canceled.

Coalescing is opt-in and equality uses all three `FTaskCoalescingKey` fields.
At admission, an equal not-started entry may be replaced atomically. Capacity
is evaluated as though the old reservation were removed; if the replacement
still does not fit, the new entry is rejected and the old entry remains.
Otherwise the old node becomes `Canceled/Superseded` and the replacement keeps
its own task ID and reservation. Running or terminal entries are never
superseded. Owner domains are stable nonzero subsystem-defined constants;
generation prevents a new owner lifetime from colliding with an old one.

The normal pump safe point is in `FEngineLoop::Tick()` immediately after
`GEngine->Tick()` returns and before application events, UI rendering, garbage
collection, or frame publication. This lets engine/subsystem ticks finish
their mutations before deferred publication begins and keeps rendering/RHI
work on their existing paths. The pump runs every engine tick, including when
all windows are minimized; minimized waiting happens afterward, so deferred
work continues at the existing 20 Hz minimized cadence. Frame-critical waits,
render fences, RHI backpressure, and object destruction never consume this
queue or its budget.

Metrics include current/peak entry and declared-byte depth, accepted, rejected,
superseded, canceled, expired-generation and callback-failure counts, oldest
age, per-priority depth, pump count/items/time, and long-callback identity and
duration. Declared bytes remain attributable to owner domain and task ID but
are not presented as measured allocator usage.

### Cross-executor shutdown

The existing `ShutdownTaskScheduler(bool)` remains the worker-only entry point
for isolated programs and tests. `FEngineLoop::Exit()` instead calls
`ShutdownTaskSystem(ETaskShutdownMode)` on GameThread. Root task and
continuation submission are distinct from dispatch of already-accepted graph
nodes.

Drain shutdown follows:

```text
Running
  -> ClosingRoots: reject all new Launch/Then calls
  -> Draining: keep internal worker/deferred dispatch open
       pump GameThreadDeferred without frame item/time budgets
       wait on graph-progress notification when no deferred item is ready
       repeat until every accepted graph node is terminal
  -> ClosingExecutors: close and uninstall deferred adapter, then stop workers
  -> Stopped: publish final diagnostics
```

Cancel shutdown follows:

```text
Running
  -> ClosingRoots
  -> Canceling: cancel waiting/queued worker and deferred nodes, request
       cooperative cancellation of running bodies, and keep accepted-node
       propagation open until every running body and graph node is terminal
  -> ClosingExecutors
  -> Stopped
```

Cancel shutdown does not invoke a not-started deferred callback. Drain shutdown
may invoke every accepted deferred callback and therefore keeps its owning
modules and `GEngine` alive until quiescence. Producer detachment remains ahead
of `ShutdownTaskSystem`; object drain, module unload, render admission close,
and RHI shutdown remain afterward. Both modes close root admission exactly
once, never reopen it, and leave every accepted handle terminal.

Nested `LaunchTask`, typed launch, `Then`, or `ThenOutcome` from a shutdown-pump
callback is a rejected root submission. `CancelTask` and diagnostic/result
queries remain allowed. Recursive normal pumping returns without executing an
entry and increments a reentrancy diagnostic. A recursive
`ShutdownTaskSystem` call is rejected and the outer shutdown remains the sole
coordinator. These rules prevent callbacks from extending the bounded shutdown
graph or recursively consuming queue ownership.

### Asset Compatibility Audit pilot boundary

The pilot retains these existing behaviors:

- copied, path-sorted package inputs and a value-owned reflection catalog cross
  to the worker;
- each completed package record is streamed through the request-serial mailbox
  and may update progress before terminal completion;
- canceled/interrupted packages publish no partial record;
- rerun and project change cancel and drain the old request, advance the serial,
  and prevent stale records from entering the new path-keyed model;
- fingerprint reconciliation and every editor-model mutation remain on
  GameThread; and
- shutdown closes audit admission and releases worker/model state before module
  teardown.

After Stage 3, the mailbox `FNotice` carries records only. The worker returns an
owned `FAssetCompatibilityAuditSummary` containing request serial, processed
count, terminal classification, and copied failure text. A
`ThenOutcome(..., GameThreadDeferred)` callback owns a weak model-lifetime
token plus the request serial, checks both immediately before mutation, and
publishes terminal state exactly once. Scheduler failure, cancellation,
dispatch rejection, supersession, stale generation, and callback failure map
to distinct diagnostics; none are converted to `Completed`. Streaming record
order, progress timing, latest-request policy, and reconciliation remain
unchanged.

### Frozen test inventory

Stage 1 adds focused Core tests for:

- void API source compatibility and unchanged prerequisite propagation;
- typed success, move-only result construction, no extra result copy, result
  destruction after the last graph/handle/outcome owner, and empty access for
  invalid/nonterminal/failed/canceled handles;
- worker `Then` chains, typed-to-void and void-to-typed links, fan-out sharing
  one immutable result, fan-in release exactly once, duplicate prerequisite
  deduplication, and callback registration racing predecessor completion;
- deterministic lowest-ID fan-in blocker with failure-over-cancellation
  precedence, completion-edge invocation for every terminal predecessor state,
  and owned outcome lifetime after predecessor-handle release;
- cancellation before release, before worker claim, while running, and racing
  successful result publication; callback exception precedence and unknown
  exception diagnostics;
- invalid/foreign-lifetime predecessor rejection, stopped/closing scheduler
  rejection, nested continuation submission, worker wait helping only eligible
  worker work, and drain/cancel shutdown quiescence; and
- parent, prerequisite, edge-kind, target, reason, timing, retained-result, and
  nonterminal diagnostics without payload retention.

Stage 2 adds manual-pump Core/Launch tests for:

- no inline GameThread execution, game-thread affinity, FIFO per priority,
  cross-priority selection, item/time budget exhaustion, and minimized-tick
  pumping;
- entry/byte/per-entry saturation, zero estimate rejection, atomic reservation
  release, successful supersession, failed replacement preserving the old
  entry, running-entry non-supersession, and owner/generation key isolation;
- stale generation, explicit cancellation, missing/closing/mismatched adapter,
  callback failure, recursive pump, forbidden GameThread wait, and exactly-once
  terminal reporting for every rejection race;
- drain shutdown worker-to-GameThread chains after the frame loop, cancel
  shutdown with queued and running work, root rejection during shutdown,
  callback reentrancy, adapter teardown order, and final graph/queue
  quiescence; and
- proof that render commands, render fences, RHI commands, and their admission
  counters are unaffected by deferred-queue saturation.

Stage 3 extends the existing Asset Compatibility Audit tests with streaming
progress before terminal publication, typed summary ownership, serial and weak
lifetime rechecks, rerun/project-change replacement, editor close, cancellation
at every package boundary, scheduler drain/cancel, dispatch rejection, stale
generation, and exactly-once terminal diagnostics.

## Implementation Stages

### Stage 0: Freeze the V1 contract and shutdown state machine

Dependencies: current `TaskSystem.md`, existing task tests, and current
worker/render/RHI lifecycle contracts.

- [x] Define the public names and ownership of `TTaskHandle<T>`, immutable shared
  result access, `FTaskOutcome<T>`, `Then`, `ThenOutcome`, continuation options,
  `ETaskTarget`, priority, cancellation, and coalescing identity.
- [x] Record success and completion dependency-edge state diagrams, including
  fan-in aggregation, result lifetime, invalid access, predecessor propagation,
  dispatch rejection, supersession, and callback failure.
- [x] Specify the GameThread pump location, safe-point ordering, default
  budget, queue limits, explicit payload estimates, priority order,
  supersession behavior, and minimized-window behavior.
- [x] Specify the cross-executor drain and cancel shutdown state machines,
  including root-admission closure, accepted-node dispatch, shutdown pumping,
  callback reentrancy, adapter generations, and teardown order.
- [x] Characterize the Asset Compatibility Audit pilot and record its streaming
  progress, partial-result, request-serial, cancellation, and terminal-summary
  invariants.
- [x] Add API/state-machine test cases for every decision that would otherwise
  remain ambiguous during implementation.

#### Acceptance Gate

- The API and state diagrams identify who owns each task payload, callback,
  queue, and terminal outcome.
- No open decision remains about dependency-edge semantics, result observation,
  target rejection, GameThread pumping, cancellation/failure propagation,
  supersession, or shutdown behavior for Stage 1.
- The pilot has a bounded before/after behavior specification and a list of
  existing mailbox semantics that must not change.

### Stage 1: Add typed results and worker continuations

Dependencies: Stage 0; existing worker scheduler and task-state tests.

- [x] Add additive typed-result task state and handle APIs while keeping the
  current void API behavior unchanged.
- [x] Implement worker-target `Then` with success edges and `ThenOutcome` with
  completion edges, both returning new handles and pinning immutable shared
  result/outcome state without exposing task-local storage.
- [x] Implement success, failure, cancellation, invalid-handle,
  cross-scheduler/lifetime, and fan-in propagation for both edge types.
- [x] Keep public wrappers copyable under the existing C++20 `std::function`
  boundary; add compile-time and runtime coverage for move-only result types.
- [x] Extend diagnostics and shutdown quiescence to include continuation nodes
  and result storage.
- [x] Add focused unit tests for result lifetime, move/copy policy, chain
  ordering, fan-in, cancellation races, failure propagation, reentrancy, and
  scheduler shutdown.

#### Acceptance Gate

- A typed worker task, including one producing a move-only `T`, can feed one or
  more read-only worker continuations with no caller mailbox and deterministic
  terminal outcomes.
- Existing void-task callers and existing worker wait/cancellation tests pass
  unchanged.
- No task body or continuation can observe a dangling result, run after a
  failed prerequisite, or leave an accepted node nonterminal at shutdown.

### Stage 2: Add `GameThreadDeferred` and cross-executor shutdown

Dependencies: Stage 1; engine frame/lifecycle ownership identified in Stage 0.

- [x] Add the V1 `AnyWorker`/`GameThreadDeferred` target vocabulary and
  executor/dispatcher boundary without exposing native thread IDs.
- [x] Implement `FGameThreadDeferredWorkQueue` with bounded admission, priority,
  explicit payload estimates, optional coalescing key, supersession,
  cancellation/generation checks, pump budget, and diagnostics.
- [x] Add the engine-owned GameThread pump and lifecycle admission/shutdown
  hooks at the documented safe point.
- [x] Add the cross-executor shutdown entry point and replace the engine exit
  path that would otherwise block the GameThread before deferred work can run.
- [x] Implement `Then(..., GameThreadDeferred, ...)` and prove that it never
  executes on a worker or completion thread.
- [x] Add manual-pump tests and engine integration tests for ordering, budget
  exhaustion, queue saturation, supersession, stale generations, cancellation,
  dispatch rejection, drain/cancel shutdown, and callback reentrancy.
- [x] Verify that frame-critical synchronization remains outside the deferred
  queue and that GameThread ordinary waits cannot pump or deadlock it.

#### Acceptance Gate

- A worker-to-`GameThreadDeferred` continuation executes only from the
  game-thread pump, remains observable while deferred, and reaches a terminal
  state on normal shutdown or cancellation.
- Entry-count and declared-payload limits are enforced under sustained producer
  pressure, and inaccurate estimates remain attributable in diagnostics.
- Normal engine exit can drain accepted worker-to-`GameThreadDeferred` chains
  after the regular frame loop stops, without deadlock or
  use-after-module-shutdown.

### Stage 3: Migrate and validate the editor pilot

Dependencies: Stage 2; existing Asset Compatibility Audit behavior and tests.

- [x] Change the Asset Compatibility Audit worker to return an owned typed
  terminal summary while retaining per-package streaming notices in its
  request-serial mailbox.
- [x] Publish the terminal summary through an explicit
  `GameThreadDeferred` outcome continuation that rechecks request serial and
  model lifetime immediately before mutation.
- [x] Verify project-change, cancellation, replacement, editor close, and
  scheduler shutdown races while preserving incremental progress and partial
  results.
- [x] Add tracing/diagnostics that show the full worker-to-target continuation
  chain and distinguish stale-drop, cancellation, rejection, and callback
  failure.

#### Acceptance Gate

- The pilot has no lost, duplicated, stale, or cross-thread model publication
  under its existing replacement and shutdown scenarios.
- Existing per-package progress remains visible before terminal completion and
  cancel/project-change behavior does not regress.
- A continuation chain can cross worker and `GameThreadDeferred` while every
  node has a terminal diagnostic and the process exits without nonterminal work.

### Stage 4: Measure, document, and decide broader adoption

Dependencies: Stage 3; profiler evidence from the pilot and representative
editor workloads.

- [x] Measure queue latency, pump cost, allocation/capture cost, continuation
  throughput, stale-drop rate, and frame impact under normal and saturated
  workloads.
- [x] Review additional call sites such as async import and thumbnail work;
  migrate only where the generic continuation contract improves ownership or
  observability without removing necessary domain mailbox semantics.
- [x] Evaluate RenderThread and RHIThread adapters only as follow-up candidates.
  Require a named production caller, module-owned callable contract, adapter
  lifetime rules, and non-blocking worker-side admission before opening a new
  implementation plan.
- [x] Move stable rules from this plan into `Documentation/Runtime/Core/TaskSystem.md`
  and the relevant thread/lifecycle documentation.
- [x] Add developer guidance showing when to use a typed continuation, a
  subsystem mailbox, a serialized pipe/lane, or a render/RHI command queue.
- [x] Record deferred work that lacks workload evidence, including dedicated IO,
  fibers/coroutines, work stealing, and broad subsystem migration.
- [x] Complete the final validation, full build, applicable editor smoke test,
  stage handoff, and plan status update.

#### Acceptance Gate

- The final design has measured bounds and an explicit owner for every queue and
  continuation target.
- The CPU task-system documentation describes the lasting contract without
  requiring readers to interpret this plan as runtime behavior.
- Broader adoption is either justified by evidence and migrated incrementally,
  or explicitly deferred with a named reason and follow-up owner.

#### Qualification Evidence And Adoption Decision

The Debug representative workload fills a 256-entry queue with 16 KiB of
declared captures and invalidates 32 generations before an unlimited pump. On
the 2026-08-07 Agent profile run it measured 22.1382 ms admission, 9.5563 ms
pump time, 19.84325 ms average queue residency, 31.7329 ms maximum residency,
224 executed callbacks, and 32 stale drops (125,000 ppm). This intentionally
saturated batch is qualification evidence rather than a release performance
promise. Existing bounded-pump tests cover normal-frame item/time effects;
capacity tests cover count, total-byte, per-entry, and missing-estimate
rejection without growth.

No broader production migration is justified in this stage:

- AssetImportCore retains its coordinator mailbox because that owner defines
  provider closure, latest-by-owner replacement, explicit drain, and result
  take semantics.
- LevelEditor retains the source-thumbnail queues because that cache defines
  visible-request priority, decode concurrency, per-frame upload throttling,
  serial validation, and RenderThread/RHI upload ownership.
- RenderCore owns any future RenderThread adapter proposal; it first requires a
  named caller, render-command context and lifetime rules, and non-blocking
  worker admission.
- The active RHI backend owns any future RHIThread adapter proposal; it first
  requires a named caller and non-blocking admission compatible with current
  ordered backpressure.

Each completed stage ends with a compact handoff recording the baseline commit,
working set, key symbols and decisions, open questions, and validation outcome.
Each implementation stage lands as an independent local commit under the
repository handoff rules.

### Stage 0 Handoff

- Baseline commit: `054e2914a8b029c00905670cb0789f71291b964b`.
- Working set: this plan; validated against `Threading/Task.h`, `Task.cpp`,
  `LaunchEngineLoop.cpp`, `AssetCompatibilityAudit.h/.cpp`, Core task tests, and
  Asset Compatibility Audit tests.
- Key decisions: immutable shared typed results; continuation nodes with
  aggregate success or completion edges; no inline named-target execution;
  bounded priority GameThread queue at the post-engine-tick safe point; explicit
  generation/coalescing metadata; and engine-owned drain/cancel coordination.
- Open questions: none blocking Stage 1. Default limits remain configurable so
  Stage 4 measurements may tune values without changing admission semantics.
- Validation: plan structure validation passed; source and existing-test review
  found no conflict with the frozen additive API or pilot preservation rules.

### Stage 1 Handoff

- Baseline commit: `e37e01fa3413b9bb78c23af4c574d6c12b054dfd`.
- Working set: `Threading/Task.h`, `Threading/Task.cpp`, Core
  `ThreadingTests.cpp`, and this plan.
- Key symbols: `TTaskHandle<T>`, `FTaskOutcome<T>`, `Then`, `ThenOutcome`,
  `FTaskContinuationOptions`, `ETaskDependencyKind`, `ETaskTerminalReason`,
  `FTaskStateData::OnPrerequisiteTerminal`, and typed-result completion
  publication.
- Key decisions: existing launch prerequisites retain eager propagation while
  new continuation fan-in waits for every predecessor; typed values are
  published only with `Succeeded`; executor wrappers and handles share immutable
  result storage; completion hooks run outside the task-state lock; and Stage 1
  rejects the not-yet-installed `GameThreadDeferred` target.
- Open questions: none blocking Stage 2. Priority, payload, generation, and
  coalescing fields remain to be added with the deferred executor.
- Validation: all 73 `CoreConcurrencyTests` passed, including nine new focused
  continuation tests; a full `all` build passed under the Agent profile.

### Stage 2 Handoff

- Baseline commit: `382d316591c7f4de8f9b24fbba1e120b6fd01632`.
- Working set: `Threading/Task.h`, `Threading/Task.cpp`,
  `LaunchEngineLoop.cpp`, Core `ThreadingTests.cpp`, and this plan.
- Key symbols: `FGameThreadDeferredWorkQueue`,
  `FGameThreadDeferredWorkQueueConfig`, `FTaskGenerationSource`,
  `PumpGameThreadDeferredWork`, `ShutdownTaskSystem`, executor-target metadata,
  and the engine lifecycle smoke's worker-to-GameThread chain.
- Key decisions: deferred work reserves entry count and declared bytes before
  admission; priority is strict with FIFO inside each class; coalesced and
  canceled entries release callable storage without tombstone growth; normal
  frame pumping uses configured budgets; shutdown pumping is unbudgeted but
  root admission remains closed; and recursive pump/shutdown calls cannot take
  ownership from the outer callback.
- Open questions: none blocking Stage 3. Default queue limits remain subject to
  Stage 4 measurement, without changing the admission contract.
- Validation: all 80 `CoreConcurrencyTests` passed; the complete `all` build
  passed; and the hidden-window editor lifecycle smoke drained a worker-to-
  `GameThreadDeferred` continuation after the frame loop stopped before clean
  rendering/RHI shutdown.

### Stage 3 Handoff

- Baseline commit: `0b36a7ade192a9d13eaed51b5aa990eb3e40be0a`.
- Working set: `AssetCompatibilityAudit.h/.cpp`, its native tests,
  `Threading/Task.cpp` queue cleanup, and this plan.
- Key symbols: `FTerminalSummary`, `FPublicationLifetime`, the audit's worker
  and terminal handles, request `FTaskGenerationSource`, and
  `AssetCompatibility.PublishTerminal`.
- Key decisions: mailbox notices carry records only; the terminal callback
  drains those notices before publishing state; cancellation that must not pump
  explicitly terminalizes the queued publisher; shutdown invalidates the weak
  lifetime state; and current dispatch/callback failures remain distinguishable
  through terminal-task diagnostics.
- Open questions: none blocking Stage 4.
- Validation: all 11 focused Asset Compatibility Audit tests passed, including
  streaming-before-terminal, stale project change, and cross-executor shutdown;
  the complete `EditorAssetWorkflowTests` target passed.

### Stage 4 Handoff

- Baseline commit: `c82ed934e8db1fbdfdf9fea604e88a3b1188396b`.
- Working set: Core `ThreadingTests.cpp`, this plan, `TaskSystem.md`, and
  `RuntimeLifecycle.md`; adoption review covered AssetImportCore async import
  and LevelEditor source-image thumbnails without changing either owner.
- Key symbols and decisions:
  `RepresentativeWorkloadMeasuresAdmissionPumpResidencyAndStaleDrops` records
  the saturated qualification baseline; stable documentation now owns typed
  result, continuation, GameThread queue, safe-point, shutdown, and primitive-
  selection contracts. Broader mailbox migration and RenderThread/RHIThread
  adapters are deferred to their named subsystem owners pending production
  evidence and non-blocking admission contracts.
- Open questions: none. Default executor limits remain configurable; the
  qualification numbers are environment baselines rather than release
  performance promises.
- Validation: plan validation passed; all 81 `CoreConcurrencyTests` passed; the
  complete `EditorAssetWorkflowTests` ran 58 tests with 57 passed and one
  expected skip; the complete `all` build passed; and the hidden-window editor
  lifecycle smoke completed three ticks and cross-executor shutdown cleanly.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| API compatibility | Existing void launch, prerequisite, cancellation, wait, and diagnostics tests remain valid |
| Typed results | Immutable shared ownership, move-only result construction, lifetime, fan-out, invalid access, and terminal-state behavior |
| Continuations | Success/completion edges, failure, cancellation, outcome-aware cleanup, fan-in, ordering, reentrancy, and invalid-handle rejection |
| Target routing | `AnyWorker` and `GameThreadDeferred` execute only on their logical executor and report dispatch rejection exactly once |
| GameThread queue | Bounded admission, explicit payload estimates, priority, pump budget, supersession, stale generations, cancellation, callback failure, and metrics |
| Mailbox boundary | Pilot preserves request serials, latest-wins behavior, batching, and domain payload ownership |
| Render/RHI boundary | Existing command APIs and tests remain unchanged; no generic continuation bypasses their domain ownership |
| Waiting | No deferred target is executed by an ineligible waiter; GameThread ordinary waits reject self-dependent deferred work |
| Lifecycle | Startup failure, adapter absence, normal shutdown pump, cancel shutdown, late dispatch, target teardown, module lifetime, and terminal-handle quiescence |
| Concurrency | Sustained producer pressure, multiple consumers, replacement races, and nested task submission |
| Diagnostics | Parent/prerequisite IDs, target, queue age, rejection, stale-drop, cancellation, callback failure, and final outcome |
| Performance | Frame-time impact, queue latency, allocation rate, and saturation behavior in representative editor workloads |
| Integration | Focused Core, editor pilot, render/RHI smoke, and applicable full build/test validation per build documentation |

## Definition of Done

- Durin has an additive typed task/continuation API with explicit success and
  completion edges, immutable shared results, V1 logical executors, and
  deterministic outcome propagation.
- The global GameThread deferred-work queue is bounded, budgeted, observable,
  lifecycle-safe, and used only for work that permits deferred execution.
- Engine shutdown can pump accepted GameThread deferred continuations to graph
  quiescence without blocking the owning thread or reopening root admission.
- Existing subsystem mailboxes remain available for domain-specific result and
  ordering semantics; RenderThread and RHIThread retain their own queues.
- At least one production editor path uses the new model without changing its
  ownership, generation, cancellation, or shutdown guarantees.
- The worker-to-`GameThreadDeferred` chain is covered by focused tests,
  integration tests, diagnostics, workload measurements, and required full
  validation.
- Stable behavior is moved into the owning runtime/thread documentation, and
  unsupported extensions remain explicitly deferred.

## Deferred Follow-ups

- Typed task integration with broader asset import and thumbnail pipelines after
  pilot measurements.
- RenderThread continuation adapter after a production caller defines required
  render-command context, admission, cancellation, and module-lifetime rules.
- RHIThread continuation adapter after a production caller and a non-blocking
  worker-side admission contract exist; the current blocking backpressure path
  is not used directly from scheduler completion.
- Single-consumer move-out continuations and a move-only callable type-erasure
  boundary if immutable shared result observation proves insufficient.
- Dedicated IO executor and IO-specific backpressure policy.
- Work stealing, fibers, coroutine-backed waits, or replacement of the current
  worker wait-helping policy.
- RenderGraph-specific task integration.
- A general per-resource `FPipe`/serialized-lane abstraction if multiple
  subsystems demonstrate the same need.
- User-visible editor scheduling controls or a general-purpose scripting API.

## Related Documentation

- [CPU Task System](../Runtime/Core/TaskSystem.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Unreal Engine Tasks System](https://dev.epicgames.com/documentation/unreal-engine/tasks-systems-in-unreal-engine)
- [Unreal Engine `FPipe`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FPipe)
- [Unreal Engine `TFuture::Then`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/TFutureBase/Then)
- [Unreal Engine `AsyncTask`](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/Async/AsyncTask?application_version=5.5)
- [Unreal Engine `ENamedThreads`](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/ENamedThreads__Type)

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Source/Editor/DurinEd/Private/Asset/AssetCompatibilityAudit.cpp`
- `Engine/Source/Editor/AssetImportCore/Private/AsyncImport.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SourceImageThumbnailCache.cpp`
