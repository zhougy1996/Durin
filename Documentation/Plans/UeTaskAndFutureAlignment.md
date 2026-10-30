# UE Task And Future Alignment Plan

Summary: Separate scheduled tasks, task events, and external result delivery while preserving cancellation, producer retirement, and shutdown drain guarantees.

Last reviewed: 2026-10-30

Status: Completed
Completed: 2026-10-30

## Current Status

Follow-up Stage 5 removed `ThenAsync`: the workspace had no production callers.
Task composition now uses explicit prerequisites and `AddNested`, with child
results retained separately. Automatic task flattening and inner failure/result
forwarding are no longer selected contracts. Save adapters and empty fan-in keep
their private registered result machinery in this stage. Stages 0–4 evidence
below describes the original acceptance; Stage 5 acceptance is recorded next.

Stage 5 passed on tested `Engine` subtree
`671af5510dc612a1bb028baa9d49541dd0fe6b9b`, based on `3ec6fa02c`, using
`windows-msvc-x64` / `Win64-Debug-DurinEditor`, MSVC 14.44 and 14 build jobs:

- CoreConcurrencyTests: 239 passed; report
  `Build/UeTaskAndFutureAlignment/Stage5-Core.xml`.
- Nested composition, dependent result views and void fan-in: 19 cases passed
  serial isolation; report `Build/UeTaskAndFutureAlignment/Stage5-Isolated.xml`.
  Events synchronize pending-child and deferred-executor cases without sleeps.
- Incremental workspace `build all` passed, including Engine, Sandbox and
  RoadWeaver; log `Build/.agent-state/logs/20261030-173940-247048-4832-cmake.log`.
- Source/test audit found no remaining `ThenAsync` or `TInnerTask` across the
  three workspace projects. Runtime bodies and domain save adapters are unchanged;
  fresh execution is bounded to Core composition/lifecycle coverage, with all
  template consumers compiled. Original integration receipts remain historical.
- Changed-document and all-plan validation passed. Only documentation evidence
  changed after runtime verification; no Stage 5 gates remain.

Stage 0 contract and ownership audit completed against
`d724be255599a8e532e2719701fc4e8e9dda3e59`. The refreshed source/test inventory
is retained in ignored `Build/UeTaskAndFutureAlignment/Stage0Inventory.txt`.
Engine remains the only workspace project with direct callers. Searches cover
completion sources/accessors, detached construction, ready tasks, Tool aliases
and handlers, save APIs/cancellation, and `GetTaskHandle` consumers.

Texture initialization has already migrated to accepted render commands and
`ResourceFence`; the historical `TextureInitialization` row needs no source
replacement. Preserve that implementation and validate its lifecycle coverage.
Stage 1 implemented scheduler-independent futures and Core-owned readiness
markers. Win64-Debug-DurinEditor / windows-msvc-x64: all 9 `FFutureTests.*`
passed both together and serially isolated via `CoreConcurrencyTests` on the
Stage 1 working tree based on `99867ee44`. The deterministic race uses a
three-party barrier for publication/subscription/competing publication, and
latches hold a notification in flight during detachment. Tests verify zero
scheduler counter changes and unresolved-future survival across two restarts.
Reports: `Build/NativeTestResults/Win64-Debug-DurinEditor/CoreConcurrencyTests.xml`
and `Build/UeTaskAndFutureAlignment/Stage1-Isolated.xml`. The initial sandbox
MSVC C1902 was resolved by running the same build outside the sandbox.
Stage 2 migrated Tool results to `TSharedFuture<FToolResult>` and move-only
`FToolPromise`, including editor receipts, Python observations and MCP/Python
fixtures. IToolset feature version is 3. All workspace alias searches were
repeated; no old Tool completion alias remains. On the Stage 2 tree based on
`9b5a5ad8a`, Win64-Debug-DurinEditor passed `@domain=tools` (CoreToolTests and
EditorOperationTests), MCPIntegrationTests (19), PythonToolIntegrationTests (48),
and MCPPythonJobTests (14). Reports are `Build/UeTaskAndFutureAlignment/Stage2-*.xml`.
The added editor test destroys the service while retaining a receipt, then
publishes from a late producer thread; the closed receipt remains canceled.
No production handler launches unowned asynchronous work. Provider ownership
requirements remain separate from receipt/observation lifetime.
Stage 3 migrated cooked-texture readiness, RHI observer completion and package
admission to events with separate domain state, and introduced Core's private
typed save-result adapter. Texture initialization retains its existing render
command/fence path. On the Stage 3 tree based on `8641bf4dd`, the default Windows
profile/preset passed PackagePersistenceTests (34), RHIPipelineCreationTests
(17), TextureTests (150), and TextureImportWorkflowTests (24). Reports are
`Build/UeTaskAndFutureAlignment/Stage3-*.xml`. Existing package fault-injection
coverage exercises rejected admission, canceled publication, owner cleanup and
returned-task cancellation; Texture coverage retains revision/owner invalidation
and pending-load behavior. The new RHI promise/latch test holds compiler work
past observer cancellation and checks retirement after StopAndWait. All generic
RHI handle consumers were audited: they use completion/wakeups, while request
state/result controls domain behavior. No production public-source uses remain
outside Core's transitional machinery; Stage 4 removes that machinery's public
surface; final acceptance is recorded below.

Stage 4 validation decisions: the final render gate exposed a pending RHI
observer released during resource replacement. Final observer destruction must
signal its event while shared compiler retirement remains joined by LastDrain;
a deterministic dropped-observer regression covers that distinction. It also
exposed an existing RDG test assuming independent tasks execute on a different
thread. RDG already uses LaunchIndependentTask and helping waits; update the
fixture to check independent CPU authority and ordered result delivery instead
of OS thread identity. No RDG scheduling implementation or wait policy changes.

### Original acceptance (Stages 0–4)

Stages 0–4 are complete. Final tested source/test inputs are the `Engine` Git
subtree `33ebf2e13a8c983ce7ef1aef3c7ae8053037778e`, based on implementation revision
`0469417e3`; the completion commit retains this exact subtree. Only documentation
receipts changed after the last runtime verification. Configuration:
`windows-msvc-x64`, `Win64-Debug-DurinEditor`, MSVC 14.44, 14 build jobs.

- Final `build all` passed; receipt:
  `Build/.agent-state/logs/20261030-171202-013434-38440-cmake.log`.
  This incremental workspace build covers Engine, Sandbox and RoadWeaver.
- CoreConcurrencyTests: 239 passed, covering Tasks/Futures, composition,
  cancellation, dropped handles, shutdown and module retirement. Report:
  `Build/UeTaskAndFutureAlignment/Stage4-Core.xml`. Core inputs were unchanged
  by the later RHI observer fix and RDG fixture correction, so this result is
  reused. All 13 Future cases and the new drain race also passed serial isolation
  in `Final-FutureIsolated.xml` and `Final-DrainRaceIsolated.xml` in that directory.
- Final native targets passed: `@domain=tools` (CoreToolTests,
  EditorOperationTests), PackagePersistenceTests (34), RHIPipelineCreationTests
  (18), RHICommandListTests (145), RHIThreadTests (20), RenderContractTests (243),
  TextureTests (150), TextureImportWorkflowTests (24), MCPIntegrationTests (19),
  PythonToolIntegrationTests (48), MCPPythonJobTests (14). Their reports are
  `Build/UeTaskAndFutureAlignment/Final-*.xml`. The Tool/package reports precede
  the RHI-only final fix; their relevant Core, Tool and save inputs are unchanged.
  RHI, render, Texture and protocol integrations passed with the final RHI fix.
- Deterministic regressions use barriers/latches or promises/events: publication
  racing subscription/setters; in-flight callback detachment; observer cancel or
  release before compiler exit; closed admission before event signal followed by
  deferred publication before producer capture destruction. Existing package
  injected rejection/canceled-publication gates continue to pass. Checked promise
  storage-move exceptions publish failure rather than leaving consumers pending.
- Final workspace source/test searches found no `TCompletionSource`, old Tool
  completion alias, completion-source accessor, detached task-node constructor,
  or MakeReadyTask. The empty audit is retained as
  `Build/UeTaskAndFutureAlignment/Stage4RemovedSymbols.txt`. Internal registered
  graph result delivery is confined to Core implementation details and the
  narrow private save adapter. Detached observation coverage moved to Future
  tests; generic task lifetime coverage still constructs actual tracked tasks.
- Lasting boundaries are documented in TaskSystem, Tools, EditorTools, package
  persistence, RHI API stability, runtime lifecycle and module retirement.
  Application-hosted/GPU qualification was not selected: no application-host or
  native GPU behavior changed; CPU fake-RHI/lifecycle tests cover this migration.
  No required gate remains open.

### Frozen Future API

The types live in `Durin`, declared by `Threading/Future.h`:

- `EFutureState { Invalid, Pending, Succeeded, Failed, Canceled }`;
  `EFutureFailure { Exception, Abandoned }`; `FFutureFailure` contains the code
  and an optional `std::exception_ptr`. Domain errors remain values.
- `TPromise<T>`: default construction creates a state; move-only; `GetFuture()`
  retrieves its sole `TFuture<T>` once. Destruction/move assignment abandons an
  unresolved old state. `TrySetValue(T)` (no argument for void),
  `TrySetFailure(FFutureFailure)`, and `TrySetCanceled()` return false after the
  first publisher wins. Checked `SetValue`, `SetFailure`, `SetCanceled` reject
  duplicate publication. A throwing move into result storage publishes Exception failure.
- Both consumer types: default invalid construction, `IsValid()`, `IsReady()`,
  `GetState()`, `Wait()`, `WaitFor(std::chrono::duration)` returning readiness,
  `GetFailure()` for Failed, and repeatable `Get()` returning `const T&` (void
  returns nothing). Invalid access and non-successful Get throw; they never
  synthesize a successful value. Wait performs only condition-variable waiting.
- `TFuture<T>` is move-only; `GetMutable()` returns `T&`; `Consume()` moves the
  value and invalidates the consumer, including when moving throws. Void has
  Get/Consume but no mutable reference. `Share()` transfers ownership into a
  copyable `TSharedFuture<T>` and invalidates the unshared handle.
- Shared futures expose `GetResultShared()` as a nonblocking
  `std::shared_ptr<const T>` (empty unless successful) and `WithLifetime(pin)`
  returning another shared handle. Pins attach to the shared result state and
  survive every retained handle/result view. Shared futures cannot consume or
  obtain mutable references. `MakeReadyFuture(T)` returns an unshared ready
  future; Tool construction explicitly shares it.
- `Subscribe(function<void(EFutureState)>)` returns move-only
  `FFutureSubscription`; `Reset()`/destruction detaches. Registration racing
  publication invokes exactly once, possibly inline. Reset prevents callbacks
  not yet pinned for invocation; an already pinned callback may finish. Callback
  exceptions are contained, callbacks and capture destruction occur outside the
  state mutex. A callback must not block waiting for another callback in the
  same notification pass. `FFutureReadiness` and `ObserveReady()` use Core-owned
  marker code so late notification cannot reenter a retired host module.

These are intentionally separate from task states/failures and cancellation
requests. No implicit task conversion, pumping, scheduler counters, or Then/Next
API exists. As selected above, Get is repeatable reference access rather than
standard future's consuming get; Share/Consume transfer only consumer ownership.
Mutable access and producer-progress-thread wait guards remain caller duties.
Module owners must retire callbacks and result deleters before physical unload;
attaching an ownership pin does not itself install a module unload protocol.

### Producer ownership mapping

| Producer | Admission and signal | Capture retirement and owner/process join |
| --- | --- | --- |
| Tool native handlers and service receipts | InvokeRegistered visitor owns synchronous provider dispatch; invocation promise is installed before Run. Pending providers return standalone shared futures. Service observation marks readiness only; Tick resolves domain output. | Complete/Close detach observation and release Raw without waiting on GameThread. Future marker code is Core-owned. There are no production AsyncHandler assignments in the refreshed workspace; Python tool bindings are synchronous and PythonRuntime separately owns job sessions. Future asynchronous handlers must retain their own accepted task/work gate and join it before provider shutdown. Service Gate protects admission, not asynchronous producer retirement. Retained Charge/result views contain no service/editor pointer. |
| Texture initialization | Existing InitTextureResource render command, followed by ResourceFence.BeginFence. | ResourceFence/FlushRenderingCommands joins the accepted command through capture destruction; object destruction waits the resource fence. Module producers flush before render/RHI shutdown. No Future or new event bridge is needed here. |
| Cooked texture decode | Register GameThreadDeferred publication behind an event before ReloadAsync/Transform; Candidate writes precede signaling on success, error, or exception. | PackageResource.Transform is an accepted ThenCompleted/LaunchTask, whose callable owns the decode captures through retirement. Request completion/Wait joins that task; process CPU drain also tracks it independently of the early event. Publication owns weak object identity and shared Candidate, never a raw retiring texture. Retain revision/request checks. An event does not replace the Transform task or resource I/O ownership. |
| RHI pipeline observer | MakeObserver precedes queue insertion; Publish stores Published/Outcome before signaling. Observer Cancel stores Canceled then signals without canceling compilation. | LastDrain owns Drain and compiler inputs; StopAndWait closes queue admission, clears queued entries and joins LastDrain before ReleaseResources. Observer completion is only a wake dependency. RHICommandList and RHIPipelineCreationTests subscribe for wakeups; PipelineStateCache uses completion only. All read domain state/result for outcomes. |
| Package admission | Register disk task behind event, then publication task and AsyncSaves owner entry before setting explicit admitted=true and triggering. Rejection leaves decision=false and triggers. | Disk owns Save; FinishAsyncSave clears Write and moves/calls/clears Finish on GameThread before erasing AsyncSaves. Poll/DrainAsyncSaves handles canceled publication. Host save drain precedes object/executor teardown. |
| Package and asset typed saves | Register private external result node before SubmitAsyncSave. Finish callback reads domain token plus returned-task cancellation, performs Complete/Cancel and resets Operation before result publication. Setup rejection settles the registered node. | Result-ready can precede Finish capture destruction. AsyncSaves membership and accepted Package.SavePublication, plus FinishAsyncSave fallback, remain the retirement obligation until Finish is cleared and owner-list removal completes. DrainAsyncSaves is required before objects/executors retire. Do not treat result readiness as this join. |
| Task composition and fan-in | Use prerequisites for known work and AddNested inside running bodies for dynamically selected children. Empty fan-in retains its private registered result node. | Nested dependencies extend completion only; child results and outcomes remain explicit. Scheduler graph drain and owning result views preserve retirement. |

### Frozen private adapter boundary

Retain registered external task nodes in Core's private implementation namespace;
remove public source construction after caller migration. Only composition and
audited typed save adapters construct them. Domain adapters retain existing
`TTask<Result>` signatures, attribution and cancellation queries. Registration
precedes external work; one publication wins, unresolved producer destruction
fails with AbandonedSource, and cancellation requests do not settle or retire the
producer. Rejection must resolve every registered node. No detached constructor
is available to domains. A registered node accounts for pending result delivery;
where publication precedes capture destruction, the specific accepted producer
or owner join in the table remains mandatory. No extra waiting worker, polling
task, late submission, or public promise facade is introduced.

## Goal

- Keep `Tasks::TTask<T>` as a copyable scheduled-work/result handle with shared
  result lifetime and reference-returning `GetResult()`.
- Keep `Tasks::FTaskEvent` as a result-free, one-shot signal usable as a task
  prerequisite or nested completion dependency.
- Introduce scheduler-independent `TPromise<T>`, `TFuture<T>`, and
  `TSharedFuture<T>` for external data delivery. Remove public
  `Tasks::TCompletionSource<T>` after all consumers migrate.
- Preserve accepted-work drain, cancellation behavior, result ownership, and
  callback/capture retirement. A ready result does not by itself prove that its
  producer has stopped accessing resources or executing module code.

Existing implemented contracts remain authoritative until their implementation
stages land: [CPU task system](../Runtime/Core/TaskSystem.md),
[runtime lifecycle](../Runtime/Application/RuntimeLifecycle.md), and
[module retirement](../Runtime/Core/ModularFeaturesAndModuleRetirement.md).

## Selected Design

### Task and event boundaries

`FTaskEvent` has no result or terminal failure/cancellation payload. Trigger means
the signal was delivered; domain success is stored separately. Dependencies wait
for completion, not success. Do not replace a canceled completion source with an
event without migrating consumers that inspect task state or use success-only
`Then`. Event creation alone does not register work for scheduler drain.

Keep existing event copy, repeated-trigger, prerequisite, and lifetime behavior.
Do not change its scheduler-independent nature or weaken existing wait-thread
restrictions to match UE superficially.

### Promise and future boundaries

Implement a small shared result state using standard-library ownership,
synchronization, and storage primitives; do not expose `std::future` or
`std::shared_future` as aliases for the new interfaces. The public header is
`Threading/Future.h`, independent of scheduler startup and task-node admission.

- `TPromise<T>` and unshared `TFuture<T>` are move-only. Retrieve the future once;
  `Share()` transfers it into a copyable `TSharedFuture<T>`.
- `Get()` waits and returns a const reference without invalidating the future.
  Unshared `GetMutable()` exposes mutable access and `Consume()` transfers the
  result and invalidates that future. Shared consumers cannot consume the value.
  Provide appropriate void behavior. Synchronizing mutable access is the caller's
  responsibility.
- Preserve one-shot publication, readiness queries, timed waits, and move-only
  results. Provide checked setters and explicit `TrySet...` operations for races;
  exact names and signatures are frozen in Stage 0.
- Distinguish infrastructure failure/abandonment from a successful value containing
  a domain error. Retain explicit failure/canceled completion where current code
  depends on it; requesting cancellation remains separate from acknowledging it.
  An unresolved promise must wake retained consumers on destruction with an
  abandonment outcome. Never manufacture a default successful value.
- Result storage and optional attached lifetime pins survive until the last
  relevant consumer releases them. Support a nonblocking owning immutable result
  view for ToolService receipts without copying their payloads.
- Provide detachable readiness observation, including completion racing with
  subscription. Unsubscription must have a defined in-flight callback contract.
  Invoke callbacks outside state locks; observer callbacks cannot assume a
  GameThread context or submit tasks after admission closes.
- Futures do not pump GameThread, execute task work, participate in scheduler
  diagnostics/drain, or become task prerequisites implicitly. Waits on a producer's
  required progress thread are forbidden; domain APIs retain their wait guards.
- Full UE `Then`/`Next` continuation parity is deferred. Initial delivery needs
  readiness observation and explicit task/domain bridges, not a second scheduler.

The standard future differs materially: its `get()` consumes and invalidates its
state, and it has no standard `Then`/`Next`. UE separates reference access from
`Consume()`. See [UE Future API](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/TFuture)
and [UE Get contract](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/TFutureBase/Get).
UE Tasks retain their own shared task/result model; see the
[UE Tasks reference](https://dev.epicgames.com/documentation/en-us/unreal-engine/tasks-system-references-in-unreal-engine).

### Explicit integration with the scheduler

Retain Core's external-completion node machinery as private infrastructure for
composition and audited domain adapters. Do not mechanically rename the public
completion source into a public promise that still constructs task nodes.

An external adapter must register its scheduler-owned lifetime before starting its
producer. It may expose an `FTaskEvent` signal and/or an existing typed task API,
but must hold a tracked operation until producer acknowledgement and capture
retirement. Reuse the current registered external node where needed rather than
adding an untracked callback, waiting worker, or polling task. Registration and
publication dispatch must require no new task submission after admission closes.

Result publication and producer retirement are distinct transitions. If the
callback signals before its captures are destroyed, retain a separate tracked
retirement obligation or an already accepted producer task/command with a proven
join. Releasing the observation handle never cancels or retires a producer.
Private bridge access across modules must use a narrow Core-owned adapter
boundary, not arbitrary calls into scheduler internals.

## Existing Call-Site Migration

Paths below are relative to the repository root. They identify concrete migration
sites, including aliases and internal construction rather than only literal type
names.

| Current site | Migration and required preservation |
| --- | --- |
| `Engine/Source/Runtime/Core/Public/Tools/Tool.h`: `FToolFuture`, `FToolCompletionSource`, `ReadyToolResult` | Change the copyable result alias to `TSharedFuture<FToolResult>` and use a move-only promise owned by invocation state. Replace ready detached tasks with ready futures. Preserve domain errors in `FToolResult`, infrastructure abandonment, immutable owning result views, and receipt lifetime pins. |
| `Engine/Source/Editor/DurinEd/Private/Tools/EditorToolService.cpp`: `FInvocation` detached completion | Replace `CreateDetached`/`TakeTask` with promise/future construction and sharing. Migrate `Raw`, `ObserveReady`, `IsCompleted`, state inspection, `GetResultShared`, and `WithLifetime` together. Preserve accounting while retained receipts outlive service closure. Never run provider or service-mutating callbacks under the future lock. |
| `Engine/Source/Runtime/Core/Public/Tools/NativeToolset.h`, `Private/Tools/NativeToolset.cpp`, and `Editor/DurinEd/Public/Tools/EditorToolService.h` | Migrate signatures and all providers/consumers of the aliases. Provider tasks require an explicit adapter, with provider work still owned and drained by its domain. Service closure may detach result observation; it is not permission to unload provider code or destroy provider resources. |
| `Engine/Source/Runtime/Engine/Private/Texture/Texture.cpp`: `TextureInitialization` | Use an event for the render-to-GameThread handoff. Register handoff before render admission. Preserve resource-update outcome in `FTextureResourceUpdate`; migrate success-only continuation semantics explicitly. Preserve an accepted render-command join or private tracked retirement bridge through callback/capture destruction. |
| `Engine/Source/Runtime/Engine/Private/Texture/TextureCookedData.h`: `CookedTextureDecode` | Use an event for decode/publication readiness, retaining `Candidate` as the domain result. Pre-admit publication before `ReloadAsync`. Every success/error/exception/cancel path must signal once after result writes. Replace the current exit guard carefully: callback exit readiness is not proof of capture retirement. Keep request/producer retirement joined separately and preserve revision/owner invalidation checks. |
| `Engine/Source/Runtime/RHI/Private/RHIPipelineCreation.cpp`: `FState::Source` and `Task` | Replace the observer completion with an event; keep published data and `Outcome` as domain state. `GetTaskHandle()` remains a completion dependency, but clients must inspect request outcome rather than expect `Canceled` from the generic handle. Audit every consumer of that method. Individual observer cancellation signals its event without canceling shared compilation. `StopAndWait()` must still join `LastDrain` and retire compiler captures before resource release. |
| `Engine/Source/Runtime/CoreDObject/Private/DObject/PackagePersistence.cpp`: `Package.SaveAdmission` | Replace the void source with an event plus explicit admission decision, written before triggering. Disk work checks that decision, not event task state. Rejected setup must release the gate and skip writes; injected rejection and canceled publication must remain deterministic. |
| Same file: `FSavePackageContext::SaveAsync`, `Package.SaveAsync` | Preserve the public `TTask<FPackageSaveResult>` return and task-handle cancellation in this migration. Use a private registered external result adapter until the domain save pipeline can return a real publication task with identical semantics. Do not change the return to Future or a success-only event wrapper. Settle the result once after operation cleanup, and retain producer retirement through `FinishAsyncSave` capture release and owner-list removal. |
| `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageOperations.cpp`: `FAssetPackageSaveContext::SaveAsync`, `Asset.SaveAsync` | Apply the same private typed adapter strategy, preserving `FAssetWriteResult`, cancellation requested through the returned task or domain token, GameThread commit, failure effects, and cleanup. Preserve admission-failure behavior without leaving an unacknowledged registered node. |
| `Engine/Source/Runtime/Core/Public/Threading/TaskComposition.h`: task composition and empty `BuildFanIn` | Stage 5 removes automatic task flattening. Use explicit dependencies and nested tasks; retain results independently. Keep empty fan-in and save-result machinery private without routing graph operations through standalone futures. |
| Same file: `MakeReadyTask` | Separate ready task construction from ready future construction. Tool results use the latter. Retain an actual ready-task helper only for task consumers that need it, with explicit scheduler-lifetime/dependency rules; do not retain detached observation-only tasks under a generic task name. |

Tests also constitute callers. Migrate source fixtures in CoreTests
`TaskCompositionTests.cpp`, `TaskCloseTests.cpp`, and `ThreadingTests.cpp` by
semantic purpose: event ordering, scheduler-tracked producer completion, private
composition, or standalone promise/future behavior. Move detached-result tests
to Future coverage; keep drain and module-retirement tests on tracked operations.
Migrate `ToolTests.cpp` and EngineTests `Editor/EditorToolTests.cpp`,
`MCPIntegrationTests.cpp`, and `PythonToolIntegrationTests.cpp` with the Tool API.
Do not delete coverage merely because a public source type disappears.

## Shutdown Drain Guarantee

The selected invariant is: after successful owner/process drain, no accepted
producer can still mutate retiring resources, execute retiring module code, or
enqueue required publication. A retained terminal result may outlive drain only
with the existing safe result/module ownership guarantees.

1. Close domain producer admission before enumerating pending work. Registration
   of every producer, publication consumer, and retirement obligation precedes
   starting external work. A producer that completes immediately follows the same
   ordering. Partial admission failure resolves all already registered gates.
2. Request cooperative cancellation where appropriate. Cancellation neither
   triggers an event nor proves producer retirement. Domain observer cancellation
   can complete observation while a shared producer remains tracked separately.
3. Drain owners while their required executors remain operational. Preserve the
   runtime lifecycle order: package save completion on GameThread; render/RHI
   commands, pipeline compiler joins, and resource retirement before their
   executors/resources disappear; CPU shutdown after render/RHI teardown. Keep
   GameThread deferred pumping available for accepted publication work.
4. Scheduler closure rejects new submissions but continues dispatching the
   pre-admitted graph. An accepted consumer waiting on an event remains tracked
   and prevents graph quiescence until signaled and retired. An unconsumed event
   or standalone future provides no such protection. Private external adapters
   retain registration until producer acknowledgement/retirement, even when the
   observation handle was canceled or released.
5. `FinishAsyncSave` must continue clearing write/finish captures on GameThread
   before reporting owner quiescence. The save result becoming ready inside its
   finish callback is not a substitute for this guarantee. Preserve the existing
   fallback finalization when normal publication fails.
6. ToolService close resolves public receipts and detaches subscriptions without
   waiting on its own GameThread provider. Provider work gates, domain joins, and
   module retirement remain responsible for running producers. No late callback
   may dereference a closed service/editor; state and retained results must be
   safe without scheduler availability.
7. Successful process drain requires zero accepted nonterminal nodes, running
   bodies, and executor queue entries, plus completed owner producer joins. A
   timeout or unsupported wait reports incomplete shutdown; it must not free
   resources as if drain had succeeded. Restarted schedulers cannot adopt old
   tasks; standalone futures and events retain their documented independent
   lifetimes.

## Implementation Stages

### Stage 0: Freeze contracts and producer ownership

- [x] Refresh the complete workspace symbol/alias inventory, including RHI
  `GetTaskHandle` consumers, save-task cancellation, Tool handlers, and tests.
- [x] Freeze Future failure, void, ownership, observation, and lifetime-pin APIs;
  document intentional differences from UE and `std::future`.
- [x] For each external producer, record its admission point, signal point,
  capture-destruction point, owner join, and process/module shutdown placement.
  Identify the precise accepted handle or private retirement registration that
  closes the signal-to-retirement gap. Resolve any missing join before migration.
- [x] Freeze the narrow private external adapter contract and its cancellation,
  failure, abandonment, accounting, and producer-retirement transitions. It must
  not become a new public completion-source facade.

Completion: every production row above has an executable ownership mapping and no
unresolved shutdown or result-publication contract. No API migration precedes it.

### Stage 1: Add scheduler-independent futures

Depends on Stage 0.

- [x] Implement `Threading/Future.h` and Core-owned support without task-node
  construction. Add ready future construction and required observation/pin APIs.
- [x] Validate move-only and void results, repeated publication, abandonment,
  consume/share invalidation, timed waits, publication/subscription races,
  detached observation, reentrant callbacks, and retained immutable views.
- [x] Validate creation and completion before startup, after shutdown, and across
  scheduler restart with no change to scheduler pending-node counters.

Completion: Future semantics pass focused Core tests independently of a scheduler.

### Stage 2: Migrate Tool results

Depends on Stage 1.

- [x] Migrate all Tool aliases, native handlers, service receipts, and integrations
  in the same API change across all workspace projects.
- [x] Preserve result accounting/pins, cancellation diagnostics, immediate and
  pending providers, close during dispatch, observer detachment, and late producer
  completion after editor closure without accessing retired host state.
- [x] Verify provider module/work-gate ownership independently from receipt
  completion; test abandonment and retained receipts after service destruction.

Completion: no Tool result is a detached task; Core Tool and affected editor/MCP/
Python integration coverage passes with the existing provider shutdown contract.

### Stage 3: Migrate event signals and save adapters

Depends on Stage 0; use Stage 1 only where actual data delivery requires it.

- [x] Migrate Texture, cooked Texture, RHI observers, and package admission using
  the row-specific outcome and retirement rules above.
- [x] Move typed package/asset save completion behind private tracked adapters;
  preserve public typed-task signatures and cancellation behavior.
- [x] Verify producer/observer cancel races, rejected setup, exceptions,
  disappeared owners, duplicate/late signals, and immediate completion.
- [x] Verify shutdown begun before producer completion requires no new submission,
  still pumps accepted deferred publication, and waits for capture retirement.

Completion: all production public-source uses are gone; affected CoreDObject,
Engine, RHI, and rendering lifecycle coverage demonstrates the drain invariant.

### Stage 4: Remove the public source and accept the final boundary

Depends on Stages 2 and 3.

- [x] Privatize remaining composition/source machinery, migrate tests by purpose,
  and resolve ready-task helper semantics. Search all workspace source/test roots
  for the removed public type, aliases, detached nodes, and old access patterns.
- [x] Validate flatten/fan-in behavior, failure propagation, shutdown with dropped
  handles, delayed producer acknowledgement, module retirement, and retained
  results; preserve cancellation requests separately from producer outcomes.
- [x] Add deterministic drain race tests using barriers/latches rather than
  sleeps: admission closes before signal, signal precedes capture destruction,
  observation cancels before shared producer exit, and admission setup fails.
- [x] Update lasting contracts in TaskSystem, Tool, runtime lifecycle, and module
  retirement documentation as applicable; record evidence and close this plan
  only after the final gates pass.

Completion: no public `TCompletionSource`, no detached data-only `TTask` escape,
and no producer lifetime protected only by a ready result or event.

### Stage 5: Replace task flattening with explicit composition

Depends on Stage 4. This follow-up supersedes the original flattening contract;
Stages 0–4 acceptance evidence remains historical.

- [x] Remove `ThenAsync` and its inner-task type trait; no production callers
  exist across Engine, Sandbox or RoadWeaver.
- [x] Replace flattening tests with explicit prerequisites, nested completion,
  independently retained result views, local cancellation and capture retirement.
- [x] Document that nested completion does not forward child results or outcomes.
- [x] Validate CoreConcurrencyTests, workspace template consumers and changed docs.

Completion: task flattening is absent from the public API and current contracts;
explicit composition coverage passes. Save adapters and empty fan-in remain private.

## Validation And Handoff

Follow [build selection](../Agents/BuildAndRun.md) and
[test selection and result reuse](../Agents/Testing.md). Resolve exact targets
from the test registry at implementation time; the inventory above identifies
semantic coverage, not permission to infer target names from filenames.

Each stage compiles owning modules and affected consumers across every workspace
project, with focused tests for changed behavior. Final acceptance includes one
`all` build because the Core public handle/result interfaces and Tool aliases
cross module/project boundaries and bounded builds can miss template consumers.
Run affected native coverage for Tasks/Futures, saves, Tool integrations, RHI
observer/compile lifecycle, and rendering shutdown. Application-hosted or GPU
execution is selected only when changed integration behavior requires it under
the linked workflows; any required unavailable gate remains open.

Record tested revision, configuration, selections, reproducible race setup, and
remaining gates in Current Status. Keep raw reports under ignored `Build/`.
Documentation-only planning requires changed-document validation and all-plan
validation, with no native build/test execution. Implementation commits update
this plan and use its exact path and stage title as provenance trailers.
