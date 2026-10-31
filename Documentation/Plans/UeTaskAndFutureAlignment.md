# UE Task And Future Alignment Plan

Summary: Separate scheduled tasks, task events, and external result delivery while preserving cancellation, producer retirement, and shutdown drain guarantees.

Last reviewed: 2026-10-31

Status: Completed
Completed: 2026-10-31

## Current Status

Stage 12 reorganizes task headers, implementation files and tests without changing
APIs, namespaces or runtime contracts. Task is the ordinary entrypoint; TaskRuntime
owns lower-level declarations and implementation. No compatibility forwarding
headers remain. Final tested subtrees, based on `03dec3a29`, are `Engine`
`f3b543c2fbc1ec6d54c1b0bee4b7225c3e66f636` and `RoadWeaver`
`4f9f9bde0d53991bb4ad10f746e89f0d9a54b891`, using `windows-msvc-x64` /
`Win64-Debug-DurinEditor`, 14 build jobs and serial isolation:

- The four renamed Core API/runtime files compare identically to their original
  declarations/implementations after include-path substitution. All 105 other
  include consumers compare as include-only changes. No obsolete source/header
  path or forwarding header remains in workspace source/build references.
- All 174 moved test bodies and suite/case names are unchanged. Native reports
  confirm the same complete 225-case inventory as Stage 11; CoreConcurrencyTests
  passed all 225 cases both in one process and in serial isolated processes.
- CoreToolTests passed 19 cases, RHIPipelineCreationTests 18, AssetPackageTests 207,
  TextureImportWorkflowTests 25, EditorAssetWorkflowTests 43 and
  AsyncTaskPilotLifecycleTests 1. Both real DynamicDllUnload qualification targets
  passed, retaining module/capture retirement coverage.
- Reports are under `Build/TaskFileLayout/Stage12-<Target>.xml`; shared Core and
  isolated Core reports are `Stage12-Core.xml` and `Stage12-Isolated.xml`.
  The workspace `all` incremental build and CoreConcurrencyQualificationTests
  compilation passed. No new timing or GPU claim is made.
- Affected analysis expands to all native tests because source registration and
  shared test headers changed. Bounded execution covers the moved tests and typed
  API consumers; the all build validates includes across workspace projects.
  Changed-document/all-plan validation and staged diff checks passed.

Stage 11 migrates task composition to ordinary launch prerequisites and explicit
result inspection. Remove Tasks Then/WhenAll/ThenCompleted and the private
continuation submission path; preserve attribution, subscriptions, event wait
restrictions and producer retirement. This supersedes the earlier convenience-API
design statements; the lasting contract is in TaskSystem and BulkData. Final
tested `Engine` subtree is `e33efa6fed55d1e4ada2b9b52f7047d73d8a729c`, based on
`71abad2e2`, using `windows-msvc-x64` / `Win64-Debug-DurinEditor` and 14 build jobs:

- CoreConcurrencyTests passed 225 cases; 98 task composition, close, executor,
  acceptance, construction and deferred-executor cases passed serial isolation.
- AsyncTaskPilotLifecycleTests passed 1 case, AssetPackageTests 207,
  TextureImportWorkflowTests 25, EditorAssetWorkflowTests 43, CoreToolTests 19,
  and RHIPipelineCreationTests 18. Both DynamicDllUnloadQualificationTests and
  DynamicDllUnloadFailureQualificationTests passed with the real migrated fixture.
- Reports are under `Build/UeTaskAndFutureAlignment/Stage11-<Target>.xml`, with
  isolation in `Stage11-Isolated.xml`. The initial migrated storage test exposed
  a race between public completion and dependent dispatch; restoring its original
  worker-retirement barrier fixed the fixture before the final passing runs.
- The workspace `all` incremental build and CoreConcurrencyQualificationTests
  compilation passed. The final test-only barrier adjustment was rebuilt by the
  Core test target; runtime sources, qualification harness and consumer inputs did
  not change after their passing builds. No new timing or GPU claim is made.
- Production observers retain explicit attribution and run on every predecessor
  outcome. Ordinary submission deduplicates dependencies; domain gather tasks own
  their result policy. All removed API/entrypoint symbols are absent from workspace
  source/tests. Subscriptions, event wait guards and producer retirement remain.
- Bounded targets replace the affected analysis's whole-Engine test expansion
  caused by the shared fixture header and Engine/MainFrame ownership. These cover
  the changed task, PackageResource, audit, module-unload and subscription paths;
  the all build covers remaining template consumers. Changed-document and
  all-plan validation passed.

Stage 10 removes unused task readiness markers and arbitrary task lifetime pins,
while retaining subscriptions, result-node ownership, event wait restrictions and
producer retirement. Duplicate standalone Future tests are consolidated into
the canonical ownership, notification and race cases. Final tested `Engine`
subtree is `3165fc8e5b6f2f1802176fba403c47ca3a3bce22`, based on `245cef6f0`,
using `windows-msvc-x64` / `Win64-Debug-DurinEditor` and 14 build jobs:

- All 11 affected test targets passed, including CoreConcurrencyTests and
  CoreToolTests (`Build/UeTaskAndFutureAlignment/Stage10-Affected.xml`).
- Fifty Future, task-close and event cases passed serial isolation
  (`Stage10-Isolated.xml` in that directory). RHIPipelineCreationTests passed all
  18 cases (`Stage10-RHI.xml`), covering the retained RHI subscription consumer.
- The `all` incremental build passed across workspace consumers. Sandboxed MSVC
  initially reported the documented C1902 PDB incompatibility; validation then
  passed outside the sandbox without source or toolchain changes.
- Task readiness/ObserveReady and WithLifetime/RetainLifetime are absent from
  workspace source/tests. Future markers and pins retain their production users;
  SubscribeTask and result-view node pins remain. Task lifetime coverage now uses
  an actual owned result, retaining aliases and an erased handle through shutdown.
- Three duplicate Future cases were removed; canonical tests retain publication
  races, detachable/in-flight observers, late subscription/readiness observation,
  late pin attachment and producer-held state ownership. Changed-document and
  all-plan validation passed. No event wait or producer retirement rules changed.

Stage 9 consolidated migration-era Core composition coverage without changing
runtime contracts. Combine terminal-observer coverage into one outcome/fan-out
matrix, place duplicate-input assertions in the primary fan-in case, and remove
legacy admission temporaries and incidental moves from shared results. Dedicated
wait, race, cancellation and retirement cases remain. Final tested `Engine` subtree is
`8dd14fd6b32c47829cabe9e755499de06f35208f`, based on `782d463d3`, using
`windows-msvc-x64` / `Win64-Debug-DurinEditor` and 14 build jobs:

- CoreConcurrencyTests passed 236 cases; report
  `Build/UeTaskAndFutureAlignment/Stage9-Core.xml`. Ten related cases passed
  serial isolation (`Stage9-Isolated.xml` in that directory).
- The two observer cases now share one three-outcome/two-observer matrix with
  move-only payloads/captures, full failure identity and invalid-executor coverage.
- Duplicate fan-in positions and shared result addresses are asserted by
  FanInPreservesCollectionAndHeterogeneousOrder. The removed combined duplicate/
  cancellation case's cancellation behavior remains in the dedicated nested and
  completed-edge cases; the deferred child now explicitly checks that no
  cancellation request propagated, not merely that its outcome was successful.
- Legacy admission temporaries, incidental result moves and repeated assertions
  were removed. Intentional rvalue-input and move-only result consumption checks
  remain. No wait, race or capture-retirement cases were removed.
- Affected analysis resolves only CoreConcurrencyTests. Production code and build
  configuration are unchanged; its test build covers this stage without another
  workspace build or save regression. Changed-document/all-plan validation passed.
  Only documentation evidence changed after final runtime verification.

Stage 8 migrated remaining Core tests to real scheduled tasks and events, then
removed private external-result sources and external scheduler nodes. Existing
Future coverage owns abandonment and competing setters. Events retain their
scheduler-independent lifetime, unknown-progress wait restrictions and nested
completion; actual producer tasks/joins retain capture retirement. Final `Engine` subtree is
`688c5783e28fe23aab524fa0f74eab97c19ee49e`, based on `bd1512eb7`, using
`windows-msvc-x64` / `Win64-Debug-DurinEditor` and 14 build jobs:

- Migrated tests first passed against the retained implementation: 238 cases in
  `Build/UeTaskAndFutureAlignment/Stage8-MigratedCore.xml`. Only then were the
  source/accessor and external-node branches removed. The one removed case's
  abandonment/competing setters are already covered by existing Future tests;
  void task outcomes and advisory cancellation retain task-specific coverage.
- Post-removal CoreConcurrencyTests passed 238 cases (`Stage8-Core.xml`).
  Composition/close, Future, independent-CPU and migrated executor/accounting
  cases passed 108 serially isolated cases (`Stage8-Isolated.xml`). Reports are
  in the same directory. Real producer tasks and capture-destructor signals
  exercise dispatch after closure and retirement independently of event readiness.
- PackagePersistenceTests (40) and TextureImportWorkflowTests (25) passed on final
  inputs; reports `Stage8-Package.xml` and `Stage8-AssetWorkflow.xml`. Save sources
  are unchanged. Original all-module affected selection expands by Core labels;
  execution is bounded to task/event/Future semantics and actual save consumers.
- Incremental workspace `build all` passed across Engine, Sandbox and RoadWeaver;
  log `Build/.agent-state/logs/20261031-143230-098383-46236-cmake.log`.
  Whole-Core and workspace results remain applicable across comment-only cleanup
  of stale Task.h text; subsequent save/isolation builds use that final header.
  No executable behavior, test fixtures or environment changed after those runs.
- Workspace source/test audits found no external source/accessor, adapter,
  completion flag/method, abandoned-source task code or external node predicate.
  Event unknown-progress checks, nested completion, terminal hooks and ordinary
  producer ownership remain intact. Changed-document/all-plan validation passed;
  only documentation evidence changed after final isolation. No gates remain.

Stage 7 replaced Package/Asset external result delivery with real typed
GameThread publication tasks. Admission events still gate I/O until publication
is registered. Save drain explicitly pumps the deferred executor through a
submission cutoff, rejects recursive drain and new saves during the pump, and
retires owner captures after task completion. Failed/discarded tasks retain a
GameThread cleanup fallback. Core external-result infrastructure is unchanged.
Stage 7 passed on tested `Engine` subtree
`e8f12b3ae4324d3c099b50867ed1bf5f34f97982`, based on `4137d9ff0`, with
`windows-msvc-x64` / `Win64-Debug-DurinEditor` and 14 build jobs:

- CoreConcurrencyTests (239), PackagePersistenceTests (40),
  TextureImportWorkflowTests (25), AssetSaveReadinessTests (3) and
  AssetPackageTests (207) passed. Reports are
  `Build/UeTaskAndFutureAlignment/Stage7-{Core,Package,AssetWorkflow,Readiness,AssetPackage}.xml`.
- Seven publication/drain Package cases and the Asset cancellation case passed
  serial isolation; reports `Stage7-Package-Isolated.xml` and
  `Stage7-Asset-Isolated.xml` in that directory. Coverage includes true deferred
  task identity, cancellation, I/O failure, throwing publication, discarded task
  cleanup, dropped handles, capture retirement before terminal observation,
  explicit pumping, nested accepted publication during fallback cleanup, and
  recursive drain/admission rejection.
- Incremental workspace `build all` passed across Engine, Sandbox and RoadWeaver;
  log `Build/.agent-state/logs/20261031-141348-829181-51100-cmake.log`.
- Source/test audits migrated all four production save submitters across the
  workspace. The prepared/direct void entrypoint shares the same task/owner
  path. No save producer includes ExternalTaskResult.h or instantiates an
  external result source; Core source/infrastructure files are unchanged.
- Affected selection expands by whole Engine/CoreDObject modules. Execution is
  bounded to the changed save lifecycles and their Core task contract; workspace
  compilation covers other consumers. Changed-document/all-plan checks passed.
  CoreConcurrencyTests is reused after the CoreDObject-only guard refinement;
  its Core source/test inputs are unchanged. Only documentation evidence changed
  after final save verification; no gates remain.

Stage 6 completed the removal of unreachable result forwarding and the dedicated
dynamic-edge machinery, and schedules empty fan-in
as ordinary work. Known work uses prerequisites; runtime-selected children use
nested completion. Empty fan-in now follows executor, priority and generation
rules instead of guaranteeing immediate completion. Save pipelines and their
registered external result adapters remain unchanged.

Stage 6 passed on tested `Engine` subtree
`77ea9f3d19c8cfcf0099166c20f5bcba0c1dd993`, based on `d2ced9590`, using
`windows-msvc-x64` / `Win64-Debug-DurinEditor` and 14 build jobs:

- CoreConcurrencyTests: 239 passed; report
  `Build/UeTaskAndFutureAlignment/Stage6-Core.xml`.
- Six graph/empty-fan-in cases passed serial isolation, including late event
  prerequisites, cycle rejection, nested pin release, waiter notifications and
  independent-CPU restrictions; report `Stage6-Isolated.xml` in that directory.
  Events control pending dependencies; no sleeps were introduced.
- Unchanged save consumers passed on final inputs: PackagePersistenceTests (34)
  and TextureImportWorkflowTests (24); reports `Stage6-Package.xml` and
  `Stage6-AssetWorkflow.xml` in that directory.
- Final incremental `build all` passed across Engine, Sandbox and RoadWeaver;
  log `Build/.agent-state/logs/20261031-134416-085679-35824-cmake.log`.
- Source/test audits across all workspace projects found no removed forwarding,
  binding or failure-code symbols. Production `WhenAll` has no callers outside
  its definitions, and only save adapters still create registered result nodes.
  Execution was bounded to Core graph/result behavior and those save consumers;
  all template/ABI consumers were compiled. Package/asset save sources and the
  private save adapter have no changes.
- Changed-document and all-plan validation passed. Only documentation evidence
  changed after runtime verification; no Stage 6 gates remain.

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
| Package admission | Register disk work behind an event, then its real publication task and owner entry before admission=true and Trigger. Rejection leaves admission=false and triggers. | Publication consumes the domain finish callback on GameThread. Poll/drain retires terminal owner records and supplies discarded-task cleanup. Drain explicitly pumps its publication cutoff before executor/object teardown. |
| Package and asset typed saves | Stage 7 registers a real typed GameThread publication task before releasing I/O. Its body checks domain and task tokens, commits/cancels, destroys operation captures and returns its result. | Owner records retain cleanup until I/O and publication are terminal. Discarded tasks remain failed while domain cleanup runs on GameThread. Drain explicitly pumps and reaps its cutoff; no external result bridge is used. |
| Task composition and fan-in | Use prerequisites for known work and AddNested inside running bodies for dynamically selected children. Empty fan-in uses ordinary scheduled work. | Nested dependencies extend completion only; child results and outcomes remain explicit. Scheduler graph drain and owning result views preserve retirement. |

### Frozen task/event boundary

Stage 8 removes the private external-result source and external scheduler node.
Task bodies produce task results; signals use task events and external data
uses independent futures. Actual producer tasks or domain joins cover callback
and capture retirement. A pending future or an event without an admitted
consumer does not count as scheduler-owned work. Cancellation requests do not
prove either result publication or producer retirement. Pre-admit publication
before producer work begins so closure needs no new task submission.

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

Stage 8 removes Core's external-completion node machinery after production and
test migration. Scheduled work returns task results; external data uses futures.
Do not introduce a promise facade that constructs task nodes.

Pre-admit task publication before starting an external producer. Its readiness
may be an `FTaskEvent`, but retain the actual producer task or a domain join
through acknowledgement and capture retirement. Do not add an untracked callback,
a waiting worker or a polling task as a substitute. Registration and publication
dispatch require no new task submission after admission closes.

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
| Same file: `FSavePackageContext::SaveAsync`, `Package.SaveAsync` | Preserve the public `TTask<FPackageSaveResult>` return and task-handle cancellation in this migration. Stage 7 returns a real typed GameThread publication task and explicitly pumps it during save drain. Do not change the return to Future or a success-only event wrapper. Return the domain result after operation cleanup and retain owner accounting until task terminal publication and GameThread reaping. |
| `Engine/Source/Runtime/Engine/Private/Asset/AssetPackageOperations.cpp`: `FAssetPackageSaveContext::SaveAsync`, `Asset.SaveAsync` | Apply the same real publication-task strategy, preserving `FAssetWriteResult`, cancellation requested through the returned task or domain token, GameThread commit, failure effects, and cleanup. Preserve admission-failure behavior without leaving an unacknowledged registered node. |
| `Engine/Source/Runtime/Core/Public/Threading/Task.h`: ordinary task API (historical composition helpers retired in Stage 11) | Stage 5 removes automatic task flattening. Use explicit dependencies and nested tasks; retain results independently. Stage 6 schedules empty fan-in as ordinary work and removes obsolete result forwarding and dynamic edges. Keep save-result machinery private without routing graph operations through standalone futures. |
| Same file: `MakeReadyTask` | Separate ready task construction from ready future construction. Tool results use the latter. Retain an actual ready-task helper only for task consumers that need it, with explicit scheduler-lifetime/dependency rules; do not retain detached observation-only tasks under a generic task name. |

Tests also constitute callers. Migrate source fixtures in CoreTests
`TaskTests.cpp`, `TaskLifecycleTests.cpp`, `TaskPipeTests.cpp`,
`TaskConcurrencyLimiterTests.cpp`, `TaskSchedulerTests.cpp`, and `ThreadingTests.cpp` by
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
   or standalone future provides no such protection. Actual producer tasks or domain joins
   retain ownership through acknowledgement/retirement, even when an observation
   handle was canceled or released.
5. Save publication bodies release operation captures on GameThread before
   returning a result. Owner reaping joins I/O/publication terminal states and
   retains GameThread fallback cleanup for publications whose bodies never run.
   Drain explicitly pumps its cutoff while rejecting recursive drain/admission.
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

### Stage 6: Remove obsolete forwarding and dynamic edges

Depends on Stage 5.

- [x] Remove unused result forwarding storage, publication and iterative release.
- [x] Remove dynamic-edge binding, state, its unused failure code and
  traversal/retraction special cases.
- [x] Migrate graph tests to prerequisites, nested tasks and task events; cover
  empty fan-in executor and generation behavior as ordinary scheduled work.
- [x] Update current contracts without changing package/asset save pipelines.
- [x] Validate Core concurrency and save consumers, workspace builds and docs.

Completion: obsolete symbols have no workspace references, empty fan-in uses
ordinary tasks, and focused lifecycle/consumer coverage passes.

### Stage 7: Unify save publication and drain with typed tasks

Depends on Stage 6.

- [x] Return real typed publication tasks from Package/Asset SaveAsync.
- [x] Share admission and owner retirement with direct/prepared saves; retain
  GameThread cleanup for failed/discarded publications.
- [x] Drain by explicitly pumping publication tasks, with cutoff and reentry guards.
- [x] Cover cancellation, rejection, exceptions, dropped handles and retirement;
  synchronize the plan and lasting contracts without changing Core infrastructure.
- [x] Validate save consumers, Core concurrency, workspace builds and documents.

Completion: no save producer uses external result access; task completion follows
normal publication execution and save drain joins its cutoff through retirement.

### Stage 8: Retire Core external-result infrastructure

Depends on Stage 7.

- [x] Migrate Core source-based tests by intent; reuse Future coverage for
  abandonment/one-shot setters and preserve task graph/retirement coverage.
- [x] Remove external-result access/source, unused adapter, external node kind,
  launch flags, direct failure setter and abandoned-source task failure code.
- [x] Preserve event creation, unknown-progress waits, nested dependencies and
  terminal hooks; simplify only scheduler-owned external-node special cases.
- [x] Update contracts and validate Core, event/shutdown isolation, save consumers,
  workspace builds and documents.

Completion: no external-result/node symbols remain in workspace source/tests;
ordinary tasks, task events and independent futures retain their own contracts.

### Stage 9: Consolidate migrated composition tests

Depends on Stage 8. This stage changes tests only. Production code and build configuration are
unchanged, so Core target compilation and execution cover this stage without
repeating the workspace build.

- [x] Combine observer outcome, fan-out, failure identity and move-only capture
  coverage without reducing the terminal-state matrix.
- [x] Consolidate fan-in duplicate ownership checks and retain independent nested
  cancellation coverage; remove legacy admission scaffolding and incidental moves.
- [x] Validate CoreConcurrencyTests, focused isolated cases and changed documents.

Completion: simpler tests retain distinct behavioral coverage with no runtime
changes and pass both shared-process and focused isolated execution.

### Stage 10: Remove unused task observation adapters

Depends on Stage 9. Remove task-only adapters after the Tool migration to Future;
Future readiness and lifetime pins retain their production consumers.

- [x] Remove task readiness/ObserveReady and WithLifetime/RetainLifetime, including
  the unused node pin collection; retain SubscribeTask and result-node pins.
- [x] Replace synthetic task pins with result-owned lifetime coverage and merge
  duplicate Future tests while preserving late attachment/notification assertions.
- [x] Validate Core shared-process and isolated tests, workspace consumers and
  documentation; preserve event wait and producer retirement contracts.

Completion: unused task adapters are absent, canonical tests retain their unique
assertions, and the existing subscriptions and lifetime boundaries pass regression.

### Stage 11: Unify task composition through ordinary dependencies

Depends on Stage 10. UE Future continuations remain a separate deferred concern;
task graphs use ordinary launch prerequisites, nested completion and task events.

- [x] Migrate production terminal observers and test/qualification consumers to
  ordinary tasks, preserving explicit attribution and lifetime boundaries.
- [x] Remove public composition helpers and private continuation options/entrypoints;
  normalize repeated dependencies in the ordinary submission path.
- [x] Remove obsolete propagation/aggregation policy tests while retaining distinct
  dependency, result ownership, cancellation, shutdown and module-unload coverage.
- [x] Update contracts and validate Core, affected consumers, workspace builds and
  documentation. Qualification harness changes require compilation, not timing claims.

Completion: scheduled work has one submission path, all consumers use explicit
dependencies, and event/subscription/retirement regression passes.

### Stage 12: Organize task runtime files and test responsibilities

Depends on Stage 11. Preserve declarations, implementation behavior and test names;
this stage changes file ownership, includes, build references and documentation.

- [x] Rename the ordinary TaskComposition API/implementation to Task and the old
  lower-level Task files to TaskRuntime; migrate all workspace consumers directly.
- [x] Separate limiter, pipe and lifecycle tests; retain primitive threading tests
  in ThreadingTests and move scheduler coverage to TaskSchedulerTests.
- [x] Preserve every test suite/case and body, register the new sources, and update
  current contract references without compatibility forwarding headers.
- [x] Validate Core shared-process/isolation coverage, affected consumers, workspace
  and qualification harness compilation, and documentation.

Completion: file names reflect current responsibilities, every test remains
registered once, and semantic regression passes with no compatibility layer.

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
