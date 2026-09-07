# Async Task Framework Refactor Plan

Summary: Refactor task composition, admission, owner lifetime, and executor boundaries, then qualify the API through package reads and texture compilation.

Last reviewed: 2026-09-08

Status: Completed
Completed: 2026-09-08

## Current Status

Subsequent simplification (2026-09-08): Texture now owns pending-request tracking,
compute-result polling and GameThread delivery directly. The Core owner-operation
queue/ticket abstraction and separate retained-result byte budget were removed.
The stage receipts below describe the completed original implementation; current
behavior is authoritative in [Asset compilation](../Runtime/Assets/AssetCompilation.md)
and [Task system](../Runtime/Core/TaskSystem.md).

Stages 0 through 5 are implemented and qualified. Package reads use bounded
blocking I/O and shared outcome composition. Texture compute returns unique
results into reserved outcome tickets that remain pending through GameThread
application. Both pilots remove subsystem terminal-result publication machinery.
All 72 affected Debug targets and the new Release lifecycle integration pass.
The final preselected five-cohort Release sequence passes every original Stage 0
numeric gate; allocation increases are separately explained below. Previous
failing measurements remain visible and were not used to replace the baseline.
Changed Runtime contracts and all plans validate. This plan is complete;
remaining legacy adapters are explicitly bounded compatibility follow-up.

Observed starting points:

- Before Stage 1, `WaitTask` rejected a direct GameThreadDeferred target on
  GameThread but did
  not reject a Worker target with an unfinished GameThreadDeferred ancestor.
  The Worker -> GameThreadDeferred -> Worker graph could therefore block its pump.
- Unique consumers require a void callback, preventing ownership-preserving
  multi-stage result transformations. Continuation return values are stored
  directly; returning another task does not flatten its completion relationship.
- Admission failure returns an invalid handle. An outcome continuation is
  independently admitted and can be canceled or rejected before invocation.
- Package reads submit blocking file work to the shared CPU scheduler.
  Worker dispatch does not forward continuation priority to the pool.
- Scope destruction requests cancellation of an open scope but does not join.
  Closed scopes reject new descendants as well as external roots.
- Module operation-group drain additionally checks retained results and
  callable destruction. Task completion alone is not DLL-unload authorization.
- ParallelFor defaults to a serial sentinel. This is an explicit current
  policy, not proof that Release production workloads cannot benefit from
  parallel execution.

The existing task contract remains authoritative until each replacement is
implemented. Historical usage examples must be checked against current callers;
the plan does not assume that every documented unique-result workflow remains
in production.

## Goal

Allow an owner to express a bounded read -> compute -> owning-thread commit
operation with explicit result ownership, composable completion, actionable
admission failure, and safe close/join semantics. Preserve the existing terminal
publication, cancellation, bounded accounting, and module-unload guarantees.

Completion means that the represented operation, including its required commit
stage, has finished or reached a typed terminal failure/cancellation outcome.
Creating an inner task or finishing a producer body is insufficient.

## Scope and Non-goals

This plan changes Core task APIs and their implementation, the Engine lifecycle
integration needed by them, and two production migration pilots: package range
reads/transforms and Texture2D compilation. Shared and unique legacy callers
outside the pilots remain supported through compatibility adapters.

Not included: a wholesale scheduler rewrite, mandatory work stealing, fibers,
coroutines, GPU/render-command replacement, a universal streaming mailbox,
import UI changes, or migration of every background subsystem. CPU priority and
ParallelFor policy are qualified here; work stealing requires separate evidence
and a separately selected implementation scope. Existing render/RHI ownership,
request generations, streaming, batching, and latest-wins policies remain with
their owning subsystem.

## Selected Design

### Layering and ownership

1. Core scheduling owns executable nodes, dependency readiness, bounded
   admission, executor dispatch, and terminal publication.
2. Core composition owns typed outcomes, shared/unique ownership, synchronous
   and asynchronous continuations, fan-in, and external completion sources.
3. Owner operation adapters own request acceptance, commit reservation,
   generation checks, and business-visible completion. Engine asset adapters
   own asset policy; Core does not acquire asset or editor object knowledge.

Keep one scheduler lifetime/accounting domain. Introduce a bounded blocking-I/O
executor owned by task-system lifecycle, separate from CPU execution. Initially
it may wrap an owned dedicated pool; it is not claimed to be kernel-async I/O.
All executor nodes participate in cancellation, close, diagnostics, and drain.

### API direction

The names below describe the selected API shape; Stage 0 resolves exact C++
spelling against repository types and freezes it before migrations.

| Surface | Contract |
| --- | --- |
| `TrySpawn(Group, Executor, Options, Callable)` | Returns an expected-like task-or-admission-error; no accepted work is represented by an invalid success value. |
| `Task<T>` / `Task<void>` | Move-only consumer ownership with the same composition operations; a separate non-consuming completion handle permits status observation. |
| `SharedTask<T>` / explicit `Share()` | Explicit fan-out with immutable shared results; conversion relinquishes unique consumption. |
| `Then(Task<T>&&, Executor, F)` | Moves `T` into `F(T&&)` and produces a new `Task<U>`; void is supported. |
| `ThenAsync(Task<T>&&, Executor, F)` | Flattens the returned task; value, error, cancellation, and completion follow the inner operation. |
| `WhenAll(Vector<Task<T>>)` | Consumes a dynamic collection, preserves input order, and succeeds immediately for empty input. |
| `WhenAll(tuple<...>)` | Preserves heterogeneous inputs, including void completion gates. |
| `CompletionSource<T>` | One-shot bridge for external completion with defined abandoned-source failure. |
| `TaskGroup::JoinAsync()` | Observes closed-group execution quiescence without blocking its executors. |

Use one common execution-options surface for roots and continuations. Executor
selection and CPU scheduling priority must not be silently ignored. Optional
callable context carries cancellation and child-submission authority. Queue
coalescing/generation policy belongs to the deferred adapter, not every task.

Each fallible graph-construction operation returns an explicit admission result;
the table's task types describe its success value. Unique input ownership is
transferred only after successful admission; failure preserves the input for
retry or disposal. A callable already moved into a submission may be destroyed
on rejection; its resources are not implicitly promised back to the caller.
No graph builder may discard a construction error and continue with an invalid
predecessor. Stage 0 must make this contract visible in complete C++ examples.

An explicit `ThenAsync` unwraps tasks. Ordinary `Then` must reject task-returning
callbacks in the new API rather than silently constructing `Task<Task<T>>`.
Legacy overloads preserve existing behavior until their callers are migrated.

### Failure, cancellation, and reliable completion

- Separate admission errors, task execution failure/cancellation, and domain
  errors such as a missing asset. Use existing repository result conventions
  where possible; exceptions remain contained at executor entry points.
- `Then` propagates unsuccessful input without invoking the success callback.
  Outcome continuations observe all producer outcomes but are not unconditional
  finally handlers: their own executor/admission/cancellation still applies.
- A represented operation publishes terminal outcome exactly once even if no
  user callback can execute. Status publication must not depend on enqueueing
  another ordinary fallible task.
- The owner operation adapter reserves a bounded completion record and any
  required commit ticket when accepting a request, before expensive work starts.
  Close cancels outstanding tickets or drains their commits at an authorized
  owner-thread boundary. No post-close object mutation is permitted.
- Completion records and large result payloads have separate budgets. A tiny
  ticket does not authorize unlimited payload retention. The texture pilot
  retains its existing in-flight byte budget and durable mailbox.
- Cancellation is cooperative and does not reverse already committed effects.
  External producers must finish or acknowledge cancellation before buffers and
  callback storage can be released. Late completion cannot overwrite terminal
  state or access a retired module.

### Waiting and owner lifetime

Ordinary waits never pump GameThread callbacks. Reject waits whose unfinished
dependency graph requires the waiting thread, including transitive dependencies.
Record dynamic ThenAsync dependencies before exposing them as waitable; reject
or conservatively classify unknown external execution requirements. Worker
helping is not a general solution for arbitrary locks or blocking external I/O.

Preserve legacy scope semantics. The new group distinguishes closing external
root admission from child submission by an accepted running operation. During
drain, a child may be admitted only with a live counted parent capability and
within existing capacity limits. Joining becomes complete only when no active
operation can still create a child. Cancel closes child admission as well.
Unused copyable tokens cannot keep this capability alive indefinitely.

Group destruction does not silently block GameThread. Owners explicitly close
and join before destroying captured state; misuse is diagnosed. Module-owned
groups continue to require the stronger retained-result/callable-storage audit
before unload. Do not replace `FAsyncOperationGroup::Drain` with ordinary Join.

## Implementation Stages

### Stage 0: Freeze contracts and reproduce the baseline

- [x] Inventory actual callers of launch, continuations, waits, scopes, and
  operation groups; record pilot boundaries and compatibility requirements.
- [x] Add a bounded regression for Worker -> GameThreadDeferred -> Worker
  waiting on GameThread. Ensure the harness can cancel/release and terminate
  even against the old behavior; never add a permanently hanging test.
- [x] Freeze concrete result/error types, unique ownership on rejection,
  executor capabilities for safe waits, child authority, and completion-ticket
  close semantics. Record decisions here before proceeding.
- [x] Write complete proposed package-read and texture-build call-site examples,
  including every admission failure, cancellation, owner close, and commit path.
- [x] Record baseline allocations, retained bytes, queue latency, and task
  throughput for the two pilots with build profile and measurement conditions.

Acceptance: a reproducible bounded failure case, an explicit compatibility map,
and reviewable API examples cover all terminal paths. No implementation stage
starts with competing ownership or rejection contracts unresolved.

#### Stage 0 caller inventory and compatibility map

Inventory date: 2026-09-07. Repository-owned production C++ under
`Engine/Source` was searched for launch, continuation, fan-in, wait, scope,
operation-group, and ParallelFor entry points. Tests and lifecycle smoke are
listed separately from production migration. Source paths below are relative
to `Engine/Source`.

| Caller | Actual current use | Migration or compatibility boundary |
| --- | --- | --- |
| `Runtime/Engine/Private/Asset/PackageResource.cpp` | Cancelable void read root; outcome transform edge or direct-completed transform root; `WaitTask`; request mutex/CV and terminal publication | Pilot: return composed read/transform outcomes, preserve ranges, cancellation, retirement and synchronous reads. `Wait()` currently discards `WaitStatus` and can publish IoError from a nonterminal snapshot. |
| `Runtime/Engine/Private/Texture/TextureCompilingManager.cpp` | Cancelable void build root in `FTaskScope`; `WaitAll`; cancel close and timed scope wait | Pilot: preserve request serial, interactive burst limit, in-flight byte accounting, durable completion mailbox and owner-thread application. Worker completion currently precedes final application. |
| `Runtime/Engine/Private/Asset/CookedMeshLoadManager.cpp` | Cancelable root and scope close/wait | Legacy adapter; retain residency/resource retirement and subsystem completion state. |
| `Runtime/Engine/Private/Materials/MaterialCompileLifecycle.cpp` | Cancelable root and scope cancel/timed wait | Legacy adapter; retain provider and material lifecycle. |
| `Runtime/Engine/Private/EnvironmentLighting/EnvironmentLightingBuild.cpp` | Shared typed face/LUT roots and `WaitAll` | Shared compatibility surface; preserve immutable fan-out and synchronous assembly. |
| `Editor/MainFrame/Private/AssetCompatibilityAudit.cpp` | Shared cancelable summary, deferred `ThenOutcome`, ignored wait snapshots during cancellation/shutdown | Legacy adapter; preserve streamed records, serial/generation checks and UI ownership. Include rejected waits in the later audit. |
| `Editor/DurinEd/Private/Source/SourceReferenceIndex.cpp` | Shared snapshot root and wait in service lifetime | Legacy adapter; preserve source-index publication and shutdown. |
| `Editor/ContentBrowser/Private/Assets/SourceImageThumbnailCache.cpp` | Decode root, optional owned scope or supplied scope, wait during teardown | Legacy adapter; retain cache/upload scheduling and render ownership. |
| `Editor/LevelEditor/Private/LevelEditorModule.cpp` | Creates `ThumbnailOperations` through `FModuleStartup` and supplies its scope to thumbnails | Preserve module-owned operation-group drain and retained-storage audit. Ordinary group Join is insufficient. |
| `Runtime/Launch/Private/Diagnostics/TaskSchedulerLifecycleSmoke.cpp` | Void dependencies, cancellation, Worker helping, ParallelFor and deferred continuation | Lifecycle validation caller; retain compatibility until deliberately migrated. |

No production calls to `LaunchUniqueTask`, `LaunchUniqueCancelableTask`,
`ConsumeThen`, or `ConsumeThenOutcome` were found. Unique APIs are exercised by
Core tests; historical import examples in the task-system document must not be
reported as a current unique consumer migration. No production tuple `WhenAll`
caller was found. Module fixtures and `AsyncOperationGroupTests` separately
cover group retention and unload. Scheduler lifecycle remains owned by
`Runtime/Launch/Private/EngineLoop.cpp`.

Before/after comparison starts with these explicit pilot mechanisms:

- Package request state has one mutex, one CV, `bCancelled`, `bAwaitingTask`,
  `bTerminal`, stored result, cancellation forwarding and a task handle.
  `Complete` is the one guarded publication primitive, called from bodies,
  admission rejection, direct-completed requests and the wait fallback.
- Texture queue jobs have one mutex/CV, `bCancellationRequested`,
  `bWorkerCompleted`, `bCompletionQueued`, task and completion callable.
  `FCompletion` retains the move-only work result in the manager mailbox.
  Manager queue fairness, byte budgeting and owner-side compilation records
  have subsystem purposes and are not automatically obsolete task glue.

#### Stage 0 bounded reproduction evidence

`FGameThreadDeferredTaskTests.TransitiveGameThreadWaitBaselineHasBoundedRecovery`
constructs an accepted Worker -> GameThreadDeferred -> Worker chain. The root
has completed; both downstream nodes are confirmed nonterminal before waiting.
A separate watchdog waits for the GameThread wait to return for 250 ms, then
cancels the unstarted deferred ancestor. Cancellation propagates to the Worker
tail and releases the blocked GameThread without pumping callbacks. The
watchdog is joined and the task system is cancel-shut down before test exit.

The baseline asserts that recovery was necessary, the wait returned
`Completed/Canceled`, and the deferred callback never ran. This deliberately
characterizes the old defect; Stage 1 must change the assertion to
`UnsupportedThread` with no watchdog recovery, retain the watchdog as a
regression escape path, and then cancel/drain remaining work. Passing this
baseline test is not evidence that the wait defect is fixed.

Validation on `macos-xcode-arm64`, 8 build jobs:

- Debug existing suite: 140/140 passed, 284 ms test-body total.
- Debug focused reproduction: 1/1 passed, 257 ms test-body total.
- Release suite with reproduction: 141/141 passed, 411 ms test-body total.
- Release pilot-domain correctness selection: `AssetBulkContainerTests`,
  `AssetPackageTests`, `PackageRegistryContractTests`, `TextureTests`, and
  `TextureThumbnailTests` all passed. CTest log:
  `Build/.agent-state/logs/20260907-023026-136896-4371-ctest.log`.
- Debug `test affected`: `CoreConcurrencyTests`, `CoreFileSystemTests`, and
  `CoreUtilityTests` all passed. CTest log:
  `Build/.agent-state/logs/20260907-023146-993359-6119-ctest.log`.
- Logs: `Build/.agent-state/logs/20260907-022613-529709-2915-CoreConcurrencyTests.log`,
  `Build/.agent-state/logs/20260907-022755-604001-3497-CoreConcurrencyTests.log`,
  and `Build/.agent-state/logs/20260907-022827-449195-3732-CoreConcurrencyTests.log`.

The declared deferred capture size is 32 bytes. Zero bytes causes dispatch
rejection before waiting and therefore cannot reproduce the deadlock; the
nonterminal assertions prevent that false-positive setup.

#### Stage 0 frozen source-level decisions

These spellings and contracts are frozen for implementation. The proposed pilot
examples below exercise ownership, admission, cancellation and owner close.
They remain design contracts until the owning implementation stages land.

- Place the new composition API in `Durin::Tasks`, keeping the existing
  `Durin::Then` and legacy handles source compatible. Use `TTask<T>`,
  `TSharedTask<T>`, `FTaskCompletion`, `FTaskGroup`, `FTaskContext`, and
  `TCompletionSource<T>`. `TTask<void>` is a supported specialization.
- The repository builds C++20; use `TTaskAdmission<T>` backed by
  `std::variant<T, FTaskAdmissionError>`, not C++23 `std::expected`.
  `HasValue()`, `GetError() const`, and rvalue-only `TakeValue()` expose its
  alternatives. It has no default invalid-success constructor. Error codes
  are `CapacityExhausted`, `LifetimeClosed`, `GroupClosed`,
  `InvalidPrerequisite`, `UnsupportedExecutor`, `InvalidPayloadDeclaration`,
  `UniqueConsumerClaimed`, and `DependencyCycle`. Errors carry the code and
  optional related task id; detailed strings remain diagnostics. Stage 1 adds
  `InvalidCallable` for empty erased callables, which the legacy API already
  rejects; mapping that failure to capacity or payload would be misleading.
- `ETaskExecutor` is `Worker`, `BlockingIO`, or `GameThreadDeferred`.
  `FTaskExecutionOptions` contains debug name, priority, attribution,
  cancellation token, capture-byte estimate and result-byte estimate. Group
  and executor are explicit parameters. No coalescing key is added here;
  coalescing belongs to the owner adapter.
- `TTaskOutcome<T>` contains exactly one of an owned `T`, `FTaskFailure`, or
  `FTaskCanceled`; void uses `std::monostate` for success. Framework failures
  include callable exception and abandoned external source. Existing
  `FPackageResourceReadResult` and texture results remain values containing
  their domain errors; a missing file is not a graph admission error.
- `Then(TTask<T>&&, Executor, Options, F)` returns
  `TTaskAdmission<TTask<U>>` and success invokes `F(T&&)` (no argument for
  void). `ThenAsync` has the same admission wrapper and accepts a task or
  task-admission return.
  Ordinary `Then` rejects both task types and admission wrappers as callback
  results. An asynchronous child-creation callback returns
  `TTaskAdmission<TTask<U>>`; an inner admission error becomes a typed failure
  of the already-accepted outer operation. It never becomes an invalid inner
  prerequisite. Shared inner observation must not cancel other consumers.
- Every consuming graph builder reserves all nodes, dependency storage and
  unique claims before moving an input handle. Rejection rolls back claims
  and preserves every input, including all elements of fan-in. Allocation
  failure follows the same rollback rule; callable capture ownership may have
  moved already. Result destruction occurs after locks are released.
- `Share(TTask<T>&&)` is an explicit admission operation returning
  `TTaskAdmission<TSharedTask<T>>`; success relinquishes unique consumption.
  `GetCompletion()` is non-consuming and returns only `FTaskCompletion`, with
  no value access. Sharing cannot grant a second mutable or consuming alias.
- Dynamic fan-in returns an ordered vector; tuple fan-in replaces void slots
  with `std::monostate`. Empty fan-in still returns an explicit admission
  result; its successful handle is immediately terminal. Failure takes
  precedence over cancellation, then lowest input index wins. Shared duplicate
  positions remain distinct results; unique duplicates reject atomically.
- Wait requirements are a transitive union of `GameThread` and `Unknown`
  flags, with neither flag meaning no owning-thread requirement. A
  nonterminal dynamic or external edge is `Unknown` until its requirements
  are registered. GameThread rejects either flag; rendering-thread and
  self/cycle restrictions remain. Terminal ancestors do not constrain waits.
  ThenAsync publishes/pins its inner edge under dependency synchronization
  before narrowing `Unknown`; it must never expose a temporarily unrestricted
  wait while constructing that edge.
- `FTaskContext&` is invocation-scoped, noncopyable and nonmovable. Its
  `TrySpawnChild` checks the live executing parent and charges the child's
  reservation before the parent's reservation can be released. A group token
  associates roots but grants no post-close authority. Drain close rejects
  external roots; cancel close rejects children too. Retaining a token or a
  completion handle cannot prolong child authority.
- Completion-source creation is admitted and counted in its group. A source
  registers execution requirements before exposing completion. Cancellation
  requests do not release external callback/buffer storage until producer
  acknowledgement; the producer's last source release publishes abandonment
  if unresolved. Completion publication is one-shot and late attempts fail.
- Owner acceptance reserves both a bounded terminal record and a commit slot
  before spawning work. Binding accepted producer completion to that reserved
  record is an internal, non-fallible terminal hook, not `ThenOutcome`.
  Capture/payload reservation remains separate. Close and owner-thread commit
  share an authorization boundary: after close, pending slots publish
  cancellation without invoking object mutation. Drain commits must execute
  at the authorized boundary before that close linearization point.
  Group Join includes unacknowledged external work and accepted child work;
  module Drain additionally retains its storage/unload audit.

Pilot measurements will use the same Release profile, hardware and worker
count before/after, with warm-up excluded and at least 30 measured batches per
workload. Freeze these relative gates before production changes: median and
p95 queue latency each at most 110% of baseline, peak retained bytes at most
110%, and throughput at least 90%. Report allocation counts separately and
explain every increase. A missing baseline, unmeasured payload retention or a
correctness failure cannot pass through these thresholds. These are acceptance
limits; the fixed fixtures, cache conditions and instrumentation below define
the first before/after comparison lane. Larger mixed-load and payload workloads
remain required in Stages 4 and 5.

#### Stage 0 Release performance baseline

`AsyncTaskPilotQualificationTests` is a separately registered, explicit
qualification target, currently on macOS editor builds because its allocation
replacement is verified against Mach-O linkage. Run it with
`./DevTool test AsyncTaskPilotQualificationTests --mode qualification --preset MacOS-arm64-Release-DurinEditor`.

Measured 2026-09-07 on Apple M4, 16 GiB RAM, macOS 26.6.1 (25G76),
`macos-xcode-arm64`, `MacOS-arm64-Release-DurinEditor`, two CPU Workers,
Tracy disabled, ordinary test logging enabled. The process runs alone within
this DevTool invocation; no GPU or application host is initialized. Timing is
instrumented and must be compared using the same instrumentation. Three warm-up
batches are discarded; each pilot then measures 30 batches of four operations
(120 queue-latency observations). Percentiles use nearest rank; throughput is
four operations divided by median batch duration.

| Metric | Package read -> XOR transform | Texture2D build -> owner commit |
| --- | ---: | ---: |
| Queue latency median / p95 | 30,792 / 66,750 ns | 35,084 / 203,792 ns |
| Batch latency median / p95 | 124,167 / 185,042 ns | 395,084 / 408,750 ns |
| Operations/s at median batch | 32,214.7 | 10,124.4 |
| Ordinary C++ allocations per batch median / p95 | 224 / 226 | 491 / 494 |
| Cumulative requested bytes per batch median | 566,564 | 474,668 |
| Maximum live requested bytes allocated during a measured batch | 549,468 | 177,179 |
| Retained result payload bytes per batch | 262,144 | 10,976 |
| Observed declared in-flight byte high-water | Not declared by legacy package API | 131,072 |

Package fixture: four distinct 64 KiB ranges from one freshly written 256 KiB
file, warm OS cache, no cache eviction. A timestamping file backend subclasses
`FPackageResource`; the real `ReadRangeAsync` and `Transform` implementations
perform admission, completion, cancellation and result transfer. File reads are
followed by copying/XORing every byte into an owned output. All four outputs are
retained together and validated. Queue latency ends at file-backend entry;
batch latency ends after all four request results are observed. This lane
isolates the composition path; it does not measure loose-package digest
validation or cold storage latency.

Texture fixture: four distinct package-owned objects, 64x64 RGBA8 source,
normal compression, Win64/Game target, background priority, default manager
limits (two Workers, four-request interactive burst, 1 GiB byte budget).
Each batch changes the source byte value; persistence is disabled. Input
construction is outside the measured interval. Timing includes detached input
submission, CPU build, durable mailbox and successful object-level completion
callback; it does not end at Worker return. Queue latency comes from the
manager diagnostic. Four complete mip results are retained in their objects.
No texture import UI, source decoding, GPU upload, or rendered frame is timed.

The test executable replaces ordinary `new`/`delete` only. Fixed epoch-tagged
headers count allocation calls and track live **requested** bytes for allocations
started during each batch, without a heap map. Separate epoch counters prevent
late destruction from corrupting later measurements. A direct 129-byte
allocate/free probe verifies live/peak accounting, and an Engine-owned request
allocation verifies that module allocations reach the replacement. Existing
fixture inputs, old object results, allocator metadata, direct C malloc/realloc
and over-aligned allocation are excluded. The live-byte gate applies to this
explicit C++ allocation cohort; retained payload bytes are reported separately.
It is not a claim about total process RSS or compressor-internal malloc peaks.
Large-payload qualification must additionally account for its payload budget.

Validation: the qualification passed; detailed test output was retained as
`Build/.agent-state/logs/20260907-030305-async-pilot.jsonl`, with CTest receipt
`Build/.agent-state/logs/20260907-030305-801942-12263-ctest.log`. Prior instrument
iterations are diagnostic only and are not competing baselines. No production
performance improvement is claimed at Stage 0.

Handoff validation also ran `test affected`. The new native-target declaration
resolved to `all`; every selected ordinary Debug target passed (CTest receipt
`Build/.agent-state/logs/20260907-030530-711183-13588-ctest.log`). Both Debug and
Release configuration validated source ownership and target registration.
Changed-document validation and the all-plan validator passed.

#### Stage 0 proposed pilot call sites

The following are complete proposed call-site bodies, not compiled claims about
an already available API. `Tasks` types use the spellings above;
`TTaskAdmission<T>::Success(T&&)` and `Failure(FTaskAdmissionError)` are the
explicit alternative factories. `Cancel(FTaskCompletion)` requests cancellation
without consuming a result. A task/group completion handle never grants child
submission authority.

`FPackageReadSnapshot` is an Engine adapter that pins the validated resource
and range and exposes `Read(CancellationToken) -> FPackageResourceReadResult`.
It retains the current offset/extent checks, missing/truncated-file outcomes,
resource-generation checks and buffer ownership. `FPackageTransform` is a
move-only callable on that domain result. Snapshot/range validation happens
before this function, using the existing synchronous domain result boundary.
The transform must run for domain failures as well as successful reads;
framework failure/cancellation propagates without invoking it.

```cpp
using FReadAdmission = Tasks::TTaskAdmission<
    Tasks::TTask<FPackageResourceReadResult>>;

auto ComposePackageRead(
    Tasks::FTaskGroup& Group,
    FPackageReadSnapshot Snapshot,
    FPackageTransform Transform,
    const Tasks::FTaskExecutionOptions& ReadOptions,
    const Tasks::FTaskExecutionOptions& TransformOptions) -> FReadAdmission
{
    auto ReadAdmission = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::BlockingIO,
        ReadOptions, [Snapshot = std::move(Snapshot)](Tasks::FTaskContext& Context) {
            return Snapshot.Read(Context.GetCancellationToken());
        });
    if (!ReadAdmission.HasValue()) return ReadAdmission;

    auto Read = std::move(ReadAdmission).TakeValue();
    auto Next = Tasks::Then(std::move(Read), Tasks::ETaskExecutor::Worker,
        TransformOptions,
        [Transform = std::move(Transform)](FPackageResourceReadResult&& Input) mutable {
            return Transform(std::move(Input));
        });
    if (!Next.HasValue())
    {
        // Rejection preserved Read. Its executing body still pins its snapshot.
        Tasks::Cancel(Read.GetCompletion());
    }
    return Next;
}
```

Both option objects carry the same request cancellation token and measured,
conservative capture/result byte declarations. Only successful return of the
whole builder transfers the represented operation to the caller. Failure may
leave a cooperatively canceling root, counted in `Group`; it never authorizes
resource destruction. A successful `FReadAdmission` moves into the request's
single `TTask<FPackageResourceReadResult>`. `IsReady` observes its completion,
`Cancel` requests cancellation, and `Wait` first checks `WaitStatus`. A rejected
wait returns a wait error to that caller and does not replace the stored
operation outcome. A successful wait consumes the typed outcome exactly once
or explicitly shares it for the existing copyable request facade. Sharing is
another checked admission before publishing that facade.

For Texture2D, `FTextureOperationRecords` is an Engine adapter, not another
scheduler. `TryReserve` reserves one terminal record, an owner-commit slot and
payload budget using the current request id/generation and byte policy. Its
move-only ticket exposes the preallocated completion, cancellation token and
`Bind(TTask<FTexture2DCompilationWorkResult>&&)`. Binding is infallible because
reservation already provided hook/dependency storage; it handles a producer
that finished before binding. `FailAdmission` terminalizes the accepted record
with the structured error and releases its unused commit slot. An abandoned
unbound ticket does the same with an abandoned-operation failure.

```cpp
auto FTextureOwner::Accept(FTextureBuildSnapshot Snapshot)
    -> Tasks::TTaskAdmission<Tasks::FTaskCompletion>
{
    using FAdmission = Tasks::TTaskAdmission<Tasks::FTaskCompletion>;
    auto Reservation = Records.TryReserve(Snapshot.Identity, Snapshot.EstimatedBytes);
    if (!Reservation.HasValue())
        return FAdmission::Failure(Reservation.GetError());

    auto Ticket = std::move(Reservation).TakeValue();
    auto Completion = Ticket.GetCompletion();
    Tasks::FTaskExecutionOptions Options = Snapshot.ExecutionOptions;
    Options.Cancellation = Ticket.GetCancellationToken();
    auto Build = Tasks::TrySpawn(Group, Tasks::ETaskExecutor::Worker, Options,
        [Snapshot = std::move(Snapshot)](Tasks::FTaskContext& Context) mutable {
            return Snapshot.Build(Context.GetCancellationToken());
        });
    if (!Build.HasValue())
    {
        Ticket.FailAdmission(Build.GetError());
        return FAdmission::Success(std::move(Completion));
    }
    Ticket.Bind(std::move(Build).TakeValue());
    return FAdmission::Success(std::move(Completion));
}

auto FTextureOwner::Pump() -> void
{
    Records.PumpAuthorizedCommit(CurrentGeneration,
        [this](FTexture2DCompilationWorkResult&& Work) {
            return ApplyTextureResult(std::move(Work));
        });
}

auto FTextureOwner::Cancel(uint64 RequestId) -> void
{
    Records.RequestCancellation(RequestId);
}

auto FTextureOwner::Close() -> Tasks::FTaskCompletion
{
    // GameThread boundary: no commit may enter after this call closes tickets.
    Records.CloseAndCancel();
    Group.Close(Tasks::ETaskGroupCloseMode::Cancel);
    return Group.JoinAsync();
}
```

`Snapshot.Build` uses only detached CPU data and the provider lease; it cannot
resolve `DObject` handles. `ApplyTextureResult` retains the existing request
identity checks, domain failure mapping, final object mutation and resource
handoff. `PumpAuthorizedCommit` checks owner/generation/cancellation before
calling it, runs without record/scheduler locks, contains callback exceptions,
and publishes the returned domain result only after commit. Producer framework
failure/cancellation terminalizes the record without needing a pump; successful
large payloads stay in the existing durable mailbox until authorized commit or
cancel disposal. Successful Worker return therefore leaves the represented
operation nonterminal. Supersession cancels the old ticket with its stable
reason and cannot authorize old mutation.

Owners retain captured state until the completion returned by `Close` is
terminal. `JoinAsync` copies a completion reserved when the group was created;
it cannot reject through ordinary node admission. Group creation itself is
`FTaskGroup::TryCreate(Options) -> TTaskAdmission<FTaskGroup>` and must be checked
before publishing the owner. Closed-group cancellation includes bound tickets
and unacknowledged producers, even if observers have released all handles.
Destruction checks quiescence and never silently pumps or waits. Module owners
still perform `FAsyncOperationGroup::Drain` after dropping owned results and
provider leases; ordinary Join does not replace that authorization.

These examples select an infallible *reserved binding* for durable operation
completion. Implementing it as a fresh `ThenOutcome`, allocating a new ticket
when a Worker finishes, or returning a completed build task before texture
commit would violate the frozen contract and must fail Stage 3/5 acceptance.

### Stage 1: Repair waiting and expose admission errors

Depends on Stage 0.

- [x] Reject transitive GameThread waits; cover direct, multi-hop, terminal,
  self-wait, and Worker-helping cases without introducing callback pumping.
- [x] Add the variant admission result and internal root/continuation boundary;
  preserve legacy wrappers and classify rejection at the scheduler decision.
- [x] Add structured admission errors for capacity, closed lifetime/group,
  invalid prerequisite, unsupported executor, and invalid payload declarations.
- [x] Keep legacy launch wrappers; route new graph construction through the
  explicit admission result and preserve unique inputs after rejection.
- [x] Audit affected wait callers for ignored WaitStatus, beginning with
  package requests; rejected waits must not synthesize a false task completion.

Acceptance: the Stage 0 deadlock regression passes, rejected construction is
locally diagnosable, and all accepted nodes retain exactly-once terminal state.

#### Stage 1 public construction handoff

`Threading/TaskComposition.h` supplies `TTask<T>`/void, `FTaskCompletion`,
`FTaskGroup`, invocation context, execution options, `TrySpawn`, `Then` and
explicit `Share`. New roots and edges reject invalid executors/priorities,
missing deferred executors, zero/oversized/overflowing deferred declarations,
closed scopes, and unavailable nontrivial result estimates. BlockingIO is
explicitly unsupported until Stage 4. Legacy deferred dispatch remains
compatible. The group currently uses legacy scope close/wait; Stage 3 adds
its selected child and Join contract before production migration.

Unique edge rejection preserves the input, including an injected callable-move
allocation failure after claim reservation. A retry then consumes successfully.
Result-accounting rebinding now uses a weak, allocation-free native binding so
post-admission commit does not allocate an erased callback. Synchronous unique
transformations and sharing also establish the first Stage 2 implementation.

Validation: Windows Debug Core concurrency passed 148 tests before the added
allocation regression; `test affected` then passed all four selected targets
with the regression included. Log:
`Build/.agent-state/logs/20260907-102646-341327-15144-ctest.log`.

#### Stage 1 admission foundation handoff

`Tasks::TTaskAdmission<T>` now provides the frozen variant-based result shape,
with no default construction and rvalue-only value extraction. Root and
continuation admission report errors at the actual scheduler decision under
its lock, including a related id for invalid/old-lifetime prerequisites.
Internal `TryLaunchCancelableTaskWithCompletion` and `TryLaunchContinuationTask`
return this result; the existing exported launch functions adapt rejection back
to an invalid handle. No thread-local last-error state or diagnostics snapshot
is used to infer the result of a submission.

The strict continuation boundary rejects unknown target enum values and zero
GameThread payload declarations before admission. Legacy continuations retain
their dispatch-time payload failure behavior. Queue saturation or executor close
after acceptance still produces a terminal task failure, not a retrospective
admission error. Scope rejection includes closed/old-lifetime scopes and attempts
to reparent an inherited scope; detailed scope diagnostics remain authoritative.

This is the internal admission foundation, not completion of Stage 1. Public
`TrySpawn`/composition wrappers, complete executor and payload preflight, and
structured unique-consumer rollback remain open. No group, BlockingIO executor,
or new typed composition surface is claimed by this handoff.

Validation on `windows-msvc-x64`, `Win64-Debug-DurinEditor`, 14 build jobs:
Core concurrency passed all 146 tests, including three new admission cases.
`test affected` passed all four selected targets (`CoreConcurrencyTests`,
`CoreFileSystemTests`, `CoreUtilityTests`, and `ImageCodecTests`). CTest log:
`Build/.agent-state/logs/20260907-101241-219681-26356-ctest.log`.
Changed documentation and all plans validate. No performance claim is made.

#### Stage 1 waiting handoff

`FTaskStateData::RequiresGameThread` now visits the pinned unfinished dependency
graph, holding only one state mutex at a time. Completed ancestors are skipped;
ordinary waits do not pump. The GameThread check is conditional on identity
initialization so standalone Worker-only programs retain their wait behavior.
Self-wait, dependency-cycle and rendering-thread rejection still precede this
check; Worker helping is unchanged.

The Stage 0 test is now named
`TransitiveGameThreadWaitIsRejectedWithBoundedRecovery`. Its 250 ms watchdog is
still present, but successful validation requires that it never performs
recovery. The test checks both one-hop and multi-hop Worker tails.
`TerminalDeferredAncestorDoesNotRejectWorkerWait` verifies terminal deferred
observation and a nonterminal Worker tail after that ancestor completes.
Existing direct, self/cycle and Worker-helping cases remain in the full suite.

`FPackageResourceRequest::Wait` now returns a local IoError on a rejected wait
without invoking `State->Complete`. The bounded
`RejectedRenderingWaitDoesNotPublishRequestCompletion` test holds a read open,
rejects its rendering-thread wait, checks that the request is still pending,
then releases the producer and observes the successful bytes. Its isolated
execution also exposed and verified the uninitialized-GameThread compatibility
case. Structured admission and construction wrappers keep Stage 1 open; this
handoff does not claim the new composition/group APIs are available.

The affected caller audit also checked the inventory against current edges:
environment lighting already checks both `WaitStatus` and terminal success;
audit-model teardown waits its Worker producer, not its deferred publisher;
source-reference service and thumbnails wait Worker-only roots; texture queue
shutdown waits Worker roots and then checks scope quiescence. Cooked-mesh and
material owners use their existing scope waits. Lifecycle smoke's Worker
qualification waits precede its deferred graph. None of those current graphs
acquires a new transitive rejection from this change. Their legacy ignored
wait results remain a compatibility limitation for any later graph migration;
the present audit does not authorize adding deferred ancestors to them.

Validation: the complete Core concurrency suite passed 142 tests; the isolated
package rejected-wait regression passed. `test affected` then passed all 31
selected Debug targets, including package/texture and lifecycle integration
coverage (`Build/.agent-state/logs/20260907-031217-131984-18353-ctest.log`).
Changed Runtime contracts and all plans validate. No post-change performance
claim is made by this waiting handoff; the Stage 0 baseline remains the
comparison reference for later qualification.

### Stage 2: Implement ownership-preserving composition

Depends on Stage 1.

- [x] Add unique `T -> U`, `T -> void`, and `void -> U` continuation support
  and explicit sharing without losing move-only callable support.
- [x] Implement ThenAsync with inner-task pinning, terminal propagation,
  cancellation, dynamic dependency registration, and cycle rejection. Define
  cancellation of a shared inner task as local to the observing operation;
  never implicitly cancel unrelated consumers.
- [x] Add dynamic and heterogeneous fan-in, including empty input, void gates,
  mixed failure/cancellation, and deterministic outcome ordering. Reject
  duplicate unique consumption; shared duplicates remain permitted.
- [x] Add external completion sources with exactly-once publication,
  abandonment, late completion, close, and storage-lifetime tests.

Acceptance: a multi-stage move-only pipeline and a dynamically spawned inner
operation compose without side-channel completion flags or blocking waits.
Run rejection and cancellation at every edge, including shutdown races; every
payload is consumed or destroyed exactly once outside internal locks.

#### Stage 2 composition handoff

`Then` propagates failure and cancellation without invoking the user success
callback. The public pipeline supports move-only values/captures, void gates,
and explicit immutable sharing. Shared-inner `ThenAsync` returns an owned
`shared_ptr<const T>` view; it cancels only its observing edge, preserving other
shared consumers. Unique-inner cancellation forwards both before and after
inner binding. Returning a failed inner admission publishes a typed failure of
the accepted outer operation.

Dynamic dependencies are pinned under cycle-check synchronization before the
outer unknown requirement is narrowed. GameThread rejects unknown/deferred
transitive waits; an executing task also rejects unknown external requirements.
External sources are counted scheduler nodes without occupying a CPU worker.
Cancel requests retain their accounting until producer acknowledgement; last
source release publishes abandonment. Preallocated terminal hooks bind without
another fallible scheduled task and run outside state/scheduler locks.

Dynamic and heterogeneous unique fan-in consume only after successful admission,
preserve input order and void slots, and immediately complete empty collections.
Failure precedes cancellation and the lowest failing input index wins. Shared
vector duplicates retain separate immutable owners. Failed admission preserves
all unique inputs and rolls back their claims.

Admission now reserves dependent storage and installs scope/cancellation/node
records before transferring callable ownership. Five injected allocation-failure
checkpoints verify complete rollback and a successful retry. A preparing node
cannot execute or publish cancellation. Scheduler shutdown waits explicit
submission participation; publishing terminal callbacks no longer holds the
global scheduler lock. Post-admission result accounting remains allocation-free.

Validation: Core concurrency passed 157 cases before the allocation-checkpoint
regressions. Final `test affected` passed all four selected targets with the
pre-callback cancellation check, five pre-acceptance allocation checkpoints,
and post-acceptance dispatch allocation failure included. The latter remains
an accepted terminal handle and consumes the input, rather than falsely
reporting admission rejection. CTest receipt:
`Build/.agent-state/logs/20260907-105521-736839-18580-ctest.log`.
Changed documentation and all plans validate.



#### Stage 2 outcome transformation prerequisite repair, 2026-09-08

Unique `Tasks::ThenOutcome` consumes `TTaskOutcome<T>` and returns a composable
unique `TTask<U>`. It preserves move-only values/captures, void input/output,
predecessor failure metadata, typed cancellation and structured construction
rejection. A rejected edge rolls back the unique claim so the same input can
be retried. The observing task's cancellation remains independent of recovery:
its canceled callback does not run. It cannot guarantee owner cleanup after
rejection or shutdown; the reserved owner-operation mechanism retains that role.

Three focused tests passed on macOS Debug. Their initial isolated invocation
exposed a missing fixture GameThread identity initialization; the fixture now
initializes that identity explicitly instead of relying on another test's order.
The corrected focused receipt is
`Build/.agent-state/logs/20260908-003321-868192-2169-CoreConcurrencyTests.log`.
Final `test affected` passed its complete selected integration set; receipt:
`Build/.agent-state/logs/20260908-003604-646040-2202-ctest.log`.
This prerequisite does not claim package/shared-request or Texture2D migration,
and does not change the performance baseline or accept the outstanding gate.

### Stage 3: Integrate groups and bounded operation completion

Depends on Stage 2.

- [x] Add external-root close, counted child admission, and nonblocking Join;
  integrate dynamic and external-source work into group quiescence.
- [x] Preserve module-generation ownership and extend retained-storage audits
  to new task results, completion sources, and operation tickets.
- [x] Implement the bounded operation completion/ticket mechanism needed by
  the pilots, with no arbitrary user code under scheduler or owner locks.
- [x] Test saturation, owner destruction, supersession, callback exception,
  cancel escalation, drain, and retained handles across module retirement.

Acceptance: accepting a request guarantees observable terminal completion even
when deferred dispatch is saturated or closed. Join never authorizes unloading
code that is still retained in a result, callable, or external completion source.

#### Stage 3 owner lifetime handoff

`FTaskGroup::TryCreate` reports allocation/lifetime failure. Drain close rejects
external roots while an executing counted parent can admit children through
its invocation context. Cancellation close rejects both; terminal hooks cannot
reuse child authority. `JoinAsync` observes closed-group quiescence without
node admission or callback pumping. Pending group waits reject owner/executor
threads because descendants can still acquire owning-thread requirements.

`TTaskOperationQueue<T>` reserves record and payload budgets before producer
admission. Reserved binding needs no further ordinary node or hook allocation.
The producer must be independent of its ticket. Success remains pending until
owner commit; failure, cancellation, abandonment and owner close publish without
fallible deferred dispatch. Running producers retain budget until acknowledgement.
Commit runs outside locks, contains exceptions, rejects stale generations and
supports owner destruction inside the callback without later mutation.

Shared result aliases now pin their native producer for module storage audits.
Module tests distinguish ordinary Join from stronger Drain for aliases,
external sources and committed-but-retained tickets. Result accounting updates
are serialized with extraction to prevent stale retained-byte restoration;
preparing cancellation preserves the original dependency failure attribution.

Validation: `test affected` passed all four selected Windows Debug targets,
including 2 MiB payload commit with a saturated 64-byte deferred queue, child
admission during drain, cancellation escalation, stale generations, callback
exceptions and reentrant owner destruction. Composition tests now occupy their
own source file to stay below MSVC object section limits. Receipt:
`Build/.agent-state/logs/20260907-112809-424056-19380-ctest.log`.

### Stage 4: Separate execution domains and scheduling policy

Depends on Stage 3; land before production pilot cutover.

- [x] Introduce the bounded blocking-I/O executor and integrate its shutdown,
  owner accounting, and waits with task-system lifecycle.
- [x] Make CPU root and continuation priorities effective and test bounded
  fairness so continuous interactive submissions do not starve background work.
- [x] Replace implicit ParallelFor serial selection in the new API with
  explicit Auto/Serial/ExplicitBatch policy; retain legacy behavior in adapters.
  Select Auto using measured Release workloads and record its decision rule.
- [x] Measure mixed blocking I/O and CPU work, skewed batches, and nested work.
  Record whether shared-queue contention warrants a separate scheduler plan.

Acceptance: blocked I/O does not occupy CPU Workers; configured priorities have
observable scheduling behavior; all executor lifetimes drain without abandoned
nodes. Performance claims include profile, hardware, workload, sample count,
median/p95, and the Stage 0 baseline rather than Debug-only timing conclusions.

#### Stage 4 implementation and bounded Windows measurements

The scheduler now owns a two-thread blocking-I/O pool by default, with 128
nonterminal I/O reservations in addition to shared graph capacity. Roots and
continuations select it explicitly. Startup failure cleans up partial pools;
Drain/Cancel shutdown and module callable audits include both executors.
CPU helping cannot run I/O. I/O executor helping retains its authority through
nested CPU execution, covering the one-I/O-thread -> CPU -> I/O wait graph.
Separate I/O queue/capacity/reservation fields preserve CPU diagnostic meaning.

CPU queues use three FIFO lanes. Every eighth dequeue chooses the globally
oldest lane head, so a fixed old background item cannot be starved by later
interactive arrivals. Deterministic tests cover both root and continuation
priority. Owner accounting is reserved before queue insertion, with injected
allocation rollback and reentrant callable destruction outside the pool lock.

`Tasks::ParallelFor` selects Auto/Serial/ExplicitBatch; zero explicit batch is
invalid. Auto keeps ranges below 16,384 serial; larger ranges use a
2,048-element minimum batch, at most CPU Workers plus caller chunks, and
serial nesting. Repeated 4,096-element measurements changed from a win to a
regression (325/335.8 us serial median/p95 versus 476.2/556.4 us parallel),
so that size is deliberately excluded from automatic parallel selection. Legacy options remain serial by default.
First Release measurements exposed cancellation-state mutex contention in each
iteration. Atomic read-only cancellation queries remove that contention while
registration, cancellation writes and terminal publication retain their locks.
The evidence identifies this polling lock, not shared work-queue contention;
no work-stealing implementation or separate scheduler rewrite is selected.

Windows diagnostic lane: Intel Core i5-13400F, `windows-msvc-x64`,
`Win64-Release-DurinEditor`, MSVC 14.44, two CPU and two I/O threads, 14 build
jobs. Each workload has three warmups and 30 measured samples. Uniform work
performs 64 dependent integer hash rounds per iteration; skewed work performs
1,024 rounds for the first eighth. This is a policy-selection workload, not the
Stage 0 package/texture allocation and retained-memory workload. Timing is
machine-local evidence and does not accept the original macOS pilot gates.

The measured mixed workload occupies two executor threads with blocking events
and admits a CPU callback. A 10 ms timed wait releases blocking work when CPU
execution is prevented; Windows timer scheduling can extend that interval.
The queue-latency measurement ends at CPU callback entry, before event release
on the separate-I/O path. Nested policy checks require serial inner chunks and
exact total coverage. Existing static contiguous chunks still leave skewed work
less balanced than uniform work; no stronger balancing claim is made.

Final policy measurement (microseconds; each cell is median / p95):

| Workload | Serial | Auto |
| --- | --- | --- |
| 1,024 uniform | 83.6 / 106.8 | 85.4 / 91.7 |
| 1,024 skewed | 343.8 / 348.7 | 343.9 / 434.0 |
| 4,096 uniform | 338.4 / 478.0 | 335.5 / 356.3 |
| 4,096 skewed | 1,372.6 / 1,637.9 | 1,397.7 / 1,490.9 |
| 16,384 uniform | 1,336.7 / 1,775.3 | 561.1 / 1,048.6 |
| 16,384 skewed | 5,518.5 / 5,894.1 | 4,715.1 / 5,681.3 |
| 131,072 uniform | 11,596.1 / 15,128.5 | 5,529.2 / 7,938.4 |
| 131,072 skewed | 44,946.3 / 46,556.4 | 37,564.9 / 38,989.3 |

Both policies take the identical serial path below the threshold; the separate
cohorts still show timing variance, including the 1,024-skewed p95 increase.
This noisy lane does not establish portable latency limits. Mixed CPU-entry
latency was 15,471.7 / 15,948.7 us with both CPU workers blocked and
20.3 / 29.5 us with separate I/O workers. All qualification correctness checks,
including nested coverage, passed. Qualification receipt:
`Build/.agent-state/logs/20260907-114646-335717-18684-ctest.log`.

Final affected validation passed all four selected targets in both configurations:
`Build/.agent-state/logs/20260907-114800-494307-35048-ctest.log` (Debug) and
`Build/.agent-state/logs/20260907-114724-553582-35056-ctest.log` (Release).
The allocation rollback tests now wait for the producer's scheduler reservation
to be released before checking a zero-capacity precondition; task readiness
alone intentionally precedes that final accounting step. Changed Runtime
contracts and all plans validate.

The original Stage 0 median/p95 latency, peak requested bytes and throughput
thresholds remain unchanged. The 2026-09-08 handoff below supersedes the
lack-of-runner limitation; no replacement baseline is selected.

#### Stage 4 macOS requalification and migration audit, 2026-09-08

Measured in the `Durin-architect` checkout on Apple M4, 16 GiB RAM,
macOS 26.6.1 (25G76), `macos-xcode-arm64`,
`MacOS-arm64-Release-DurinEditor`, Tracy disabled, two CPU and two blocking-I/O
threads, eight build jobs. Build and test invocations were sequential.
Each policy cohort uses three warmups and 30 measured samples. Each pilot
cohort uses the unchanged Stage 0 fixtures and instrumentation: three warmup
batches, 30 measured batches of four operations, 120 queue observations.
The measurements do not include production cutover.

Core concurrency qualification passed, including mixed blocking work and nested
coverage. Receipt: `Build/.agent-state/logs/20260908-000031-777000-85264-ctest.log`.
Measured policy times are microseconds, median / p95:

| Workload | Serial | Auto |
| --- | --- | --- |
| 1,024 uniform | 36.542 / 41.250 | 36.541 / 38.125 |
| 1,024 skewed | 186.250 / 194.541 | 186.291 / 199.125 |
| 4,096 uniform | 146.417 / 156.542 | 146.292 / 152.958 |
| 4,096 skewed | 752.666 / 792.875 | 749.458 / 783.750 |
| 16,384 uniform | 584.541 / 610.416 | 227.875 / 279.375 |
| 16,384 skewed | 2,997.040 / 3,042.920 | 2,625.380 / 2,691.830 |
| 131,072 uniform | 4,694.580 / 4,746.290 | 1,682.330 / 1,772.040 |
| 131,072 skewed | 24,006.500 / 24,141.600 | 20,930.100 / 21,005.000 |

Mixed CPU-entry latency was 12,555.2 / 12,618.2 us when the CPU workers were
blocked and 15.042 / 33.833 us with separate I/O workers. These observations
support keeping the existing Auto decision rule. They do not isolate shared
queue contention or justify selecting work stealing.

The pilot correctness test passed in all three invocations. Queue times below
are nanoseconds; peak live bytes use the Stage 0 ordinary-allocation cohort.

| Pilot / cohort | Queue median / p95 | Operations/s | Peak live bytes | Allocation median / p95 |
| --- | ---: | ---: | ---: | ---: |
| Package / 1 | 16,417 / 37,667 | 41,757.1 | 550,788 | 236 / 240 |
| Package / 2 | 18,916 / 41,292 | 41,361.2 | 550,988 | 236 / 240 |
| Package / 3 | 18,625 / 35,958 | 44,713.2 | 550,988 | 236 / 240 |
| Texture2D / 1 | 28,042 / 252,125 | 10,570.3 | 160,430 | 485 / 491 |
| Texture2D / 2 | 28,167 / 201,208 | 10,400.9 | 177,819 | 486 / 498 |
| Texture2D / 3 | 29,667 / 204,292 | 10,429.1 | 177,819 | 486 / 494 |

Retained result payload remains 262,144 package bytes and 10,976 texture bytes;
texture declared in-flight high-water remains 131,072 bytes. Package allocation
median increased from 224 to 236 and needs attribution before acceptance.
The first texture queue p95 is 123.7% of baseline, exceeding the unchanged
110% limit (224,171.2 ns). Both later cohorts pass that limit. All observed
queue medians, package p95 values, peak live bytes and throughput values pass
their respective relative limits. Keep the failed cohort visible; the current
qualification target prints timing but does not assert the baseline thresholds.
A green CTest receipt therefore proves correctness, not complete acceptance.

Receipts, in cohort order:

- `Build/.agent-state/logs/20260908-000021-065852-84278-ctest.log`
- `Build/.agent-state/logs/20260908-000047-098506-85300-ctest.log`
- `Build/.agent-state/logs/20260908-000102-614161-85548-ctest.log`


A user-requested fourth cohort ran at 00:07 on 2026-09-08 with the same
profile, fixtures, warmup and sample counts. A process check immediately before
the invocation found no competing compiler, Ninja, CMake build or CTest process;
this is a point-in-time observation, not proof of whole-machine exclusivity.
Correctness passed again, but texture queue p95 again exceeded the original
110% gate. The new observation does not support attributing the earlier tail
variation solely to another checkout's build. No threshold or baseline changed.

| Pilot / cohort | Queue median / p95 (ns) | Operations/s | Peak live bytes | Allocation median / p95 |
| --- | ---: | ---: | ---: | ---: |
| Package / 4 | 17,542 / 34,834 | 43,223.6 | 550,988 | 236 / 241 |
| Texture2D / 4 | 27,375 / 257,875 | 10,490.6 | 174,547 | 486 / 492 |

Texture p95 is 126.5% of the Stage 0 baseline, above the 224,171.2 ns limit.
All other gated metrics in this cohort pass. Batch median / p95 is
92,542 / 106,375 ns for package and 381,292 / 485,792 ns for texture.
Cumulative requested bytes per batch median is 568,212 / 477,244 respectively;
retained payload and declared in-flight bytes are unchanged from the previous
cohorts. Receipt:
`Build/.agent-state/logs/20260908-000726-461492-90135-ctest.log`.

Before Stage 5 cutover, resolve the timing variation and attribute the package
allocation increase. The production audit found a separate composition gap:
`FPackageResourceRequest::Transform` uses a completion edge and invokes its
callback with domain errors as well as successful bytes.
`FBulkData::LoadAsync` relies on that callback to transition Loading to Failed
or Retired. `Tasks::Then` and shared-task `Then` propagate failed/canceled task
states without invoking the callback. Replacing that edge with success-only
composition is insufficient. Provide outcome-aware result composition, or an
equivalent framework-owned domain-outcome adaptation, with cancellation and
admission-rejection coverage; do not restore a subsystem mutex/CV result future.
This is a prerequisite discovered by caller inspection, not an implemented API.



#### Stage 4 three-version comparison, 2026-09-08 00:22

The follow-up attribution experiment rebuilt the original baseline commit
`c7b56dbae` and Stage 4 commit `3df8497f5` in detached, isolated sibling
checkouts `Durin-async-baseline` and `Durin-async-stage4`. The current lane is
`Durin-architect` at `f837c0896`. All three use the same Apple M4/16 GiB/macOS
26.6.1 environment, external volume, shared prepared dependencies, Release
preset, two CPU Workers, Tracy setting, and instrumented pilot. The qualification
source blob is identical in all three revisions:
`5fad1eac8f83435e797665265d56aa704925feaa`. Builds finished before measurement.
Every invocation used three warmup and 30 measured batches of four operations.
Nine sequential invocations rotated order: baseline/Stage 4/current,
current/baseline/Stage 4, Stage 4/current/baseline. A process check before each
invocation found no competing compiler/build/CTest process. This check cannot
exclude all OS or application interference.

All nine correctness invocations passed. Each row is a separate cohort; times
are microseconds. These are not pooled percentiles.

| Round / version | Package queue median / p95 | Texture queue median / p95 | Package ops/s | Texture ops/s | Package / texture peak live bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 / baseline | 15.042 / 33.625 | 21.750 / 188.250 | 45,779.7 | 10,582.0 | 549,836 / 164,854 |
| 1 / stage4 | 15.917 / 37.167 | 15.458 / 199.292 | 44,965.0 | 10,370.5 | 551,052 / 165,094 |
| 1 / current | 22.000 / 47.209 | 34.166 / 345.041 | 40,472.3 | 10,422.3 | 551,028 / 178,883 |
| 2 / current | 14.250 / 34.417 | 21.625 / 196.042 | 43,301.8 | 10,487.2 | 550,988 / 173,771 |
| 2 / baseline | 20.833 / 33.959 | 20.125 / 199.083 | 46,602.1 | 10,384.0 | 550,236 / 177,499 |
| 2 / stage4 | 20.834 / 43.917 | 15.000 / 198.750 | 39,167.7 | 10,367.2 | 551,052 / 166,282 |
| 3 / stage4 | 23.166 / 44.167 | 38.375 / 196.167 | 36,641.1 | 10,443.9 | 551,052 / 177,851 |
| 3 / current | 19.250 / 40.375 | 11.333 / 197.792 | 41,793.4 | 10,474.6 | 550,988 / 165,010 |
| 3 / baseline | 21.667 / 40.583 | 17.708 / 198.000 | 43,577.3 | 10,366.1 | 549,836 / 175,027 |

Baseline and Stage 4 texture queue p95 pass the original 224.1712 us limit in
all three cohorts. Current exceeds it in round 1 (345.041 us), then passes in
rounds 2 and 3. The current round-1 texture batch p95 also rises to 1,180.625 us (1,180,625 ns); its median remains
383.792 us. The simultaneous batch tail spike is compatible with intermittent
stalling but does not identify whether code or external scheduling caused it.
Do not infer a specific later commit or approve current acceptance from the
passing cohorts alone.

All other original numeric gates pass in these nine cohorts. A new comparison
against the freshly measured baseline is diagnostic only: package throughput
ranges overlap but Stage 4 has slower cohorts, and package allocations per batch
increase from baseline median 224 to Stage 4 median 236. Current package median
is 232/236/236. This localizes the allocation increase to at or before Stage 4,
not solely later commits; allocation-site attribution remains outstanding.
Texture allocation medians are baseline 498/498/498, Stage 4 494/494/494 and
current 486/486/485. Retained payload is unchanged (262,144 package and 10,976
texture bytes), as is texture declared in-flight high-water (131,072 bytes).
No source or measurement instrumentation was changed in the historical lanes,
and no replacement baseline or pass-selection rule was introduced.

Full metric records are retained locally at
`Build/async-task-three-version-comparison.json` in `Durin-architect`.
Receipts below are relative to each row's named checkout:

- Round 1 / baseline: `Build/.agent-state/logs/20260908-002158-528261-99172-ctest.log`
- Round 1 / stage4: `Build/.agent-state/logs/20260908-002158-945485-99188-ctest.log`
- Round 1 / current: `Build/.agent-state/logs/20260908-002159-296988-99204-ctest.log`
- Round 2 / current: `Build/.agent-state/logs/20260908-002159-706142-99220-ctest.log`
- Round 2 / baseline: `Build/.agent-state/logs/20260908-002200-046609-99245-ctest.log`
- Round 2 / stage4: `Build/.agent-state/logs/20260908-002200-387843-99263-ctest.log`
- Round 3 / stage4: `Build/.agent-state/logs/20260908-002200-729903-99279-ctest.log`
- Round 3 / current: `Build/.agent-state/logs/20260908-002201-070128-99295-ctest.log`
- Round 3 / baseline: `Build/.agent-state/logs/20260908-002201-413343-99311-ctest.log`

#### Stage 4 acceptance after short-graph wait optimization, 2026-09-08 06:16

The first root-only fast path did not change the 236 package allocations and
its texture p95 still failed (250.584 us); receipt
`Build/.agent-state/logs/20260908-061014-090500-24916-ctest.log`.
Temporary ordinary-allocation stack capture then identified `RequiresGameThread`
vector/hash-set allocations while waiting for transform dependency chains.
The instrumented cohort is diagnostic only; its timing is excluded. The
qualification source was restored byte-for-byte before acceptance runs.
Graphs up to 16 nodes now use stack storage; larger graphs retain the original
cycle-safe traversal. Tests exercise both paths, including a 32-edge tail behind
an unpumped deferred ancestor. All 70 affected Debug targets passed:
`Build/.agent-state/logs/20260908-061530-195491-25985-ctest.log`.

A fixed five-cohort sequence was selected before running it, using the original
Apple M4/16 GiB/macOS 26.6.1 Release profile, two CPU Workers, Tracy disabled,
three warmup batches and 30 measured batches of four operations per invocation.
Pre-invocation process checks found no competing compiler/build/CTest process.
The revision is `ce9cef22a` plus this commit's short-graph change. All five
cohorts pass the unchanged Stage 0 queue median/p95, throughput and peak-live-byte
gates. Each row is independent; percentiles are not pooled. Earlier failures
remain evidence of intermittent variance; these runs do not prove its cause.

| Cohort | Package queue median / p95 (ns) | Texture queue median / p95 (ns) | Package / texture ops/s | Package / texture peak live bytes |
| --- | ---: | ---: | ---: | ---: |
| 1 | 15875 / 35000 | 32834 / 188625 | 45670.4 / 10608.9 | 550988 / 177819 |
| 2 | 18625 / 40459 | 14208 / 192875 | 42272.1 / 10677.3 | 550988 / 177819 |
| 3 | 17792 / 43167 | 17334 / 195916 | 40050.1 / 10513.6 | 550988 / 177819 |
| 4 | 23500 / 43416 | 25084 / 195792 | 40990.3 / 10627.7 | 550988 / 173771 |
| 5 | 19917 / 38750 | 33333 / 193917 | 39344.1 / 10776.8 | 550988 / 165014 |

Package allocation median/p95 is 224/226 in every cohort (baseline 224/226).
Texture median is 485–486 and p95 491–494. Package cumulative requested bytes
median is 567,972 versus baseline 566,564: node storage grew with structured
admission, external completion, and dependency metadata. Peak live requested
bytes are within the fixed 110% gate. Retained payload remains 262,144/10,976
bytes and texture declared in-flight high-water remains 131,072 bytes.
No additional scheduler rewrite is selected: mixed-load and nested evidence
above supports the implemented executor separation and Auto policy.
Full local records: `Build/async-task-inline-wait-qualification.json`.
Receipts:

- `Build/.agent-state/logs/20260908-061608-968049-27055-ctest.log`
- `Build/.agent-state/logs/20260908-061609-379820-27073-ctest.log`
- `Build/.agent-state/logs/20260908-061609-763116-27089-ctest.log`
- `Build/.agent-state/logs/20260908-061610-112003-27105-ctest.log`
- `Build/.agent-state/logs/20260908-061610-460809-27121-ctest.log`

### Stage 5: Migrate and qualify production operations

The continuation work first adds unique `Tasks::ThenOutcome`, accepting an owned
`TTaskOutcome<T>` and returning a composable `TTask<U>`. It preserves structured
admission/claim rollback and maps predecessor failure/cancellation to callback
input. Cancellation or rejection of the observing edge can still prevent its
callback: this is not a replacement for reserved mandatory owner completion.
Production cutover remains subject to the recorded qualification gates.

Texture migration uses the reserved operation queue as its durable result
mailbox. Extend its opt-in outcome pump so producer failure/cancellation still
reaches domain commit, and reserve a producer-ready notification before launch
for the manager's concurrency-slot release. A transferred ticket may bind on a
worker; the owner must synchronize all binding/admission acknowledgements before
closing the queue. Shutdown drains those acknowledgements, pumps domain terminal
results, then closes the queue and joins the group. Request identity, scheduling
priority/budgets and diagnostic mutation stay in the manager; results and final
operation completion belong to Core. This contract extension precedes cutover.

Depends on Stages 1 through 4.

- [x] Migrate package reads and transforms to the I/O executor and composable
  results. Preserve synchronous boundary semantics, cancellation, range/error
  handling, and resource retirement while removing redundant completion state.
- [x] Migrate the Texture2D compute/result chain; preserve its priority policy,
  memory budget, durable mailbox, request identity, and owner-thread commit.
  Track the represented operation through final commit, not just Worker return.
- [x] Compare before/after call sites: count auxiliary completion flags, mutex/
  CV result handoffs, manual publication paths, and adapter configuration.
  Record each retained mechanism's subsystem purpose; no fixed LOC target.
- [x] Run focused correctness, lifecycle, and mixed-load qualification. Include
  large payloads above deferred queue limits and saturation during shutdown.
- [x] Publish implemented contracts in the owning Runtime documentation and
  compatibility examples. Remove only obsolete pilot glue; list remaining
  legacy callers as a bounded follow-up, not an implicit completed migration.
- [x] Validate changed documentation and all plans; record evidence and mark
  this plan completed only after every acceptance gate passes.

Acceptance: both pilots express their operation with composable outcomes and
explicit ownership, without rebuilding a generic future in subsystem code.
Existing asset behavior and module-unload guarantees pass regression coverage.
If the new API still requires equivalent duplicate completion machinery, revise
the contract and affected stages before declaring the refactor complete.

#### Package migration handoff

Shared `ThenOutcome` and `GetOutcomeShared` preserve immutable aliases and
structured failure identity without consuming the input. New tests observe
success/failure/cancellation through two independently admitted edges and retry
after invalid-executor rejection. Package tests block both I/O threads while a
CPU root still runs, repeat reads through copied requests, and recover a
canceled read through a transform. Existing range, retirement, rejected rendering
wait, scheduler rejection and package regressions pass. All 71 affected Debug
targets passed; receipt
`Build/.agent-state/logs/20260908-062203-286570-27595-ctest.log`.

Package state no longer manually publishes a worker result or synthesizes a
stored result from `Wait`. The former terminal flag is removed. One binding
flag and mutex/CV remain solely for the resource-registration/retirement race;
the cancellation atomic remains the backend's cooperative control. Immediate
validation/admission errors occupy a value alternative instead of a fake task.
A completion observation is retained only if sharing fails after producer
admission, so retirement can still drain that producer. Request copies keep
their scope lifetime available for later transforms.

The first unchanged-fixture Release cohort passes all original numeric gates:
package queue median/p95 19,750/54,500 ns, 43,478.3 ops/s, peak live 558,940 bytes;
texture queue 31,709/199,792 ns, 10,463.2 ops/s, peak live 177,819 bytes.
Package allocation median/p95 is 276/278 versus 224/226 before migration:
explicit scope lifetime, unique result/failure storage and immutable lifetime
pins replace the subsystem's direct result state. Retained payload remains
262,144/10,976 bytes and texture declared in-flight remains 131,072 bytes.
Receipt: `Build/.agent-state/logs/20260908-062255-569119-28500-ctest.log`.
This is an intermediate measurement; final combined acceptance follows texture
migration and includes repeated cohorts.

#### Final production migration and acceptance, 2026-09-08 06:52

Texture compute now returns `TTask<FTexture2DCompilationWorkResult>`. A reserved
outcome ticket consumes it without another fallibly admitted continuation, and
its source completes after owner application. Explicit producer rejection and
framework failure/cancellation reach the same owner outcome pump. The manager
retains request/generation checks, the four-interactive burst policy, two active
computes, and the 1 GiB working-set policy; Core receives CPU priority as well.
A separate bounded mailbox reserves up to 1,024 operations / 4 GiB declared
retained bytes, including queued input ownership, before compute admission.
Consumed tickets leave retained diagnostics so they do not pin module storage.

Transferred binding pins its record locally because owner commit may retire a
ticket while its producer hook unwinds. Queue close requires binding
acknowledgement first. Texture shutdown uses its selected-work count and group
drain to establish that boundary, delivers domain results, then closes the queue.
The producer-ready callback carries no result; it releases compute concurrency
and wakes the manager's timed wait. It does not replace mandatory owner commit.

Mechanism comparison (business diagnostic fields excluded):

| Pilot mechanism | Before | After / retained purpose |
| --- | --- | --- |
| Package completion flags | `bAwaitingTask`, `bTerminal` | One `bBinding` flag for registration/retirement race only |
| Package mutex/CV result handoffs | One request result handoff | Zero; the mutex/CV synchronizes initial binding only |
| Package manual terminal publication | `Complete` called by read, transform, rejection and wait fallback | Zero task-result publishers; binding installs immutable task or immediate error |
| Package cancellation | Atomic plus forwarding callback and native handle | Atomic/backend forwarding retained; Core completion cancellation |
| Package adapter configuration | Attribution plus legacy CPU root/edge | Explicit I/O root, CPU outcome edge, scope lifetime, retained-result estimate, immutable sharing |
| Texture completion flags | `bWorkerCompleted`, `bCompletionQueued` | Zero; Core ticket readiness; `bAdmitted` only balances manager concurrency |
| Texture result handoffs | Subsystem result deque plus CV readiness flag | Core reserved result queue; CV only wakes timed diagnostic/readiness observation |
| Texture manual publication | Three early `CompleteAdmitted` paths and final success path, plus queued rejection publication | Worker returns a value; reserved ticket handles all framework terminals and rejected admission |
| Texture adapter configuration | Scope, attribution, manual result queue | Scope, attribution, CPU priority, record/payload reservation and outcome pump |

The first broad run exposed an existing skybox test assumption that replacement
must have a different address from a destroyed proxy. The allocator reused that
address. The test now checks successful removal plus exactly one live replacement
with updated data; its focused rerun and final aggregate pass. No renderer
behavior changed. Original failure receipt:
`Build/.agent-state/logs/20260908-063249-906832-30539-ctest.log`.

All 72 affected Debug targets pass, including Core module/operation-group
regressions, shared outcome fan-out and transferred ticket failure/cancellation:
`Build/.agent-state/logs/20260908-065228-146558-39400-ctest.log`.
`AsyncTaskPilotLifecycleTests` owns a separate process lifecycle because the
aggregate cannot restart after terminal shutdown. It verifies a 256x256 texture
result larger than the saturated 64-byte deferred queue can still commit, then
fills all eight scheduler reservations with blockers and four accepted texture
operations. Shutdown delivers exactly one callback per accepted operation and
leaves zero nonterminal nodes. It passes Debug and Release:

- `Build/.agent-state/logs/20260908-063959-193667-33069-AsyncTaskPilotLifecycleTests.log`
- `Build/.agent-state/logs/20260908-064217-100369-34519-ctest.log`

The Stage 4 mixed-I/O/CPU and nested policy measurements remain applicable;
package's focused two-blocked-I/O test additionally verifies CPU execution after
migration. Existing module group tests continue to distinguish ordinary Join
from retained-result/module Drain. Provider call retirement remains synchronous
inside the modular-feature gate. No application smoke or GPU qualification is
required for this CPU-only cutover.

Final performance uses the original Apple M4/16 GiB/macOS 26.6.1 Release lane,
Tracy disabled, two CPU and two I/O workers. All builds completed first. A fixed
five-cohort sequence was selected before execution, with no competing compiler/
build/CTest process observed before any invocation. Each cohort has three warmup
batches, 30 measured batches of four operations and 120 queue observations.
The original measurement function and allocation instrumentation are unchanged.
The revision is `eeb44c2b8` plus this commit's texture migration; all 40 numeric
comparisons against the original Stage 0 thresholds pass. Percentiles remain
separate per cohort. Temporary lifecycle cases first attempted in the performance
process failed its non-restartable aggregate precondition; they were moved to
the independent integration target above, not skipped or made unconditional.
An intermediate texture p95 of 245,416 ns remains a diagnostic failure
(`Build/.agent-state/logs/20260908-063639-422311-31906-ctest.log`).

| Cohort | Package queue median / p95 (ns) | Texture queue median / p95 (ns) | Package / texture ops/s | Package / texture peak live bytes |
| --- | ---: | ---: | ---: | ---: |
| 1 | 24125 / 59958 | 28583 / 192042 | 37676.5 / 10357.1 | 559124 / 184923 |
| 2 | 21209 / 49625 | 23833 / 194875 | 39867.2 / 10344.8 | 558940 / 184923 |
| 3 | 17583 / 40417 | 31459 / 197709 | 41432.7 / 10289.4 | 559124 / 184923 |
| 4 | 23042 / 52125 | 26750 / 187959 | 39784.4 / 10372.8 | 558940 / 180875 |
| 5 | 18250 / 46125 | 32417 / 185916 | 41775.5 / 10459.8 | 558940 / 180875 |

Package allocation median is 276 (baseline 224), p95 278–279 (226);
texture median is 570–572 (491), p95 576–582 (494). The package increase pays
for scope lifetime, typed result/failure storage and immutable result pins.
Texture adds preallocated owner records, counted completion sources, cancellation
state, terminal hooks/notifications and unique result/failure storage. These
ownership/lifetime allocations replace the uncounted mailbox, rather than being
reported as a speedup. Package cumulative requested bytes median is 572,964
(baseline 566,564), texture 489,512–489,888 (474,668). Retained output is unchanged
at 262,144/10,976 bytes and texture declared compute high-water is 131,072 bytes.
Worst peak live requested bytes are 101.8% / 104.4% of baseline, both below 110%.
The extra allocation count has no separate numeric rejection threshold; it is
reported independently from passing latency, throughput and peak-live-byte gates.
Full local metrics: `Build/async-task-final-qualification.json`. Receipts:

- `Build/.agent-state/logs/20260908-065222-702468-39313-ctest.log`
- `Build/.agent-state/logs/20260908-065223-053569-39331-ctest.log`
- `Build/.agent-state/logs/20260908-065223-406457-39347-ctest.log`
- `Build/.agent-state/logs/20260908-065223-757161-39363-ctest.log`
- `Build/.agent-state/logs/20260908-065224-106310-39379-ctest.log`

Final review moved notification callable conversion into `TryReserve`'s
allocation-failure boundary. A throwing-copy notification test verifies structured
`CapacityExhausted` and zero leaked record/byte reservations. All six operation
cases pass (`Build/.agent-state/logs/20260908-065039-275752-38199-CoreConcurrencyTests.log`).
The preceding five passing performance cohorts are retained locally in
`Build/async-task-final-pre-review-qualification.json`; the final table above
repeats the fixed five-cohort protocol after this admission repair.

The repeat aggregate also exposed a material single-flight fixture race:
a warm compile could finish before its second consumer was submitted. Its
existing overlap assertions now hold worker entry until both requests attach,
with release/drain cleanup on assertion exits. No material runtime code changed.
All 110 MaterialTests pass (`Build/.agent-state/logs/20260908-065128-229443-38630-MaterialTests.log`).
The original fixture failure remains at
`Build/.agent-state/logs/20260908-064939-548121-37308-ctest.log`.

#### Bounded compatibility follow-up

The following production adapters remain on legacy launch/shared-handle/wait
entry points. They are outside the two selected pilots and remain regression
covered; this plan does not claim their migration. Paths are relative to
`Engine/Source`:

- `Runtime/Engine/Private/Asset/CookedMeshLoadManager.cpp`: residency retirement.
- `Runtime/Engine/Private/Materials/MaterialCompileLifecycle.cpp`: material/provider lifecycle.
- `Runtime/Engine/Private/EnvironmentLighting/EnvironmentLightingBuild.cpp`: immutable fan-out and synchronous assembly.
- `Runtime/Engine/Private/StaticMesh/StaticMeshCompilingManager.cpp`: bounded mesh compilation and owner delivery.
- `Editor/MainFrame/Private/AssetCompatibilityAudit.cpp`: streamed audit records and UI generation.
- `Editor/DurinEd/Private/Source/SourceReferenceIndex.cpp`: snapshot publication and shutdown.
- `Editor/ContentBrowser/Private/Assets/SourceImageThumbnailCache.cpp`: decode/upload cache and supplied module scope.
- `Editor/ContentBrowser/Private/Panels/ContentBrowserModel.cpp`: item/tree snapshots and module-body drain.

LevelEditor's module operation registration, Launch lifecycle validation and
legacy ParallelFor adapters remain compatibility/lifecycle boundaries, not
additional pilot cutovers. Runtime TaskSystem, BulkData and AssetCompilation
contracts describe the implemented ownership and completion behavior.

## Validation and Handoff

Follow [agent build guidance](../Agents/BuildAndRun.md) before build/run work and
[agent testing guidance](../Agents/Testing.md) before selecting native tests.
Discover current registered targets rather than inferring them from filenames.
Use bounded synchronization and deterministic fault injection for race tests.
Run affected validation at each implementation handoff; broaden only for changed
integration boundaries. Never overlap builds in this checkout.

Stage 0 records numeric performance acceptance thresholds for pilot queue
latency, peak retained memory, and throughput before implementation changes.
Compare in the same Release environment and report improvements and regressions
separately; do not accept latency regression solely because throughput rises.
All correctness failures block acceptance regardless of timing results.

Each implementation commit updates this plan's status/checklists and records
exact Plan and Stage trailers under repository rules. A plan-authoring commit
does not claim Stage 0 implementation or validation completion.

The active [Content Browser Import Extensions plan](ContentBrowserImportExtensions.md)
owns import-menu behavior and its remaining manual qualification. This plan
does not absorb or close that work. Any later import consumer migration must
preserve its owner registration and retirement boundary.

## Related Documentation

- [CPU task system](../Runtime/Core/TaskSystem.md)
- [Runtime lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Async asset operations](../Editor/Architecture/AsyncAssetOperations.md)
- [Asset compilation](../Runtime/Assets/AssetCompilation.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Threading/Task.h`
- `Engine/Source/Runtime/Core/Private/Threading/Task.cpp`
- `Engine/Source/Runtime/Core/Private/Threading/QueuedThreadPool.cpp`
- `Engine/Source/Runtime/Core/Public/Modules/AsyncOperationGroup.h`
- `Engine/Source/Runtime/Core/Private/Modules/AsyncOperationGroup.cpp`
- `Engine/Source/Runtime/Launch/Private/EngineLoop.cpp`
- `Engine/Source/Runtime/Engine/Private/Asset/PackageResource.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCompilingManager.cpp`
