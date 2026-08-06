# Task Continuations and Thread Dispatch Plan

Summary: Extend Durin's bounded CPU task system with typed completion continuations, explicit execution targets, and a bounded global GameThread deferred-work executor without collapsing subsystem, render, or RHI queues.

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

This plan is an initial, deliberately revisable design. It adds a common task
graph and dispatch vocabulary while preserving domain-specific result ownership
and the existing RenderThread/RHIThread boundaries.

## Goal

Provide an opt-in asynchronous composition model in which:

- a task may return an owned typed result;
- a continuation is represented as a new task node with an explicit
  prerequisite and returned handle;
- a continuation may target `AnyWorker`, `GameThread`, `RenderThread`, or
  `RHIThread` through logical execution targets rather than native thread IDs;
- the GameThread target is backed by one bounded, observable, low-priority
  deferred-work queue pumped at an engine-owned safe point;
- cancellation, failure, stale-generation checks, shutdown, and queue limits
  remain explicit and observable; and
- existing subsystem mailboxes and render/RHI queues remain valid owners for
  domain-specific sequencing, batching, coalescing, and backpressure.

## Scope

- Define the task continuation contract, typed result ownership, outcome
  propagation, and execution-target vocabulary.
- Extend the core task graph without changing the behavior of existing void
  task callers unless they opt into the new API.
- Add a global GameThread deferred-work executor with bounded depth, priority,
  frame budget, cancellation, coalescing hooks, and diagnostics.
- Adapt continuation dispatch to the existing worker, render, and RHI
  executors without moving their ownership or shutdown policy into the generic
  task scheduler.
- Prove the design with one editor-side pilot that already uses copied inputs,
  value-owned results, cancellation, and a generation/request mailbox.
- Update the CPU task-system contract after the implementation decisions become
  stable.

## Non-Goals

- Replacing every subsystem mailbox with one untyped global queue.
- Moving RenderThread or RHIThread work through the GameThread queue.
- Exposing arbitrary `std::thread::id` or native thread handles as task targets.
- Creating a dedicated native thread for every serialized resource; use a pipe
  or serialized task lane when affinity is not required.
- Making low-priority deferred work a guaranteed per-frame completion mechanism.
- Adding fibers, coroutine-backed waits, work stealing, dedicated IO scheduling,
  or RenderGraph integration in the initial implementation.
- Making `Then` a hidden synchronous callback that may run on the completion
  thread when the caller requested a named target.
- Removing existing domain mailboxes merely to demonstrate the generic API.

## Design Decisions and Invariants

### Task graph and continuation semantics

- A continuation is a new task node whose prerequisite is the predecessor
  task. `Then` returns a handle for the continuation so chains and fan-in can
  be composed without storing ad hoc callbacks in subsystem state.
- Existing `FTaskHandle` and void-oriented launch APIs remain source-compatible
  during the initial rollout. Typed results are additive and use owned values,
  move ownership, or an explicitly shared immutable payload; a task never
  exposes a reference to worker-local storage.
- A successful predecessor enables its continuation. Failure or cancellation
  propagates to a normal success-only continuation without invoking its body.
  Cleanup and error-reporting paths use an explicit always-run or outcome-aware
  continuation rather than silently treating failure as success.
- Invalid handles, cross-lifetime prerequisites, self-waits, and dependency
  cycles remain rejected deterministically.
- The default target for new CPU work is `AnyWorker`; a named target must be
  selected explicitly. The implementation never infers thread affinity from
  the thread that launched or completed the predecessor.
- A named-target continuation is queued to that target even when the
  predecessor completes on the target thread by default. Inline execution is
  an explicit optimization, not an accidental reentrancy behavior.

### Execution targets are logical executors

- `AnyWorker` maps to the current `FTaskScheduler`.
- `GameThread` maps to the global `FGameThreadDeferredWorkQueue`, which is
  pumped only by the owning game thread at an engine-approved safe point.
- `RenderThread` maps to the existing render command pipe and preserves its
  command admission, fence, and shutdown rules.
- `RHIThread` maps to the existing RHI queue and preserves its ordering,
  batching, backpressure, completion, and shutdown rules.
- The core API owns task identity and dependency state; each executor owns how
  ready work is admitted, run, pumped, and stopped.
- A logical target may be implemented by an actual named thread or by a
  configured engine mode in which two roles share a thread. Callers depend on
  the role contract, not a native thread identity.

### Global GameThread deferred-work queue

- The queue is an execution adapter for low-urgency GameThread continuations,
  not the owner of every asynchronous result in the process.
- Every entry is bounded by count and, where relevant, payload/capture budget.
  Submission has an observable accepted, rejected, canceled, or coalesced
  outcome; it never silently grows without limit.
- Entries carry priority and optional coalescing identity. Repeated refresh,
  cache-maintenance, telemetry, or latest-generation work may replace older
  entries when the owning caller opts in.
- The pump enforces a time/item budget and records queue depth, age, rejected
  work, coalescing, execution time, and expired generation counts.
- Queue execution never waits for a worker, render, or RHI task while holding a
  subsystem or ownership lock. A continuation that needs a result must receive
  it through its task payload or an immutable shared state.
- Shutdown closes admission, cancels not-started deferred work, waits for
  running callbacks at the engine-owned lifecycle point, and leaves every
  accepted task handle terminal.
- UObject/editor model mutation remains owned by GameThread. A queue entry
  must use a documented weak/lifetime token or an owned state object; a raw
  pointer capture is not made safe by queueing.

### Domain mailbox boundary

- Subsystem mailboxes remain responsible for request serials, generation
  checks, latest-wins policy, batching, domain-specific payloads, and result
  streams. A generic continuation may enqueue or drain such a mailbox, but it
  does not erase those semantics.
- The Asset Compatibility Audit remains the first pilot candidate because its
  worker inputs are copied, its output is value-only, and its current mailbox
  already rejects stale requests.
- Render and RHI command queues are not treated as generic mailboxes. Their
  ownership and ordering rules are stronger than the low-priority GameThread
  queue contract.

### Waiting, cancellation, and observability

- Building a dependency graph is preferred to blocking waits. A worker may
  retain the current bounded same-scheduler helping behavior for eligible
  worker tasks, but it may never execute a GameThread, RenderThread, or RHIThread
  continuation while waiting.
- Waiting from GameThread on a deferred continuation is allowed only at an
  explicitly documented synchronization point and must not accidentally pump
  unbounded work.
- Cancellation is cooperative for running bodies and prevents not-started
  continuations when cancellation wins. A queued GameThread entry checks its
  token and generation immediately before publication or mutation.
- Parent/continuation IDs, target, queue residency, timestamps, rejection,
  cancellation, stale-drop, and final outcome are included in task diagnostics
  without retaining unbounded terminal payloads.

## Current Foundations and Gaps

| Area | Current foundation | Gap closed by this plan |
| --- | --- | --- |
| Worker execution | Bounded `FTaskScheduler`, prerequisites, cancellation, waits, diagnostics | Typed result handles and first-class continuation nodes |
| Dependency graph | Immutable prerequisite list and terminal propagation | Fluent `Then`, explicit outcome-aware continuation, and reusable fan-in |
| GameThread | Existing subsystem-owned drain points only | One bounded, engine-pumped deferred-work executor |
| Rendering | Existing `FRenderThreadCommandPipe` | A target adapter that keeps render ownership outside the core scheduler |
| RHI | Existing ordered RHI queue and backpressure | A target adapter that preserves RHI-specific semantics |
| Mailboxes | Asset import and compatibility-audit mailboxes plus thumbnail/result queues | Clear distinction between generic execution and domain result ownership |
| Lifecycle | Scheduler and thread shutdown contracts already documented | Cross-executor admission, pump, and shutdown ordering |
| Documentation | Generic mailbox and typed results are deferred features in `TaskSystem.md` | Evidence-backed lasting contract after implementation |

## Implementation Stages

### Stage 0: Freeze the contract and choose the pilot

Dependencies: current `TaskSystem.md`, existing task tests, and current
worker/render/RHI lifecycle contracts.

- [ ] Define the public names and ownership of `TTaskHandle<T>`, task outcome,
  continuation options, `ETaskTarget`, priority, cancellation, and coalescing
  identity.
- [ ] Decide whether typed result storage is inline, heap-backed, or shared by
  immutable ownership for the first implementation, including result lifetime
  after the caller releases its handle.
- [ ] Define success-only, always-run, and outcome-aware continuation behavior,
  including the diagnostic retained when a predecessor fails or cancels.
- [ ] Specify the GameThread pump location, safe-point ordering, default
  budget, queue limits, priority order, coalescing behavior, and shutdown
  sequence.
- [ ] Specify the adapter contract for RenderThread and RHIThread, including
  what happens when their command admission is closed.
- [ ] Characterize the Asset Compatibility Audit as the first pilot and record
  the request/generation invariants that must survive migration.
- [ ] Add API/state-machine test cases for every decision that would otherwise
  remain ambiguous during implementation.

#### Acceptance Gate

- The API and state diagrams identify who owns each task payload, callback,
  queue, and terminal outcome.
- No open decision remains about named-target execution, GameThread pumping,
  cancellation/failure propagation, or shutdown behavior for Stage 1.
- The pilot has a bounded before/after behavior specification and a list of
  existing mailbox semantics that must not change.

### Stage 1: Add typed results and worker continuations

Dependencies: Stage 0; existing worker scheduler and task-state tests.

- [ ] Add additive typed-result task state and handle APIs while keeping the
  current void API behavior unchanged.
- [ ] Implement worker-target `Then` as a prerequisite-backed task node that
  returns a new handle and carries result ownership without exposing references
  to task-local storage.
- [ ] Implement success, failure, cancellation, invalid-handle, and
  cross-scheduler/lifetime propagation for continuation chains and fan-in.
- [ ] Add outcome-aware cleanup/error continuation support without making
  exception or failure handling implicit.
- [ ] Extend diagnostics and shutdown quiescence to include continuation nodes
  and result storage.
- [ ] Add focused unit tests for result lifetime, move/copy policy, chain
  ordering, fan-in, cancellation races, failure propagation, reentrancy, and
  scheduler shutdown.

#### Acceptance Gate

- A typed worker task can feed one or more worker continuations with no caller
  mailbox and with deterministic terminal outcomes.
- Existing void-task callers and existing worker wait/cancellation tests pass
  unchanged.
- No task body or continuation can observe a dangling result, run after a
  failed prerequisite, or leave an accepted node nonterminal at shutdown.

### Stage 2: Add logical execution targets and the GameThread executor

Dependencies: Stage 1; engine frame/lifecycle ownership identified in Stage 0.

- [ ] Add `ETaskTarget` and the executor/dispatcher boundary without exposing
  native thread IDs.
- [ ] Implement `FGameThreadDeferredWorkQueue` with bounded admission, priority,
  optional coalescing key, cancellation/generation checks, pump budget, and
  diagnostics.
- [ ] Add the engine-owned GameThread pump and lifecycle admission/shutdown
  hooks at the documented safe point.
- [ ] Implement `Then(..., GameThread, ...)` and prove that it never executes
  on a worker or completion thread when the target is GameThread.
- [ ] Add manual-pump tests and engine integration tests for ordering, budget
  exhaustion, queue saturation, coalescing, stale generations, cancellation,
  shutdown, and callback reentrancy.
- [ ] Define the first priority policy so low-urgency work cannot displace
  required frame-critical synchronization work.

#### Acceptance Gate

- A worker-to-GameThread continuation executes only from the game-thread pump,
  remains observable while deferred, and reaches a terminal state on normal
  shutdown or cancellation.
- Queue limits and budgets are enforced under sustained producer pressure; no
  unbounded capture/result growth is possible.
- A subsystem-specific mailbox can still preserve serial/generation semantics
  when called from a generic GameThread continuation.

### Stage 3: Adapt RenderThread/RHIThread and migrate the pilot

Dependencies: Stage 2; existing render and RHI command-queue tests and
lifecycle contracts.

- [ ] Implement `RenderThread` and `RHIThread` continuation adapters using the
  existing command submission paths, not a second generic queue.
- [ ] Preserve render/RHI admission, ordering, backpressure, fences, and
  shutdown behavior when a predecessor completes on any worker.
- [ ] Migrate the Asset Compatibility Audit's completion path to one typed
  worker task plus an explicit GameThread continuation while retaining its
  request-serial mailbox and stale-result checks.
- [ ] Verify project-change, cancellation, replacement, editor close, and
  scheduler shutdown races against the pilot's existing behavior.
- [ ] Add tracing/diagnostics that show the full worker-to-target continuation
  chain and distinguish stale-drop, cancellation, rejection, and callback
  failure.

#### Acceptance Gate

- The pilot has no lost, duplicated, stale, or cross-thread model publication
  under its existing replacement and shutdown scenarios.
- Render/RHI smoke and synchronization tests prove that target adapters do not
  move resource work onto the GameThread queue or worker pool.
- A continuation chain can cross worker and named targets while every node has
  a terminal diagnostic and the process exits without nonterminal work.

### Stage 4: Measure, document, and decide broader adoption

Dependencies: Stage 3; profiler evidence from the pilot and representative
editor workloads.

- [ ] Measure queue latency, pump cost, allocation/capture cost, continuation
  throughput, stale-drop rate, and frame impact under normal and saturated
  workloads.
- [ ] Review additional call sites such as async import and thumbnail work;
  migrate only where the generic continuation contract improves ownership or
  observability without removing necessary domain mailbox semantics.
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
| Typed results | Ownership, move/copy policy, result lifetime, multiple consumers, and terminal-state behavior |
| Continuations | Success, failure, cancellation, always-run cleanup, fan-in, ordering, reentrancy, and invalid-handle rejection |
| Target routing | Worker, GameThread, RenderThread, and RHIThread execute only on their logical executor |
| GameThread queue | Bounded admission, priority, pump budget, coalescing, stale generations, cancellation, callback failure, and metrics |
| Mailbox boundary | Pilot preserves request serials, latest-wins behavior, batching, and domain payload ownership |
| Render/RHI integration | Existing queue ordering, backpressure, fences/completion, admission closure, and shutdown behavior |
| Waiting | No target task is executed by an ineligible waiter; documented synchronization points do not deadlock |
| Lifecycle | Startup failure, normal drain, cancel shutdown, late submission, target shutdown, and terminal-handle quiescence |
| Concurrency | Sustained producer pressure, multiple consumers, replacement races, and nested task submission |
| Diagnostics | Parent/prerequisite IDs, target, queue age, rejection, stale-drop, cancellation, callback failure, and final outcome |
| Performance | Frame-time impact, queue latency, allocation rate, and saturation behavior in representative editor workloads |
| Integration | Focused Core, editor pilot, render/RHI smoke, and applicable full build/test validation per build documentation |

## Definition of Done

- Durin has an additive typed task/continuation API with explicit logical
  execution targets and deterministic outcome propagation.
- The global GameThread deferred-work queue is bounded, budgeted, observable,
  lifecycle-safe, and used only for work that permits deferred execution.
- Existing subsystem mailboxes remain available for domain-specific result and
  ordering semantics; RenderThread and RHIThread retain their own queues.
- At least one production editor path uses the new model without changing its
  ownership, generation, cancellation, or shutdown guarantees.
- The worker-to-target chain is covered by focused tests, integration tests,
  diagnostics, workload measurements, and required full validation.
- Stable behavior is moved into the owning runtime/thread documentation, and
  unsupported extensions remain explicitly deferred.

## Deferred Follow-ups

- Typed task integration with broader asset import and thumbnail pipelines after
  pilot measurements.
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
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp`
- `Engine/Source/Runtime/RHI/Private/RHIThread.cpp`
- `Engine/Source/Editor/DurinEd/Private/Asset/AssetCompatibilityAudit.cpp`
- `Engine/Source/Editor/AssetImportCore/Private/AsyncImport.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SourceImageThumbnailCache.cpp`
