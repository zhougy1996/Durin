# UE-Style Asset Compilation Ownership Plan

Summary: Move Texture2D asynchronous compilation orchestration into Engine while reducing TextureBuild to a module-owned, value-only build provider, preserving the Runtime module graph and existing DDC ownership.

Last reviewed: 2026-09-01

Status: Archived
Completed: 2026-09-01

## Current Status

All stages are complete. Engine is the single Texture2D asynchronous orchestration authority and object-owned freshness is explicit:

- The pre-migration `FTexture2DTests.*` and `FTextureResourceCompletionTests.*` baseline passed 35 tests.
- The provider contract test proves duplicate providers fail closed, and Core's module-gate fixtures cover admission retirement and in-flight drain.
- Engine now owns the value-only request, settings, input identity, provider descriptor, execution control, product, and `ITexture2DBuildProvider` contract.
- The asset-visible API uses a thin combination: `DTexture2D` retains only its Engine-owned current request serial, while the Engine Texture2D manager/free-function surface owns submit, wait, cancel, diagnostics, and publication.
- TextureBuild registers the concrete `ITexture2DBuildProvider`; the Engine worker invokes it synchronously through a guarded helper and receives only Engine-owned product and identity values.
- The provider input carries explicit cook platform/profile, but the current TextureBuild implementation continues to admit only Win64/Game, preserving existing DDC facts and payload semantics.
- The Texture2D domain, worker queue, Engine task scope, cancellation, mailbox, diagnostics, request state, stale checks, mutation publication, and public compilation API now compile in Runtime/Engine.
- The aggregate manager registers Texture2D as a built-in Engine domain alongside Material; TextureBuild no longer registers or owns an asset-compilation domain or async operation group.
- Publication moved into Engine. TextureBuild owns only the synchronous provider, algorithms, DDC operations, codecs, and build-function registration.
- Post-cutover validation passed 36 Texture2D/provider/resource tests and all 111 MaterialTests.
- `DTexture2D` owns its process-local request serial and last-request diagnostic handle; these fields are independent of serialized/DDC identity.
- The active registry is keyed by generation-safe `FObjectHandle`, contains active requests only, and erases every terminal/dead-owner record.
- A 300-object stress test proves active, queue, running, mailbox, and in-flight byte counters return to zero while retained work diagnostics stay bounded at 256.
- A same-path replacement test proves a destroyed owner's completion cannot resolve or publish to the replacement object.
- The private TextureBuild compilation header dependency and legacy forwarding surface are gone. The complete focused suite now passes 38 tests.
- TextureBuild startup no longer duplicates TextureCube feature registration, and its owned callback gate is named for the DDC build-function resource it actually protects.
- Test-environment teardown now finishes accepted Texture2D work and asserts zero active records, queued/running work, completion-mailbox entries, and in-flight bytes before aggregate shutdown; terminal retained diagnostics are empty after shutdown.
- The repository affected-test selection passed 46 affected targets, the Runtime/game `DurinLauncher` graph built without TextureBuild/DDC, the editor/provider graph built through TextureTests, and all 14 modular-feature/module-retirement tests passed.
- The Material/Texture2D comparison leaves reuse at the aggregate domain contract, `FObjectHandle`, task/cancellation primitives, detached typed completions, terminal retirement, and GameThread publication admission.
- Material keeps program single-flight, retained/last-known-good programs, and Renderer publication; Texture2D keeps its priority/byte-bounded provider queue, DDC producer boundary, mutation-aware CPU publication, and separate GPU readiness.
- No generic compiler state bag, composite asset-status enum, or speculative Mesh plan was introduced. Package/catalog revisions, load generation, DDC identity, cook profile, CPU/GPU/physics readiness, mutation state, cancellation, and module retirement remain independently owned dimensions.

Before this migration, Texture2D compilation implemented the right high-level execution shape—build a detached candidate off-thread and publish it on the GameThread—but its ownership boundary was inverted relative to the asset:

- `DTexture2D` and `FTexturePlatformData` lived in Engine.
- The Texture2D compilation manager, worker queue, request generations, weak object tracking, completion mailbox, cancellation, and publication checks lived in Developer/TextureBuild.
- TextureBuild also owned the actual texture build algorithms, DDC recipes, producer versions, and build-function registration.
- Engine's aggregate asset-compilation manager could pump and stop the external Texture2D domain, but it did not own that domain's task state.

This coupled object lifetime and publication policy to a developer module and made TextureBuild responsible for both orchestration and production. It also encouraged every future asset family to copy its own generation, cancellation, weak-owner, mailbox, and stale-publication machinery.

The selected direction follows the useful Unreal Engine ownership split without copying concrete plugin task ownership across the module boundary:

- Engine owns the asset-visible asynchronous state, scheduler integration, cancellation, latest-request policy, and GameThread publication.
- TextureBuild owns the texture build implementation, DDC interaction, codecs, and deterministic producer identity.
- A synchronous, value-only provider contract connects the Engine-owned worker to TextureBuild while a module invocation gate protects the entire call.

The long-lived [Asset Compilation](../../../Runtime/Assets/AssetCompilation.md) contract now records the smallest proven reuse boundary and explicitly rejects collapsing orthogonal asset states.

## Goal

Establish one reusable asset-compilation ownership model in which lifecycle follows the object-owning Runtime module and specialized Developer modules provide deterministic build functions without exporting their concrete asynchronous tasks.

Texture2D is the reference migration. At completion:

1. Engine is the only owner of Texture2D async request admission, queueing, in-flight state, cancellation, completion delivery, stale-result rejection, and publication.
2. TextureBuild is the only owner of texture build algorithms, DDC keys and records, codecs, build-function registration, and provider implementation.
3. Engine has no link dependency on TextureBuild or DerivedDataCache.
4. No TextureBuild-defined task, callback, deleter, allocator-owned object, or concrete result type survives a provider invocation or module retirement.
5. CPU payload currentness, GPU/physics resource readiness, catalog/package identity, and mutation transaction state remain separate state dimensions.
6. The aggregate asset-compilation lifecycle can stop, cancel, drain, and diagnose Texture2D work without reaching into a Developer module's private scheduler.

## Scope

This plan includes:

- The public Engine-owned Texture2D build request/product and provider interface needed at the module boundary.
- An Engine-owned Texture2D compiling manager/domain and Engine-owned per-request async state.
- Migration of Texture2D callers, diagnostics, tests, and shutdown integration.
- Removal of the legacy TextureBuild-owned Texture2D compilation domain after cutover.
- Contract documentation for future asset families.

This plan does not include:

- Moving texture codecs, mip generation, DDC access, or build-function registration into Engine.
- Refactoring synchronous TextureCube or TextureVolume build entry points unless a shared value type must move to preserve the boundary.
- Refactoring Material compilation, which already has Engine-owned orchestration.
- Unifying ordinary package loading, cooked mesh loading, catalog revisioning, GPU upload, physics cooking, or mutation transactions into one state machine.
- Creating a universal typeless asset-job framework before the Texture2D migration proves the abstraction.
- Changing package formats, cooked payload formats, or DDC key semantics except where an explicit provider identity field is required.

Future Mesh or other authoring pipelines may adopt the proven provider pattern in separate plans. Runtime package load remains a different operation family even when it uses the same detached-candidate/publication discipline.

## Selected Architecture

### Responsibility Split

| Concern | Engine | TextureBuild |
| --- | --- | --- |
| Asset/object identity and lifetime | Owns | Must not retain |
| Request serial and latest-wins policy | Owns | Receives immutable input only |
| Queue, worker task, task scope, cancellation source | Owns | Observes cancellation checkpoints |
| Completion mailbox and GameThread dispatch | Owns | Does not publish |
| Stale-result validation and publication | Owns | Does not inspect live objects |
| CPU payload readiness state | Owns | Produces candidate CPU payload |
| GPU resource enqueue/readiness | Owns separately from compilation | Does not own |
| Texture algorithms, codecs, mip generation | Does not own | Owns |
| DDC lookup/store, record schema, producer identity | Does not own | Owns |
| Provider registration and module gate | Invokes through Core modular features | Registers and retires |
| Diagnostics | Aggregates lifecycle/request metrics | Reports build/DDC details as values |

### Engine-Owned Contract

Stage 0 must freeze exact names, but the contract has the following semantic shape:

- `FTexture2DBuildInputIdentity`: deterministic identity of the immutable build inputs plus the provider/producer version required to interpret the result. It is not an object generation and does not include GPU readiness, catalog revision, or mutation state.
- `FTexture2DBuildRequest`: an Engine-owned value containing source/build settings snapshots, target/profile inputs, the input identity, and cancellation observation. It contains no `DObject*`, weak object reference, GameThread callback, or module-owned lifetime.
- `FTexture2DBuildProduct`: an Engine-owned value containing detached CPU platform data and value-only diagnostics. Destruction remains valid after TextureBuild is unloaded.
- `ITexture2DBuildProvider`: an Engine public modular-feature interface with a synchronous `Build` operation. TextureBuild implements it and may perform synchronous DDC lookup/build/store internally.

The provider operation is synchronous from the caller's perspective even though Engine invokes it on an Engine-owned worker. This is the critical boundary: TextureBuild does not return a concrete async task or retain a callback into Engine.

The Engine worker invokes the singleton provider through the existing module-owned callback gate. The gate remains admitted for the whole `Build` call. TextureBuild shutdown retires provider admission and waits for admitted calls before unregistering build functions or releasing DDC-facing state.

All request/product members crossing the boundary must use Engine/Core-owned storage and destruction. If existing platform-data types contain TextureBuild-specific ownership, Stage 0 must replace or encapsulate those members before the provider is introduced; a custom deleter whose code lives in TextureBuild is not permitted.

### Engine-Owned Orchestration

Engine gains a Texture2D compilation manager registered as an Engine-owned built-in domain under the aggregate asset-compilation manager. It owns:

- Request admission and a monotonic per-object `RequestSerial`.
- The Engine worker queue, task scope, cancellation sources, pending byte/job accounting, and completion mailbox.
- Active request records keyed by stable object handle plus request serial, never by a permanent asset-path registry.
- A weak publication target used only on the GameThread.
- Exactly-once terminal reporting for success, failure, cancellation, supersession, owner destruction, provider absence, and shutdown rejection.
- Removal of terminal/dead active records so unique-path churn cannot grow the registry indefinitely.

`DTexture2D` or its Engine-owned platform-data holder keeps only Engine-defined async state/handle information. The concrete worker callable also lives in Engine. The asset must not own `FTextureBuildTask`, a TextureBuild implementation object, or any equivalent type whose destructor or cancellation code resides in the provider module.

### Request and Publication Flow

1. A GameThread caller freezes source/build inputs and opens or joins the required asset mutation transaction.
2. Engine allocates the next `RequestSerial`, records the immutable input identity, and supersedes the preceding request for the same object handle.
3. An Engine worker enters the provider gate and calls the synchronous value-only provider.
4. TextureBuild resolves DDC or produces a detached candidate, checking the Engine-provided cancellation observation at documented checkpoints.
5. The Engine worker places an Engine-owned completion value in the manager mailbox.
6. The GameThread accepts a completion only when the object handle still resolves, the request is still the asset's current request, the input identity still matches, the manager still admits publication, and the owning mutation transaction can commit.
7. Successful publication makes the CPU platform payload current and may enqueue a separate GPU resource generation. GPU readiness does not retroactively change the build request's currentness.
8. Every rejected or accepted completion closes exactly once and releases its active registry entry and accounting.

`RequestSerial` answers “is this still the latest request for this live object?” The deterministic input/provider identity answers “what content did we build and cache?” They must not be collapsed into a single generation number.

### Failure and Shutdown Semantics

- No registered provider at request execution: finish once as provider unavailable; do not fall back to touching DDC from Engine.
- Provider retired before a queued Engine task enters: reject the invocation and finish once as provider unavailable or shutdown-cancelled according to aggregate shutdown state.
- Provider retirement during a build: the admitted call may finish; retirement waits for the gate, after which no provider code or value remains reachable.
- Supersession: request cooperative cancellation immediately; late completion is still consumed and rejected exactly once on the GameThread.
- Owner destruction or handle reuse: reject publication by handle plus serial; a reused asset path is never sufficient identity.
- Aggregate shutdown: stop admission, request cancellation, pump/consume terminal completions, drain Engine-owned tasks, then allow TextureBuild provider/build-function retirement and later Core task-system shutdown.
- Forced module unload must not destroy Engine-owned task state from provider code. If safe drain cannot be proven, unload remains blocked until the gate reaches zero.

## Implementation Stages

### Stage 0: Freeze behavior and provider boundary

- [x] Record the current Texture2D compilation call graph, public callers, private test dependencies, TextureBuild registration order, and aggregate shutdown order in the implementation change notes.
- [x] Add or strengthen characterization coverage for latest-wins publication, exactly-once completion, owner destruction, same-path object replacement, cancellation checkpoints, mutation rollback, and terminal active-record removal while the old implementation is still authoritative.
- [x] Add a module-lifecycle fixture that can prove a registered provider is protected by the module gate for the full invocation and that provider retirement rejects new entry.
- [x] Freeze the final Engine public names and fields for the value-only request, input identity, product, cancellation observation, provider descriptor, and provider interface.
- [x] Audit every cross-boundary member for allocator, destructor, virtual dispatch, callback, and module-code ownership; record or remove any TextureBuild-owned lifetime.
- [x] Decide whether the asset-visible Engine API is expressed as `Begin/Finish/Cancel/IsCompiling` methods on `DTexture2D`, an Engine-owned handle on `FTexturePlatformData`, or a thin combination. The decision must still leave scheduling and publication in the Engine manager.
- [x] Capture baseline TextureTests and TextureFailureTests results before moving ownership.

Stage completion condition: the old implementation still owns execution, all required behaviors are locked by tests, and the cross-module contract can be implemented without an Engine dependency on TextureBuild or DerivedDataCache.

### Stage 1: Introduce the value-only build provider

- [x] Add the Engine-owned request/product/identity types and `ITexture2DBuildProvider` modular-feature contract.
- [x] Implement the TextureBuild provider as a thin adapter over the existing synchronous Texture2D build and DDC path.
- [x] Register provider retirement ahead of TextureBuild build-function and DDC-facing teardown; protect the entire synchronous provider invocation with the module-owned callback gate.
- [x] Adapt the existing TextureBuild-owned compilation worker to call the new provider contract temporarily, without changing request admission, publication, or public behavior.
- [x] Add contract tests for provider absence, duplicate providers, retirement before entry, retirement during an admitted call, cancellation observation, and destruction of returned products after provider retirement.
- [x] Verify the Runtime Engine target still has no TextureBuild or DerivedDataCache link dependency.

This is a deliberate compatibility stage. It validates the provider seam while the old scheduler remains the single authority. It must not add a second Texture2D manager.

Stage completion condition: all texture construction and DDC work used by async Texture2D compilation is reachable only through the value-only provider contract, and no provider-owned lifetime escapes the guarded call.

### Stage 2: Move scheduling and publication into Engine

- [x] Add the Engine-owned Texture2D compiling manager/domain, Engine worker task, task scope, cancellation source, mailbox, diagnostics, and bounded active-request registry.
- [x] Register the Texture2D manager as an Engine-owned built-in domain of the aggregate asset-compilation manager, alongside the existing Engine-owned Material domain.
- [x] Move request serial allocation, supersession, weak object/handle tracking, completion dispatch, stale checks, mutation commit/rollback, and CPU payload publication into Engine.
- [x] Route Texture2D async submission, wait, cancel, finish, diagnostics, and aggregate lifecycle calls to the Engine manager.
- [x] Cut TextureBuild over in one change from compilation-domain owner to provider only: remove its domain registration and async operation group before enabling Engine admission.
- [x] Ensure provider absence, provider retirement, aggregate stop, and task-system admission failure each produce one terminal result and release all accounting.
- [x] Preserve the prior public call surface through a temporary forwarding layer only where necessary to keep callers compiling.

Stage completion condition: Engine is the only Texture2D async orchestration authority, TextureBuild owns no queue or live-object registry, and the existing latest-wins and transaction tests pass through the new manager.

### Stage 3: Make asset ownership explicit and migrate callers

- [x] Store the selected Engine-only async state/handle on `DTexture2D` or its Engine-owned platform-data holder, with documented GameThread ownership and destruction rules.
- [x] Expose the final Engine-owned begin/finish/cancel/query surface and migrate editor import, property-change, save/cook readiness, and test callers to it.
- [x] Replace path-keyed lifetime assumptions with stable object handle plus request serial everywhere in the Texture2D path.
- [x] Erase terminal and dead-owner registry records and add stress coverage showing bounded active state under repeated unique asset creation/destruction.
- [x] Remove private TextureBuild compilation-domain includes from tests and callers.
- [x] Delete any temporary forwarding API introduced in Stage 2; permanent dual ownership or duplicate state is not allowed.

Stage completion condition: the asset and aggregate manager expose only Engine types, all callers use the Engine surface, and TextureBuild public headers describe production rather than asset lifecycle.

### Stage 4: Remove the legacy domain and qualify lifecycle

- [x] Delete the TextureBuild-owned Texture2D compilation manager/domain, queue, mailbox, request-generation map, task scope, and obsolete diagnostics.
- [x] Remove obsolete module dependencies, source entries, registrations, and test-only access to the legacy domain.
- [x] Add shutdown-order coverage proving: aggregate admission closes; Engine requests cancel; terminal completions are consumed; Engine tasks drain; provider admission retires; build functions unregister; module objects release; Core task admission closes.
- [x] Add assertions/counters for zero active requests, zero queued completions, zero pending bytes, zero admitted provider calls, and no retained module resources at test shutdown.
- [x] Run the repository affected-test selection and the targeted TextureTests, TextureFailureTests, MaterialTests, and relevant module-lifecycle coverage according to the repository testing guide.
- [x] Build both a Runtime/game configuration that excludes TextureBuild/DDC and an editor/developer configuration that includes the provider, according to the repository build guide.
- [x] Update the long-lived asset-compilation, asset-data lifecycle, runtime-lifecycle, and module-boundary documentation in the same implementation stage.

Stage completion condition: no legacy TextureBuild scheduler remains, both module graphs build, targeted and affected tests pass, shutdown counters return to zero, and long-lived documentation names Engine as the Texture2D orchestration owner.

### Stage 5: Evaluate reuse without generalizing prematurely

- [x] Compare the completed Texture2D path with Material compilation and document the smallest shared Engine primitives that have proved stable: request serials, active-record retirement, completion envelopes, cancellation outcomes, and publication admission.
- [x] Keep asset-specific managers if extracting a common helper would erase typed invariants or introduce typeless state bags.
- [x] Create separate follow-up plans for Mesh authoring/cooking or other asset families only when their ownership and publication requirements are known.
- [x] Explicitly exclude package load, catalog/package revision, GPU readiness, physics readiness, and mutation transaction state from any shared “asset status” enum.

Stage completion condition: reuse decisions are evidence-based, and this plan does not remain open merely to host unrelated asset-pipeline work.

## Acceptance Gates

| Gate | Required evidence |
| --- | --- |
| Module graph | Engine public/private dependencies do not include TextureBuild or DerivedDataCache; Runtime/game builds without the provider module. |
| Single authority | Exactly one Texture2D compilation manager admits requests at every migration stage. |
| Module safety | Provider retirement prevents new calls, waits admitted calls, and leaves no provider-owned task, callback, deleter, or result alive. |
| Freshness | Object handle, request serial, immutable input identity, manager admission, and transaction validity are checked before publication. |
| State separation | CPU platform-data readiness and GPU resource readiness retain separate revisions/outcomes; DDC identity is not used as object generation. |
| Completion | Success, failure, cancellation, supersession, owner destruction, provider absence, and shutdown each complete exactly once. |
| Bounded lifetime | Terminal/dead active records and completion values are released; stress tests return all queue/byte/gate counters to zero. |
| Shutdown | Aggregate manager drains Engine work before provider/build-function retirement and before Core task-system shutdown. |
| Compatibility | Existing Texture2D publication, cancellation, DDC, mutation, and resource-generation behavior remains covered by targeted tests. |
| Documentation | Long-lived documentation and module tables match the final ownership and shutdown order. |

## Migration Constraints

- Do not move DDC APIs or TextureBuild implementation headers into Runtime Engine to make the cutover easier.
- Do not let Engine enqueue a lambda whose closure, function body, virtual target, or deleter is owned by TextureBuild after provider retirement.
- Do not use asset paths as lifetime identity or retain a permanent per-path state map.
- Do not publish directly from a worker or provider callback.
- Do not treat cooperative cancellation as the only stale-result defense; publication validation remains mandatory.
- Do not introduce a global composite asset-status enum. Shared infrastructure may transport typed outcomes, but each orthogonal revision/state remains independently owned.
- Do not keep both the old TextureBuild domain and the new Engine manager active behind feature flags. Cut ownership over atomically after the provider seam is proven.
- Do not preserve a temporary forwarding header beyond Stage 3.

## Validation and Handoff

Each implementation stage must be independently buildable, testable, reviewable, and revertible. Use the repository's [Build and Run guide](../../../Agents/BuildAndRun.md) and [Testing guide](../../../Agents/Testing.md) rather than copying command details into this plan.

At each stage:

1. Run the narrow tests added or moved by that stage.
2. Run repository affected-test selection before handoff.
3. Confirm the selected module graph with an appropriate Runtime/game or editor/developer build when dependencies changed.
4. Update this plan's checkboxes and Current Status in the same commit.
5. Record exact `Plan` and `Stage` commit trailers as required by repository policy.

## Related Documentation and Code

- [Asset Compilation](../../../Runtime/Assets/AssetCompilation.md)
- [Asset Data Lifecycle](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Runtime Lifecycle](../../../Runtime/Core/RuntimeLifecycle.md)
- [Async Asset Operations](../../../Editor/Architecture/AsyncAssetOperations.md)
- [Engine Asset Compiling Manager](../../../../Engine/Source/Runtime/Engine/Private/Asset/AssetCompilingManager.cpp)
- [Texture2D Asset Type](../../../../Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h)
- [Texture2D Post-Load Provider Contract](../../../../Engine/Source/Runtime/Engine/Public/Texture/Texture2DPostLoad.h)
- [TextureBuild Module Lifecycle](../../../../Engine/Source/Developer/TextureBuild/Private/TextureBuildModule.cpp)
- [Texture2D Compilation Manager](../../../../Engine/Source/Runtime/Engine/Private/Texture/Texture2DCompilation.cpp)
- [TextureBuild Tests](../../../../Engine/Tests/Native/EngineTests/Private/Texture/TextureBuildTests.cpp)
