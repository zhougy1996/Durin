# Multithreading System Plan

Last reviewed: 2026-07-20

## Current Status

Durin has a usable minimal worker-task foundation, but it is not yet a general
task scheduler. The first three stages of
`Documentation/Plans/MultithreadingRoadmap.md` are substantially
implemented: named threads have roles, a manual-reset event primitive exists,
the engine owns a fixed worker pool, and callers can launch named tasks and wait
for one or many handles. Worker-side waits help execute queued work, which
allows a one-worker pool to complete a parent task that launches and waits for a
child task.

The rendering thread is a separate, established command system with explicit
wait/wake behavior, command fences, frame synchronization, and game/render
thread checks. The generic worker scheduler does not replace it and currently
has no API for scheduling a continuation back to the game or rendering thread.

Real worker-pool use is still narrow. Async mesh import parses source files on a
worker, and the editor decodes source-image thumbnails on workers before
uploading them through render commands. Task dependencies, `ParallelFor`,
priorities, cancellation, result/error propagation, dedicated IO scheduling,
and RenderGraph integration are not implemented.

The focused Core test selection passed on 2026-07-20 with 20 tests from six
suites, and the focused async AssetCore selection passed with four tests:

```powershell
.\BuildTool.bat test --target CoreTests --filter "*Thread*:*Task*"
.\BuildTool.bat test --target AssetCoreTests --filter "*Async*"
```

These are native unit/integration tests. They do not validate rendering-thread
shutdown, editor shutdown under background load, scheduling latency, or a full
`DurinEditor` runtime session.

## Implemented

- [x] `FRunnableThread` and `FRunnableThreadStd` creation, cooperative stop, and
  join behavior.
- [x] Thread-local current-thread tracking plus game, rendering, worker, and IO
  role definitions.
- [x] Thread name, ID, and role queries and thread-role assertion helpers.
- [x] Named thread startup, exit, and cooperative-stop logging.
- [x] Manual-reset `FThreadEvent` with blocking and timed waits.
- [x] Fixed-size `FQueuedThreadPool` with a protected FIFO queue and worker
  wakeups.
- [x] Concurrent producers, idle waiting, graceful drain, queued-work discard,
  and rejection after shutdown at the pool level.
- [x] Process-wide worker-pool initialization during `FEngineLoop::PreInit()`
  and draining shutdown during `FEngineLoop::Exit()`.
- [x] Named `FTaskHandle`, completion polling, `WaitTask`, and `WaitAll`.
- [x] Worker-side helping while waiting, including a tested nested task on a
  single-worker pool.
- [x] Async mesh import with a thread-safe shared result state.
- [x] Async editor thumbnail decoding followed by render-thread upload.
- [x] Dedicated rendering thread, double-buffered render-command pipe, render
  fences, and frame synchronization.

## P0: Correctness and Lifecycle Hardening

Complete these before making the task API a general dependency of more runtime
systems.

- [ ] Give every accepted task a terminal state when queued work is discarded.
  Today `FQueuedThreadPool::Destroy(false)` clears queued lambdas without marking
  their `FTaskCompletionState`; a retained handle can remain incomplete forever
  and `WaitTask` can block forever.
- [ ] Make global pool acquisition and shutdown lifetime-safe. `LaunchTask`
  reads the raw `GThreadPool` pointer without participating in the mutex that
  replaces and destroys the global pool, so concurrent submission and shutdown
  do not have a documented safe lifetime boundary.
- [ ] Define scheduler startup failure policy. `FEngineLoop::PreInit()` ignores
  the return value of `InitEngineThreadPool()`, leaving later task submission to
  fail piecemeal.
- [ ] Document and enforce legal wait sites. At minimum, reject or diagnose a
  task waiting on itself, `WaitForIdle()` from one of that pool's active workers,
  waits while holding subsystem locks, and routine waits on the rendering
  thread.
- [ ] Define task callable failure behavior and guarantee terminal completion
  for every supported failure path. The current completion flag is set only
  after the callable returns normally and carries no failure information.
- [ ] Make `FRunnableThread` destruction safe by contract or by RAII. A live
  `std::thread` still requires callers to stop/join before destruction; misuse
  can terminate the process.
- [ ] Either implement thread priority and stack-size requests or explicitly
  report them as unsupported. `FRunnableThreadStd::CreateInternal()` currently
  stores these arguments without applying them.
- [ ] Harden rendering-thread lifecycle: guard duplicate init/shutdown, define
  whether shutdown drains or rejects late render commands, and test the policy.

## P1: Complete the Core Task Abstraction

- [ ] Replace the completion boolean with an explicit task state such as Queued,
  Running, Succeeded, Failed, and Canceled.
- [ ] Add dependency and continuation scheduling with immutable prerequisites,
  fan-in, and fan-out. Common pipelines should not need blocking waits between
  stages.
- [ ] Add game-thread and rendering-thread continuation/finalization queues so
  workers can publish immutable results without directly mutating thread-owned
  engine state.
- [ ] Add a cancellation token and define cancellation propagation through
  dependencies. Cancellation must be cooperative for already-running work.
- [ ] Decide whether the generic API exposes typed results or keeps results in
  caller-owned shared state; standardize error propagation either way.
- [ ] Add `FTaskGroup` or an equivalent dynamic fan-out/fan-in primitive rather
  than using sequential `WaitAll` calls as the only grouping tool.
- [ ] Add `ParallelFor` with a synchronous small-range fallback, static chunking
  initially, a minimum batch size, and defined nested behavior.
- [ ] Add high, normal, and background priority classes after defining fairness
  and starvation rules. Keep dependency readiness separate from priority.
- [ ] Separate public scheduling APIs from the concrete global pool so tests and
  future specialized schedulers do not depend directly on `GThreadPool`.

## P1: Expand Real Engine Integration

- [ ] Convert async mesh import from one monolithic worker task into dependency
  stages where useful: read, parse, CPU build, game-thread asset finalization,
  and render-thread resource creation.
- [ ] Move texture source decoding and platform-data building off the game
  thread, coordinated with the texture work tracked in
  `Documentation/Plans/TextureSupport.md`.
- [ ] Add request generation or cancellation to async imports and thumbnail
  work so stale editor requests have explicit scheduler-visible outcomes, not
  only consumer-side result rejection.
- [ ] Choose another frame-relevant CPU workload for the first `ParallelFor`
  integration and measure task overhead before widening use.
- [ ] Keep DObject registry access and mutable world state on the game thread.
  Worker tasks may transport copied handles and immutable payloads, but must not
  resolve `TWeakObjectPtr` or mutate DObjects.

## P2: Diagnostics, Scale, and Specialized Scheduling

- [ ] Expose task ID, enqueue/start/finish timestamps, queue delay, execution
  time, executing thread, and parent task ID to diagnostics or the profiler.
- [ ] Add queue-depth, active-worker, completed-task, canceled-task, and long-wait
  counters without relying on trace logging every small task.
- [ ] Log or assert outstanding tasks and handles during scheduler shutdown.
- [ ] Add a dedicated IO queue/thread role only when async loading needs latency
  isolation from CPU work; the `IOThread` role currently has no scheduler.
- [ ] Add work stealing or worker-local queues only after profiling shows global
  FIFO contention or load imbalance.
- [ ] Integrate RenderGraph CPU preparation and parallel command recording only
  after the single-threaded RenderGraph and Vulkan command-pool ownership rules
  exist.
- [ ] Defer fibers, coroutine-backed waits, lock-free queues, and frame-local
  task allocators until measurements identify a concrete need.

## Validation Gaps

- [ ] Test that discarded queued tasks become Canceled and every retained handle
  stops waiting.
- [ ] Stress concurrent `LaunchTask` calls against global pool shutdown and
  reinitialization under a defined supported policy.
- [ ] Test self-wait, cyclic dependency rejection, worker `WaitForIdle`, and
  nested waits deeper than one parent/child pair.
- [ ] Test multiple waiters on one task and concurrent polling while the task
  completes.
- [ ] Test dependency chains, fan-in, fan-out, continuation thread affinity,
  cancellation before start, and cancellation during execution.
- [ ] Add deterministic `ParallelFor` edge and batching tests.
- [ ] Add rendering command-pipe tests for multiple producers, fence ordering,
  wake without work, late enqueue, and shutdown drain/reject behavior.
- [ ] Run an editor smoke test that starts background imports and thumbnail
  decodes, closes the relevant editor state, and exits while work is active.
- [ ] Add stress coverage for thousands of small tasks and mixed long/short work;
  record latency and fairness rather than only final completion counts.
- [ ] Run a successful full `all` build and `DurinEditor` startup/shutdown smoke
  test after lifecycle or cross-thread ownership behavior changes.

## Recommended Implementation Order

1. Fix discarded-task terminal states and global pool lifetime safety.
2. Enforce wait and thread-lifecycle contracts, including rendering shutdown.
3. Introduce explicit task states, dependencies, and thread-affine
   continuations.
4. Add cancellation and standard result/error handling.
5. Implement and validate `ParallelFor`.
6. Split one asset pipeline into worker stages plus game/render finalization.
7. Add low-overhead metrics and profiler integration.
8. Add priorities, IO isolation, work stealing, and RenderGraph parallelism only
   in response to measured workloads.

## Related Docs

- `Documentation/Plans/MultithreadingRoadmap.md`
- `Documentation/Architecture/RuntimeArchitecture.md`
- `Documentation/Setup/NativeTests.md`
- `Documentation/Plans/TextureSupport.md`
