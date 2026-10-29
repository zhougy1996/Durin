# UE Task And Future Alignment Plan

Summary: Separate scheduled tasks, task events, and external result delivery while preserving cancellation, producer retirement, and shutdown drain guarantees.

Last reviewed: 2026-10-29

Status: Active
Completed:

## Current Status

Planning only. No runtime, public API, or test implementation has changed.
The inventory below was checked against revision
`cac748bd0d5943099170c6296be001712ce02df6` across the source and test roots of
Engine, Sandbox, and RoadWeaver declared in `Durin.dworkspace`. Current direct
production sources are in Engine; Sandbox and RoadWeaver have no matching direct
uses. Repeat symbol and alias searches before each API migration.

All implementation stages remain open. UE alignment means matching abstraction
boundaries, not reproducing every implementation detail or removing Durin's
existing failure, cancellation, and module-lifetime guarantees.

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
| `Engine/Source/Runtime/Core/Public/Threading/TaskComposition.h`: `ThenAsync`, empty `BuildFanIn`, `FCompletionSourceAccess` | Keep the machinery private; preserve dynamic dependency binding, nested/module lifetime pins, shared move-only results, failure/cancel propagation, and iterative retirement. Do not route internal graph operations through scheduler-independent futures. |
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

- [ ] Refresh the complete workspace symbol/alias inventory, including RHI
  `GetTaskHandle` consumers, save-task cancellation, Tool handlers, and tests.
- [ ] Freeze Future failure, void, ownership, observation, and lifetime-pin APIs;
  document intentional differences from UE and `std::future`.
- [ ] For each external producer, record its admission point, signal point,
  capture-destruction point, owner join, and process/module shutdown placement.
  Identify the precise accepted handle or private retirement registration that
  closes the signal-to-retirement gap. Resolve any missing join before migration.
- [ ] Freeze the narrow private external adapter contract and its cancellation,
  failure, abandonment, accounting, and producer-retirement transitions. It must
  not become a new public completion-source facade.

Completion: every production row above has an executable ownership mapping and no
unresolved shutdown or result-publication contract. No API migration precedes it.

### Stage 1: Add scheduler-independent futures

Depends on Stage 0.

- [ ] Implement `Threading/Future.h` and Core-owned support without task-node
  construction. Add ready future construction and required observation/pin APIs.
- [ ] Validate move-only and void results, repeated publication, abandonment,
  consume/share invalidation, timed waits, publication/subscription races,
  detached observation, reentrant callbacks, and retained immutable views.
- [ ] Validate creation and completion before startup, after shutdown, and across
  scheduler restart with no change to scheduler pending-node counters.

Completion: Future semantics pass focused Core tests independently of a scheduler.

### Stage 2: Migrate Tool results

Depends on Stage 1.

- [ ] Migrate all Tool aliases, native handlers, service receipts, and integrations
  in the same API change across all workspace projects.
- [ ] Preserve result accounting/pins, cancellation diagnostics, immediate and
  pending providers, close during dispatch, observer detachment, and late producer
  completion after editor closure without accessing retired host state.
- [ ] Verify provider module/work-gate ownership independently from receipt
  completion; test abandonment and retained receipts after service destruction.

Completion: no Tool result is a detached task; Core Tool and affected editor/MCP/
Python integration coverage passes with the existing provider shutdown contract.

### Stage 3: Migrate event signals and save adapters

Depends on Stage 0; use Stage 1 only where actual data delivery requires it.

- [ ] Migrate Texture, cooked Texture, RHI observers, and package admission using
  the row-specific outcome and retirement rules above.
- [ ] Move typed package/asset save completion behind private tracked adapters;
  preserve public typed-task signatures and cancellation behavior.
- [ ] Verify producer/observer cancel races, rejected setup, exceptions,
  disappeared owners, duplicate/late signals, and immediate completion.
- [ ] Verify shutdown begun before producer completion requires no new submission,
  still pumps accepted deferred publication, and waits for capture retirement.

Completion: all production public-source uses are gone; affected CoreDObject,
Engine, RHI, and rendering lifecycle coverage demonstrates the drain invariant.

### Stage 4: Remove the public source and accept the final boundary

Depends on Stages 2 and 3.

- [ ] Privatize remaining composition/source machinery, migrate tests by purpose,
  and resolve ready-task helper semantics. Search all workspace source/test roots
  for the removed public type, aliases, detached nodes, and old access patterns.
- [ ] Validate flatten/fan-in behavior, failure propagation, shutdown with dropped
  handles, delayed producer acknowledgement, module retirement, and retained
  results; preserve cancellation requests separately from producer outcomes.
- [ ] Add deterministic drain race tests using barriers/latches rather than
  sleeps: admission closes before signal, signal precedes capture destruction,
  observation cancels before shared producer exit, and admission setup fails.
- [ ] Update lasting contracts in TaskSystem, Tool, runtime lifecycle, and module
  retirement documentation as applicable; record evidence and close this plan
  only after the final gates pass.

Completion: no public `TCompletionSource`, no detached data-only `TTask` escape,
and no producer lifetime protected only by a ready result or event.

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
