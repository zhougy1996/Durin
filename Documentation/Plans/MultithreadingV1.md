# Multithreading V1 Plan

Summary: Production-safe CPU scheduler lifecycle, task states, dependencies, cancellation, parallel loops, and diagnostics.

Last reviewed: 2026-08-02

Status: Completed
Completed: 2026-08-02

## Current Status

V1 is complete. The process scheduler now has a UE-style one-start,
one-shutdown engine lifetime, observable terminal outcomes, immutable
dependencies, cooperative cancellation, bounded waiting and worker helping,
diagnostics, and a measured `ParallelFor`. Sequential restarts remain available
only for isolated tests and programs, and scheduler instances never overlap.
The complete native-test suite, full `all` build, and hidden-window editor exit
with representative CPU work passed on `Win64-Debug-DurinEditor-Tests`.
`Documentation/Runtime/Core/TaskSystem.md` now owns the lasting task-system and
thread-ownership contracts; `RuntimeLifecycle.md` owns process shutdown order.

Established before Stage 1:

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

Stage 1 added:

- process-owned scheduler admission and shutdown without public raw-pool access
- serialized `Stopped`, `Running`, and `ShuttingDown` scheduler lifetime plus
  accepted-node quiescence
- `Waiting`, `Queued`, `Running`, `Succeeded`, `Failed`, and `Canceled` task
  states, copied diagnostics, and outcome-returning waits
- explicit queued-work discard callbacks and exception-safe pool bookkeeping
- callable failure capture for standard and unknown exceptions
- mandatory scheduler startup failure propagation through engine `PreInit()`
- safe joinable native-thread destruction and explicit rejection of unsupported
  stack-size and priority requests
- deterministic rejection of self-wait, rendering-thread wait, same-pool worker
  idle wait, and restart while shutdown is in progress

Stage 2 added:

- scheduler-owned nonterminal task nodes with immutable prerequisite lists
- exactly-once release for dependency chains, fan-in, fan-out, and terminal
  registration races
- failure and cancellation propagation without executing blocked dependents
- per-task cancellation plus shared cancellation sources and callable tokens
- cooperative running-task cancellation with exception-over-cancellation
  precedence
- outcome-preserving `WaitAll` and graph-aware drain/discard quiescence

Stages 1 through 3 validate the thread, pool, scheduler, handle, dependency,
cancellation, failure, discard, drain, admission-close, serialized lifecycle,
wait boundaries, bounded parallel loops, and queryable diagnostics. Existing
rendering lifecycle work covers command admission and shutdown. There is not yet one V1
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
- The engine starts the process scheduler once during `PreInit()` and shuts it
  down once during `Exit()`. Core keeps explicit sequential startup/shutdown for
  isolated tests and programs, but initialization during `ShuttingDown` is
  rejected and two process scheduler instances never overlap.
- Task nodes and public handles do not expose scheduler generations. A node
  weakly references its owning scheduler only where waiting and terminal
  bookkeeping require lifetime participation; retained terminal handles remain
  queryable after scheduler shutdown.
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
- The dependency API accepts only valid, already-submitted tasks owned by the
  currently running scheduler instance. Every new edge therefore points to an
  older published node, which makes cycles structurally impossible. Retained
  handles from an earlier sequential test/program lifetime are rejected.
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
- A worker waiting for a task owned by its scheduler instance may execute an
  eligible queued task while retaining scheduler lifetime participation. It
  never executes itself or a dependent that is still waiting.
- Self-wait is rejected deterministically. Dependency cycles are prevented by
  publication order, and defensive diagnostics reject any internal violation
  rather than spinning or recursing forever.
- `WaitForIdle()` is a scheduler lifecycle/test operation and is rejected from a
  worker belonging to that scheduler. Scheduler shutdown waits for scheduler
  quiescence rather than using pool idle as a substitute.
- Routine waits on the rendering thread are unsupported. Render work uses render
  fences and game/render frame synchronization.
- Long waits report the waiting task or thread, target task ID and state, and
  elapsed duration without changing wait semantics.

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
  and long-wait counters are scoped to the process scheduler lifetime. Shutdown
  reports accepted nodes that remain nonterminal and distinguishes retained
  terminal handles from scheduler-storage leaks.
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

## V1 Result

| Area | Implemented foundation | V1 disposition |
| --- | --- | --- |
| Native threads | Cooperative stop, idempotent join, destructor join, roles, names, and explicit unsupported-attribute rejection | Callable bounded-return remains a scheduler contract; native preemption is out of scope |
| Worker pool | Fixed workers, FIFO queue, drain/discard modes, explicit discard callbacks, exception-safe bookkeeping, dependency release, cooperative running-task cancellation, bounded `ParallelFor`, and scheduler counters | Complete and qualified by framework workload and full regression tests |
| Global lifetime | Process scheduler facade serializes `Stopped`, `Running`, and `ShuttingDown`; production starts once, isolated fixtures may restart only after shutdown completes, and shutdown waits graph quiescence | Complete and qualified under concurrent admission close and CPU-load shutdown |
| Engine lifecycle | Mandatory scheduler startup propagates failure from `PreInit`; ordered exit drains accepted tasks | Complete through real editor startup and controlled exit workload |
| Task handle | Name, ID, parent/dependency IDs, monotonic timing, executing thread, full state query, copied diagnostic, completion poll, immutable dependencies, cancellation, and outcome-returning waits | Complete through deterministic unit, graph, and shutdown integration coverage |
| Waiting | Same-scheduler worker helping plus self-wait, same-pool idle, rendering-thread enforcement, and queryable long-wait diagnostics | Complete through boundary tests and engine-exit long-wait evidence |
| Rendering | Deterministic command admission, drain, audit, and thread termination | Existing contract passed regression and scheduler-before-render shutdown integration |
| Diagnostics | Per-task relationships/timing/outcome snapshots, scheduler-lifetime gauges and counters, retained-handle distinction, long-wait records, and optional trace per launch/execution | Complete through final zero-nonterminal snapshot and retained-handle evidence |

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

- [x] Introduce the scheduler facade and remove general task submission's direct
  dependence on `GThreadPool`.
- [x] Synchronize admission with shutdown, serialize process scheduler lifetime,
  and count accepted nodes through terminal propagation.
- [x] Replace boolean completion with the V1 task state and diagnostic model.
- [x] Give queued work an explicit discard callback that terminalizes its task
  node; Stage 2 extends that terminalization through dependency propagation.
- [x] Catch standard and unknown exceptions at the task-callable boundary and
  guarantee pool bookkeeping for every queued-work exit path.
- [x] Make failed worker-pool initialization fail engine `PreInit()` with complete
  partial-worker cleanup.
- [x] Make native-thread destruction safe against a joinable `std::thread`, and
  explicitly reject or apply unsupported stack-size and priority requests.
- [x] Add self-wait, same-pool worker `WaitForIdle`, rendering-thread wait, and
  shutdown-in-progress restart checks.

#### Acceptance Gate

- Every accepted task becomes `Succeeded`, `Failed`, or `Canceled` under normal
  execution, callable failure, drain shutdown, discard shutdown, and partial
  pool initialization failure.
- Deterministic latch/barrier tests cover submission racing admission close and
  task completion racing discard without sleeps as correctness conditions.
- Thread and scheduler startup failure cannot leave the engine partially
  initialized, and no public path dereferences unpinned scheduler storage.

#### Stage 1 Handoff

- Baseline commit: `a92c76227e651b73a2b0aad9efda9b5d779e7ffd`.
- Resulting stage commits: the original `fix(core): make task scheduler
  lifecycle safe` commit and the `refactor(core): align scheduler with process
  lifetime` follow-up containing this revised handoff.
- Working set: Core task, queued-pool, and standard-thread APIs and
  implementations; Launch startup/exit; Core concurrency and AssetImport tests.
- Key decisions: the scheduler owns its pool privately; global admission is one
  serialized linearization boundary; engine startup/shutdown is one process
  lifetime; isolated fixtures may restart only after shutdown completes;
  handles retain task nodes but only weakly reference scheduler storage;
  discard callbacks publish `Canceled`; worker helping compares actual
  scheduler ownership rather than a public generation identifier.
- Open questions: none that invalidate the Stage 1 gate. Dependency waiting and
  running-task cancellation deliberately remain Stage 2 work.
- Validation: `CoreConcurrencyTests` passed; the scheduler/task cases passed 100
  repeated runs; `AssetImportTests` passed; the `DurinLauncher` target built;
  and the all-plan validator passed on `Win64-Debug-DurinEditor-Tests`.

### Stage 2: Add immutable dependencies and cooperative cancellation

Dependencies: Stage 1.

- [x] Represent scheduler-owned task nodes separately from public handles and
  queued callable storage.
- [x] Add immutable prerequisite submission, prerequisite counters, and
  dependent release for chains, fan-in, and fan-out.
- [x] Handle prerequisite completion racing dependent registration exactly once.
- [x] Reject invalid prerequisites and handles not owned by the currently
  running scheduler instance; keep publication order unable to construct cycles.
- [x] Propagate prerequisite failure and cancellation without executing blocked
  dependents and include the direct cause in diagnostics.
- [x] Add cancellation source/token support for waiting, queued, and running
  tasks with the selected terminal-state precedence.
- [x] Return and expose wait outcomes without conflating terminal completion with
  success.
- [x] Preserve worker helping without allowing a task to execute itself or an
  ineligible dependent.

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

#### Stage 2 Handoff

- Baseline commit: `34a3de007abc461682d8f4cb45cd1706fb9fc902`.
- Resulting stage commit: `feat(core): add task dependencies and cancellation`.
- Working set: Core task API and implementation plus Core concurrency tests;
  the private queued-pool implementation required no change because scheduler
  admission now distinguishes new submissions from internal release of already
  accepted nodes.
- Key symbols: `FTaskLaunchOptions`, `FTaskCancellationSource`,
  `FTaskCancellationToken`, `LaunchCancelableTask`, `CancelTask`,
  `FTaskStateData::OnPrerequisiteTerminal`, and
  `FTaskScheduler::QueueTask`.
- Key decisions: the scheduler strongly owns every nonterminal node; dependency
  edges only target older handles from the same live scheduler; a shared source
  maintains an unregistering weak task registry; failure wins over a concurrent
  cancellation request; discard requests running-task cancellation and waits
  for callables to return; and drain keeps internal queueing available until the
  accepted graph reaches quiescence.
- Open questions: none that invalidate the Stage 2 gate. Long-wait detection,
  parent/dependency timing diagnostics, and scheduler counters remain Stage 3.
- Validation: `CoreConcurrencyTests` passed; the scheduler/task cases passed 100
  repeated runs, with the dependency registration race itself using 64
  barrier-synchronized iterations per run; `AssetImportTests` passed; the
  `DurinLauncher` target built; and the all-plan validator passed on
  `Win64-Debug-DurinEditor-Tests`.

### Stage 3: Add bounded `ParallelFor` and diagnostics

Dependencies: Stage 2.

- [x] Add `ParallelFor` with empty/small synchronous paths, bounded static
  chunking, caller participation, serial nested behavior, and task-group outcome
  aggregation.
- [x] Capture exceptions and cancellation uniformly across caller and worker
  chunks and prevent silent success after partial coverage.
- [x] Measure crossover cost and choose defaults from recorded native-test or
  benchmark data rather than hardware-thread count alone.
- [x] Record task ID, parent ID, dependency IDs, monotonic
  enqueue/start/finish timestamps, executing thread, and terminal state.
- [x] Add scheduler-lifetime queue-depth, active-worker, completed, failed,
  canceled, rejected, and long-wait counters without requiring per-task trace
  logging.
- [x] Diagnose nonterminal nodes and distinguish them from valid retained
  terminal handles at shutdown.

#### Acceptance Gate

- Tests prove exact range coverage for zero, one, small, large, uneven, and
  nested no-failure ranges, and prove bounded behavior plus non-success outcomes
  for failure and cancellation.
- A repeatable measurement records the profile, hardware, workload, sampling
  method, results, and chosen batching threshold.
- A failed or slow task can be identified from diagnostics without enabling
  trace logging for every task.

#### Stage 3 Measurement

- Agent Build Profile: `windows-msvc-x64`; preset:
  `Win64-Debug-DurinEditor-Tests`; baseline commit:
  `6200241500c2829f7ccf0ab583e6bf351a94e822`.
- Hardware: 12th Gen Intel Core i7-12700, 20 logical processors; Windows NT
  `10.0.26200.0`; the controlled scheduler used four workers.
- Workload: one independent 64-round xorshift/multiply transform and one
  contiguous `uint64` store per iteration. Each range used two warm-ups and
  nine samples; the table records median `steady_clock` nanoseconds. The
  parallel candidate used a minimum batch of 256 and at most four worker chunks
  plus the caller chunk.
- Repeat command: run `CoreConcurrencyTests.exe` with
  `--gtest_filter=DISABLED_FParallelForBenchmarks.MeasuresSerialParallelCrossover`
  and `--gtest_also_run_disabled_tests` from the documented native-test binary
  directory.

| Range | Serial median ns | Parallel median ns | Chunks |
| ---: | ---: | ---: | ---: |
| 64 | 9,400 | 31,800 | 1 |
| 256 | 36,100 | 70,300 | 1 |
| 1,024 | 156,300 | 343,300 | 4 |
| 4,096 | 593,300 | 901,800 | 5 |
| 16,384 | 2,346,500 | 3,076,300 | 5 |
| 65,536 | 8,758,800 | 10,427,700 | 5 |
| 262,144 | 36,013,000 | 41,275,500 | 5 |
| 1,048,576 | 234,348,800 | 252,036,700 | 5 |

No crossover appeared in this Debug native workload. The evidence-based V1
default therefore uses the sentinel maximum batch size and remains serial;
callers with measured heavy iterations may opt into a smaller batch. Stage 4
owns framework/profiling qualification and may lower the default only when its
recorded workload demonstrates a crossover.

#### Stage 3 Handoff

- Baseline commit: `6200241500c2829f7ccf0ab583e6bf351a94e822`.
- Resulting stage commit: `feat(core): add bounded parallel loops and diagnostics`.
- Working set: Core task API and implementation, Core concurrency tests, and
  this plan. The queued-pool implementation required no change because its
  existing queue-depth query and bounded worker ownership were sufficient.
- Key symbols: `FParallelForOptions`, `FParallelForResult`,
  `FParallelForCancellationToken`, `ParallelFor`, `ParallelForCancelable`,
  `FTaskDiagnostics`, `FTaskSchedulerDiagnostics`,
  `FTaskHandle::GetDiagnostics`, and `GetTaskSchedulerDiagnostics`.
- Key decisions: static contiguous chunks are bounded by range, batch, and
  worker count plus the caller; nesting is task-local and serial; group failure
  cancels unclaimed work and selects the lowest failing chunk start; diagnostic
  snapshots own copied strings and monotonic timestamps; scheduler storage owns
  only nonterminal nodes while weak lifetime records distinguish retained
  terminal handles; and the default remains serial because the Stage 3 Debug
  measurement found no crossover.
- Open questions: none that invalidate the Stage 3 gate. Stage 4 must decide
  whether a framework/profiling workload justifies lowering the conservative
  default, then qualify engine shutdown with CPU work and rendering active.
- Validation: `CoreConcurrencyTests` passed; the `ParallelFor` and diagnostic
  cases passed 50 repeated runs; the disabled crossover measurement completed
  and its medians are recorded above; `AssetImportTests` passed; the
  `DurinLauncher` target built; and the all-plan validator passed on
  `Win64-Debug-DurinEditor-Tests`.

### Stage 4: Qualify framework workloads and engine lifecycle

Dependencies: Stage 3.

- [x] Add a repeatable Core benchmark or qualification workload for
  `ParallelFor` and retain its defaults only when measurement beats the serial
  path above the selected threshold. No production feature migration is required
  for V1 completion.
- [x] Exercise engine one-shot scheduler startup, admission close, drain,
  discard, serialized fixture restart, and retained terminal handles through
  framework-owned integration fixtures with controlled task gates.
- [x] Verify existing render-command admission, accepted-command drain, late
  rejection, and final audit while controlled CPU tasks are active; change
  rendering code only for an evidenced lifecycle gap.
- [x] Verify engine exit ordering while short and long tasks, dependency
  propagation, cancellation, waits, and scheduler diagnostics are active.
- [x] Run the full `all` build and hidden-window editor startup/shutdown smoke on
  the same Agent Build Profile used for final handoff.

#### Stage 4 Qualification Evidence

The repeatable Core qualification workload added in Stage 3 was rerun for the
Stage 4 gate on `windows-msvc-x64`, preset
`Win64-Debug-DurinEditor-Tests`, with four controlled workers, a candidate
minimum batch of 256, two warmups, nine samples, and median steady-clock
nanoseconds. It again found no crossover:

| Range | Serial median (ns) | Parallel median (ns) | Parallel chunks |
|---:|---:|---:|---:|
| 64 | 7,700 | 20,300 | 1 |
| 256 | 30,400 | 74,200 | 1 |
| 1,024 | 121,800 | 343,900 | 4 |
| 4,096 | 482,400 | 766,900 | 5 |
| 16,384 | 1,946,600 | 2,652,300 | 5 |
| 65,536 | 7,880,000 | 9,856,600 | 5 |
| 262,144 | 31,963,200 | 38,988,200 | 5 |
| 1,048,576 | 130,638,300 | 242,380,500 | 5 |

The V1 default therefore remains the serial sentinel. Callers may opt into a
smaller batch only with workload-specific evidence.

The hidden editor smoke ran three ticks and then entered the normal
`FEngineLoop::Exit` path with the diagnostic lifecycle workload enabled. Its
final scheduler snapshot recorded 27 completed tasks, one intentionally failed
task, two canceled tasks, one admission rejection, one long-wait diagnostic,
zero nonterminal tasks, zero active workers, and nine retained terminal handles.
Rendering then reached `Stopped` without a live render resource, pending
deferred cleanup, or pending RHI deletion diagnostic.

#### Stage 4 Handoff

- Baseline commit: `c1dea23765ace87f17a55a1ef32a761cee9d16f8`.
- Resulting stage commit: `feat(launch): qualify scheduler engine lifecycle`.
- Working set: Launch startup parameters and engine exit workload, RenderCore
  lifecycle integration tests, build/run documentation, and this plan.
- Key symbols: `FEngineStartupParams::bRunTaskSchedulerLifecycleSmoke`,
  `FEngineTaskSchedulerLifecycleSmoke`,
  `SchedulerDrainCompletesBeforeRenderAdmissionCloses`, and
  `SchedulerDiscardCancelsAcceptedWorkAndRetainedHandlesSurviveRestart`.
- Key decisions: the diagnostic workload is opt-in through
  `--task-scheduler-lifecycle-smoke`; normal editor sessions pay no workload
  cost; production rendering code was unchanged because existing admission,
  drain, final resource audit, and RHI deletion contracts passed under CPU load;
  and the `ParallelFor` default remains serial because qualification still found
  no crossover.
- Open questions: none that invalidate the Stage 4 gate. Stage 5 must run the
  complete validation matrix and move stable contracts into owning runtime
  documentation.
- Validation: Core concurrency and RenderCore contract suites passed; the new
  CPU/render lifecycle fixtures passed 50 repeated runs; the disabled
  qualification workload completed; the full `all` target built; and the
  hidden-window editor lifecycle smoke passed on
  `Win64-Debug-DurinEditor-Tests`. The first 18-job full build encountered
  concurrent GoogleTest discovery timeouts; the clean incremental rerun with
  one job completed successfully without source changes.

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

- [x] Run the complete validation matrix using the root DurinDevTool workflow.
- [x] Move stable scheduler, waiting, shutdown, cancellation, and thread-ownership
  contracts into explicitly named owning runtime documentation.
- [x] Record profiler or benchmark evidence for any feature proposed beyond V1.
- [x] Update `Current Status`, every evidence-backed checklist, `Last reviewed`,
  `Status: Completed`, and `Completed: YYYY-MM-DD` after all gates pass.
- [x] Run the all-plan validator after marking the plan completed.

#### Stage 5 Evidence

- Agent Build Profile: `windows-msvc-x64`; preset:
  `Win64-Debug-DurinEditor-Tests`; baseline commit:
  `3df6fab05a18b4a21beb2854fe1f35b6f73bc896`.
- The root DurinDevTool completed the full `all` build and full `all` native-test
  suite with one job. The single-job setting avoids the GoogleTest discovery
  resource contention already characterized during Stage 4; it does not reduce
  target or test coverage.
- The hidden-window editor ran three ticks with
  `--task-scheduler-lifecycle-smoke` and exited successfully. Its final CPU
  snapshot recorded 27 completed tasks, one intentional failure, two canceled
  tasks, one post-close rejection, one long-wait diagnostic, zero nonterminal
  tasks, zero active workers, and nine retained terminal handles. Rendering
  then stopped without a live-resource, deferred-cleanup, or pending-RHI-delete
  diagnostic.
- `Documentation/Runtime/Core/TaskSystem.md` owns scheduler lifetime, task
  states, dependencies, cancellation, waiting, `ParallelFor`, diagnostics, and
  CPU-side ownership. `RuntimeLifecycle.md` owns the engine's CPU/object/module/
  render shutdown order.
- No post-V1 feature was selected or proposed for implementation. Every deferred
  feature remains explicitly gated on future workload-specific profiler or
  benchmark evidence, so Stage 5 adds no speculative measurement or default.

#### Stage 5 Handoff

- Baseline commit: `3df6fab05a18b4a21beb2854fe1f35b6f73bc896`.
- Resulting stage commit: `docs(core): complete multithreading v1 handoff`.
- Working set: the new CPU task-system runtime contract, runtime lifecycle and
  documentation routing links, and this completed plan.
- Key documents: `Documentation/Runtime/Core/TaskSystem.md` and
  `Documentation/Runtime/Core/RuntimeLifecycle.md`.
- Key decisions: the runtime contract, rather than the completed plan, is the
  lasting source of truth; the production scheduler remains one-start/
  one-shutdown; the `ParallelFor` default remains serial; and all advanced
  scheduling features remain evidence-gated.
- Open questions: none. Monthly physical archival is a separate maintenance
  action and is not required for V1 completion.
- Validation: the changed-document validator, full `all` build, full `all`
  native-test suite, hidden-window lifecycle smoke, diff check, and all-plan
  validator passed on `Win64-Debug-DurinEditor-Tests`.

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
| Scheduler lifetime | reject before init/after close, reject restart during shutdown, drain, discard | concurrent producers and running child submission racing close | mixed short/long tasks and retained terminal handles during exit |
| Task states | success, standard/unknown exception, queued cancel, running cancel, cancel/complete precedence | failure and cancellation propagation through graphs | nonterminal-node and retained-terminal-handle shutdown diagnostics |
| Waiting | invalid handle, multiple waiters, self-wait, one-worker nested wait | game-thread sync point and same-pool idle rejection | long-wait diagnostics without starvation |
| Dependencies | chain, fan-in, fan-out, registration/terminal race, invalid and foreign-lifetime handle rejection | scheduler quiescence and serialized fixture restart | seeded randomized DAG completion, failure, cancellation, drain, and discard |
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

- [x] Every Stage 1 through Stage 5 acceptance gate passes.
- [x] No public task path retains an unsynchronized raw pointer to process
  scheduler storage.
- [x] Every accepted task and dependent reaches exactly one terminal state, and
  shutdown quiescence includes waiting dependency nodes and propagation work.
- [x] Dependencies, cancellation, waiting, admission close, and shutdown
  precedence are enforced by deterministic tests.
- [x] Framework APIs remain independent of subsystem-specific object ownership,
  result delivery, and feature lifecycle policy.
- [x] `ParallelFor` has a measured threshold from a repeatable framework
  qualification workload while its API tests remain complete.
- [x] Existing render-command admission and shutdown contracts pass regression
  and CPU-load integration validation.
- [x] The full `all` build and hidden-window `DurinEditor` startup/shutdown smoke
  pass on the same Agent Build Profile.
- [x] Lasting contracts are documented in their owning domain, the plan is marked
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

- `Documentation/Runtime/Core/TaskSystem.md`
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
