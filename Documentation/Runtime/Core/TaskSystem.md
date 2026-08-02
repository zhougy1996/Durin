# CPU Task System

Last reviewed: 2026-08-02

Durin's CPU task system provides process-wide bounded background execution for
runtime and editor subsystems. It owns task admission, dependencies,
cooperative cancellation, waiting, shutdown, and diagnostics. It does not own
game-thread, rendering-thread, object, or subsystem workflow policy.

The public API is declared in `Threading/Task.h`. Runtime code submits through
that API rather than retaining the process scheduler or its raw worker pool.
`FQueuedThreadPool` remains a lower-level primitive for the scheduler, focused
tests, and explicitly owned dedicated pools.

## Process Lifetime

The engine uses a UE-style process lifetime: `FEngineLoop::PreInit()` calls
`InitializeTaskScheduler()` once, startup fails if worker creation fails, and
`FEngineLoop::Exit()` calls `ShutdownTaskScheduler(true)` once after CPU-work
producers are detached. The normal engine does not restart the scheduler.

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

V1 callables return `void`. Results belong to caller-owned shared state. A
terminal-state observation acquires publication from the callable, but the
consumer may use a payload only when the outcome permits it. Exceptions never
escape a task or worker entry point: a standard exception produces `Failed`
with its message, and an unknown exception produces a stable diagnostic.

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

V1 has no universal game-thread continuation pump. Each subsystem drains its
own completed-result mailbox during an existing game-thread tick or waits at a
documented synchronization point.

## Shutdown And Diagnostics

`ShutdownTaskScheduler(true)` first closes admission and then drains every
accepted task to a terminal state. `ShutdownTaskScheduler(false)` cancels
waiting and queued tasks, requests cooperative cancellation of running tasks,
propagates cancellation through dependencies, and still waits for running
callables to return before native workers are destroyed. Both modes leave every
accepted handle in exactly one terminal state.

`FTaskSchedulerDiagnostics` exposes the live lifetime or the final snapshot
after shutdown: worker count, queue depth, active workers, completed, failed,
canceled, rejected, and long-wait counters, nonterminal task diagnostics, and
retained terminal-handle count. At successful shutdown, queue depth, active
workers, and nonterminal count are zero. Retained terminal handles are external
owners of completed state, not leaked scheduler storage.

Normal engine exit detaches CPU-work producers, drains the scheduler, performs
the object and module drains, and only then closes render-command admission and
stops the rendering thread. The complete process order is owned by
`RuntimeLifecycle.md`.

## Deferred Features

Priorities, dedicated IO scheduling, work stealing, typed results, a generic
game-thread mailbox, fibers or coroutine-backed waits, and RenderGraph task
integration require workload-specific profiler or benchmark evidence and a
clear ownership contract before adoption.

## Related Documentation

- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Runtime/Core/GarbageCollection.md`
- `Documentation/Development/Build/Profiling.md`

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Public/Threading/RunnableThread.h`
- `Engine/Source/Runtime/Core/Public/Threading/QueuedThreadPool.h`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
