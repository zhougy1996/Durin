# Multithreading Roadmap

This document describes a staged plan for Durin's multithreading framework. The goal is to grow from the current runnable-thread and rendering-thread primitives into a task system that can support async loading, CPU computation, render command scheduling, and eventually RenderGraph compilation and execution.

The roadmap is intentionally incremental. Each stage should leave the engine in a usable state and should produce APIs that later systems can depend on without knowing about lower-level scheduler details.

## Design Goals

- Keep the core task system domain-neutral. It should schedule work, express dependencies, wait on completion, and report diagnostics. It should not know about assets, worlds, RHI objects, or RenderGraph passes.
- Make thread ownership explicit. Systems should be able to assert whether code is running on the game thread, rendering thread, worker threads, or another named thread.
- Prefer message passing, immutable snapshots, handles, and fences over shared mutable state.
- Make waiting semantics conservative and well documented. Deadlocks and thread-pool starvation are usually caused by unclear wait rules, not by task execution itself.
- Build observability early. Thread names, task names, queue depth, task timings, and long-wait diagnostics should exist before the framework becomes complex.
- Keep early implementation simple. Add work stealing, fibers, cancellation, and priority scheduling only after real workloads show the need.

## Existing Starting Point

Current relevant runtime pieces include:

- `Engine/Source/Runtime/Core/Public/Threading/RunnableThread.h`
- `Engine/Source/Runtime/Core/Public/Threading/StdRunnableThread.h`
- `Engine/Source/Runtime/Core/Private/Threading/RunnableThread.cpp`
- `Engine/Source/Runtime/Core/Private/Threading/StdRunnableThread.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderingThread.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp`

The engine already has:

- a basic `FRunnableThread` abstraction
- current-thread tracking through thread-local state
- game-thread and rendering-thread identity checks
- a render command pipe
- render command fences
- frame synchronization helpers

The rendering thread currently runs a command launch loop. Even if that loop is usually throttled by RHI/GPU work in real frames, future versions should still prefer explicit wait/wake semantics where practical. This makes low-load behavior, shutdown, profiling, and deterministic tests easier to reason about.

## Target Thread Model

Durin should distinguish thread roles before adding complicated scheduling features.

### Game Thread

The game thread owns high-level engine state, world updates, object lifetime decisions, input dispatch, and most editor-facing state. It may schedule worker tasks and enqueue render commands.

The game thread may wait on worker tasks at explicit synchronization points, but it should avoid waiting while holding engine-wide locks or resource registry locks.

### Rendering Thread

The rendering thread owns render command consumption and render-side synchronization. It should be the boundary between game/editor state and RHI-facing work.

Early RenderGraph work can run on this thread. Later stages may parallelize pass compilation and command recording with worker tasks, but render-resource ownership rules should remain explicit.

### Worker Threads

Worker threads execute CPU tasks. They should not directly mutate game-thread-owned world state or rendering-thread-owned RHI state.

Workers are appropriate for:

- parsing and decoding asset data
- CPU-side mesh and texture preparation
- animation, visibility, culling, and scene data preparation
- background computation
- parallel loops
- RenderGraph pass setup or command recording when the backend supports it

### IO Threads

IO threads are optional in the early implementation. A first version may perform file reads on worker threads. Dedicated IO queues can be added once async loading needs clearer priority and latency control.

### Optional RHI Submit Thread

An RHI submit thread should be delayed until there is a concrete need. Vulkan-first rendering can benefit from a submit thread or parallel command recording, but this should not be the first stage of the task framework.

## Core Rules

### Thread Ownership

Every major subsystem should define where its mutable state is owned.

Examples:

- world and actor mutation: game thread
- render command execution: rendering thread
- GPU resource destruction: rendering thread or deferred deletion queue
- asset file parsing: worker threads
- final asset registration: game thread and/or rendering thread, depending on the resource

Cross-thread work should pass data through tasks, command queues, or handles. Shared mutable containers should be treated as a last resort.

### Lambda Capture Safety

Task and render-command lambdas should avoid capturing references across threads unless the lifetime is proven by a fence or task dependency.

Preferred capture forms:

- value types
- `std::shared_ptr` to immutable or thread-safe state
- stable handles or IDs
- copied snapshots
- move-only payloads that transfer ownership

Risky capture forms:

- raw object pointers without lifetime ownership
- references to stack objects
- references to game-thread-owned mutable objects
- references to render resources that may be released before command execution

### DObject Handle Transport

`TWeakObjectPtr<T>` may be copied into worker tasks as an opaque object identity, but the worker must not call `Get()`, `IsValid()`, or otherwise resolve its handle. Creating a weak pointer from a `DObject*` and resolving it are game-thread operations because the object registry and object lifetime state are game-thread-owned.

The initial supported flow is:

1. Create the weak pointer on the game thread and capture it by value.
2. Let the worker operate only on copied values, immutable input, and thread-safe result state.
3. Wait for or poll task completion at a game-thread synchronization point.
4. Resolve the weak pointer once on the game thread and apply the result only when `Get()` succeeds.

Copies published to another thread must be treated as independent values. Concurrently reading and writing the same weak-pointer instance is not supported. `TWeakObjectPtr` does not pin an object, make object fields thread-safe, or authorize worker access to a `DObject`.

### Waiting Rules

The first version of the scheduler should define conservative waiting behavior:

- The game thread may wait on worker tasks at known synchronization points.
- Worker threads waiting on task handles should be able to execute other eligible tasks if possible.
- Rendering thread waits should be rare and explicit.
- Avoid waits while holding locks.
- Avoid scheduling a task that waits for work queued behind itself on the same single-threaded queue.
- Avoid using full render flushes as routine synchronization inside gameplay or editor code.

### Shutdown Rules

Shutdown should be deterministic:

- stop accepting new background tasks
- wake all worker threads
- finish or cancel pending tasks according to documented policy
- join worker threads
- flush or shut down render commands at a known point
- destroy thread-owned resources after their owning queues are idle

Unsafe thread killing should not be part of normal engine shutdown.

## Stage 1: Stabilize Threading Primitives

Goal: make the existing thread layer safe enough to build a scheduler on top.

Recommended work:

- Define clear lifecycle semantics for `FRunnableThread`.
- Prefer cooperative stop over forceful thread kill.
- Decide whether `Suspend()` and `Resume()` remain supported APIs or become unsupported placeholders.
- Make stop flags atomic or lock-protected.
- Add or wrap a reusable event primitive, such as `FEvent` or `FThreadEvent`.
- Add thread role metadata:
  - unknown
  - game thread
  - rendering thread
  - worker thread
  - IO thread
- Add helpers:
  - `IsInWorkerThread()`
  - `IsInTaskThread()`
  - `CheckGameThread()`
  - `CheckRenderingThread()`
  - `CheckThreadRole(...)`
- Ensure thread startup and shutdown are logged with thread name and ID.

Validation:

- native tests for event wait/signal behavior
- native tests for cooperative stop
- manual editor/runtime startup and shutdown

Exit criteria:

- threads can start, stop, and join deterministically
- thread names and roles are observable in logs
- unsafe lifecycle operations are clearly unsupported or well defined

## Stage 2: Build a Minimal Worker Pool

Goal: provide a simple pool that can run background CPU work.

Recommended work:

- Add a `FTaskThreadPool` or `FQueuedThreadPool`.
- Start with a fixed number of worker threads.
- Use a single protected queue first.
- Wake workers through a condition variable or engine event primitive.
- Support graceful shutdown.
- Add named task submission:

```cpp
FTaskHandle Handle = GTaskScheduler.Submit("BuildMeshLOD", []() {
    // CPU work
});
```

Do not add work stealing yet unless the simple queue immediately becomes a measured bottleneck.

Validation:

- submit many tasks and verify all complete
- submit tasks from multiple threads
- shut down with an empty queue
- shut down with pending work according to the chosen policy

Exit criteria:

- background work can run on worker threads
- task execution is named and logged
- worker shutdown is deterministic

## Stage 3: Add Task Handles and Waiting

Goal: allow systems to wait for specific work without exposing worker-pool internals.

Recommended types:

- `FTaskHandle`
- `FTaskEvent`
- `FTaskFence`
- `FTaskGroup`

Recommended API:

```cpp
FTaskHandle LaunchTask(const char* Name, TFunction<void()>&& Function);
void WaitTask(const FTaskHandle& Task);
void WaitAll(TSpan<const FTaskHandle> Tasks);
```

Implementation notes:

- A task handle should reference shared completion state.
- Completion state should support wait, poll, and debug naming.
- Waiting from a worker thread should eventually help execute other tasks to avoid starvation.
- A first version may use blocking waits, but document where blocking waits are allowed.

Validation:

- wait for one task
- wait for many tasks
- nested task submission
- waiting from game thread
- waiting from worker thread, if supported

Exit criteria:

- users can submit work and wait on completion without knowing queue details
- waits are tested and documented

## Stage 4: Add Dependencies and Continuations

Goal: express task ordering without manual waits in the middle of execution.

Recommended API:

```cpp
FTaskHandle Load = LaunchTask("ReadTextureFile", []() {});
FTaskHandle Decode = LaunchTask("DecodeTexture", { Load }, []() {});
FTaskHandle Finalize = LaunchTask("FinalizeTexture", { Decode }, []() {});
```

Implementation notes:

- Store a prerequisite counter on each task.
- Completed tasks decrement dependent tasks.
- A task becomes runnable when its prerequisite count reaches zero.
- Keep continuation behavior simple at first.
- Avoid dynamic dependency mutation after a task has become visible to the scheduler.

Validation:

- dependency chains
- dependency fan-in
- dependency fan-out
- dependency completion order
- failed or canceled dependency policy, if failure is supported

Exit criteria:

- simple DAG-style scheduling works
- manual blocking waits are not required for common ordering

## Stage 5: Add ParallelFor

Goal: make data-parallel CPU work convenient and hard to misuse.

Recommended API:

```cpp
ParallelFor("SkinVertices", VertexCount, [](uint32 Index) {
    // independent work per item
});
```

Implementation notes:

- Start with static chunking.
- Add a minimum batch size to avoid creating too many tiny tasks.
- Support a synchronous fallback for small ranges.
- Consider nested `ParallelFor` behavior before allowing it freely.

Validation:

- zero elements
- one element
- small ranges
- large ranges
- exception/assert behavior according to engine policy

Exit criteria:

- common CPU loops can be parallelized through a stable API
- small workloads avoid task overhead

## Stage 6: Integrate Async Asset Loading

Goal: use the task system for real engine work without breaking thread ownership.

Recommended loading pipeline:

1. File read task.
2. Parse/decode task.
3. CPU build task.
4. Game-thread finalization.
5. Render-thread resource creation command.

Example flow:

```text
Request asset
  -> worker or IO task reads file
  -> worker task parses data
  -> worker task builds CPU-side resource payload
  -> game thread registers asset handle
  -> render command creates GPU resource
  -> asset becomes ready
```

Important rules:

- Worker tasks should produce immutable payloads.
- Asset handles should survive across async boundaries.
- Cancellation should be allowed before finalization.
- GPU resource creation should stay behind render-thread or RHI ownership rules.
- Hot reload should use versioned asset handles or generation counters.

Validation:

- load one asset
- load many assets
- cancel a pending load
- destroy requester before load completion
- reload an asset while an older request is in flight

Exit criteria:

- asset loading uses task dependencies instead of ad-hoc background threads
- no worker task directly mutates game-thread or render-thread-only state

## Stage 7: Prepare RenderGraph Support

Goal: build RenderGraph on top of clear scheduling and render ownership rules.

Early RenderGraph should be single-threaded:

- collect passes
- declare resources
- compile dependencies
- infer barriers
- allocate transient resources
- execute passes

Once that works, parallelize selected parts:

- pass setup
- resource analysis
- command recording
- CPU-side preparation for passes

Vulkan-specific notes:

- command pools should generally be thread-local or externally synchronized
- command buffers recorded in parallel need clear ownership
- descriptor allocation and transient resource allocation need thread-safe strategies
- queue ownership and barriers must remain explicit

Validation:

- render pass ordering
- resource lifetime
- barrier correctness
- command recording from multiple worker threads, if enabled
- frame shutdown and resource release after fences

Exit criteria:

- RenderGraph can use tasks for CPU-side work without hiding RHI ownership
- pass execution remains deterministic

## Stage 8: Add Priorities and Specialized Queues

Goal: improve scheduling quality once real workloads exist.

Possible queues:

- normal CPU tasks
- high-priority game-critical tasks
- async loading tasks
- IO tasks
- render preparation tasks

Possible priority classes:

- high
- normal
- background

Rules:

- Priority should not break dependencies.
- Background work should not starve frame-critical work.
- IO priority should be separate from CPU priority if a dedicated IO queue exists.

Validation:

- frame-critical work is not delayed by bulk asset loading
- background tasks eventually complete
- shutdown handles every queue

Exit criteria:

- real workloads can be prioritized without special-case thread creation

## Stage 9: Add Advanced Scheduler Features Only If Needed

Potential advanced features:

- work stealing
- task-local queues
- cancellation tokens
- task failure propagation
- continuations with result values
- fibers or coroutine-backed waits
- frame-local task allocators
- lock-free queues

These features should be added in response to measured problems.

Examples:

- Add work stealing when a single global queue becomes a bottleneck or workers become imbalanced.
- Add cancellation when async loading and editor workflows need request invalidation.
- Add fibers only when blocking waits frequently cause starvation and helping execution is not enough.
- Add lock-free queues only when profiler data shows queue locking is a real cost.

## Diagnostics and Profiling

The task system should emit enough information to diagnose behavior without a debugger.

Recommended diagnostics:

- task name
- task ID
- parent task ID, if available
- enqueue timestamp
- start timestamp
- finish timestamp
- executing thread name
- queue depth
- long wait warnings
- shutdown warnings for unfinished tasks

Recommended future profiler view:

```text
Frame N
  GameThread
    TickWorld
    SubmitRenderCommands
  RenderingThread
    BuildRenderGraph
    SubmitRHI
  Worker 0
    LoadTexture
    DecodeTexture
  Worker 1
    ParallelFor SkinVertices chunk 3
```

## Testing Strategy

Start with native tests under the existing test infrastructure.

Suggested early tests:

- event signal and wait
- thread start and cooperative stop
- submit one task
- submit many tasks
- wait one task
- wait all tasks
- dependency chain
- fan-in and fan-out dependency graphs
- `ParallelFor` range edge cases
- scheduler shutdown with no work
- scheduler shutdown with pending work

Suggested stress tests:

- thousands of small tasks
- tasks submitting more tasks
- many waiters on one task
- async load cancellation patterns
- long-running background tasks plus frame-critical tasks

Avoid renderer/window tests in the first task-system suite. Keep low-level scheduler tests deterministic and independent of editor startup.

## Suggested Initial Implementation Order

1. Harden `FRunnableThread` lifecycle and thread role tracking.
2. Add an event primitive.
3. Add a minimal worker pool with named task submission.
4. Add `FTaskHandle` and blocking waits.
5. Add dependency counters and dependent task release.
6. Add `WaitAll`.
7. Add `ParallelFor`.
8. Add diagnostics.
9. Integrate one small async-loading path.
10. Use tasks for a small RenderGraph preparation prototype.

This order keeps the system useful at every step and avoids designing advanced scheduler behavior before Durin has real multithreaded workloads to measure.

## Related Docs

- Runtime architecture: `Documentation/Architecture/RuntimeArchitecture.md`
- Native tests: `Documentation/Setup/NativeTests.md`
- Build and run workflow: `Documentation/Setup/BuildAndRun.md`
