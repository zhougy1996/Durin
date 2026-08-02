# Multithreading V1 Plan

Summary: Production-safe CPU scheduler lifecycle, task states, dependencies, cancellation, parallel loops, and diagnostics.

Last reviewed: 2026-08-02

Status: Active
Completed:

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
- named `FTaskHandle`, polling, `WaitTask`, sequential `WaitAll`, and worker-side
  helping while waiting
- rendering command admission with `Stopped`, `Running`, and `Draining` states,
  rejection of late commands, accepted-command drain, render-resource audit,
  and deterministic rendering-thread termination

The focused task validation recorded before this revision covered the existing
thread, pool, handle, and wait suites. Later rendering lifecycle work added
command-admission and shutdown coverage. There is not yet one V1
qualification run covering concurrent scheduler shutdown, discarded task
handles, callable failures, dependency propagation, cancellation races,
`ParallelFor`, editor shutdown under CPU load, and the final full editor smoke
test on one Agent Build Profile.

Stage 0 refreshed this plan against the task, pool, rendering-thread, runtime
lifecycle, and native-test working sets. Rendering command admission and drain
are established runtime behavior rather than new V1 implementation work; this
plan retains only scheduler lifecycle integration and shutdown evidence.

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
- scheduler integration obeys established game-, render-, and object-ownership
  boundaries without owning subsystem workflows

## Scope

- `Core` runnable-thread lifecycle, worker-pool ownership, scheduler lifetime,
  task state, waits, dependencies, cancellation, `ParallelFor`, and diagnostics
- engine startup and shutdown policy for the process-wide scheduler
- deterministic tests for supported concurrency and lifecycle behavior
- framework benchmarks and engine-lifecycle integration using controlled task
  workloads
- regression and shutdown-under-load validation at the CPU scheduler and
  rendering-thread lifecycle boundary

## Non-Goals

- replacing the rendering command pipe with the CPU task scheduler
- redesigning the established rendering command admission or resource-drain
  protocol
- a generic game-thread or rendering-thread continuation executor
- mutating Worlds, DObjects, editor models, or RHI objects from workers
- typed task results, futures, or automatic storage of arbitrary return values
- preempting, terminating, or recovering a callable that never returns
- subsystem-specific background workflows, result mailboxes, or feature
  migrations
- dedicated IO or RHI-submit threads
- priority queues, work stealing, fibers, coroutines, lock-free queues, or
  frame-local task allocation
- parallel RenderGraph compilation, pass execution, or Vulkan command recording
- concurrent garbage collection

These are deferred until V1 framework workloads and profiler data establish a
concrete need. RenderGraph work also depends on its own single-threaded
ownership model before scheduler integration is designed.

## Design Decisions and Invariants

### Scheduler ownership and shutdown

- A process-owned scheduler facade is the only public route for general task
  submission. `FQueuedThreadPool` remains a low-level primitive for its owning
  scheduler, focused tests, and explicitly owned dedicated pools; callers do not
  acquire or retain the process scheduler's raw pool or `GThreadPool` storage.
- Every successful scheduler initialization receives a monotonically increasing,
  nonzero generation. A task node and its public handles retain that generation
  without retaining raw scheduler or pool storage.
- Submission acquires scheduler lifetime participation before testing admission.
  Acceptance publishes and counts the task node before participation is
  released. Shutdown closes admission at one linearization boundary, waits for
  submitters that entered before it, and rejects every later submission.
- Admission close also rejects child tasks submitted by already-running tasks.
  Dynamic child creation is legal only while admission is open; callers must
  handle a rejected child without waiting for work that was never accepted.
- Normal engine shutdown first detaches CPU-work producers, then drains accepted
  tasks before object drain, module unload, render-command close, and rendering
  termination, following `RuntimeLifecycle.md`.
- Drain shutdown reaches scheduler quiescence only when every accepted node is
  terminal and all dependency release or cancellation propagation is complete;
  an empty worker queue alone is not sufficient.
- Discard shutdown cancels waiting and queued nodes, requests cooperative
  cancellation of running nodes, propagates cancellation through dependents,
  and waits for running callables to return before destroying native threads.
  It still completes every accepted handle as `Succeeded`, `Failed`, or
  `Canceled` and never kills a native thread.
- Worker-pool initialization is mandatory engine startup. `PreInit()` returns
  failure immediately if worker creation fails and cleans up every partially
  created worker and scheduler object.
- No submission, wait, helping, query, or shutdown path may dereference
  scheduler or pool storage after releasing the lifetime participation that
  protects it.

### Task state, outcomes, and failure

- An invalid handle reports `Invalid` and is not a task state. A valid task has
  exactly one of `Waiting`, `Queued`, `Running`, `Succeeded`, `Failed`, or
  `Canceled`.
- The only legal state transitions are:
  - `Waiting -> Queued | Canceled`
  - `Queued -> Running | Canceled`
  - `Running -> Succeeded | Failed | Canceled`
  Terminal states never transition again. A task without prerequisites starts
  at `Queued`.
- `IsComplete()` means `Succeeded`, `Failed`, or `Canceled`; it remains false for
  an invalid handle. A state query reports `Invalid` distinctly.
- `WaitTask` returns the observed terminal state, or `Invalid` immediately for
  an invalid handle. `WaitAll` waits every valid handle and preserves each
  handle's queryable outcome instead of reducing all terminal states to success.
- Failure and cancellation diagnostics are owned by the task node and exposed as
  thread-safe copied values, so callers never retain a view into mutable or
  destroyed storage.
- V1 callables remain `void`. Results stay in caller-owned shared state.
  Publication of that state happens-before the acquire operation that observes a
  terminal task state, and consumers use the payload only when the task outcome
  permits it.
- The cancellation-aware callable form receives its task token and returns
  `void`; a convenience form adapts a `void()` callable that does not need to
  observe cancellation. A caller-created cancellation source may request a
  group, while cancellation through a handle requests only that task node.
- A callable returning normally succeeds unless a running cancellation request
  won before terminal publication. A `std::exception` records `Failed` with its
  message; an unknown exception records a stable generic diagnostic. No
  exception escapes a task or worker entry point, and pool active-work
  bookkeeping is restored by scope-bound cleanup even for lower-level queued
  work. Engine fatal checks keep their process-failure behavior.

### Cancellation

- Canceling `Waiting` or `Queued` races atomically with prerequisite release and
  worker claim. If cancellation wins, the callable never starts and the task
  becomes `Canceled`; if worker claim wins, the task follows running-task rules.
- Canceling `Running` records a request visible through the task's cancellation
  token. The task remains incomplete until its callable returns. If cancellation
  was requested before successful terminal publication, normal return produces
  `Canceled`; a thrown exception produces `Failed` so the failure is not hidden.
- Cancellation arriving after terminal publication has no effect. Repeated
  cancellation is idempotent.
- Cooperative cancellation does not roll back side effects from an already
  running callable. Callables with externally visible publication commit that
  publication only after their last cancellation check.
- Accepted callables are required to return in bounded time or cooperate with
  cancellation. A nonreturning callable can be diagnosed but cannot be made
  terminal safely in V1; drain and discard wait rather than terminating its
  native thread.
- Canceling a dependency or scheduler-owned task group propagates a diagnostic
  cause containing the direct source task ID. Propagation never executes a
  dependent callable.

### Dependencies

- Prerequisites are immutable after submission. A task is queued exactly once
  when all prerequisites succeed.
- The dependency API accepts only valid, already-submitted tasks from the current
  scheduler generation. Every new edge therefore points to an older published
  node, which makes cycles structurally impossible.
- Registration handles prerequisites that become terminal concurrently: the
  dependent is either registered before terminal fan-out or observes the
  terminal state itself, never both and never neither.
- A failed or canceled prerequisite cancels its dependent without running the
  dependent callable. That cancellation propagates through chains, fan-in, and
  fan-out and participates in scheduler quiescence.
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
  a documented synchronization point.
- Cross-thread lambdas capture values, move-owned payloads, or shared/weak state
  with an explicit synchronization contract. Raw pointers and references require
  a proven fence or task lifetime and are not the default.

### Waiting and worker helping

- Game-thread waits are allowed only at explicit synchronization points and
  never while holding subsystem or registry locks.
- A worker waiting for a task from its own scheduler generation may execute an
  eligible queued task while retaining scheduler lifetime participation. It
  does not help a different or stale generation and never executes itself or a
  dependent that is still waiting.
- Self-wait is rejected deterministically. Dependency cycles are prevented by
  publication order, and defensive diagnostics reject any internal violation
  rather than spinning or recursing forever.
- `WaitForIdle()` is a scheduler lifecycle/test operation and is rejected from a
  worker belonging to that scheduler. Scheduler shutdown waits for scheduler
  quiescence rather than using pool idle as a substitute.
- Routine waits on the rendering thread are unsupported. Render work uses render
  fences and game/render frame synchronization.
- Long waits report the waiting task or thread, target task ID and state,
  scheduler generation, and elapsed duration without changing wait semantics.

### `ParallelFor`

- V1 uses static contiguous chunks over the process CPU scheduler, a measured
  minimum batch size, and synchronous execution for empty or small ranges.
- The number of chunks is bounded by the range, the minimum batch size, and at
  most the scheduler worker count plus the caller's chunk. The caller executes
  one chunk and waits for the remaining group at the API boundary.
- A task-local nesting depth detects nested `ParallelFor`; nested calls execute
  serially in V1 so recursive iteration bodies cannot create an unbounded task
  tree.
- Coverage and per-iteration behavior are deterministic when no failure or
  cancellation occurs; execution order is not. Callers must make iterations
  independent.
- When any chunk fails, work not yet claimed is canceled. Already-running chunks
  may finish. The group reports `Failed` if any chunk failed, otherwise
  `Canceled` if cancellation was requested, otherwise `Succeeded`. Failure
  diagnostics select the failing chunk with the lowest range start so the
  reported cause is stable across schedules.
- Exceptions from the caller-owned chunk pass through the same task-outcome
  conversion as worker chunks. `ParallelFor` never silently reports success for
  partial coverage.
- The batching threshold is chosen from a recorded benchmark containing the
  Agent Build Profile, hardware, workload, range sizes, warm-up policy, sample
  count, and raw or machine-readable results.

### Diagnostics

- Task IDs are process-unique and nonzero. Parent ID means the currently
  executing task that submitted this task, or zero for a non-task submitter;
  dependency IDs are recorded separately.
- Enqueue, start, and finish timestamps use one monotonic clock. Queue delay and
  duration are derived from that clock and never from wall-clock time.
- Aggregate queue depth, active workers, completed, failed, canceled, rejected,
  and long-wait counters are scoped by scheduler generation. Shutdown reports
  accepted nodes that remain nonterminal and handles that outlive their node's
  scheduler generation without treating a valid retained terminal handle as a
  leak.
- Per-task trace logging remains optional. Aggregate counters and slow/failure
  diagnostics are available without tracing every task.

### Rendering thread baseline

- Rendering command admission, accepted-command drain, late-enqueue rejection,
  render-resource audit, and rendering-thread termination are established
  runtime contracts owned by `RuntimeLifecycle.md`.
- Render fences are game/render synchronization primitives, not generic task
  handles, and remain outside the CPU dependency graph.
- V1 changes rendering code only when scheduler lifecycle integration exposes a
  missing ownership or shutdown test. It does not reopen the rendering lifecycle
  design or route render commands through the CPU scheduler.

## Current Foundations and Gaps

| Area | Current foundation | V1 gap |
| --- | --- | --- |
| Native threads | Cooperative `Kill`, join, roles, names | A joinable `std::thread` can still reach destruction; stack size and priority requests are stored but not applied or rejected |
| Worker pool | Fixed workers, FIFO queue, drain/discard modes | Discard erases closures without completing task state; exception-safe active-work bookkeeping is not guaranteed |
| Global lifetime | Mutex protects pool replacement | `LaunchTask` and worker helping read raw `GThreadPool` outside that lifetime lock |
| Engine lifecycle | Pool starts in `PreInit` and drains during ordered `Exit` | Startup ignores initialization failure; concurrent admission/shutdown and scheduler quiescence are undefined |
| Task handle | Name, completion poll, waits | Boolean-only completion; no failure, cancellation, dependency, or scheduler generation |
| Waiting | Worker helping prevents the tested one-worker parent/child deadlock | No self-wait, pool-idle, stale-generation, or rendering-thread enforcement |
| Rendering | Deterministic command admission, drain, audit, and thread termination | Scheduler shutdown ordering still needs controlled CPU-load regression evidence |
| Diagnostics | Names, IDs, thread logs, trace per launch/execution | No state, queue delay, duration, parent, cancellation, rejection, or generation-scoped counters |

## Implementation Stages

### Stage 0: Refresh the baseline and freeze V1 contracts

Dependencies: none.

- [x] Compare the plan with the current task, pool, rendering, runtime lifecycle,
  and native-test working sets.
- [x] Add lifecycle metadata, remove stale test totals, and remove stale or
  unrelated source ownership and related paths.
- [x] Reclassify rendering command admission and drain as an implemented runtime
  baseline while retaining CPU-load integration validation.
- [x] Select state transitions, cancellation precedence, shutdown quiescence,
  dynamic-child admission, wait outcomes, nested `ParallelFor`, and failure
  aggregation semantics.
- [x] Validate the active and archived plan set with the root documentation tool.

#### Acceptance Gate

- The plan matches current source and owning runtime documentation, has no known
  stale related-code path, and leaves no open V1 concurrency semantic for later
  stages to decide implicitly.
- `doc plan validate --scope all` passes.

### Stage 1: Close lifecycle and terminal-state holes

Dependencies: Stage 0.

- [ ] Introduce the scheduler facade and remove general task submission's direct
  dependence on `GThreadPool`.
- [ ] Synchronize admission with shutdown, assign scheduler generations, and
  count accepted nodes through terminal propagation.
- [ ] Replace boolean completion with the V1 task state and diagnostic model.
- [ ] Give queued work an explicit discard callback that terminalizes its task
  node; Stage 2 extends that terminalization through dependency propagation.
- [ ] Catch standard and unknown exceptions at the task-callable boundary and
  guarantee pool bookkeeping for every queued-work exit path.
- [ ] Make failed worker-pool initialization fail engine `PreInit()` with complete
  partial-worker cleanup.
- [ ] Make native-thread destruction safe against a joinable `std::thread`, and
  explicitly reject or apply unsupported stack-size and priority requests.
- [ ] Add self-wait, same-pool worker `WaitForIdle`, rendering-thread wait, and
  stale-generation helping checks.

#### Acceptance Gate

- Every accepted task becomes `Succeeded`, `Failed`, or `Canceled` under normal
  execution, callable failure, drain shutdown, discard shutdown, and partial
  pool initialization failure.
- Deterministic latch/barrier tests cover submission racing admission close and
  task completion racing discard without sleeps as correctness conditions.
- Thread and scheduler startup failure cannot leave the engine partially
  initialized, and no public path dereferences unpinned scheduler storage.

### Stage 2: Add immutable dependencies and cooperative cancellation

Dependencies: Stage 1.

- [ ] Represent scheduler-owned task nodes separately from public handles and
  queued callable storage.
- [ ] Add immutable prerequisite submission, prerequisite counters, and
  dependent release for chains, fan-in, and fan-out.
- [ ] Handle prerequisite completion racing dependent registration exactly once.
- [ ] Reject invalid and cross-generation prerequisites at the public boundary;
  keep the one-shot API unable to construct cycles.
- [ ] Propagate prerequisite failure and cancellation without executing blocked
  dependents and include the direct cause in diagnostics.
- [ ] Add cancellation source/token support for waiting, queued, and running
  tasks with the selected terminal-state precedence.
- [ ] Return and expose wait outcomes without conflating terminal completion with
  success.
- [ ] Preserve worker helping without allowing a task to execute itself, an
  ineligible dependent, or work from another scheduler generation.

#### Acceptance Gate

- Deterministic tests cover chains, fan-in, fan-out, invalid graph input,
  simultaneous prerequisite completion, failure/cancel propagation,
  cancellation versus worker claim, cancellation versus normal completion,
  exception versus cancellation, shared-source group cancellation, multiple
  waiters, and one-worker nested waits.
- Drain and discard both terminalize graphs containing waiting nodes; scheduler
  quiescence never relies only on an empty worker queue.
- Common CPU pipelines express ordering without a blocking wait inside each
  stage.

### Stage 3: Add bounded `ParallelFor` and diagnostics

Dependencies: Stage 2.

- [ ] Add `ParallelFor` with empty/small synchronous paths, bounded static
  chunking, caller participation, serial nested behavior, and task-group outcome
  aggregation.
- [ ] Capture exceptions and cancellation uniformly across caller and worker
  chunks and prevent silent success after partial coverage.
- [ ] Measure crossover cost and choose defaults from recorded native-test or
  benchmark data rather than hardware-thread count alone.
- [ ] Record task ID, parent ID, dependency IDs, scheduler generation,
  monotonic enqueue/start/finish timestamps, executing thread, and terminal
  state.
- [ ] Add generation-scoped queue-depth, active-worker, completed, failed,
  canceled, rejected, and long-wait counters without requiring per-task trace
  logging.
- [ ] Diagnose nonterminal nodes and distinguish them from valid retained
  terminal handles at shutdown.

#### Acceptance Gate

- Tests prove exact range coverage for zero, one, small, large, uneven, and
  nested no-failure ranges, and prove bounded behavior plus non-success outcomes
  for failure and cancellation.
- A repeatable measurement records the profile, hardware, workload, sampling
  method, results, and chosen batching threshold.
- A failed or slow task can be identified from diagnostics without enabling
  trace logging for every task.

### Stage 4: Qualify framework workloads and engine lifecycle

Dependencies: Stage 3.

- [ ] Add a repeatable Core benchmark or qualification workload for
  `ParallelFor` and retain its defaults only when measurement beats the serial
  path above the selected threshold. No production feature migration is required
  for V1 completion.
- [ ] Exercise scheduler startup, admission close, drain, discard, generation
  rollover, and retained terminal handles through framework-owned integration
  fixtures with controlled task gates.
- [ ] Verify existing render-command admission, accepted-command drain, late
  rejection, and final audit while controlled CPU tasks are active; change
  rendering code only for an evidenced lifecycle gap.
- [ ] Verify engine exit ordering while short and long tasks, dependency
  propagation, cancellation, waits, and scheduler diagnostics are active.
- [ ] Run the full `all` build and hidden-window editor startup/shutdown smoke on
  the same Agent Build Profile used for final handoff.

#### Acceptance Gate

- The framework benchmark records a justified batching threshold or evidence
  that the serial path remains preferable for the measured range while the API
  itself stays validated.
- Rendering admission reaches `Stopped` with no accepted command, render
  resource, deferred cleanup, or RHI deletion left behind.
- A hidden-window editor startup/shutdown smoke passes with representative
  framework tasks active.

### Stage 5: Complete V1 evidence and architecture handoff

Dependencies: Stages 1 through 4.

- [ ] Run the complete validation matrix using the root DurinDevTool workflow.
- [ ] Move stable scheduler, waiting, shutdown, cancellation, and thread-ownership
  contracts into explicitly named owning runtime documentation.
- [ ] Record profiler or benchmark evidence for any feature proposed beyond V1.
- [ ] Update `Current Status`, every evidence-backed checklist, `Last reviewed`,
  `Status: Completed`, and `Completed: YYYY-MM-DD` after all gates pass.
- [ ] Run the all-plan validator after marking the plan completed.

#### Acceptance Gate

- All V1 behavior has automated or recorded runtime evidence, the full build and
  smoke test pass on the same Agent Build Profile, and owning runtime
  documentation is the lasting source of truth.
- The completed plan is ready for the separate monthly archive workflow; physical
  movement into `Plans/Archive` is not a completion prerequisite.

## Stage Handoff Requirements

Each implementation stage ends with one validated local commit and a compact
handoff that records:

- the baseline commit and resulting stage commit
- the stage working set and key symbols
- selected decisions and any evidence-backed divergence from this plan
- open questions that do not invalidate the acceptance gate
- targeted test, integration, benchmark, build, and runtime outcomes

Subsequent stages start from that handoff and recorded diff instead of
rediscovering completed architecture.

## Validation Matrix

| Concern | Unit | Integration | Runtime / stress |
| --- | --- | --- | --- |
| Thread lifecycle | cooperative stop, natural join, duplicate join, unsupported attributes | partial worker creation cleanup | repeated engine startup/shutdown where supported |
| Scheduler lifetime | reject before init/after close, generation pinning, drain, discard | concurrent producers and running child submission racing close/reinit | mixed short/long tasks and retained old-generation handles during exit |
| Task states | success, standard/unknown exception, queued cancel, running cancel, cancel/complete precedence | failure and cancellation propagation through graphs | nonterminal-node and retained-terminal-handle shutdown diagnostics |
| Waiting | invalid handle, multiple waiters, self-wait, one-worker nested wait | game-thread sync point, same-pool idle rejection, stale-generation wait | long-wait diagnostics without starvation |
| Dependencies | chain, fan-in, fan-out, registration/terminal race, invalid and cross-generation rejection | scheduler quiescence and generation rollover | seeded randomized DAG completion, failure, cancellation, drain, and discard |
| `ParallelFor` | edge ranges, uneven chunks, serial nesting, worker/caller failure and cancellation | measured framework qualification workload | crossover, throughput, bounded task count, and fairness measurement |
| Render command lifecycle | established admission, drain, fence order, late rejection regressions | scheduler closes before render admission | hidden-window shutdown with controlled CPU tasks active |
| Thread boundaries | launch, wait, and affinity checks where applicable | synthetic CPU payload crosses only through explicit game/render handoff | engine exit preserves CPU, object, module, and rendering order |

Concurrency correctness tests use latches, barriers, events, or instrumented
hooks to create the required interleavings. Sleeps may bound a stress test or
detect a hang but are not the proof that a race was exercised. Randomized tests
record their seed and iteration count on failure.

Build, test, benchmark, and smoke commands come from
`Documentation/Development/Build/BuildAndRun.md`,
`Documentation/Development/Build/NativeTests.md`, and the owning profiling
documentation; this plan does not duplicate commands or mutable test totals.

## Definition of Done

- [ ] Every Stage 1 through Stage 5 acceptance gate passes.
- [ ] No public task path retains an unsynchronized raw pointer to process
  scheduler storage.
- [ ] Every accepted task and dependent reaches exactly one terminal state, and
  shutdown quiescence includes waiting dependency nodes and propagation work.
- [ ] Dependencies, cancellation, waiting, admission close, and shutdown
  precedence are enforced by deterministic tests.
- [ ] Framework APIs remain independent of subsystem-specific object ownership,
  result delivery, and feature lifecycle policy.
- [ ] `ParallelFor` has a measured threshold from a repeatable framework
  qualification workload while its API tests remain complete.
- [ ] Existing render-command admission and shutdown contracts pass regression
  and CPU-load integration validation.
- [ ] The full `all` build and hidden-window `DurinEditor` startup/shutdown smoke
  pass on the same Agent Build Profile.
- [ ] Lasting contracts are documented in their owning domain, the plan is marked
  `Completed`, and the all-plan validator passes. Monthly physical archival
  remains a separate maintenance operation.

## Deferred Follow-ups

- Scheduler priorities only after a demonstrated latency conflict between
  frame-critical and background work.
- Dedicated IO scheduling only after file workloads show CPU-pool latency
  isolation is necessary.
- Worker-local queues and work stealing only after queue contention or load
  imbalance is measured.
- Typed results only after multiple call sites repeat the same safe shared-result
  wrapper.
- A generic game-thread mailbox only after multiple subsystems need identical
  delivery semantics and its pump/lifetime owner is clear.
- Fibers or coroutine-backed waits only if worker helping cannot prevent measured
  starvation.
- RenderGraph task integration only after its single-threaded graph and Vulkan
  command-pool ownership contracts exist.

## Related Documentation

- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Runtime/Core/GarbageCollection.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/NativeTests.md`
- `Documentation/Development/Build/Profiling.md`

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
- `Engine/Tests/Native/CoreTests/Private/ThreadingTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderResourceLifecycleTests.cpp`
