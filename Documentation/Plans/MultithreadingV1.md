# Multithreading V1 Plan

Last reviewed: 2026-07-24

## Current Status

Durin has the foundations of a CPU task system, but the public task API is not
yet safe enough to become a general runtime dependency.

Implemented today:

- named `FRunnableThread` instances with game, rendering, worker, and IO roles
- cooperative stop, join, thread identity queries, and unsupported
  suspend/resume diagnostics
- manual-reset `FThreadEvent`
- a fixed-size `FQueuedThreadPool` with a protected FIFO queue, worker wakeups,
  concurrent producers, idle waiting, drain shutdown, and discard shutdown
- a process-wide worker pool created in `FEngineLoop::PreInit()` and drained in
  `FEngineLoop::Exit()`
- named `FTaskHandle`, polling, `WaitTask`, and sequential `WaitAll`
- worker-side helping while waiting, including a tested parent/child task on a
  one-worker pool
- a separate rendering thread with a blocking command pipe, render fences, and
  frame synchronization
- worker use in async mesh import and editor thumbnail decoding; thumbnail
  results cross back through game-thread polling and render commands

The last recorded focused validation, from 2026-07-20, covered 20 Core tests in
six threading/task suites and four async AssetCore tests. It did not cover
concurrent scheduler shutdown, discarded task handles, task callable failures,
render-command shutdown, editor shutdown under load, or a full editor smoke
test.

This plan supersedes the former Multithreading System and Multithreading
Roadmap plans. Those documents mixed current priorities with a from-scratch
roadmap and proposed advanced scheduler features before the existing lifecycle
was safe.

## Goal

Deliver a V1 CPU task system that engine and editor subsystems can depend on for
bounded background work:

- every accepted task reaches an observable terminal state
- submission, waiting, cancellation, and shutdown have race-free contracts
- immutable task dependencies express CPU work ordering without blocking
  workers between stages
- a measured `ParallelFor` covers ordinary data-parallel CPU work
- diagnostics explain task latency, failure, cancellation, and shutdown
- current async consumers obey game-, render-, and object-ownership boundaries

## Scope

- `Core` runnable-thread lifecycle, worker-pool ownership, task state, waits,
  dependencies, cancellation, `ParallelFor`, and diagnostics
- engine startup and shutdown policy for the process-wide scheduler
- tests for supported concurrency and lifecycle behavior
- migration of async mesh import and editor thumbnail decoding to the V1
  contracts
- rendering-thread lifecycle hardening where it is required for safe handoff
  from those consumers

## Non-Goals

- replacing the rendering command pipe with the CPU task scheduler
- a generic game-thread or rendering-thread continuation executor
- mutating Worlds, DObjects, editor models, or RHI objects from workers
- typed task results, futures, or automatic storage of arbitrary return values
- dedicated IO or RHI-submit threads
- priority queues, work stealing, fibers, coroutines, lock-free queues, or
  frame-local task allocation
- parallel RenderGraph compilation, pass execution, or Vulkan command recording
- concurrent garbage collection

These are deferred until V1 workloads and profiler data establish a concrete
need. RenderGraph work also depends on its own single-threaded ownership model
before scheduler integration is designed.

## Design Decisions and Invariants

### Scheduler ownership and shutdown

- A process-owned scheduler facade is the only public route for general task
  submission. Callers do not acquire or retain the raw global pool.
- Submission participates in scheduler lifetime synchronization. Once shutdown
  closes admission, new submissions return an invalid handle; already accepted
  tasks are either drained or transitioned to `Canceled` according to the
  selected shutdown mode.
- Normal engine shutdown drains accepted tasks before object, asset, render, and
  module teardown. Discard shutdown exists for tests and abnormal setup cleanup,
  but still completes every accepted handle as `Canceled`.
- Worker-pool initialization is mandatory engine startup. `PreInit()` fails
  immediately if worker creation fails instead of allowing unrelated later
  submissions to fail piecemeal.
- Scheduler teardown waits for concurrent submissions that entered before the
  admission boundary. No code may dereference scheduler or pool storage after
  releasing that lifetime participation.

### Task state and failure

- A valid handle observes exactly one monotonic state sequence:
  `Waiting -> Queued -> Running -> Succeeded`, with `Failed` or `Canceled` as
  alternate terminal states. A task without prerequisites starts at `Queued`.
- `IsComplete()` means any terminal state. The handle also exposes the terminal
  state and a diagnostic failure message; invalid remains distinct from
  canceled.
- V1 callables remain `void`. Results stay in caller-owned shared state whose
  publication is synchronized before the task becomes terminal.
- A callable returning normally succeeds. A caught exception records `Failed`
  and does not escape a worker entry point. Engine fatal checks keep their
  existing process-failure behavior.
- Cancellation is cooperative once a task is running. Canceling a waiting or
  queued task prevents its callable from starting and moves it to `Canceled`.
  Cancellation never kills a native thread.

### Dependencies

- Prerequisites are immutable after submission. A task is queued exactly once
  when all prerequisites succeed.
- The dependency API accepts only already-submitted tasks as prerequisites, so
  immutable forward publication makes cycles structurally impossible.
  Submission rejects invalid handles and handles from another scheduler
  generation.
- A failed or canceled prerequisite cancels its dependent without running the
  dependent callable. That cancellation propagates through the graph.
- Fan-in and fan-out are supported. Dynamic child creation remains legal, but
  callers must not add prerequisites to an already-published task.
- Dependencies order CPU work only. They do not imply game-thread or
  rendering-thread execution.

### Thread ownership and result delivery

- The game thread owns mutable World, DObject, asset-registration, and editor
  model state. Workers operate on copied input, immutable snapshots, or
  explicitly thread-safe shared state.
- Independent `TWeakObjectPtr` copies may cross threads as opaque identity.
  Workers do not create, resolve, validate, or mutate DObject references.
- The rendering thread remains a dedicated command consumer. Workers create CPU
  payloads; owners enqueue render commands for RHI work.
- V1 does not add a universal main-thread pump. Subsystems poll or drain their
  own completed-result mailbox during an existing game-thread Tick, or wait at
  a documented synchronization point. This keeps delivery lifetime and
  reentrancy owned by the subsystem.
- Cross-thread lambdas capture values, move-owned payloads, or shared/weak state
  with an explicit synchronization contract. Raw pointers and references require
  a proven fence or task lifetime and are not the default.

### Waiting

- Game-thread waits are allowed only at explicit synchronization points and
  never while holding subsystem or registry locks.
- A worker waiting for another task may execute eligible queued work. It must
  detect self-wait and dependency cycles rather than spin or recurse forever.
- `WaitForIdle()` is a scheduler lifecycle/test operation and is rejected from a
  worker belonging to that scheduler.
- Routine waits on the rendering thread are unsupported. Render work uses render
  fences and game/render frame synchronization.
- `WaitAll` may keep its sequential implementation because terminal state is
  per handle; it must preserve failure/cancellation visibility rather than
  silently treating every terminal state as success.

### `ParallelFor`

- V1 starts with static chunking over the global CPU scheduler, a configurable
  minimum batch size, and synchronous execution for empty or small ranges.
- The caller participates in the work and waits for the group at the API
  boundary. Nested calls use the same worker-helping rules and do not create
  unbounded task trees.
- Range processing is deterministic with respect to coverage, not execution
  order. Callers must make iterations independent.
- Priority and work stealing are not prerequisites for `ParallelFor`; measured
  overhead and load balance decide whether they are needed later.

### Rendering thread

- Duplicate rendering-thread initialization and shutdown are diagnosed and do
  not dereference null or stale globals.
- Shutdown closes render-command admission, drains commands accepted before the
  boundary, wakes the consumer, then joins it. Late enqueue is rejected with a
  diagnostic.
- Render fences are game/render synchronization primitives, not generic task
  handles, and remain outside the CPU dependency graph in V1.

## Current Foundations and Gaps

| Area | Current foundation | V1 gap |
| --- | --- | --- |
| Native threads | Cooperative `Kill`, join, roles, names | A joinable `std::thread` can still reach destruction; stack size and priority requests are stored but not applied or rejected |
| Worker pool | Fixed workers, FIFO queue, drain/discard modes | Discard erases closures without completing task state |
| Global lifetime | Mutex protects pool replacement | `LaunchTask` and worker helping read raw `GThreadPool` outside that lifetime lock |
| Engine lifecycle | Pool starts in `PreInit` and drains in `Exit` | Startup ignores initialization failure; concurrent admission/shutdown is undefined |
| Task handle | Name, completion poll, waits | Boolean-only completion; no failure, cancellation, dependency, or scheduler generation |
| Waiting | Worker helping prevents the tested one-worker parent/child deadlock | No self-wait, pool-idle, cycle, lock-site, or render-thread enforcement |
| Async mesh import | Thread-safe shared result and worker execution | No scheduler-visible failure/cancellation; result delivery is one monolithic task |
| Thumbnails | Worker decode, serial rejection, game-thread drain, render upload | Stale requests are rejected only by the consumer; active-task accounting depends on a normal callback |
| Rendering | Blocking command pipe, fences, double-buffered execution | Duplicate lifecycle and enqueue-versus-shutdown policy are undefined and untested |
| Diagnostics | Names, IDs, thread logs, trace per launch/execution | No state, queue delay, duration, parent, cancellation, or aggregate counters |

## Implementation Stages

### Stage 1: Close lifecycle and terminal-state holes

Dependencies: none.

- [ ] Introduce the scheduler facade and remove general task submission's direct
  dependence on `GThreadPool`.
- [ ] Synchronize admission with shutdown and identify each scheduler lifetime
  with a generation.
- [ ] Replace boolean completion with the V1 task state model and failure
  diagnostics.
- [ ] Give queued work an explicit discard path that transitions its task to
  `Canceled`.
- [ ] Catch exceptions at the task-callable boundary, record task failure, and
  guarantee pool bookkeeping even if lower-level queued work leaks an exception.
- [ ] Make failed worker-pool initialization fatal to engine `PreInit()`.
- [ ] Make native-thread destruction safe against a joinable `std::thread`, and
  explicitly reject or apply unsupported stack-size and priority requests.
- [ ] Add self-wait, same-pool worker `WaitForIdle`, and rendering-thread wait
  checks.

#### Acceptance Gate

- Every accepted task becomes `Succeeded`, `Failed`, or `Canceled` under normal
  execution, callable failure, drain shutdown, and discard shutdown.
- Submission racing shutdown has deterministic test coverage with no raw pool
  lifetime race.
- Thread and scheduler startup failure cannot leave the engine partially
  initialized.

### Stage 2: Add immutable dependencies and cooperative cancellation

Dependencies: Stage 1.

- [ ] Represent scheduler-owned task nodes separately from public handles and
  queued callable storage.
- [ ] Add immutable prerequisite submission, prerequisite counters, and
  dependent release for chains, fan-in, and fan-out.
- [ ] Reject invalid and cross-generation prerequisites at the public boundary;
  keep the one-shot API unable to construct cycles.
- [ ] Propagate prerequisite failure and cancellation without executing blocked
  dependents.
- [ ] Add cancellation source/token support for waiting, queued, and running
  tasks.
- [ ] Define handle queries and wait results so callers can distinguish success,
  failure, and cancellation.
- [ ] Preserve worker helping without allowing a task to execute itself or an
  ineligible dependent.

#### Acceptance Gate

- Deterministic tests cover dependency ordering, invalid graph input, propagation,
  cancellation before start, cooperative cancellation during execution,
  multiple waiters, and nested waits on a one-worker scheduler.
- Common CPU pipelines can express ordering without a blocking wait inside each
  stage.

### Stage 3: Add bounded `ParallelFor` and diagnostics

Dependencies: Stage 2.

- [ ] Add `ParallelFor` with empty/small synchronous paths, static chunking, a
  minimum batch size, caller participation, and bounded nested behavior.
- [ ] Measure crossover cost and choose defaults from recorded native-test or
  benchmark data rather than hardware-thread count alone.
- [ ] Record task ID, parent ID, scheduler generation, enqueue/start/finish
  timestamps, queue delay, duration, executing thread, and terminal state.
- [ ] Add aggregate queue-depth, active-worker, completed, failed, canceled, and
  long-wait counters without requiring per-task trace logging.
- [ ] Diagnose outstanding task nodes/handles at shutdown.

#### Acceptance Gate

- Tests prove exact range coverage for zero, one, small, large, uneven, and
  nested ranges.
- A repeatable measurement documents the chosen batching threshold.
- A failed or slow task can be identified from diagnostics without enabling
  trace logging for every task.

### Stage 4: Migrate current consumers and harden cross-thread shutdown

Dependencies: Stages 1 through 3.

- [ ] Migrate async mesh import to explicit task status, failure, and
  cancellation. Split it only where a real independent CPU stage exists; do not
  manufacture read/parse/build tasks around a library call that owns all three.
- [ ] Migrate thumbnail decoding to cancellation-aware completion so cache
  destruction, request replacement, and scheduler shutdown cannot strand active
  accounting.
- [ ] Retain the thumbnail cache's game-thread result drain and render-command
  upload as the reference V1 cross-thread ownership pattern.
- [ ] Select one independent CPU loop from a current workload for the first
  `ParallelFor` integration and retain it only when measurement beats the serial
  path above the selected threshold.
- [ ] Harden rendering-thread duplicate init/shutdown and command
  admission/drain behavior without routing render commands through the CPU
  scheduler.
- [ ] Verify engine exit ordering while imports, decodes, and render uploads are
  active.

#### Acceptance Gate

- Destroying or replacing an async requester produces a terminal scheduler
  outcome and no late mutation of requester-owned state.
- Worker code does not resolve DObject handles, mutate game-thread state, or
  perform RHI work.
- A hidden-window editor startup/shutdown smoke test passes while representative
  background work is active.

### Stage 5: Complete V1 evidence and architecture handoff

Dependencies: Stages 1 through 4.

- [ ] Run the complete validation matrix using the root BuildTool workflow.
- [ ] Move stable scheduler, waiting, shutdown, and thread-ownership contracts
  into Architecture documentation.
- [ ] Record profiler or benchmark evidence for any feature proposed beyond V1.
- [ ] Archive this plan only after every required acceptance gate and the
  Definition of Done are satisfied.

#### Acceptance Gate

- All V1 behavior has automated or recorded runtime evidence, the full build and
  smoke test pass on the same preset, and Architecture is the lasting source of
  truth.

## Validation Matrix

| Concern | Unit | Integration | Runtime / stress |
| --- | --- | --- | --- |
| Thread lifecycle | cooperative stop, natural join, duplicate join, unsupported attributes | partial worker creation cleanup | repeated engine startup/shutdown where supported |
| Scheduler lifetime | reject before init/after close, drain, discard | concurrent producers racing shutdown/reinit | thousands of mixed short/long tasks during exit |
| Task states | success, exception failure, queued cancellation, running cancellation | failure/cancel propagation through graphs | outstanding-handle shutdown diagnostics |
| Waiting | invalid handle, multiple waiters, self-wait, one-worker nested wait | game-thread sync point and same-pool idle rejection | long-wait diagnostics without starvation |
| Dependencies | chain, fan-in, fan-out, invalid and cross-generation rejection | async consumer pipeline | randomized DAG completion and cancellation |
| `ParallelFor` | edge ranges, uneven chunks, nested call | one measured engine workload | crossover, throughput, and fairness measurement |
| Render command lifecycle | wake without work, multiple producers, fence order, late enqueue | worker result to render upload | hidden-window shutdown with queued uploads |
| Ownership | compile/runtime thread checks where applicable | weak-handle transport and game-thread application | editor closes requester while work is active |

Build, test, and smoke commands come from
`Documentation/Setup/BuildAndRun.md` and
`Documentation/Setup/NativeTests.md`; this plan does not duplicate them.

## Definition of Done

- [ ] Every Stage 1 through Stage 5 acceptance gate passes.
- [ ] No public task path retains an unsynchronized raw pointer to global
  scheduler storage.
- [ ] Every accepted task and dependent reaches exactly one terminal state.
- [ ] Dependencies, cancellation, waiting, and shutdown policies are enforced by
  deterministic tests.
- [ ] Current async consumers use explicit lifetime-safe result handoff.
- [ ] `ParallelFor` has a measured threshold and at least one justified
  integration, or the integration is rejected with evidence while the API tests
  remain complete.
- [ ] Render-command admission and shutdown are deterministic and tested.
- [ ] The full `all` build and hidden-window `DurinEditor` startup/shutdown smoke
  test pass on the same Agent Build Profile.
- [ ] Lasting contracts are documented in Architecture and this plan is moved to
  `Documentation/Plans/Archive/`.

## Deferred Follow-ups

- Scheduler priorities only after a demonstrated latency conflict between
  frame-critical and background work.
- Dedicated IO scheduling only after file workloads show CPU-pool latency
  isolation is necessary.
- Worker-local queues and work stealing only after queue contention or load
  imbalance is measured.
- Typed results only after multiple consumers repeat the same safe shared-result
  wrapper.
- A generic game-thread mailbox only after multiple subsystems need identical
  delivery semantics and its pump/lifetime owner is clear.
- Fibers or coroutine-backed waits only if worker helping cannot prevent measured
  starvation.
- RenderGraph task integration only after its single-threaded graph and Vulkan
  command-pool ownership contracts exist.

## Related Documentation

- `Documentation/Architecture/RuntimeArchitecture.md`
- `Documentation/Architecture/GarbageCollection.md`
- `Documentation/Plans/TextureSupport.md`
- `Documentation/Setup/BuildAndRun.md`
- `Documentation/Setup/NativeTests.md`

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/RunnableThread.h`
- `Engine/Source/Runtime/Core/Private/Threading/StdRunnableThread.cpp`
- `Engine/Source/Runtime/Core/Public/Threading/QueuedThreadPool.h`
- `Engine/Source/Runtime/Core/Private/Threading/QueuedThreadPool.cpp`
- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderingThread.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp`
- `Engine/Source/Runtime/AssetCore/Private/AssetCore.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SourceImageThumbnailCache.cpp`
- `Engine/Source/Programs/Tests/CoreTests/Private/ThreadingTests.cpp`
- `Engine/Source/Programs/Tests/AssetCoreTests/Private/AssetImportTests.cpp`
