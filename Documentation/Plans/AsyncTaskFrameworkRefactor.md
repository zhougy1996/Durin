# Async Task Framework Refactor Plan

Summary: Refactor task composition, admission, owner lifetime, and executor boundaries, then qualify the API through package reads and texture compilation.

Last reviewed: 2026-09-07

Status: Active
Completed:

## Current Status

Stage 0 is complete: the caller inventory, bounded deadlock reproduction,
frozen source-level contracts, proposed pilot call-site bodies and first Release
performance baseline are recorded below. The new qualification target exercises
real file I/O through package request/transform composition and Texture2D builds
through successful owner-thread commit. Stage 1 is next; production task APIs
and the waiting defect are unchanged by Stage 0. The small fixed workloads are
comparison baselines, not production-scale performance claims.

Observed starting points:

- `WaitTask` rejects a direct GameThreadDeferred target on GameThread but does
  not reject a Worker target with an unfinished GameThreadDeferred ancestor.
  The Worker -> GameThreadDeferred -> Worker graph can therefore block its pump.
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
  optional related task id; detailed strings remain diagnostics.
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

- [ ] Reject transitive GameThread waits; cover direct, multi-hop, terminal,
  self-wait, and Worker-helping cases without introducing callback pumping.
- [ ] Add structured admission errors for capacity, closed lifetime/group,
  invalid prerequisite, unsupported executor, and invalid payload declarations.
- [ ] Keep legacy launch wrappers; route new graph construction through the
  explicit admission result and preserve unique inputs after rejection.
- [ ] Audit affected wait callers for ignored WaitStatus, beginning with
  package requests; rejected waits must not synthesize a false task completion.

Acceptance: the Stage 0 deadlock regression passes, rejected construction is
locally diagnosable, and all accepted nodes retain exactly-once terminal state.

### Stage 2: Implement ownership-preserving composition

Depends on Stage 1.

- [ ] Add unique `T -> U`, `T -> void`, and `void -> U` continuation support
  and explicit sharing without losing move-only callable support.
- [ ] Implement ThenAsync with inner-task pinning, terminal propagation,
  cancellation, dynamic dependency registration, and cycle rejection. Define
  cancellation of a shared inner task as local to the observing operation;
  never implicitly cancel unrelated consumers.
- [ ] Add dynamic and heterogeneous fan-in, including empty input, void gates,
  mixed failure/cancellation, and deterministic outcome ordering. Reject
  duplicate unique consumption; shared duplicates remain permitted.
- [ ] Add external completion sources with exactly-once publication,
  abandonment, late completion, close, and storage-lifetime tests.

Acceptance: a multi-stage move-only pipeline and a dynamically spawned inner
operation compose without side-channel completion flags or blocking waits.
Run rejection and cancellation at every edge, including shutdown races; every
payload is consumed or destroyed exactly once outside internal locks.

### Stage 3: Integrate groups and bounded operation completion

Depends on Stage 2.

- [ ] Add external-root close, counted child admission, and nonblocking Join;
  integrate dynamic and external-source work into group quiescence.
- [ ] Preserve module-generation ownership and extend retained-storage audits
  to new task results, completion sources, and operation tickets.
- [ ] Implement the bounded operation completion/ticket mechanism needed by
  the pilots, with no arbitrary user code under scheduler or owner locks.
- [ ] Test saturation, owner destruction, supersession, callback exception,
  cancel escalation, drain, and retained handles across module retirement.

Acceptance: accepting a request guarantees observable terminal completion even
when deferred dispatch is saturated or closed. Join never authorizes unloading
code that is still retained in a result, callable, or external completion source.

### Stage 4: Separate execution domains and scheduling policy

Depends on Stage 3; land before production pilot cutover.

- [ ] Introduce the bounded blocking-I/O executor and integrate its shutdown,
  owner accounting, and waits with task-system lifecycle.
- [ ] Make CPU root and continuation priorities effective and test bounded
  fairness so continuous interactive submissions do not starve background work.
- [ ] Replace implicit ParallelFor serial selection in the new API with
  explicit Auto/Serial/ExplicitBatch policy; retain legacy behavior in adapters.
  Select Auto using measured Release workloads and record its decision rule.
- [ ] Measure mixed blocking I/O and CPU work, skewed batches, and nested work.
  Record whether shared-queue contention warrants a separate scheduler plan.

Acceptance: blocked I/O does not occupy CPU Workers; configured priorities have
observable scheduling behavior; all executor lifetimes drain without abandoned
nodes. Performance claims include profile, hardware, workload, sample count,
median/p95, and the Stage 0 baseline rather than Debug-only timing conclusions.

### Stage 5: Migrate and qualify production operations

Depends on Stages 1 through 4.

- [ ] Migrate package reads and transforms to the I/O executor and composable
  results. Preserve synchronous boundary semantics, cancellation, range/error
  handling, and resource retirement while removing redundant completion state.
- [ ] Migrate the Texture2D compute/result chain; preserve its priority policy,
  memory budget, durable mailbox, request identity, and owner-thread commit.
  Track the represented operation through final commit, not just Worker return.
- [ ] Compare before/after call sites: count auxiliary completion flags, mutex/
  CV result handoffs, manual publication paths, and adapter configuration.
  Record each retained mechanism's subsystem purpose; no fixed LOC target.
- [ ] Run focused correctness, lifecycle, and mixed-load qualification. Include
  large payloads above deferred queue limits and saturation during shutdown.
- [ ] Publish implemented contracts in the owning Runtime documentation and
  compatibility examples. Remove only obsolete pilot glue; list remaining
  legacy callers as a bounded follow-up, not an implicit completed migration.
- [ ] Validate changed documentation and all plans; record evidence and mark
  this plan completed only after every acceptance gate passes.

Acceptance: both pilots express their operation with composable outcomes and
explicit ownership, without rebuilding a generic future in subsystem code.
Existing asset behavior and module-unload guarantees pass regression coverage.
If the new API still requires equivalent duplicate completion machinery, revise
the contract and affected stages before declaring the refactor complete.

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
