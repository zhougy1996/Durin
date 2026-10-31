# CPU Task System

Summary: Define accepted CPU work, typed ownership, dependencies, cancellation, waiting, and scheduler lifetime.

Modules: Core

Last reviewed: 2026-10-31

## Construction And Acceptance

`Threading/Task.h` defines the ordinary API in `Durin::Tasks`.
`LaunchTask(Executor, Options, Callable)` returns `TTask<T>` directly,
including `TTask<void>` for a void callable. `Tasks::FTask` is the public generic
completion handle, an alias of `Tasks::Private::FTaskHandle`, and can observe any
task or event without a typed result accessor. Callables accept no arguments or
`const FTaskCancellationToken&`. Dependent bodies capture the input handles
they need and inspect their states before accessing results.
`LaunchTask(Name, Callable, Options)`
is the Worker convenience form. Tasks participate in the process scheduler
lifetime. Ordinary resource owners keep
their task handles and completion events, stop their own producer admission,
then cancel/wait the accepted work before releasing captures or resource state.
There is no general task-group or group-join facade.
`LaunchTask(Executor, Options, Callable)` supports a selected executor and
records the executing parent identity for nested submissions. Package payload
reads use this form when dispatching file work to BlockingIO.
Domain queues own their admission policy. Accepted producer and publication tasks
retain their scheduler participation through ordinary terminal publication.
Asset compilation uses explicit operation completion dependencies as
defined in [Derived Data Build](../Assets/Build/DerivedDataBuild.md).
Launching a child with `LaunchTask` does not add a
parent completion dependency. Use `AddNested` when parent completion must include
that child.
Children use the same `LaunchTask` API as roots. Pass a cancellation token in
the child's options when cancellation should be shared; `AddNested` affects
completion only and does not forward cancellation. There is no context or
child-launch facade.

A valid submission while its scheduler admission is open is accepted.
Busy executors queue work. Ordinary roots, dependent tasks and children do not
return admission errors or failed surrogate tasks
when scheduler thresholds are exceeded. They do not retry, wait for capacity,
or run a callable inline. Acceptance does not promise execution success.

Options carry debug name, priority, cancellation, attribution,
and prerequisites. Ordinary callers do
not declare scheduler capture/result byte budgets. Actual domain payload limits
and producer throttling belong to resource owners; see
[Asset Compilation](../Assets/Build/AssetCompilation.md).

Invalid dependencies/executors and submission after
closure are programming/lifetime violations. Ordinary construction uses the
always-enabled `requiref` failure path, including Shipping. Allocation failure
in scheduler construction/registration/dispatch/result storage is fatal; it is
not ordinary task failure. Exceptions from user work produce Failed tasks.

`Threading/TaskRuntime.h` defines the common task handle, shared result storage,
cancellation, wait operations, lifecycle and kernel primitives. The ordinary API
includes this lower-level header in one direction. Internal erased submissions
also return handles directly and use the same fatal
construction contract. There is no checked capacity-admission API or admission
result wrapper. Terminal callbacks remain available internally for owners that
must settle a domain operation before the task publishes completion.

## Storage And Execution Limits

Accepted waiting/ready nodes remain in scheduler lifetime tracking.
Worker ready queues and the deferred queue own their callable storage until
execution or an explicit producer outcome. Cancellation requests do not release
waiting or ready nodes. There is no untracked overflow queue.
Pending storage can grow with accepted work; this is not an unlimited-memory
guarantee. The scheduler does not inspect allocations behind containers or
pointers captured by a callable.

`TaskWarningThreshold` defaults to 16,384 and `BlockingIOWarningThreshold` to 128.
These are diagnostic thresholds only; neither rejects nor delays tasks. CPU and
blocking-I/O execution uses the installed thread pools. Explicit limits on a
selected set of Worker tasks belong to `FTaskConcurrencyLimiter` below.

The deferred executor retains its frame time/item budget, priorities and FIFO.
Core does not merge or replace queued tasks by key. Request freshness and request
coalescing belong to the domain owner: use serial checks in publication bodies,
or retain the latest payload behind one owner-managed publication task when
queue reduction is needed. Superseded remains a domain result/status rather than
a Core terminal reason. Core provides no task generation source/token or
automatic generation-expiry outcome. `QueuedEntriesWarningThreshold` (1,024),
`QueuedBytesWarningThreshold` (8 MiB), and `EntryBytesWarningThreshold` (1 MiB)
only report overload. Accepted entries remain queued beyond them. Storage is
estimated consistently for every entry; callers do not declare admission budgets.
Domain payload budgets remain with their resource owners. Deferred shutdown and
selected module drain see the entire accepted queue.

`FTaskSchedulerDiagnostics::ExecutorStorage` is indexed by native `ETaskTarget`.
It reports current/peak node count, a unified estimate of scheduler-owned task
storage, current/peak pending nodes and bytes,
current/peak running bodies, and node-threshold crossings. Pending includes
admitted tasks awaiting prerequisites or dispatch; events alone do not register
scheduler work. Running bodies include suspended parents while helping a child. Threshold transitions emit
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
Terminal publication removes scheduler tracking exactly once; retained
terminal handles/results have separate lifetimes.

External integration uses Core-private registered result nodes for composition
and audited typed save adapters. Data-only delivery uses standalone futures;
result-free ordering uses events. Register accepted consumers and producer
retirement obligations before external work starts so shutdown requires no new
submission. Event/future creation alone does not register work for drain.

## Concurrency Limiter

`Threading/TaskConcurrencyLimiter.h` provides `Tasks::FTaskConcurrencyLimiter`.
Construct it with `MaxConcurrency` and an optional priority, then call
`Push(Name, Callable)` and `Wait(TimeoutSeconds)`. The callable receives a slot
index in `[0, MaxConcurrency)`, unique among active tasks. It may additionally
accept a cancellation token; pass that token as the optional third `Push`
argument. Cancellation remains cooperative and exceptions use normal task failure.

Each push creates one scheduler-tracked void task. Ready tasks wait in the
limiter's FIFO without occupying a worker or allocating another completion task.
A slot transfers to the next ready task when the callable returns or throws,
before capture retirement and nested completion. This matches the body-return
handoff in UE 5.8.0 (CL 53629095), `Tasks/TaskConcurrencyLimiter.h` and
`Private/Tasks/TaskConcurrencyLimiter.cpp`. Normal task completion and scheduler
shutdown still wait for capture retirement and nested work. The limiter bounds
callable concurrency, not queue length or memory. Captures and nested work must
not keep using a slot's scratch storage after the callable returns. A callable
must not wait for work queued behind its own slot.

Stop producers before `Wait`. A blocking wait attempts execution across all
scheduled slots before sleeping on shared work progress; it does not join nested
completion or capture retirement. UE 5.8's completion event is one-shot, but its
`Wait` drains current work before checking that event, even after an earlier
successful wait. Durin likewise checks current work rather than latching success.
Prefer a fresh limiter for another batch, as UE's public contract recommends.
Durin retains deadline checks and notification-based sleeping instead of UE 5.8's
unbounded retraction scan before the timed event wait. Empty and zero-timeout
waits allocate no waiting state. Own-limiter waits, independent CPU contexts,
and rendering-thread waits return false while callable work remains.
Timeouts do not interrupt an executing callable. Destroying the
limiter does not cancel accepted work: bodies retain its state through capture
retirement, and
scheduler shutdown still drains all pending tasks. Completed tasks are not
retained by the limiter.

## Shared Task Results

`TTask<T>` and `FTaskEvent` derive from `Tasks::Private::FTaskHandle`, exposed
publicly as `Tasks::FTask`. All task handles are copyable and movable. Copies
share one execution node and result; the result type itself may be move-only.
The node retains successful result storage until its final reference is released,
including generic `FTask` references. Releasing a
typed handle does not discard a result still retained by another handle.
Non-void results occupy aligned storage in their result state without a separate
value allocation. Task nodes store their move-only callback directly; execution
and capture retirement happen outside the node mutex.
Result storage, the task body, the completion callback and the node kind are
bound during construction for executable tasks and events. The node's result-storage binding and kind cannot change afterward.
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
Void tasks compose without a value argument and allocate no result state or result
publication callback. They expose no `GetResult()` or `GetResultShared()`; use
`Wait()` and inspect its outcome. Retained void handles require neither a submitting module's result destructor
code nor a module result reservation. Nested completion does not alias child
result storage; retain a child handle or result view explicitly when the value
must outlive its producer.

Publication/discard precedes externally visible terminal state and dependent
release. Failed or canceled pending results are destroyed exactly once. Successful
results remain until the node is released. Captures and results are destroyed outside task, scheduler and
executor queue locks. Before a task exposes terminal state or completes a wait,
its scheduler-owned callable and completion callback have been released. This
does not release results or shared captures still owned by callers.
Executor entries retain task state rather than user callables. Cancellation is
cooperative: requesting it sets a flag without skipping a body, releasing a
dependency or changing an outcome. Tasks still wait for prerequisites and execute;
bodies may inspect their token and return an appropriate result. Ignoring a
request permits normal successful completion and result retention. Shared tokens
read one atomic flag without registering nodes. `FTask::IsCancellationRequested()`
queries the task's own request or its shared token independently of completion.
Publication follows body exit and callable destruction. Destructors may query task state, but must
not wait for their own task's completion. A terminal handle does not retain a
runnable callback.

## Composition

Task graphs use ordinary scheduled tasks and explicit completion dependencies,
following [UE task composition](https://dev.epicgames.com/documentation/en-us/unreal-engine/tasks-system-references-in-unreal-engine).
There are no Tasks Then, ThenCompleted or WhenAll helpers, or private continuation
submission paths. Future continuation API parity is a separate concern.

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

For known work, launch the next task with explicit prerequisites, capture any
input handles it needs, and return its own result. The callable runs after all
inputs complete, including failure and cancellation. Check an input's state
before GetResult; required cleanup must handle every terminal state. To gather
results, launch an ordinary task behind the input collection and explicitly
construct the domain result. No framework failure-priority or result-aggregation
policy is implied. Empty collections follow ordinary task scheduling rules.

For children selected inside a running body, call `AddNested(Child)` to extend
parent completion without blocking its worker. Nested completion does not
forward child results, failure or cancellation. Retain the child handle or an
owning domain result explicitly; share a cancellation token only when
cancellation should be shared. There is no automatic task-flattening API.

Ordinary submission ignores empty prerequisite handles and normalizes repeated
nodes once, preserving first-occurrence diagnostic order. Application input
collections and results retain their own order and duplicate positions.
Prerequisites do not supply attribution: set Options.Attribution explicitly when
a dependent must retain a producer's owner/category. Otherwise ordinary launch
inherits the current executing task's attribution, where available.

Copying task handles preserves the native terminal reason and diagnostic text.
GetFailure returns that reason from the node's outcome, without separate injected
failure storage or dependency-propagation metadata. A dependent callable exception
remains its own callback failure; it is not relabeled as a failed prerequisite.
Internal terminal hooks can read the native outcome before public completion, while
GetFailure still requires Failed. Canceling a dependent does not cancel its inputs
or suppress its body; mandatory cleanup remains an owner lifecycle responsibility.

Task results are produced by ordinary task bodies; there is no externally
settled task-result source or external scheduler node. External data delivery
uses independent promises/futures, while task graphs use `FTaskEvent` for readiness
and real producer tasks or explicit domain joins for retirement. An event alone
does not register work for scheduler drain. Terminal hooks dispatch without new
submission; readiness alone never proves producer/capture retirement.

There is no public completion source or detached data-only task constructor.
Ready data uses MakeReadyFuture; scheduled work that returns a constant is an
ordinary LaunchTask with normal scheduler lifetime and dependency rules. No
observation-only MakeReadyTask helper is exposed.

`TTask::Subscribe(Callback)` returns a move-only `FTaskSubscription`.
Notification runs once, possibly inline, on the completing/subscribing thread.
It runs after result storage publication, before task terminal publication may
become visible: only mark work runnable, then read readiness at the owner boundary.
Callback owners synchronize in-flight callbacks before unloading their own code.
Dropping/Resetting observation releases its captures and does not cancel domain
work; an already in-flight notification may finish. Notification exceptions are
contained. Task handles and `GetResultShared()` views retain the node's own result
storage. Additional ownership belongs in the task result or the domain owner;
tasks expose no readiness marker or arbitrary lifetime-pin attachment API.

## Standalone Futures

`Threading/Future.h` supplies `Durin::TPromise<T>`, move-only `TFuture<T>` and
copyable `TSharedFuture<T>`. These use independent result storage, mutexes and
condition variables: no task node, executor, admission, dependency conversion,
helping, or GameThread pumping. Unresolved futures do not delay scheduler drain;
they remain usable before startup, after shutdown, and across restarts.

GetFuture retrieves one consumer. Share transfers it to a shared consumer;
Consume transfers the value and invalidates an unshared consumer. Get waits and
returns a repeatable const reference; unshared GetMutable permits caller-managed
mutable access. Void supports Get/Consume without a value. Ready construction
uses MakeReadyFuture, explicitly Share it for copyable delivery. Invalid access
throws logic_error. Get on failed/canceled completion throws, rethrowing a stored
producer exception when available. Failed state distinguishes Exception and
Abandoned; domain errors remain ordinary successful values.

TrySetValue/TrySetFailure/TrySetCanceled settle once; checked setters reject
repeated publication. Promise destruction or replacement fails an unresolved
state with Abandoned and wakes waiters. Cancellation acknowledgement is a
producer outcome, separate from any domain cancellation request. WaitFor accepts
chrono durations and returns readiness. A wait never progresses its producer;
domain APIs must prohibit waits on threads required for that producer's progress.

Subscribe returns FFutureSubscription. Notifications run outside state locks,
possibly inline on subscription or publication; exceptions are contained. Reset
or destruction prevents unpinned callbacks, while an in-flight call may finish.
Owners must synchronize in-flight provider callbacks before unloading code.
FFutureReadiness/ObserveReady use Core-owned marker code for detachable host
observation. Reentrant reads/subscriptions are supported; notification callbacks
must not wait for another callback in the same publication pass.

Shared GetResultShared is a nonblocking immutable owning view, empty until
success. WithLifetime attaches a pin to the state, retained by all consumer
handles and result views through result destruction. Pins do not automatically
install a module-unload protocol. A ready result is not a producer retirement
join; domains retain their accepted producer task, command or explicit owner
drain separately. Futures are not a second task scheduler and expose no implicit
continuations.

## Serial Pipes

`Threading/TaskPipe.h` provides noncopyable, nonmovable `Tasks::FPipe`.
`Launch(Name, Callable[, Prerequisites][, OptionsOrPriority])` returns the same
typed task and preserves ordinary result, exception and cooperative cancellation
semantics. Pipes currently dispatch Worker tasks with the existing priority set.
Construction allocates no task or queue; the pipe keeps a weak tail and a count
of all outstanding tasks, including tasks still waiting on prerequisites.

As in [UE pipes](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FPipe),
prerequisites resolve **before** tasks enter the serial chain. An earlier launch
with unmet prerequisites cannot block later ready work. Ready tasks execute in
pipe-entry order regardless of priority; sequential prerequisite-free launches
therefore execute FIFO. Concurrent launches and prerequisite completion races
have no guaranteed order. Different pipes can execute concurrently.
Each task waits for its pipe predecessor's full completion, including nested
tasks and result publication. Pipe edges participate in dependency retraction,
wait restrictions and cycle detection. Waiting for or nesting a successor from
the same pipe creates a dependency cycle; a cycle discovered during delayed
pipe entry is a programming-contract violation.

`IsInContext()` identifies the currently executing task's pipe, including during
wait retraction; `HasWork()` includes all accepted unfinished work.
`WaitUntilEmpty(TimeoutSeconds)` is a blocking drain without dependency helping
or executor pumping. Stop producers before draining and ensure prerequisites can
progress independently. It returns false on timeout or when outstanding work
cannot be joined from the current pipe, an independent CPU leaf, or RenderingThread.
The pipe must outlive every task it launches; destruction checks that it is empty.
Completed task handles and result aliases may outlive the pipe.

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
signal and repeated `Trigger()` calls are harmless, including on a moved-from empty
event. Trigger state, synchronization and pending-completion ownership belong to
the node; the event facade has only the common handle. The node must be completed
before its final reference is released. Generic handles retain this lifetime check
after the last event facade is released. Cancellation does not trigger an event. External producers must signal
after their callbacks and captures have retired. Events default to unknown external
execution requirements. Passing `false` as the constructor's second argument
declares that signaling never requires GameThread or unknown external progress;
transitive prerequisite wait restrictions still apply.

Events have no scheduler owner or admission reservation. They can be created before
startup, triggered after shutdown, and used across scheduler restarts. Drain and
Cancel do not settle an event; an accepted task waiting on one still belongs to
the scheduler and Drain waits for its signal. A triggered event retains its pending
completion until all prerequisites finish, even after its event facades are released.
Event waits and explicit retraction can execute eligible scheduled prerequisites.
AddNested expresses an explicit parent completion dependency; events do not
collect work on behalf of an owner.

Empty prerequisite handles are ignored, as already-completed dependencies.
Nonempty scheduled task prerequisites must belong to the same scheduler lifetime;
scheduler-independent events can connect tasks and events without that restriction.
Standalone futures are not task prerequisites or nested work. Ordinary launch
prerequisites are completion dependencies: the body becomes runnable once every
prerequisite finishes, including Failed and Canceled outcomes. They do not
forward failure or cancellation. Consumers requiring successful admission or
usable predecessor data must check that outcome explicitly in the task body.
The kernel supports completion dependencies only; domain code owns success checks.
Registration racing node closure either joins the open dependent list or observes
the closed outcome; it never waits for terminal callbacks and cannot lose or
duplicate dependent release. A task id is process-unique and nonzero; parent
and prerequisite identities are distinct.

Nodes move through Open, BodyFinished, Closing and Closed phases independently
of their execution outcome. BodyFinished records success, failure or acknowledged
producer cancellation after callable retirement; nested work can still keep the
node open. Ordinary tasks, events and external producers all use
the same Close path once the body has finished and no nested work remains.
Close publishes result storage and retires terminal callbacks before atomically
closing callback/dependent registration and exposing the terminal outcome.
Public task state remains Waiting, Queued or Running until that point; bound
result-storage metadata is independent of completion. There is no hidden raw
terminal state or separate publication-finished flag.
Synchronous close propagation is limited to 128 levels. Deeper closures retain
their nodes in a thread-local worklist, drained iteratively by the outermost close
on that same thread. This covers nested parent notifications and terminal-hook
cascades without requiring scheduler work or unbounded native recursion.

Execution prerequisites and nested completion have separate phase counters.
Execution and nested dependencies share one phase-aware edge collection.
Each edge stores its stable task identity, weak backlink and optional owning pin;
there are no parallel graph and lifetime arrays. Execution edges occupy a fixed
prefix, while nested edges occupy the completion suffix. Graph walks,
retraction and executor checks use the same backlink view. Closing trims completion
edges while preserving execution identities for diagnostics.
One subsequent list carries both execution-ready and nested-complete notifications.
Nested registration reserves both sides before publishing an edge and honors
slots already reserved by ordinary task admission.
Unfinished prerequisites hold their nodes and results alive after outside handles
are released. Already-completed inputs do not acquire persistent dependency pins.
Execution pins are released before the dependent body starts. Nested pins are
released when the parent closes; cancellation requests retain dependency pins.
Closing makes completion visible before retiring those pins, allowing child-result
destructors to observe their parent's completed state.
A retraction attempt consumes the corresponding execution/nested pins as its
local traversal takes ownership, even if the target cannot finish in that pass.
Pins are detached under the node lock and destroyed outside it, so result
destructors may reenter scheduler diagnostics without taking a held node lock.
Retraction visits execution backlinks only while prerequisites remain and
nested backlinks only while children remain. Persistent weak graph metadata
continues to support cycle, authority and diagnostic checks. An unsuccessful
attempt leaves unfinished links available to other waiters and later progress.

Nodes notify their waiters after ready-node insertion, nested registration and
full terminal publication. Each blocking wait owns a signal;
the target and dependencies inspected during retraction register weak observers
of that signal. This lets later nested readiness repeat retraction
without broadcasting unrelated scheduler activity. Observers expire when the
wait returns and retain neither task nodes nor schedulers. Notification does not
imply completion or bypass callable/terminal-hook retirement.

`WaitTask` returns wait status separately from task state. Completed means either
an empty handle or a closed node with a terminal outcome.
SelfWait, DependencyCycle, UnsupportedThread and TimedOut do not prove completion.
`Task.Wait(TimeoutSeconds)`, `Tasks::Wait(TaskOrCollection, TimeoutSeconds)`,
`WaitTask` and `WaitAll` accept a timeout in seconds. Omitting it waits indefinitely.
A collection shares one steady-clock deadline across all inputs and preserves one
result per input, including completed or invalid entries after the deadline.
It attempts progress across all eligible inputs before blocking on a shared signal;
one unfinished input does not prevent retraction of later inputs. Rejected waits
remain individual outcomes and do not stop progress on other inputs.
Timeout does not request cancellation or consume results. Nonpositive timeouts
only inspect completion, without executing user code or requiring blocking authority; positive
timeouts stop starting work after the deadline. Executed callables cannot be
interrupted, so a helping wait may exceed its timeout. NaN is treated as zero.

GameThread rejects unknown or transitive deferred requirements;
executing tasks reject unknown external requirements. RenderingThread rejects
ordinary task waits. It may wait for an explicit independent CPU leaf
created with `Tasks::LaunchIndependentTask(Name, Callable, Options)`. This entry
uses Worker execution and ordinary scheduler/owner lifetime. It rejects launch
prerequisites and non-Worker targets. An independent leaf
cannot submit children, acquire nested completion dependencies or block on
nonterminal tasks. Construction contract violations are fatal and report
`InvalidExecutionContract`. Completed values remain readable without blocking.
The callable must not depend on owner-thread, RHI, I/O or other external progress;
these ordinary C++ effects remain a caller contract. This explicit declaration
permits render preparation/recording joins without relaxing general wait policy.
Cancellation and exceptions still publish the normal terminal state, and result
access still requires success. Waits do not pump GameThread callbacks.
Waiting callers, including ordinary threads, help only the waited task and its
transitive execution and nested dependencies. The task state is the queue execution node; scheduler
dispatch adds no callback wrapper or separate queue-node allocation. Ordinary
pool callbacks use an adapter. Retraction pins the task node and detaches it in
constant time under the owning pool's lock;
there is no ID lookup or queue scan. The priority lanes use owning forward links
and non-owning backward links, with iterative teardown. Normal dispatch and
retraction share the same removal path; stale and wrong-pool handles fail. Generic handles
do not retain callable captures, and callable/discard storage is released outside
the queue lock. Retraction never runs unrelated queue entries. Graph traversal is iterative rather than recursive. Kernel Worker
helping runs CPU work; I/O helping stays within the blocking-I/O authority,
including a single-thread I/O parent waiting for its child. Ordinary callers can
execute eligible Worker bodies on their own thread; they do not gain BlockingIO
authority or bypass RenderingThread and GameThread wait restrictions. Consumers
must release locks needed by the target graph before waiting and must not assume
that Worker bodies always run on a pool thread.
Wait rechecks authority after retraction, before sleeping, because executed bodies
can introduce new deferred or external dependencies.

A retraction pass visits prerequisites before attempting the node, then inspects
nested backlinks again to include work added by its body. The local
postorder stack, dependency scratch vector and open-addressed task-ID table are
reused during one wait. Cycle and executor-authority checks share one inspection
with separate reachability flags for closed ancestors. Both checks still run
before retraction and before sleeping; no graph result is cached across passes.
Pins are released between passes; there is no graph
revision cache or recursion-depth limit. An expired deadline starts no work.
A started body is not preempted.

`Task.TryRetractAndExecute()` explicitly attempts the target and its execution
and nested dependencies on the caller, without sleeping for work owned
by another thread. It returns whether the target is complete; an empty handle
returns true. It uses the same eligible Worker execution path as ordinary waits,
without waiting for progress. The same thread and dependency restrictions apply;
BlockingIO execution still requires BlockingIO authority and deferred callbacks
are never pumped. Callables may run arbitrarily long, and unrelated queued tasks
are not executed. A false return does not cancel or fail the target.

Waiting prepares its signal serial before inspection, rechecks
terminal state and deadline after retraction, then sleeps only while that serial
is unchanged. Ready insertion and newly registered dependencies wake the waiter
as well as terminal publication. Dependency observers register before inspecting
each node, preventing a readiness change between inspection and sleep from being
lost. Events use the same node notifications and keep no waiting-scheduler list.
Waiting uses no worker polling slice; the long-wait diagnostic has one deadline
of its own. Scheduler quiescence notification remains separate for shutdown.

Node construction and prerequisite-slot reservation run outside the scheduler
lock, under the in-flight submission lifetime guard. Construction checks closure,
then commits registry entries and accounting under the lock.
Allocation failures are fatal. Preparation rollback releases prerequisite slots
and payloads outside the lock; registry rollback restores tracking. Dependency
registration follows the commit, then one activation enables dispatch. Standalone completions
and events are constructed active without scheduler admission. The body is retired
before terminal publication. Task handles and shared result views retain the
node's result storage beyond execution without retaining retired callback
captures. Queue-node membership is
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
most Worker count plus caller chunks, and serial nesting. Chunks use ordinary
submission with no capacity rejection or partial-admission fallback. All chunks
are joined before captures are released. The single-chunk path runs before
allocating parallel shared state or an internal cancellation source, as in UE 5.8.
Ordinary and cancelable bodies share the implementation without an extra erased
callback wrapper; parallel state owns the body directly. Serial execution retains
cancellation checks, exception precedence, diagnostic ranges and capture retirement.
Correctness
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
and publishes its shared terminal summary through a serial/admission/weak-lifetime
checked GameThread task. A stale or closed publication returns without updating
the model and completes normally; CancelAndDrain still owns worker retirement.
Texture, material and mesh managers retain owner polling, reservations and
GameThread result delivery. Source-image thumbnails retain visibility priority,
four decodes, two uploads per frame, the cache budget and serial checks. The
owner reaps every decode task once, including exceptions, cancellation and
stale results, before moving successful pixels to the render upload boundary.
Pending requests not yet launched remain in the owner queue.

## Shutdown And Diagnostics

Engine startup/exit placement is defined by [Runtime Lifecycle](../Application/RuntimeLifecycle.md).
Normal operation has one process scheduler lifetime. Fully stopped schedulers
may restart for isolated tests/tools; old terminal handles remain observable
but cannot become direct task prerequisites in the new lifetime. Events remain
usable across lifetimes. Starting during shutdown
is rejected and scheduler instances never overlap.

`ShutdownTaskSystem(Drain)` closes submission, retains internal dispatch, pumps
deferred work without the frame budget and drains the entire graph before
uninstalling the adapter and joining pools. Cancel first requests cooperative
cancellation of all accepted nodes, then performs the same drain. Pending external
signals must still arrive, queued bodies still execute, and deferred work must
still be pumped on GameThread. Executor rejection and callable exceptions are
execution failures with explicit reasons, not cancellation requests. Business
publication bodies that ignore obsolete requests complete normally. Owners still
reap every accepted producer and publication, including failed or skipped work.
The deferred adapter retains internal dispatch until the accepted graph is
terminal, including completion publications from external producers and capture
destructors during shutdown. Only then is its queue closed and uninstalled.
Worker-only owners may use ShutdownTaskScheduler. Recursive deferred pump or
shutdown from a deferred callback is rejected. Queue depth, running bodies and
nonterminal tracking counts are zero at successful shutdown.

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

Diagnostic reads do not emit profiler work. EngineLoop requests fixed
attribution plots through PublishTaskSchedulerProfilerPlots. Collection runs
only while Tracy is connected and at most ten times per second; disconnected
and Tracy-disabled execution skip registry copying and aggregate traversal.
See [CPU Profiling](../../Development/Build/Profiling.md).

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/TaskRuntime.h`
- `Engine/Source/Runtime/Core/Public/Threading/TaskPipe.h`
- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp` (ordinary API adapters)
- `Engine/Source/Runtime/Core/Private/Threading/TaskRuntime.cpp` (scheduler runtime)
- `Engine/Source/Runtime/Launch/Private/EngineLoop.cpp`

CoreConcurrencyTests groups ordinary task API coverage in TaskTests.cpp,
scheduler/diagnostic/deferred execution in TaskSchedulerTests.cpp, and primitive
thread/event/pool behavior in ThreadingTests.cpp. Pipe, concurrency limiter and
lifecycle/retirement coverage have separate files. Their existing test suite/case
names are preserved; moving a test does not change its filter or behavioral contract.
