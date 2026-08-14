# Dynamic Module Registry Safety Audit Plan

Summary: Classify and integrate cross-DLL registries and callbacks with explicit owner retirement and unload-quiescence proof.

Last reviewed: 2026-08-15

Status: Active
Completed:

## Current Status

Milestone 3 is complete and supplies the owner identity, bounded invocation,
operation-group, and fail-closed unload primitives. Stage 0 is active. The
initial scan identifies AssetImportCore provider/handler registries,
AssetBuildCore build-host and local-build registries, editor workspace,
thumbnail, customization, viewport-mode, startup-command, and asset-reference
registrations as the highest-probability process-retained Plugin boundaries.
Delegates, timers, watchers, render callbacks, and executor submissions require
call-site classification because most `std::function` uses are bounded values
or same-module implementation details rather than cross-DLL registrations.

## Goal

Ensure every process-retained callable or polymorphic provider originating in a
dynamic module has explicit owner identity, retirement admission, bounded
invocation, and destruction or operation-drain proof before native unload,
while preserving each specialized registry's domain selection semantics.

## Scope

- Inventory dynamic-module registrations into Core, Runtime, Developer, and
  Editor-owned long-lived storage.
- Integrate asset-import, build-host, local-build, workspace, thumbnail,
  customization, viewport-mode, startup-command, and reference-store paths with
  module-owner retirement or prove a narrower owning lifetime.
- Classify delegates, timers, watchers, render callbacks, external executors,
  virtual providers, custom deleters, and copied callable snapshots that cross
  module boundaries.
- Add focused race, stale-token, retained-callable, and shutdown-audit tests.
- Publish lasting specialized-registry ownership rules and create the dynamic
  DLL qualification plan after the exit gate passes.

## Non-Goals

- Do not replace domain ranking, identity, file-extension, asset-class, or
  provenance selection with generic modular-feature cardinality.
- Do not owner-tag transient callables that are created, invoked, and destroyed
  wholly inside one synchronous stack or one module instance.
- Do not redesign ordinary object/component registration, reflection metadata,
  or task-system internals already covered by module/object and operation-group
  drains.
- Do not perform repeated physical DLL unload/reload; Milestone 5 owns the real
  dynamic fixture and stress qualification.

## Design Decisions and Invariants

- A registration that can retain Plugin code beyond the registering call must
  carry manager-created owner identity or be owned by an already audited module
  instance whose shutdown precedes provider unload.
- Specialized registries keep their selection rules and expose bounded
  invocation/lease APIs; generic `InvokeSingle<T>` is used only for true typed
  singleton services.
- Registration tokens are move-only and generation-bearing. Stale or foreign
  tokens cannot remove a replacement entry.
- Registry retirement closes admission and waits for admitted callbacks before
  destroying callable, virtual provider, deleter, or allocator storage.
- Async work started by a registry callback belongs to an explicit operation
  group before the callback returns. A lease or shared pointer does not replace
  task/callback quiescence.
- Copying Plugin callables out of a registry lock is forbidden unless the copy
  is held behind an admitted owner invocation whose destruction is included in
  retirement.
- Audit evidence distinguishes cross-DLL retained storage from local/transient
  `std::function`, test hooks, data callbacks, and callbacks whose code resides
  in a non-unloadable owner.

## Current Foundations and Gaps

| Boundary | Existing semantics | Initial risk |
| --- | --- | --- |
| Asset import providers and handlers | Identity/ranking, shared provider leases, explicit unregister | Shared polymorphic Plugin objects and identity-only retirement are not module-owner attributed |
| Asset build host contributions | Generation handles plus start/stop/pump/wait/drain/snapshot callbacks | Pump/wait snapshots copy Plugin callables outside registry storage |
| Local build functions | Typed build keys and local `std::function` executors | Registration and invocation lifetime need owner attribution |
| Workspaces and asset-editor routes | Batch registration handles held by editor modules/MainFrame | Cross-module orchestration and destruction ordering need proof |
| Thumbnail providers | Scoped handles and polymorphic providers | Provider/capture destruction must occur before provider DLL unload |
| Editor customizations and viewport modes | Identity-bearing handles | Factories and virtual objects need admitted-call and live-instance rules |
| Startup commands and asset-reference stores | Scoped handles in module fields | Global storage requires owner-aware retirement audit |
| Delegates, timers, watchers, render/executor callbacks | Mixed handles, raw removal, or bounded submissions | Must separate safe local uses from process-retained Plugin execution paths |

## Implementation Stages

### Stage 0: Freeze the cross-DLL inventory and disposition matrix

- [ ] Enumerate every production module registration into storage owned by a
  different module or process-lifetime subsystem.
- [ ] Record storage owner, provider module, callable/provider type, selection
  semantics, registration identity, invocation thread, async escape paths,
  shutdown ordering, and current destruction proof.
- [ ] Classify each candidate as migrate, owner-integrate, module-instance
  ordered, bounded/transient, non-unloadable, test-only, or false positive.
- [ ] Freeze the symbol and call-site checklist for later deletion/audit.

#### Acceptance Gate

- Every roadmap-named registry/callback category has an evidence-backed row and
  no production cross-DLL retained callable remains unclassified.
- The next stages contain only concrete required integrations, not open-ended
  repository search.

### Stage 1: Integrate asset import and build registries

- [ ] Add common owner identity and bounded retirement to asset-import provider,
  single-asset-handler, and import-record-handler registrations without losing
  identity, ranking, provenance, or lease semantics.
- [ ] Integrate build-host contributions and local build functions with owner
  retirement; eliminate unaudited callable snapshots across invocation.
- [ ] Associate asynchronous import/build work with owner operation groups and
  require zero leases, admitted calls, tasks, results, and retained callables at
  retirement.
- [ ] Add stale-token, concurrent invocation/retire, async drain, and retained
  provider/callable destruction tests.

#### Acceptance Gate

- StandardAssetImport, GeometryBuild, TextureBuild, and AssetBuildCore can be
  retired without a provider, handler, host, or local build callback remaining
  callable or destructible after its DLL becomes unloadable.
- Import/build selection and functional tests retain existing results.

### Stage 2: Integrate Editor extension registries

- [ ] Apply owner identity and bounded retirement to workspace/asset-editor,
  thumbnail, customization, viewport-mode, startup-command, and reference-store
  registrations that retain Plugin code.
- [ ] Define live workspace/editor/customization/provider instance destruction
  ordering and reject unload while an escaped instance remains.
- [ ] Replace identity-only or parameterless removal where stale registrations
  can remove replacements.
- [ ] Add focused registration-race, stale-handle, instance-lifetime, and
  destructor-sensitive tests.

#### Acceptance Gate

- Editor extension modules retire registrations and destroy every retained
  callable/provider/instance while mapped, without changing route or
  customization selection behavior.

### Stage 3: Close delegate, timer, watcher, render, and executor findings

- [ ] Audit every remaining candidate from Stage 0 and implement owner
  retirement or operation-group drain for required cross-DLL paths.
- [ ] Record explicit exemptions for bounded/transient, same-module,
  non-unloadable, and test-only callables with code evidence.
- [ ] Remove copied callable snapshots, raw Plugin observer registrations, and
  shutdown ordering assumptions that are not mechanically enforced.
- [ ] Add focused drain and destructor-sensitive tests for each migrated
  asynchronous/external execution family.

#### Acceptance Gate

- No production process-retained Plugin callable lacks owner attribution,
  closed admission, and destruction proof.
- Render/executor work and external threads/watchers cannot reenter a retired
  module, and drain failure prevents unload.

### Stage 4: Regression, lasting contract, and qualification handoff

- [ ] Run the frozen symbol/call-site audit and affected Runtime, Developer,
  Editor, build, import, rendering, and module-manager validations.
- [ ] Publish the lasting specialized-registry and callback ownership contract.
- [ ] Update the parent roadmap with Milestone 4 evidence and complete this
  plan only after every required disposition is closed.
- [ ] Create `Documentation/Plans/DynamicDllUnloadQualification.md` only after
  the Milestone 4 exit gate passes.

#### Acceptance Gate

- Repository-targeted audit and native tests prove all required owner paths
  retire and destroy Plugin code while mapped and all documented exemptions
  are bounded by construction.
- The Milestone 5 plan has a closed inventory and concrete real-DLL fixture
  dependency set.

## Validation Matrix

| Area | Validation | Evidence required |
| --- | --- | --- |
| Inventory | Targeted symbol and module-registration searches | Every cross-DLL candidate has one disposition |
| Registration identity | Registry unit tests | Foreign/stale token cannot retire replacement |
| Invocation retirement | Barrier-controlled concurrency tests | Entered callback drains; late callback is rejected |
| Callable destruction | Destructor-sensitive captures/providers | Destruction observed before retirement/unload succeeds |
| Async ownership | Import/build/render/executor tests | Zero tasks, results, deferred callables, and worker wrappers |
| Selection semantics | Existing domain tests | Ranking, identity, provenance, routes, and exact-class behavior unchanged |
| Editor extensions | Workspace/thumbnail/customization tests | Registration and escaped-instance lifetime are explicit |
| Module manager | Fail-closed unload tests | Any live registry/external path rejects native release |
| Regression | Affected targets and bounded domains | Existing functional results remain stable |

## Definition of Done

- Every production cross-DLL retained callable or polymorphic provider is
  owner-attributed and retired/drained, or has a documented construction-level
  proof that it cannot outlive its provider module.
- Specialized registries preserve domain semantics and use identity-bearing,
  stale-safe registration and bounded invocation.
- Async and external execution paths prove callable/provider destruction before
  unload; failures leave the DLL mapped.
- Lasting documentation owns the implemented contract and the dynamic DLL
  unload qualification plan is active.

## Deferred Follow-ups

- Real repeated `LoadLibrary`/`FreeLibrary` unload/reload stress, generation
  replacement, injected drain failure, and Windows diagnostics remain in
  Milestone 5.

## Related Documentation

- [Parent roadmap](../Roadmaps/ModularFeatureAndDllUnloadSafety.md)
- [Modular features and module retirement](../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
- [Task System](../Runtime/Core/TaskSystem.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)

## Related Code

- [Asset import registries](../../Engine/Source/Editor/AssetImportCore/Public/AssetImportCore.h)
- [Standard asset import providers](../../Engine/Source/Editor/StandardAssetImport/Private/StandardAssetImportProviders.cpp)
- [Build host](../../Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildHost.h)
- [Local build registry](../../Engine/Source/Developer/AssetBuildCore/Public/AssetBuild/BuildRegistry.h)
- [Workspace manager](../../Engine/Source/Editor/DurinEd/Public/Editor/WorkspaceManager.h)
- [Thumbnail service](../../Engine/Source/Editor/DurinEd/Public/Thumbnail/RenderedAssetThumbnailService.h)
- [Level editor customizations](../../Engine/Source/Editor/LevelEditor/Public/LevelEditorCustomizations.h)
- [Viewport edit modes](../../Engine/Source/Editor/LevelEditor/Public/LevelEditorViewportEditing.h)
- [Module manager](../../Engine/Source/Runtime/Core/Public/Modules/ModuleManager.h)
