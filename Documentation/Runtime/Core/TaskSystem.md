# CPU Task System

Summary: Define accepted CPU work, typed ownership, dependencies, cancellation, waiting, and scheduler lifetime.

Modules: Core

Last reviewed: 2026-09-08

## Construction And Acceptance

`Threading/TaskComposition.h` defines the ordinary API in `Durin::Tasks`.
`LaunchTask(Group, Executor, Options, Callable)` returns `TTask<T>` directly,
including `TTask<void>` for a void callable. Callables accept no arguments,
`FTaskContext&`, or a cancellation token. `LaunchTask(Name, Callable, Options)`
is the Worker convenience form; `Options.Scope` may borrow an owner scope,
otherwise it inherits the executing task scope, or participates in the
scheduler lifetime when submitted outside a task. `FTaskGroup` owns
an explicit scope or borrows an existing module scope without taking its drain
responsibility. Group construction requires a running scheduler.

A valid submission while its scheduler and owner scope are open is accepted.
Busy executors queue work. Ordinary roots, continuations, children, fan-in and
completion sources do not return admission errors or failed surrogate tasks
when scheduler thresholds are exceeded. They do not retry, wait for capacity,
or run a callable inline. Acceptance does not promise execution success.

Options carry debug name, priority, cancellation, attribution, borrowed scope,
prerequisites, and deferred generation/coalescing identity. Ordinary callers do
not declare scheduler capture/result byte budgets. Actual domain payload limits
and producer throttling belong to resource owners; see
[Asset Compilation](../Assets/AssetCompilation.md).

Invalid dependencies/executors, consumed unique inputs, and submission after
closure are programming/lifetime violations. Ordinary construction uses the
always-enabled `requiref` failure path, including Shipping. Allocation failure
in scheduler construction/registration/dispatch/result storage is fatal; it is
not ordinary task failure. Exceptions from user work produce Failed tasks.

`Threading/Task.h` retains erased handles, unique storage, scopes, cancellation,
wait operations, lifecycle and kernel primitives. Checked admission is limited
to `Private::TryLaunchCancelableTaskWithCompletion` and
`Private::TryLaunchContinuationTask`, Core kernel tests, deliberate lifecycle
close probes, and ParallelFor's bounded infrastructure chunk submission.
These operations retain structured capacity/lifetime/scope/prerequisite errors
and transactional rollback. They always validate construction. Business code
must use the ordinary API rather than selecting the checked policy.

## Storage And Execution Limits

Accepted waiting/ready nodes remain in scheduler lifetime and scope tracking.
Worker ready queues and the deferred queue own their callable storage until
execution or explicit cancellation. There is no untracked overflow queue.
Pending storage can grow with accepted work; this is not an unlimited-memory
guarantee. The scheduler does not inspect allocations behind containers or
pointers captured by a callable.

`MaxNonterminalTasks` defaults to 16,384 and `MaxBlockingIOTasks` to 128.
They remain checked-kernel reservation limits and are overload thresholds for
ordinary work. Actual CPU and blocking-I/O execution is limited by the installed
thread pools. A waiting parent never holds a capacity permit that prevents its
child or prerequisite from progressing through executor-specific helping.

The deferred executor retains its frame time/item budget, priorities, FIFO,
generations and coalescing. Its configured 1,024 entries, 8 MiB queued payload,
and 1 MiB per-entry limits apply to checked work; ordinary accepted entries
remain queued beyond them. Large domain results remain in owner-controlled
records or mailboxes with independent payload budgets. Deferred shutdown and
selected module drain see the entire accepted queue.

`FTaskSchedulerDiagnostics::ExecutorStorage` is indexed by native `ETaskTarget`.
It reports current/peak node count, logical scheduler-owned node/callable/
prerequisite/result storage bytes, current/peak pending nodes and bytes,
current/peak running bodies, and node-threshold crossings. Pending includes
external sources awaiting acknowledgement; running bodies exclude them and
include suspended parents while helping a child. Threshold transitions emit
power-of-two rate-limited warnings outside internal locks. This accounting
excludes allocator overhead and separately allocated domain payloads and is
not process RSS. Existing attribution gauges distinguish waiting, queued,
running and nonterminal nodes and report callable/result storage peaks.
Deferred diagnostics include current/peak retained queue storage, queue depth
and crossings of configured count/byte thresholds.
Terminal publication releases scheduler reservations exactly once; retained
terminal handles/results have separate lifetimes.

## Unique And Shared Results

`TTask<T>` is move-only. `GetCompletion()` observes execution without value
access. `IsCompleted()` includes Succeeded, Failed and Canceled and does not
imply a value exists. `GetFailure()` requires Failed. `GetResult()` waits for
success and returns an immutable reference valid until consumption/destruction.
`std::move(Task).TakeResult()` waits for success, transfers the value exactly
once and invalidates the task. Rejected waits, failure and cancellation must be
handled before accessing successful results. Void tasks compose without a
value argument and do not retain a module result reservation merely because
they have a terminal hook.

`Share(std::move(Task))` directly returns `TSharedTask<T>` and relinquishes the
unique claim. `GetResultShared()` is a nonblocking nullable immutable owner and
returns empty before success or after failure/cancellation. The alias retains
producer lifetime while the result outlives the task facade. `GetResult()` on a
shared task waits for success and references the same immutable storage.
Copyable versus move-only value types do not select ownership implicitly.

Publication/discard precedes externally visible terminal state and dependent
release. Unique claims are reserved and committed transactionally; storage is
discarded exactly once for cancellation, callback failure, dropped ownership,
and shutdown. Captures and results are destroyed outside task, scheduler and
executor queue locks. A terminal handle does not retain a runnable callback.

## Composition

All ordinary composition returns task handles directly:

| Operation | Behavior |
| --- | --- |
| `Then(TTask<T>&&, Executor, Options, F)` | Consume success into another unique task; void inputs omit the value; callbacks may take context first |
| `Then(const TSharedTask<T>&, Executor, Options, F)` | Observe immutable success and produce a unique result |
| `Then(FTaskCompletion, Executor, Options, F)` | Success edge with an erased prerequisite and no input value |
| `ThenCompleted(const TSharedTask<T>&, Executor, Options, F)` | Receive the shared task in any terminal state; inspect before accessing a result |
| `ThenAsync(TTask<T>&&, Executor, Options, F)` | Flatten a returned unique task or shared immutable result owner |
| `WhenAll(vector<TTask<T>>&&)` | Unique values in input order |
| `WhenAll(tuple<TTask<Ts>...>&&)` | Heterogeneous unique results; void positions use monostate |
| `WhenAll(const vector<TSharedTask<T>>&)` | Immutable owning result views, including duplicate positions |

Then rejects task-returning callbacks; ThenAsync is explicit flattening.
Empty fan-in without additional prerequisites is immediately successful in
the current scheduler lifetime (or its explicit owner scope). Additional
prerequisites retain their dependency and executor requirements.
Fan-in waits for all inputs; failure precedes cancellation and the lowest
failing input index determines the framework failure. Shared observers are
independently cancelable. A canceled observing edge may never invoke its
callback, so mandatory cleanup belongs to the owner lifecycle boundary.

ThenAsync binds and cycle-checks its dynamic dependency before clearing unknown
execution requirements. Unique inner cancellation is forwarded; shared inner
cancellation remains local to its observer. An inner user exception remains
execution failure, not a scheduler admission result.

`TCompletionSource<T>::Create` returns a counted external producer directly.
It does not occupy a worker. `TakeTask()` transfers its one unique task;
`TrySetValue`, `TrySetFailure` and `TrySetCanceled` settle once and report late
publication attempts. Last unresolved source release fails with abandonment.
Cancellation remains requested until the external producer acknowledges it.
Preallocated terminal hooks run outside internal locks without scheduling a
new task. Source scope/known-requirement construction also supports internal
composition such as empty fan-in and dynamically bound inner tasks.

## Dependencies, Waiting And CPU Policy

Prerequisites must be valid in the same scheduler lifetime. Success dependencies
release once after every prerequisite succeeds; failure/cancellation skips the
body and records the blocking relationship. Completion edges observe all
terminal states. Registration racing terminal publication cannot lose or
duplicate dependent release. A task id is process-unique and nonzero; parent
and prerequisite identities are distinct.

`WaitTask` returns wait status separately from task state. Completed means a
terminal state was observed; InvalidTask, SelfWait, DependencyCycle and
UnsupportedThread do not prove completion. `WaitAll` preserves one result per
input. GameThread rejects unknown or transitive deferred requirements;
executing tasks reject unknown external requirements. RenderingThread cannot
wait. Waits do not pump GameThread callbacks. Kernel Worker helping runs CPU
work; I/O helping stays within the blocking-I/O authority, including a
single-thread I/O parent waiting for its child.

The process scheduler owns a distinct blocking-I/O pool, defaulting to two
threads. It wraps blocking calls and makes no kernel-async I/O claim. Worker
roots/continuations use three FIFO priority lanes; every eighth dispatch selects
the oldest lane head across priorities. Deferred priority policy is separate.
This API retirement does not move existing Worker filesystem workloads to I/O.

ParallelFor keeps its existing CPU policy, cancellation and stable failure
selection. The native default MinBatchSize remains the serial sentinel.
`Tasks::ParallelFor` offers Auto, Serial and ExplicitBatch; zero explicit batch
is invalid. Auto uses a 16,384-element threshold and 2,048 minimum batch, at
most Worker count plus caller chunks, and serial nesting. If checked chunk
submission fails after earlier chunks were accepted, those chunks stay owned,
are canceled/drained and are waited before captures are released. Correctness
validation of queueing is not performance-equivalence evidence.

## Owner And Module Lifetime

Owners stop producing before scope closure, then cancel/drain accepted work
while captures, callbacks and module code remain alive. Scheduler submission
linearizes under its Submit mutex; scope participation linearizes at TryAdmit.
Submission participation protects registration through publication or rollback.
Cancel racing registration is reconciled before dispatch.

Group Drain excludes external roots but a live counted `FTaskContext` may use
`LaunchChild` until the parent retires. Cancel and global scheduler closure
exclude new children. `JoinAsync` observes closed-group quiescence without
submitting a node. Pending group waits on owner/executor threads are rejected
because live parents can still acquire owner-thread dependencies. Destruction
diagnoses nonquiescence and never implicitly pumps or joins. Borrowed module
scopes retain their stronger drain authority, including retained results and
external source lifetimes.

The GameThread owns mutable Worlds, DObjects, registry/editor state and resource
publication. Workers use detached values, immutable snapshots or synchronized
mailboxes. Weak object references cross threads only as opaque identity.
Render/RHI commands retain their existing rendering-thread ownership; task
migration does not unify those command queues.

ContentBrowser and source-reference builds retain generation, navigation and
registry checks. Asset Compatibility Audit streams serial-tagged value records
and publishes its shared terminal summary through a generation/weak-lifetime
checked GameThread observer; CancelAndDrain is the mandatory fallback cleanup.
Texture, material and mesh managers retain owner polling, reservations and
GameThread result delivery. Source-image thumbnails retain visibility priority,
four decodes, two uploads per frame, the cache budget and serial checks. The
owner reaps every decode task once, including exceptions, cancellation and
stale results, before moving successful pixels to the render upload boundary.
Pending requests not yet launched remain in the owner queue.

## Shutdown And Diagnostics

Engine startup/exit placement is defined by [Runtime Lifecycle](RuntimeLifecycle.md).
Normal operation has one process scheduler lifetime. Fully stopped schedulers
may restart for isolated tests/tools; old terminal handles remain observable
but cannot become prerequisites in the new lifetime. Starting during shutdown
is rejected and scheduler instances never overlap.

`ShutdownTaskSystem(Drain)` closes submission, retains internal dispatch, pumps
deferred work without the frame budget and drains the entire graph before
uninstalling the adapter and joining pools. Cancel terminalizes queued/waiting
work and requests cooperative cancellation of running bodies, then joins them.
Worker-only owners may use ShutdownTaskScheduler. Recursive deferred pump or
shutdown from a deferred callback is rejected. Queue depth, running bodies and
nonterminal reservations are zero at successful shutdown.

Attribution uses process-lifetime low-cardinality owner/category labels with
256-owner/1,024-pair bounds and overflow counters. Explicit attribution wins;
otherwise edges inherit the primary input and child roots inherit the executing
task. Additional dependencies do not merge identity. Snapshot collection pins
the live cohort under short locks and resolves details outside the global
scheduler-lifetime mutex. Bounded aggregate counters/histograms retain no
completed-task history. Result-owner counters can outlive scheduler shutdown
without contaminating a restarted lifetime.

Diagnostic reads do not emit profiler work. EngineLoop publishes fixed
attribution plots once per frame through PublishTaskSchedulerProfilerPlots;
Tracy-disabled execution remains a no-op after bounded aggregate traversal.
See [CPU Profiling](../../Development/Build/Profiling.md).

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/TaskComposition.h`
- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Source/Runtime/Launch/Private/EngineLoop.cpp`
