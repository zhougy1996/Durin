# CPU Task System

Summary: Define accepted CPU work, typed ownership, dependencies, cancellation, waiting, and scheduler lifetime.

Modules: Core

Last reviewed: 2026-10-20

## Construction And Acceptance

`Threading/TaskComposition.h` defines the ordinary API in `Durin::Tasks`.
`LaunchTask(Executor, Options, Callable)` returns `TTask<T>` directly,
including `TTask<void>` for a void callable. `Tasks::FTask` is the public generic
completion handle, an alias of `Tasks::Private::FTaskHandle`, and can observe any
task or event without a typed result accessor. Callables accept no arguments or
`const FTaskCancellationToken&`. Value continuations may accept the token
before their input value; void continuations may accept the token alone.
`LaunchTask(Name, Callable, Options)`
is the Worker convenience form. Tasks participate in the process scheduler
lifetime. Ordinary resource owners keep
their task handles and completion events, stop their own producer admission,
then cancel/wait the accepted work before releasing captures or resource state.
There is no general task-group or group-join facade.
`LaunchTask(Executor, Options, Callable)` supports a selected executor and
records the executing parent identity for nested submissions. Package payload
reads use this form when dispatching file work to BlockingIO.
Domain queues own their admission policy. External completion tasks retain
their scheduler participation until their producers acknowledge completion.
Asset compilation uses explicit operation completion dependencies as
defined in [Derived Data Build](../Assets/DerivedDataBuild.md).
Launching a child with `LaunchTask` does not add a
parent completion dependency. Use `AddNested` when parent completion must include
that child.
Children use the same `LaunchTask` API as roots. Pass a cancellation token in
the child's options when cancellation should be shared; `AddNested` affects
completion only and does not forward cancellation. There is no context or
child-launch facade.

A valid submission while its scheduler admission is open is accepted.
Busy executors queue work. Ordinary roots, continuations, children, fan-in and
completion sources do not return admission errors or failed surrogate tasks
when scheduler thresholds are exceeded. They do not retry, wait for capacity,
or run a callable inline. Acceptance does not promise execution success.

Options carry debug name, priority, cancellation, attribution,
prerequisites, and deferred generation/coalescing identity. Ordinary callers do
not declare scheduler capture/result byte budgets. Actual domain payload limits
and producer throttling belong to resource owners; see
[Asset Compilation](../Assets/AssetCompilation.md).

Invalid dependencies/executors and submission after
closure are programming/lifetime violations. Ordinary construction uses the
always-enabled `requiref` failure path, including Shipping. Allocation failure
in scheduler construction/registration/dispatch/result storage is fatal; it is
not ordinary task failure. Exceptions from user work produce Failed tasks.

`Threading/Task.h` defines the common task handle, shared result storage, cancellation,
wait operations, lifecycle and kernel primitives. Checked admission is limited
to `Private::TryLaunchCancelableTaskWithCompletion` and
`Private::TryLaunchContinuationTask`, Core kernel tests, deliberate lifecycle
close probes, and ParallelFor's bounded infrastructure chunk submission.
These operations retain structured capacity/lifetime/prerequisite errors
and transactional rollback. They always validate construction. Business code
must use the ordinary API rather than selecting the checked policy.

## Storage And Execution Limits

Accepted waiting/ready nodes remain in scheduler lifetime tracking.
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
It reports current/peak node count, a unified estimate of scheduler-owned task
storage, current/peak pending nodes and bytes,
current/peak running bodies, and node-threshold crossings. Pending includes
external sources awaiting acknowledgement; running bodies exclude them and
include suspended parents while helping a child. Threshold transitions emit
power-of-two rate-limited warnings outside internal locks. This accounting
includes node and callable-wrapper storage, prerequisite references, and the
concrete callable, completion callback, result-state and value sizes known before
type erasure. It excludes allocator overhead and dynamic allocations behind
captures and results, and is not process RSS. `FTaskDiagnostics::EstimatedStorageBytes`
reports the same per-task estimate. No separate callable or result byte gauges,
histograms or lifetime byte callbacks are maintained. Shared prerequisite results
are not charged again as continuation payloads. Existing attribution gauges
distinguish waiting, queued, running and nonterminal nodes and report payload peaks.
Deferred diagnostics include current/peak retained queue storage, queue depth
and crossings of configured count/byte thresholds.
Terminal publication releases scheduler reservations exactly once; retained
terminal handles/results have separate lifetimes.

External producers can use `TCompletionSource<T>::Create(Options)` in the
scheduler lifetime.
Register consumers before starting external work so producer acknowledgement
does not require another submission during shutdown.

## Shared Task Results

`TTask<T>` and `FTaskEvent` derive from `Tasks::Private::FTaskHandle`, exposed
publicly as `Tasks::FTask`. All task handles are copyable and movable. Copies
share one execution node and result; the result type itself may be move-only.
The node retains successful result storage until its final reference is released,
including generic `FTask` references and external completion sources. Releasing a
typed handle does not discard a result still retained by another handle.
Ordinary dependencies accept task/event objects directly. Constructing or assigning
a generic `FTask` from an lvalue task/event copies observation; an rvalue transfers
the base handle and leaves the source invalid. Moving a complete typed task
transfers its execution and typed result references together.
Empty handles report completed and waits return `Completed` with task state
`Invalid`. They represent no pending work, not a successful typed result.

`IsCompleted()` includes Succeeded, Failed and Canceled and does not imply a
successful result. `GetFailure()` requires Failed. `GetResult()` waits for success
and returns a mutable reference to the node's result without invalidating a handle.
Callers synchronize mutation. Explicit `std::move(Task.GetResult())` moves the
stored value and leaves a moved-from value visible to every alias. There is no
separate shared task type, sharing conversion, or framework consuming accessor.
Rejected waits, failure and cancellation must be handled before result access.
`GetResultShared()` is a nonblocking nullable immutable view retaining the entire
node; it returns empty before publication or after failure/cancellation.
Void tasks compose without a value argument. Core allocates their trivial result
storage so retained void handles do not require a submitting module's destructor
code or a module result reservation.

Publication/discard precedes externally visible terminal state and dependent
release. Failed or canceled pending results are destroyed exactly once. Successful
results remain until the node is released. Captures and results are destroyed outside task, scheduler and
executor queue locks. Before a task exposes terminal state or completes a wait,
its scheduler-owned callable and completion callback have been released. This
does not release results or shared captures still owned by callers.
Executor entries retain task state rather than user callables. Execution and
cancellation claim a queued callable under the task lock; cancellation destroys
it outside the lock before publication, without waiting for executor progress.
If execution wins, cancellation remains cooperative and publication follows
body exit and callable destruction. Destructors may query task state, but must
not wait for their own task's completion. A terminal handle does not retain a
runnable callback.

## Composition

All ordinary composition returns task handles directly:

| Operation | Behavior |
| --- | --- |
| `Then(const TTask<T>&, Executor, Options, F)` | Observe a shared successful result by reference; void inputs omit the value; callbacks may take a cancellation token first |
| `Then(FTask or FTaskEvent, Executor, Options, F)` | Success edge with a completion handle and no input value |
| `ThenCompleted(const TTask<T>&, Executor, Options, F)` | Receive the task in any terminal state; inspect before accessing a result |
| `ThenAsync(const TTask<T>&, Executor, Options, F)` | Flatten a returned task by retaining its node and result without moving the value |
| `WhenAll(const vector<TTask<T>>&)` | Immutable owning result views in input order, including duplicate positions |
| `WhenAll(const tuple<TTask<Ts>...>&)` | Heterogeneous immutable owning result views; void positions view monostate |

`LaunchTask(Executor, Options, Callable, Dependency)` accepts a task, event,
or generic `FTask` directly, or a vector, array, or span of these types. The named
Worker overload accepts `LaunchTask(Name, Callable, Dependency, Options)`.
`Prerequisites(A, B, ...)` creates an owning array of `FTask` handles for mixed
typed tasks, generic handles, and events; it never consumes a result. These
overloads append their dependencies to `Options.Prerequisites`, and bind them
before the supplied collection can go out of scope. `Wait(Task)` and `Cancel(Task)`
accept task/event facades directly. `Wait(Collection)` accepts the same collections
and returns one `FTaskWaitResult` per input in input order, including invalid, failed,
or canceled entries; waiting never consumes results. Empty collections produce no
dependencies and an empty wait result.

Owners combine known work with task prerequisites, `AddNested`, and task events.
Completion of a resource's business record does not prove its worker callable
has retired: retain its task handle until terminal publication. Pipeline drain
tasks form a dependency chain so the last handle covers every earlier drain;
Material compilation retains workers independently of its result mailbox, and
CookedMesh completion records retire only after their worker task is terminal.
The RHI device queue captures a weak scheduler identity and rejects reuse after
a scheduler restart; this read-only binding creates no task node.

Then rejects task-returning callbacks; ThenAsync is explicit flattening.
Empty fan-in without additional prerequisites is immediately successful in
the current scheduler lifetime. Additional
prerequisites retain their dependency and executor requirements.
Fan-in waits for all inputs; failure precedes cancellation and the lowest
failing input index determines the framework failure. Shared observers are
independently cancelable. A canceled observing edge may never invoke its
callback, so mandatory cleanup belongs to the owner lifecycle boundary.

ThenAsync binds and cycle-checks its dynamic dependency before clearing unknown
execution requirements. Canceling the outer observer does not implicitly cancel
the shared inner task. Callers can explicitly share a cancellation token when
that behavior is required. Caller prerequisites bind on the runner before returning
and their borrowed span is not retained in the later observer. An inner user exception remains
execution failure, not a scheduler admission result.
Copying and flattening preserve the original failure reason, code, dependency
error and input identity. A flattening callback exception remains a callback
failure; it is not relabeled as a failed prerequisite.

`TCompletionSource<T>::Create` returns a counted external producer directly.
It does not occupy a worker. `TakeTask()` obtains its typed handle once; that
handle can subsequently be copied freely.
`TrySetValue`, `TrySetFailure` and `TrySetCanceled` settle once and report late
publication attempts. Last unresolved source release fails with abandonment.
Cancellation remains requested until the external producer acknowledges it.
Preallocated terminal hooks run outside internal locks without scheduling a
new task. Source known-requirement construction also supports internal
composition such as empty fan-in and dynamically bound inner tasks.

`TCompletionSource<T>::CreateDetached()` reuses the same node, result storage,
terminal publication and abandonment contract without scheduler admission. It
creates no callable or execution queue and remains valid across scheduler startup
and shutdown. Use it for bounded owner-managed data completion, such as ToolService;
it does not register pending work for scheduler drain. Owners must stop/drain
producers themselves. `MakeReadyTask(Value)` returns its already settled shared
result. Detached nodes are not scheduler prerequisites or nested execution work.
Erased-handle Cancel returns false for these observation-only nodes; only their
producer can acknowledge cancellation. Domain cancellation uses its own token.

`TTask::Subscribe(Callback)` returns a move-only `FTaskSubscription`.
Notification runs once, possibly inline, on the completing/subscribing thread.
It runs after result storage publication, before task terminal publication may
become visible: only mark work runnable, then read readiness at the owner boundary.
`FTaskReadiness` plus `ObserveReady` supplies a Core-owned data/code marker for
host observers that must safely detach from late producers. Generic callback owners
still synchronize in-flight callbacks before unloading their own code.
Dropping/Resetting observation releases its captures and does not cancel domain
work; an already in-flight notification may finish. Notification exceptions are
contained. `WithLifetime(SharedOwner)` attaches ownership to the shared node and
all its handles and result aliases; this supports accounting that outlives receipt eviction.

## Dependencies, Waiting And CPU Policy

`Tasks::AddNested(Task)` adds a completion dependency to the currently executing
task. It must be called from a running, non-independent task body, accepts typed
tasks, events, or a generic `Tasks::FTask`, and rejects cycles and foreign
schedulers. The parent publishes its result and terminal state only after its
body and all registered children finish. Its worker is available after the body
returns. Failure in the parent still waits for child retirement; cancellation
while awaiting children remains a request until they complete. Child failure or
cancellation does not change the parent's outcome, and this edge does not forward
cancellation. Owners request cancellation explicitly.

`Tasks::FTaskEvent(DebugName)` is a shared, explicit one-shot signal with no worker
body or result storage. Events can be supplied directly as execution prerequisites
or nested completion dependencies. A generic `Tasks::FTask` can retain
observation separately. `AddPrerequisites` accepts task/event facades
or vectors, arrays, and spans of task/event or generic handles
before `Trigger()`; the event completes only after it is triggered and every
prerequisite finishes, regardless of prerequisite outcomes. Copies share one
signal, repeated `Trigger()` calls are harmless, and the last handle must be triggered before
release. Cancellation does not trigger an event. External producers must signal
after their callbacks and captures have retired. Events default to unknown external
execution requirements. Passing `false` as the constructor's second argument
declares that signaling never requires GameThread or unknown external progress;
transitive prerequisite wait restrictions still apply.

Events participate in scheduler lifetime. AddNested expresses an explicit parent
completion dependency; events do not collect work on behalf of an owner.

Prerequisites must be valid in the same scheduler lifetime. Success dependencies
release once after every prerequisite succeeds; failure/cancellation skips the
body and records the blocking relationship. Completion edges observe all
terminal states. Registration racing terminal publication cannot lose or
duplicate dependent release. A task id is process-unique and nonzero; parent
and prerequisite identities are distinct.

Execution prerequisites and nested completion have separate phase counters.
Retraction visits execution backlinks only while prerequisites remain and
nested backlinks only while children remain. Persistent weak graph metadata
continues to support cycle, authority and diagnostic checks. An unsuccessful
attempt leaves unfinished links available to other waiters and later progress.
Dynamic inner binding is one-shot; a second binding returns
`InvalidExecutionContract` before mutation or cancellation forwarding.

The scheduler publishes progress after ready-node insertion, dynamic binding,
nested registration, callable retirement and full terminal publication. Its
progress serial and condition variable use a separate mutex; observers prepare
before inspecting work and wait only while the serial is unchanged. Notification
does not imply completion or permit bypassing callable/terminal-hook retirement.

`WaitTask` returns wait status separately from task state. Completed means either
an empty handle or a terminal state with finished terminal publication.
SelfWait, DependencyCycle, UnsupportedThread and TimedOut do not prove completion.
`Task.Wait(TimeoutSeconds)`, `Tasks::Wait(TaskOrCollection, TimeoutSeconds)`,
`WaitTask` and `WaitAll` accept a timeout in seconds. Omitting it waits indefinitely.
A collection shares one steady-clock deadline across all inputs and preserves one
result per input, including completed or invalid entries after the deadline.
Timeout does not request cancellation or consume results. Nonpositive timeouts
only inspect completion, without executing user code or requiring blocking authority; positive
timeouts stop starting work after the deadline. Executed callables cannot be
interrupted, so a helping wait may exceed its timeout. NaN is treated as zero.

GameThread rejects unknown or transitive deferred requirements;
executing tasks reject unknown external requirements. RenderingThread rejects
ordinary task waits. It may wait for an explicit independent CPU leaf
created with `Tasks::LaunchIndependentTask(Name, Callable, Options)`. This entry
uses Worker execution and ordinary scheduler/owner lifetime. It rejects launch
prerequisites, external completion and non-Worker targets. An independent leaf
cannot submit children, acquire dynamic task dependencies, block on nonterminal
tasks. Contract violations reject checked-kernel
admission with `InvalidExecutionContract`; ordinary construction treats them as
programming violations. Completed values remain readable without blocking.
The callable must not depend on owner-thread, RHI, I/O or other external progress;
these ordinary C++ effects remain a caller contract. This explicit declaration
permits render preparation/recording joins without relaxing general wait policy.
Cancellation and exceptions still publish the normal terminal state, and result
access still requires success. Waits do not pump GameThread callbacks.
Executing tasks help only the waited task and its transitive execution, nested,
and dynamic dependencies. The task state is the queue execution node; scheduler
dispatch adds no callback wrapper or separate queue-node allocation. Ordinary
pool callbacks use an adapter. Retraction pins the task node and detaches it in
constant time under the owning pool's lock;
there is no ID lookup or queue scan. The priority lanes use owning forward links
and non-owning backward links, with iterative teardown. Normal dispatch and
retraction share the same removal path; stale and wrong-pool handles fail. Generic handles
do not retain callable captures, and callable/discard storage is released outside
the queue lock. Retraction never runs unrelated queue entries. Graph traversal is iterative rather than recursive. Kernel Worker
helping runs CPU work; I/O helping stays within the blocking-I/O authority,
including a single-thread I/O parent waiting for its child. External callers
retain blocking-only behavior and executor-affinity restrictions remain in force.

A retraction pass visits prerequisites before attempting the node, then inspects
dynamic/nested backlinks again to include work added by its body. The local
postorder stack, dependency scratch vector and task-ID deduplication buckets are
reused during one wait. Pins are released between passes; there is no graph
revision cache or recursion-depth limit. An expired deadline starts no work.
A started body is not preempted.

`Task.TryRetractAndExecute()` explicitly attempts the target and its execution,
nested and dynamic dependencies on the caller, without sleeping for work owned
by another thread. It returns whether the target is complete; an empty handle
returns true. Unlike ordinary external waits, external callers may explicitly
execute eligible Worker tasks. The same thread and dependency restrictions apply;
BlockingIO execution still requires BlockingIO authority and deferred callbacks
are never pumped. Callables may run arbitrarily long, and unrelated queued tasks
are not executed. A false return does not cancel or fail the target.

Waiting prepares the scheduler progress serial before inspection, rechecks
terminal state and deadline after retraction, then sleeps only while that serial
is unchanged. Ready insertion and newly registered dependencies wake the waiter
as well as terminal publication. The former worker 1 ms polling slice is removed;
the long-wait diagnostic has one deadline of its own. The scheduler notification
is shared, so unrelated progress can wake a waiter, but cannot authorize execution
of unrelated work. This is a correctness refactor, not a timing qualification.

Callable ownership transfers only after admission succeeds, and the body is
retired before terminal publication. Typed result storage remains separately
owned so unique reservations and shared views can outlive execution without
changing their claims or retaining callback captures. Queue-node membership is
one-shot; an executed or discarded node cannot be admitted again.

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

Owners stop submission, cancel or wait their own accepted work, and release
callbacks, results and resource state before shutdown completes. The Task
kernel does not collect module tasks or track module retirement. Module
shutdown owns this policy; see [module retirement](ModularFeaturesAndModuleRetirement.md#asynchronous-operation-boundary).

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
The deferred adapter retains internal dispatch until the accepted graph is
terminal, including completion publications from external producers and capture
destructors during cancellation. Only then is its queue closed and uninstalled.
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

Routine task acceptance and queued-work start/finish do not emit text logs.
Use task snapshots, aggregate counters and Tracy execution zones to inspect
normal activity. Text logs retain executor lifecycle summaries, rejected/invalid
operations, exceptions, overload transitions and long waits.

Diagnostic reads do not emit profiler work. EngineLoop publishes fixed
attribution plots once per frame through PublishTaskSchedulerProfilerPlots;
Tracy-disabled execution remains a no-op after bounded aggregate traversal.
See [CPU Profiling](../../Development/Build/Profiling.md).

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/TaskComposition.h`
- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Source/Runtime/Launch/Private/EngineLoop.cpp`
