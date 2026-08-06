# Task Continuations and Thread Dispatch Plan

Summary: Extend Durin's bounded CPU task system with typed worker continuations and a bounded GameThread deferred executor while preserving subsystem, render, and RHI ownership.

Last reviewed: 2026-08-07

Status: Active
Completed:

## Current Status

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

## Implementation Stages

### Stage 0: Freeze the V1 contract and shutdown state machine

Dependencies: current `TaskSystem.md`, existing task tests, and current
worker/render/RHI lifecycle contracts.

- [ ] Define the public names and ownership of `TTaskHandle<T>`, immutable shared
  result access, `FTaskOutcome<T>`, `Then`, `ThenOutcome`, continuation options,
  `ETaskTarget`, priority, cancellation, and coalescing identity.
- [ ] Record success and completion dependency-edge state diagrams, including
  fan-in aggregation, result lifetime, invalid access, predecessor propagation,
  dispatch rejection, supersession, and callback failure.
- [ ] Specify the GameThread pump location, safe-point ordering, default
  budget, queue limits, explicit payload estimates, priority order,
  supersession behavior, and minimized-window behavior.
- [ ] Specify the cross-executor drain and cancel shutdown state machines,
  including root-admission closure, accepted-node dispatch, shutdown pumping,
  callback reentrancy, adapter generations, and teardown order.
- [ ] Characterize the Asset Compatibility Audit pilot and record its streaming
  progress, partial-result, request-serial, cancellation, and terminal-summary
  invariants.
- [ ] Add API/state-machine test cases for every decision that would otherwise
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

- [ ] Add additive typed-result task state and handle APIs while keeping the
  current void API behavior unchanged.
- [ ] Implement worker-target `Then` with success edges and `ThenOutcome` with
  completion edges, both returning new handles and pinning immutable shared
  result/outcome state without exposing task-local storage.
- [ ] Implement success, failure, cancellation, invalid-handle,
  cross-scheduler/lifetime, and fan-in propagation for both edge types.
- [ ] Keep public wrappers copyable under the existing C++20 `std::function`
  boundary; add compile-time and runtime coverage for move-only result types.
- [ ] Extend diagnostics and shutdown quiescence to include continuation nodes
  and result storage.
- [ ] Add focused unit tests for result lifetime, move/copy policy, chain
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

- [ ] Add the V1 `AnyWorker`/`GameThreadDeferred` target vocabulary and
  executor/dispatcher boundary without exposing native thread IDs.
- [ ] Implement `FGameThreadDeferredWorkQueue` with bounded admission, priority,
  explicit payload estimates, optional coalescing key, supersession,
  cancellation/generation checks, pump budget, and diagnostics.
- [ ] Add the engine-owned GameThread pump and lifecycle admission/shutdown
  hooks at the documented safe point.
- [ ] Add the cross-executor shutdown entry point and replace the engine exit
  path that would otherwise block the GameThread before deferred work can run.
- [ ] Implement `Then(..., GameThreadDeferred, ...)` and prove that it never
  executes on a worker or completion thread.
- [ ] Add manual-pump tests and engine integration tests for ordering, budget
  exhaustion, queue saturation, supersession, stale generations, cancellation,
  dispatch rejection, drain/cancel shutdown, and callback reentrancy.
- [ ] Verify that frame-critical synchronization remains outside the deferred
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

- [ ] Change the Asset Compatibility Audit worker to return an owned typed
  terminal summary while retaining per-package streaming notices in its
  request-serial mailbox.
- [ ] Publish the terminal summary through an explicit
  `GameThreadDeferred` outcome continuation that rechecks request serial and
  model lifetime immediately before mutation.
- [ ] Verify project-change, cancellation, replacement, editor close, and
  scheduler shutdown races while preserving incremental progress and partial
  results.
- [ ] Add tracing/diagnostics that show the full worker-to-target continuation
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

- [ ] Measure queue latency, pump cost, allocation/capture cost, continuation
  throughput, stale-drop rate, and frame impact under normal and saturated
  workloads.
- [ ] Review additional call sites such as async import and thumbnail work;
  migrate only where the generic continuation contract improves ownership or
  observability without removing necessary domain mailbox semantics.
- [ ] Evaluate RenderThread and RHIThread adapters only as follow-up candidates.
  Require a named production caller, module-owned callable contract, adapter
  lifetime rules, and non-blocking worker-side admission before opening a new
  implementation plan.
- [ ] Move stable rules from this plan into `Documentation/Runtime/Core/TaskSystem.md`
  and the relevant thread/lifecycle documentation.
- [ ] Add developer guidance showing when to use a typed continuation, a
  subsystem mailbox, a serialized pipe/lane, or a render/RHI command queue.
- [ ] Record deferred work that lacks workload evidence, including dedicated IO,
  fibers/coroutines, work stealing, and broad subsystem migration.
- [ ] Complete the final validation, full build, applicable editor smoke test,
  stage handoff, and plan status update.

#### Acceptance Gate

- The final design has measured bounds and an explicit owner for every queue and
  continuation target.
- The CPU task-system documentation describes the lasting contract without
  requiring readers to interpret this plan as runtime behavior.
- Broader adoption is either justified by evidence and migrated incrementally,
  or explicitly deferred with a named reason and follow-up owner.

Each completed stage ends with a compact handoff recording the baseline commit,
working set, key symbols and decisions, open questions, and validation outcome.
Each implementation stage lands as an independent local commit under the
repository handoff rules.

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
