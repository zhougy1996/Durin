# CPU Task System

Summary: Define task scheduling, dependencies, cancellation, waiting, and worker ownership.

Modules: Core

Last reviewed: 2026-08-08

Durin's CPU task system provides process-wide bounded background execution for
runtime and editor subsystems. It owns task admission, dependencies, typed
results, continuations, cooperative cancellation, waiting, shutdown, and
diagnostics. It also provides one bounded low-priority GameThread deferred
executor. It does not own rendering-thread, object, or subsystem workflow
policy.

The public API is declared in `Threading/Task.h`. Runtime code submits through
that API rather than retaining the process scheduler or its raw worker pool.
`FQueuedThreadPool` remains a lower-level primitive for the scheduler, focused
tests, and explicitly owned dedicated pools.

## Process Lifetime

The engine uses a UE-style process lifetime: `FEngineLoop::PreInit()` initializes
the worker scheduler and then installs the GameThread deferred executor. Startup
fails if either initialization fails. `FEngineLoop::Exit()` calls
`ShutdownTaskSystem(Drain)` after CPU-work producers are detached. The normal
engine does not restart either executor.

Core permits a fully stopped scheduler to be started again so isolated tests
and non-engine programs can run sequential lifetimes. A start while shutdown is
in progress is rejected, and scheduler instances never overlap. Handles from a
previous lifetime cannot be prerequisites for new tasks, although their
terminal state and copied diagnostics remain queryable.

Scheduler admission has one close boundary. A submission that acquired
lifetime participation before that boundary either publishes an accepted task
or reports rejection before releasing participation. Every later submission,
including child work launched by an already-running task, is rejected. Callers
must treat an invalid returned handle as work that was never accepted and must
not wait for it as though it were pending.

`FTaskSchedulerConfig::MaxNonterminalTasks` bounds the whole accepted graph for
one scheduler lifetime and defaults to 16,384. Each root, waiting node,
continuation, typed fan-in node, unique-result sink, and scheduled parallel-for
chunk consumes one reservation before task-state publication; a queue
transition consumes no second reservation. A full scheduler rejects before it
retains a task node, reports `CapacityExhausted` in bounded diagnostics, and
destroys or returns caller-owned state outside internal locks. The reservation
is released exactly once only after result publication and direct dependent
propagation finish. `FQueuedThreadPool` remains independently usable and has no
second scheduler-capacity policy.

## Task States And Results

An invalid handle is reported as `ETaskState::Invalid`; it is not an accepted
task. Every valid task follows one of these paths:

```text
Waiting -> Queued -> Running -> Succeeded
   |          |         |  \-> Failed
   |          |         \----> Canceled
   |          \--------------> Canceled
   \--------------------------> Canceled
```

A task without prerequisites begins in `Queued`. Terminal states never change.
`FTaskHandle::IsComplete()` is true only for `Succeeded`, `Failed`, or
`Canceled`; it is false for an invalid handle. `WaitTask()` returns the observed
state, while `WaitAll()` waits every valid input and returns one outcome per
input without collapsing failure or cancellation into success.

Void launch APIs remain supported. Typed launch APIs return `TTaskHandle<T>`;
the callable moves one value into shared result state and successful consumers
observe it through `std::shared_ptr<const T>`. Result access is non-blocking and
returns no owner for an invalid, nonterminal, failed, or canceled task. Handles
and continuation outcome snapshots pin the immutable result, so fan-out does
not copy or consume it. Exceptions never escape a task or executor entry point:
a standard exception produces `Failed` with its message, and an unknown
exception produces a stable diagnostic.

Launch and continuation forwarding overloads accept move-constructible
callables, including lambdas that uniquely capture `unique_ptr`. Existing
`FTaskFunction`, `FCancelableTaskFunction`, typed `std::function` overloads,
and their result-conversion behavior remain available. Core erases one user
callable into a move-only owner, then moves that owner through the task node and
selected executor. Capture move and destruction never occur while task,
scheduler, Worker-queue, or GameThread-queue locks are held.

Unique publication is explicit and type-distinct. `LaunchUniqueTask<T>` and
`LaunchUniqueCancelableTask<T>` return a move-only `TUniqueTaskHandle<T>` with
ordinary status, diagnostic, and erased `FTaskHandle` access, but no shared
result observer or direct take operation. `ConsumeThen` claims one successful
`T` and invokes a terminal `void(T&&)` sink. `ConsumeThenOutcome` runs after any
producer terminal state and invokes a terminal
`void(FUniqueTaskOutcome<T>&&)` sink whose optional value exists only for
success. Consuming sinks may target `AnyWorker` or `GameThreadDeferred` and do
not produce another unique result.

Exactly one sink can claim a unique result. Registration reserves the claim
before graph admission, commits it only after the consumer node is accepted,
and rolls it back if callable, prerequisite, scheduler-lifetime, target, or byte
validation rejects the node. A rejected call preserves the source unique
handle; successful registration invalidates it. Duplicate claims increment a
bounded scheduler diagnostic. Failure, cancellation, rejection, stale
generation, supersession, callback failure, dropped handles, and either
shutdown mode discard the value exactly once outside internal locks. Copyable
or move-only `T` does not select ownership mode: callers choose shared or unique
launch explicitly.

Task IDs are process-unique and nonzero. A parent ID identifies the executing
task that submitted the task, if any; prerequisite IDs describe dependency
edges separately. `FTaskDiagnostics` returns an owned, thread-safe copy of
identity, relationships, timestamps, execution thread, state, and diagnostic
text.

## Dependencies

`FTaskLaunchOptions::Prerequisites` is immutable after submission. Every
prerequisite must be a valid task from the currently running scheduler
lifetime. Because a new task can depend only on already-published tasks, every
edge points to an older node and dependency cycles cannot be constructed
through the public API.

A dependent is queued exactly once after every prerequisite succeeds. If a
prerequisite fails or is canceled, the dependent becomes `Canceled` without
running, records the direct source task in its diagnostic, and propagates that
outcome through chains, fan-in, and fan-out. Concurrent terminal publication
and dependency registration cannot lose or duplicate the release.

Scheduler quiescence includes waiting nodes and all release or cancellation
propagation. An empty worker queue is not sufficient evidence that accepted
work is complete.

`Then` creates a success-dependent task node; it runs only after its primary
predecessor and any additional prerequisites succeed. `ThenOutcome` creates a
completion-dependent node and receives an owned `FTaskOutcome<T>` after the
primary predecessor reaches any terminal state. Success fan-in waits for every
predecessor, gives failure precedence over cancellation, and records the
lowest-ID direct blocker. A continuation always returns its own handle and is
never an inline completion callback.

`ConsumeThen` uses the same success edge, while `ConsumeThenOutcome` uses the
same completion edge. Terminal state becomes externally observable only after
the task's completion hook has published or discarded result storage. A
concurrent dependent registration therefore cannot run before a successful
shared or unique result is ready.

`WhenAll` composes a non-empty heterogeneous
`std::tuple<TTaskHandle<Ts>...>` into one success-dependent continuation and
invokes its callback as `const Ts&...` in tuple order. `WhenAllOutcome` creates
the completion-dependent form and passes `TTaskAggregateOutcome<Ts...>` with
one owned `FTaskOutcome<T>` snapshot per tuple position. The aggregate is
successful only when every typed input succeeds; otherwise failure wins over
cancellation and the smallest task id wins within that state. Its blocking id,
reason, and diagnostic come from that selected input. Repeated tuple positions
remain visible to the callback while graph edges and prerequisite diagnostics
remain deduplicated. Captured shared handles retain results through callback
completion even when callers release their copies.

Typed fan-in accepts move-only callbacks and follows normal continuation return
rules: `void` returns `FTaskHandle`, while an object value returns
`TTaskHandle<U>`. Empty tuples, void or unique handles, reference-returning
callbacks, `WhenAny`, and implicit cancellation of other inputs are not
supported. Additional continuation prerequisites are scheduling gates; they do
not become elements of the public aggregate outcome.

`FTaskContinuationOptions::Target` selects a logical executor. `AnyWorker` uses
the process worker scheduler. `GameThreadDeferred` is always queued, even when
created on GameThread; it runs only from the engine-owned pump. Missing or
closing executors reject dispatch and terminalize the accepted node as
`Canceled/DispatchRejected`.

## Structured Owner Scopes

`FTaskScope` is a move-only owner controller for one bounded task lifetime;
`FTaskScopeToken` is its copyable launch association. A scope does not own a
scheduler, executor, native thread, result mailbox, or subsystem policy. An
invalid token preserves the existing unscoped behavior.

Roots select an explicit token through `FTaskLaunchOptions::Scope`. Otherwise,
a root launched by a scoped task inherits that executing scope. Continuations,
typed fan-in, and unique-result sinks inherit their primary predecessor before
admission; additional prerequisites never merge scopes. A scoped task cannot
reparent a descendant to a different explicit scope. Parallel-for selects one
scope for the logical operation and forwards it only to scheduled Worker
chunks.

Scope admission and close are linearized with scheduler admission. Every
accepted scoped node is charged once before publication and released once only
after result publication and direct dependent propagation finish. Closing in
`Drain` mode rejects new roots and descendants while accepted work completes.
Closing in `Cancel` mode also requests cooperative cancellation; a draining
scope may escalate to cancel. Work racing a completed close is rejected without
retaining its callable or task node.

`FTaskScope::Wait()` and `WaitFor()` observe scope quiescence, not merely an
empty Worker queue or one producer handle. External threads may wait after the
owner has closed admission. A Worker may help a different closed scope, but a
self-scope wait is rejected. Rendering-thread waits and GameThread waits whose
scope contains deferred GameThread work are rejected. Owners must release any
mailbox, cache, provider, object, render, RHI, or async-state lock before
waiting. Scope destruction never waits: abandoning an open controller performs
a non-blocking cancel close and records the event.

`FTaskScopeDiagnostics` reports the numeric scope id, state, accepted, active,
terminal, rejected, and peak counts plus at most 64 nonterminal task snapshots.
Scheduler diagnostics report live, open, nonquiescent, abandoned-open, and
scope-rejected totals through a weak live-scope registry. The registry neither
keeps completed scopes alive nor retains completed task history. A quiescent
scope can remain live briefly while an external handle or terminal queue node
still references it.

Scheduler shutdown closes root admission, then closes every pinned live scope
before executor teardown. This is a safety net, not an owner-lifetime policy:
production owners close publication and scope admission at their own explicit
shutdown boundary and wait only where their thread contract permits it.

## Cancellation

`CancelTask()` requests cancellation for one task.
`FTaskCancellationSource` supplies a caller-owned token for a related group,
and `LaunchCancelableTask()` passes the effective token to the callable.
Cancellation is cooperative; Durin never terminates a native thread.

- Cancellation of `Waiting` or `Queued` races atomically with dependency
  release or worker claim. If cancellation wins, the callable never starts.
- Cancellation of `Running` becomes visible through
  `FTaskCancellationToken::IsCancellationRequested()`. The task remains
  nonterminal until its callable returns.
- Normal return after a winning cancellation request produces `Canceled`. A
  thrown exception produces `Failed`, so cancellation cannot hide a failure.
- Cancellation after terminal publication has no effect, and repeated requests
  are idempotent.

Running callables are responsible for checking their token at bounded
intervals. A callable with externally visible side effects publishes them only
after its last cancellation check when cancellation must prevent publication.
Cancellation does not roll back effects already committed.

## Waiting Boundaries

Waiting is a synchronization boundary, not a routine scheduling technique.

- The game thread may wait only at an explicit synchronization point and never
  while holding a subsystem, registry, or ownership lock needed by task work.
- A worker waiting for another task from the same scheduler may help execute an
  eligible queued task while its scheduler lifetime remains protected.
- Self-wait is rejected deterministically. A same-pool worker call to
  `FQueuedThreadPool::WaitForIdle()` returns false rather than deadlocking.
- Routine waits on the rendering thread are unsupported. Rendering work uses
  render commands, render fences, and the game/render synchronization contract.
- Long waits preserve normal wait semantics while recording the waiter, target
  ID and state, and elapsed duration in scheduler diagnostics.

Workers never help by executing themselves or a dependency that is still in
`Waiting`. Scheduler shutdown waits for graph quiescence rather than using pool
idle as a substitute.

GameThread must not use ordinary `WaitTask()` on a nonterminal
`GameThreadDeferred` node. Workers never help by executing that target. Build a
continuation graph instead; the cross-executor shutdown coordinator is the only
pump-until-quiescent path.

## GameThread Deferred Executor

`FGameThreadDeferredWorkQueue` is a low-priority execution adapter, not a
universal result mailbox or a frame-critical synchronization lane. The engine
pumps it immediately after `DEngine::Tick()` at a fixed safe point. Each frame
pump is limited by configured item and time budgets; priority is strict and
entries are FIFO within one priority.

Admission reserves both an entry and the caller-declared estimated payload
bytes. The byte estimate is bounded metadata, not an exact measurement of
`std::function` allocation. Optional coalescing keys are scoped by owner, work
identity, and generation. Superseded, canceled, and stale-generation entries
release their callable storage and publish distinct terminal reasons.
Diagnostics expose depth, declared bytes, rejection and stale counts, queue
age, residency, and pump cost.

A unique producer declares retained result bytes, defaulting to `sizeof(T)`.
The declaration includes dynamic storage exclusively reachable from `T`; a
dynamically owning result must provide a conservative nonzero estimate.
Explicit zero normalizes to `sizeof(T)` only for trivially copyable, trivially
destructible values and otherwise rejects launch. A consuming
`GameThreadDeferred` node charges the checked sum of its callback payload and
the producer's retained result bytes against per-entry and total queue limits.
Overflow rejects registration without consuming the source handle. AnyWorker
remains count bounded, while retained bytes remain visible in task and scheduler
diagnostics until consume or discard.

The default bounds are 1,024 queued entries, 8 MiB total declared payload, and
1 MiB declared payload per entry. A normal frame executes at most 64 callbacks
and stops after the first callback that takes the pump past 1 ms; callbacks over
2 ms are diagnosed as long. These are configurable admission and observation
bounds, not permission to place frame-critical or predictably long work there.

A Debug qualification workload saturates a representative 256-entry batch with
64 declared capture bytes per continuation. It measures admission, unbudgeted
pump time, average/maximum residency, continuation throughput, and a deliberate
32-of-256 stale-generation drop. These values are emitted by
`RepresentativeWorkloadMeasuresAdmissionPumpResidencyAndStaleDrops`; they are
environment baselines, not performance guarantees. Budget and capacity tests
separately verify that normal frames stop at their limits and saturation rejects
additional work without unbounded growth.

The move-ownership Debug qualification uses 128 copyable and 128 move-only
callables plus 32 shared and 32 unique 64 KiB transfers. On the 2026-08-07 Agent
Debug run, admission/execution/destruction took 15.45 ms for copyable callables
and 16.01 ms for move-only callables; shared transfers took 8.27 ms and unique
transfers 7.40 ms. These single-environment totals are regression evidence, not
performance promises. A separate unique-result saturation admits four 64-byte
retained results into a 256-byte deferred queue, rejects the fifth, then returns
both queued and retained bytes to zero after pumping.

## ParallelFor

`ParallelFor()` and `ParallelForCancelable()` synchronously cover `[0, Num)`
with bounded, static contiguous chunks. The caller executes one chunk and waits
for the remaining group. The chunk count is bounded by the range, the requested
minimum batch size, and at most the worker count plus the caller chunk. Empty,
small, and nested ranges use the serial path; serial nesting prevents recursive
iteration bodies from creating an unbounded task tree.

Iterations must be independent because coverage is deterministic but execution
order is not. On failure, unclaimed chunks are canceled while already-running
chunks may finish. The result is `Failed` if any chunk failed, otherwise
`Canceled` if cancellation won, otherwise `Succeeded`. When several chunks
fail, the diagnostic from the lowest range start is selected for schedule-
independent reporting.

The default `FParallelForOptions::MinBatchSize` is the serial sentinel. The V1
Debug qualification workload found no parallel crossover through 1,048,576
iterations for its synthetic CPU workload. A subsystem may opt in with an
explicit batch size only after measuring its real workload and build profile.

## Thread And Object Ownership

`FRunnableThread` records a name and an `EThreadRole`. Engine-owned roles are
game, rendering, worker, and IO. Native-thread shutdown is cooperative: request
stop, let the runnable return, and join. Destruction of a joinable platform
thread waits safely; unsupported stack-size, priority, suspend, and resume
requests are rejected or diagnosed rather than silently pretending to apply.

The task system schedules CPU work but grants no extra ownership rights:

- The game thread owns mutable Worlds, `DObject` state, asset registration, and
  editor models. Workers use copied inputs, immutable snapshots, or explicitly
  synchronized shared state.
- Independent `TWeakObjectPtr` copies may cross threads as opaque identity, but
  workers do not create, resolve, validate, or mutate object references.
- Workers may prepare CPU payloads for rendering. The owning game-thread or
  subsystem code enqueues render commands; RHI and render-resource operations
  stay on the rendering thread.
- Cross-thread callables capture values, move-owned payloads, or shared/weak
  state with a documented synchronization contract. Raw references and pointers
  require a proven lifetime fence.

Subsystems retain completed-result mailboxes when they own streaming, batching,
latest-wins, or take-result policy. DurinEd's Asset Compatibility Audit uses
that boundary deliberately: one cancelable worker consumes copied package
inputs and an immutable reflection catalog, streams value-only records through
its request-serial mailbox, and returns an owned typed terminal summary. A
`GameThreadDeferred` outcome continuation drains earlier records and publishes
terminal model state only after rechecking request serial, generation, and weak
model lifetime. No package object or mutable editor state crosses into the
worker task.

Texture2D CPU builds also retain a subsystem mailbox because their move-only 4K
and 16K payloads exceed the deferred executor's per-entry and total payload
bounds. Engine owns a reliable, explicitly ordered GameThread frame pump for
that mailbox after the bounded `GameThreadDeferred` pump. A deferred
continuation is not its durable wakeup: bounded admission or shutdown rejection
must never strand the last completion. Explicit waits and Engine asset-service
shutdown drain the mailbox directly.

Choose the execution primitive by ownership requirement:

| Need | Primitive | Owner |
| --- | --- | --- |
| Immutable result feeds one or more readers | Shared typed task plus `Then`/`ThenOutcome` | Core task graph and selected executor |
| Multiple immutable typed results feed one aggregate callback | Shared typed tuple plus `WhenAll`/`WhenAllOutcome` | Core task graph and selected executor |
| One result moves into exactly one terminal owner | Unique typed task plus `ConsumeThen`/`ConsumeThenOutcome` | Core task graph until sink invocation, then the sink owner |
| Streaming, batching, latest-wins, provider closure, or explicit result-taking | Subsystem mailbox, optionally triggered by a continuation | Subsystem |
| Ordered access to a resource without native-thread affinity | Subsystem serialized pipe/lane | Resource-owning subsystem |
| Render or RHI context, ordering, fences, or resource lifetime | Existing render/RHI command queue | RenderCore or RHI backend |

Async import remains mailbox-owned because its coordinator defines owner and
provider admission, latest-by-owner replacement, explicit drain, and result
take semantics. Its plan worker publishes one unique `FImportPlanResult` to an
AnyWorker outcome sink, which moves success or a stable terminal failure into
request state and queues the existing serial notice. The coordinator tracks the
publisher node for drain while retaining a private producer cancellation
target; no Worker publishes editor state. Source-image thumbnails remain
cache-owned because they combine
visibility prioritization, bounded decode concurrency, per-frame upload
throttling, serial validation, and render/RHI command ownership. Neither path
is migrated merely to replace its mailbox with a generic callback.

Asset-registry reconciliation remains synchronous and publishes its registry,
reference index, diagnostics, revision, and cache snapshots on the owning
thread. A future asynchronous full-validation path must snapshot mount and
cache inputs plus an immutable reflection catalog, return value-owned results
through a generation-checked mailbox, and reject publication after any newer
scan, save, move, delete, mount change, or cancellation. Worker execution does
not authorize direct mutation of the live registry or reference index.

## Task Attribution And Aggregate Diagnostics

Every accepted task has one process-local `FTaskAttribution` owner/category
token. `RegisterTaskAttribution(Owner, Category)` copies stable UTF-8 labels of
1-63 bytes into a process-lifetime registry. Exact duplicate registration
converges under concurrency. The registry is bounded to 256 owners and 1,024
owner/category pairs, including `Unattributed` and `Overflow`; invalid labels or
exhausted capacity select `Overflow` and increment the registration-overflow
counter without rejecting task work. Tokens are diagnostic identity only and
must not be serialized or used as resource identity. Production callers use
low-cardinality literals, never paths, object ids, request serials, or other
per-item values.

Launch, continuation, and parallel-for options accept an explicit
`Attribution`. An explicit non-default token wins. Otherwise, a continuation,
unique-result sink, or typed fan-in inherits its primary predecessor; a root
launched from a task in the same scheduler inherits that executing task; and an
external root is `Unattributed`. Additional prerequisites never merge or select
identity. Parallel-for selects one token for the logical operation and forwards
it to every scheduled chunk while reporting its logical operation count
separately from task-node counts.

`FTaskDiagnostics` resolves the task's owner/category ids and labels and reports
the erased callable's retained storage plus execution nanoseconds. Callable
storage is the inline wrapper size or concrete heap target size; it excludes
separately declared payload and result bytes. Execution duration is zero before
start, elapsed while running, and frozen at terminal publication.

`FTaskSchedulerDiagnostics::OwnerCategoryDiagnostics` contains at most 1,024
entries. Each `FTaskOwnerCategoryDiagnostics` reports accepted, succeeded,
failed, canceled, and rejected counts; bounded terminal-reason counts; current
waiting, queued, running, and nonterminal gauges; current and peak callable,
payload, result, and retained-unique-result bytes; and five fixed 32-bucket
histograms for queue residency, execution, callable bytes, payload bytes, and
result bytes. Bucket zero represents value zero; a positive value uses
`min(31, 1 + floor(log2(value)))` in nanoseconds or bytes.

Accepted work charges its attribution once after node admission and releases
all current gauges at the one winning terminal transition. Pre-node rejection
charges only rejection and byte-distribution evidence. Retained unique-result
bytes transfer from producer to consuming sink without changing the global
total. Shared global counters and retained bytes reconcile with the sum of all
owner/category entries. Snapshot queries may allocate their bounded result but
do not mutate counters, retain completed task history, or add hot-path label
lookup and formatting.

Terminal accounting has two bounded lifetimes. Scheduler outcome and
owner/category totals are fixed counters charged at the final terminal hook;
they retain no task id or state pointer. Retained-terminal task and result
gauges are charged when the terminal-publication barrier opens and released
when the last external task state or result owner is destroyed, including
after scheduler shutdown. Diagnostic observation never performs cleanup or
changes either lifetime.

Attribution registration survives scheduler restart, while a new scheduler
lifetime starts with zeroed aggregate slots. The current production pairs are
`AssetImport/PreparePlan`, `AssetImport/PublishPlan`, and
`SourceImageThumbnail/Decode`. Their subsystem mailboxes, cancellation,
request-serial checks, cache policy, and render/RHI ownership remain separate
from task attribution. Tracy correlation and fixed owner/category plots are
owned by the [CPU profiling](../../Development/Build/Profiling.md) contract.

## Shutdown And Diagnostics

`ShutdownTaskScheduler(true)` remains available to isolated worker-only callers
and first closes admission before draining every
accepted task to a terminal state. `ShutdownTaskScheduler(false)` cancels
waiting and queued tasks, requests cooperative cancellation of running tasks,
propagates cancellation through dependencies, and still waits for running
callables to return before native workers are destroyed. Both modes leave every
worker-only accepted handle in exactly one terminal state.

Engine code uses `ShutdownTaskSystem(Drain)` or `ShutdownTaskSystem(Cancel)`.
Drain closes root admission while preserving internal dispatch for already-
accepted graph nodes, then pumps GameThread work without the frame budget until
the whole graph is quiescent. Cancel terminalizes not-started work and requests
cooperative cancellation of running bodies. Only after every accepted node is
terminal does lifecycle close and uninstall the GameThread adapter. Recursive
pump or shutdown entry from a deferred callback is rejected.

`FTaskSchedulerDiagnostics` exposes the live lifetime or the final snapshot
after shutdown: worker count, queue depth, active workers, completed, failed,
canceled, rejected, and long-wait counters, nonterminal task diagnostics, and
retained terminal-handle count. At successful shutdown, queue depth, active
workers, and nonterminal count are zero. Retained terminal handles are external
owners of completed state, not leaked scheduler storage.

The global scheduler-lifetime mutex is held only long enough to pin the live
scheduler or immutable stopped snapshot. A live scheduler copies fixed
aggregates and pins its current task cohort without holding that global mutex;
it then releases the scheduler mutex before resolving labels or querying each
task's coherent diagnostic snapshot. Concurrent admission, terminal
publication, and shutdown therefore do not wait for deep snapshot traversal,
and a pinned snapshot remains lifetime-safe if shutdown wins concurrently.
`ParallelForCancelable` reads only the scheduler's immutable Worker count and
does not enter this diagnostic path.

Unique-result diagnostics add per-task estimated and currently retained result
bytes plus scheduler-wide retained unique bytes and duplicate-claim count.
They retain no result payload, callable, claim token, or terminal history.

Diagnostic reads have no profiler side effects. Task profiler events correlate
an optional scope with one fixed-width numeric scope id; they do not create
scope-named zones, plots, source locations, or retained history.
`FEngineLoop::Tick()` calls
`PublishTaskSchedulerProfilerPlots()` once at the frame boundary immediately
before `DURIN_PROFILE_FRAME_MARK()`. The publication surface copies only the
fixed owner/category aggregates; with Tracy disabled it remains a no-op after
the same bounded runtime traversal.

Normal engine exit detaches CPU-work producers, drains the scheduler, performs
the object and module drains, and only then closes render-command admission and
stops the rendering thread. The complete process order is owned by
`RuntimeLifecycle.md`.

## Deferred Features

Dedicated IO scheduling, work stealing, fibers or coroutine-backed waits,
multi-stage unique-result production, a general
serialized-lane abstraction, and RenderGraph task integration require
workload-specific evidence and a clear owner. RenderThread and RHIThread are not
generic task targets; any adapter requires a named production caller, an
owning-module callable and lifetime contract, and non-blocking worker-side
admission.

## Related Documentation

- `Documentation/Roadmaps/Archive/2026-08/TaskSystemEvolution.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Runtime/Core/GarbageCollection.md`
- `Documentation/Development/Build/Profiling.md`

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Public/Threading/RunnableThread.h`
- `Engine/Source/Runtime/Core/Public/Threading/QueuedThreadPool.h`
- `Engine/Source/Runtime/Launch/Private/EngineLoop.cpp`
