# Dynamic Module Registry Safety Audit Plan

Summary: Classify and integrate cross-DLL registries and callbacks with explicit owner retirement and unload-quiescence proof.

Last reviewed: 2026-08-15

Status: Completed
Completed: 2026-08-15

## Current Status

Milestone 3 supplies the owner identity, bounded invocation, operation-group,
and fail-closed unload primitives. Stage 0 is complete. The frozen disposition
matrix below covers every production cross-module registration found by the
module/callable scan. Stages 1-3 contain the required owner integrations;
bounded synchronous submissions, same-module C callbacks, test-only local
build functions, and process-foundation callbacks are explicitly separated
from retained Plugin registrations. Stage 1 is complete. Asset-import providers
and handlers, build-host contributions, and local-build functions now use
manager-created owner gates; retained provider/callable storage is audited
before unload, and module-owned TextureBuild work uses an explicit operation
group. Stage 2 is complete: Editor workspaces, thumbnails, customizations,
viewport modes, startup commands, reference stores, and move observers now use
owner admission and retained-instance auditing. Stage 3 is complete. Console
commands now use owner admission and persistent registration leases,
module-authored task roots use operation groups, and render/RHI shutdown
ordering is mechanically drained before native release. The remaining
callbacks are bounded, same-module, test-local, or provided by process-resident
libraries rather than unloadable module instances. Stage 4 and this plan are
complete: the frozen audit is closed, the lasting Runtime Core contract owns
the implemented rules, broad regression passes, and the real-DLL qualification
plan is active.

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

## Frozen Stage 0 Disposition Matrix

| Storage owner / boundary | Provider module(s) | Retained code and selection | Thread / async escape | Disposition |
| --- | --- | --- | --- | --- |
| AssetImportCore provider registry | StandardAssetImport | Shared virtual providers by provider id and recognition | Calling thread; plans and results retain leases; async import escapes | Stage 1 owner-integrate leases, invocation, and async groups |
| AssetImportCore single-asset handlers | StandardAssetImport | Shared virtual handlers by exact asset class | Calling thread; plans retain handler | Stage 1 owner-integrate and drain admitted calls/leases |
| AssetImportCore import-record handlers | StandardAssetImport | Shared virtual handler by provider id | Calling thread; multi-output plans/candidates escape | Stage 1 owner-integrate with provider retirement |
| AssetBuildCore build-host contributions | GeometryBuild, TextureBuild | Seven callbacks by service identity and drain order | Game Thread pump/wait; TextureBuild worker completion | Stage 1 owner-integrate; stop copying callbacks outside admission |
| AssetBuildCore local build functions | No production provider; native tests only | Function by portable build identity | Synchronous call; terminal callback bounded to call | Stage 1 harden public registration API and tests for future modules |
| WorkspaceManager batches and asset-editor routes | LevelEditor, MaterialEditor, TextureEditor, StaticMeshEditor, SkeletalMeshEditor | Shared virtual workspaces plus value routes | Game Thread; open documents retain workspace | Stage 2 owner-integrate and close/destroy live documents before retirement |
| Rendered thumbnail provider registry | Same five Editor modules | Virtual provider by exact asset class and provider generation | Game Thread capture, worker/render sessions | Stage 2 attach owner; reuse existing generation cancel/drain and lease invalidation |
| LevelEditor customization registry | LevelEditor | Shared virtual visualizers/customizations by reflected class | Game Thread; lookup returns shared provider | Stage 2 bounded owner lease/invocation; forbid escaped provider across retire |
| Level viewport edit-mode registry | LevelEditor | `CanActivate`/factory callables by id/priority; manager owns live virtual mode | Game Thread Tick | Stage 2 owner-integrate descriptors and close live mode before provider retirement |
| Startup command registry | LevelEditor | Command callable by name and numeric handle | Startup/control thread; no async escape | Stage 2 owner-integrate bounded dispatch |
| Asset reference-store registry | AssetImportCore, LevelEditor | Raw virtual store pointer by numeric handle | Calling/control thread; rewrite contribution values may escape | Stage 2 owner-integrate bounded virtual calls |
| Asset move observer registry | LevelEditor-owned coordinator instance | Raw virtual observer by numeric handle | Game Thread publication; no async callback escape | Stage 2 owner-integrate or prove coordinator lifetime nested under LevelEditor workspace |
| Console command registry | DurinEd, LevelEditor panels, Renderer | `std::function` by name and numeric handle | Game Thread; synchronous execution, copied descriptor list | Stage 3 owner-integrate production module commands; metadata listing must not copy executable callables |
| Asset delete contributors | Engine | Process-global functions by exact class, no removal token | Synchronous asset delete planning | Stage 3 migrate to owner-aware handles or classify Engine as required resident dependency |
| ModuleManager object callbacks | CoreDObject | Two `std::function` callbacks | Module control thread, synchronous | Process-foundation exemption: installed once by CoreDObject and required while dynamic loading exists |
| GLFW window callbacks | ApplicationCore | Static C function pointers stored by GLFW | Platform thread; dispatch through live window user pointer | Same-module/platform-window exemption; window destruction clears external lifetime |
| Render command and RHI operation submissions | Engine, Renderer, DurinEd, VulkanRHI | Move-only lambdas/command objects, not registry entries | Render/RHI threads; fences and global renderer shutdown drain | Stage 3 audit module unload ordering and add owner drain only where producer unload can precede renderer shutdown |
| Core task/deferred queues | All async-capable modules | Move-only task/continuation callables | Worker/Game Thread | Already owner-integrated when module operation groups are used; Stage 3 closes remaining unscoped Plugin roots |
| Delegates/timers/file watchers | No production module registration calls or watcher/timer service found | Template definitions and local callbacks only | None found across module boundary | Closed false-positive class; reopen only on concrete production registration |
| Local algorithm/data callbacks | Multiple | Stack-bounded predicates, visitors, failure hooks, serializers | Same synchronous call | Bounded/transient exemption; never stored by another module |

## Implementation Stages

### Stage 0: Freeze the cross-DLL inventory and disposition matrix

- [x] Enumerate every production module registration into storage owned by a
  different module or process-lifetime subsystem.
- [x] Record storage owner, provider module, callable/provider type, selection
  semantics, registration identity, invocation thread, async escape paths,
  shutdown ordering, and current destruction proof.
- [x] Classify each candidate as migrate, owner-integrate, module-instance
  ordered, bounded/transient, non-unloadable, test-only, or false positive.
- [x] Freeze the symbol and call-site checklist for later deletion/audit.

#### Acceptance Gate

- Every roadmap-named registry/callback category has an evidence-backed row and
  no production cross-DLL retained callable remains unclassified.
- The next stages contain only concrete required integrations, not open-ended
  repository search.

### Stage 1: Integrate asset import and build registries

- [x] Add common owner identity and bounded retirement to asset-import provider,
  single-asset-handler, and import-record-handler registrations without losing
  identity, ranking, provenance, or lease semantics.
- [x] Integrate build-host contributions and local build functions with owner
  retirement; eliminate unaudited callable snapshots across invocation.
- [x] Associate asynchronous import/build work with owner operation groups and
  require zero leases, admitted calls, tasks, results, and retained callables at
  retirement.
- [x] Add stale-token, concurrent invocation/retire, async drain, and retained
  provider/callable destruction tests.

#### Completion Evidence

- `FModuleOwnedCallbackRegistration`, its copyable admission gate, and retained
  resource leases provide one manager-owned retirement audit for specialized
  registries without replacing their domain-specific selection rules.
- StandardAssetImport owns the import-registry gate and its terrain operation
  group. Provider plans/results retain audited leases; provider admission closes
  and outstanding asynchronous imports cancel and drain before unregistration.
- GeometryBuild and TextureBuild own build-host gates. TextureBuild worker tasks
  additionally run in `TextureBuild.Operations`, while every copied host/local
  callable is destroyed inside an admitted invocation and retained-resource
  lease.
- `CoreConcurrencyTests` passed 138/138,
  `AssetImportCoreTests` passed 27/27, `AssetBuildCoreTests` passed 11/11,
  `TextureTests` passed 66 with two existing skips, the complete
  `@asset-import` domain passed, and the TextureBuild module target compiled.

#### Acceptance Gate

- StandardAssetImport, GeometryBuild, TextureBuild, and AssetBuildCore can be
  retired without a provider, handler, host, or local build callback remaining
  callable or destructible after its DLL becomes unloadable.
- Import/build selection and functional tests retain existing results.

### Stage 2: Integrate Editor extension registries

- [x] Apply owner identity and bounded retirement to workspace/asset-editor,
  thumbnail, customization, viewport-mode, startup-command, and reference-store
  registrations that retain Plugin code.
- [x] Define live workspace/editor/customization/provider instance destruction
  ordering and reject unload while an escaped instance remains.
- [x] Replace identity-only or parameterless removal where stale registrations
  can remove replacements.
- [x] Add focused registration-race, stale-handle, instance-lifetime, and
  destructor-sensitive tests.

#### Completion Evidence

- Each unloadable Editor module creates `Editor.ExtensionRegistries`; all of its
  workspace routes, thumbnail providers, LevelEditor customizations/modes,
  startup command, reference store, and move observer registrations use that
  owner gate. No-gate overloads remain explicitly limited to process-owned and
  test providers.
- DurinEd publishes host-owned workspace/customization proxies that gate every
  virtual call and retain the Plugin instance. An escaped proxy or active
  viewport mode keeps a resource lease, so the module audit fails closed until
  that instance is destroyed; module shutdown uses the original mapped object
  only for ordered document cleanup.
- Thumbnail capture/session calls are admitted through the provider owner and
  captured inputs/sessions retain an audited lease. Reference-store Fix Up plans
  likewise retain the provider across prepared callbacks, while every store and
  move-observer call re-enters owner admission.
- `CoreUtilityTests` passed 74/74, `EditorShellTests` passed 38/38,
  `ViewportTests` passed 95/95, `ThumbnailTests` passed 54/54,
  `AssetReferenceStoreTests` passed 7/7, the complete `@thumbnail` domain
  passed, and the full `all` target compiled.

#### Acceptance Gate

- Editor extension modules retire registrations and destroy every retained
  callable/provider/instance while mapped, without changing route or
  customization selection behavior.

### Stage 3: Close delegate, timer, watcher, render, and executor findings

- [x] Audit every remaining candidate from Stage 0 and implement owner
  retirement or operation-group drain for required cross-DLL paths.
- [x] Record explicit exemptions for bounded/transient, same-module,
  non-unloadable, and test-only callables with code evidence.
- [x] Remove copied callable snapshots, raw Plugin observer registrations, and
  shutdown ordering assumptions that are not mechanically enforced.
- [x] Add focused drain and destructor-sensitive tests for each migrated
  asynchronous/external execution family.

#### Completion Evidence

- `FConsoleCommandRegistry` retains one owner resource for every stored module
  command, admits dispatch through the owner gate, and returns metadata-only
  command descriptions. Renderer and LevelEditor registrations use their
  manager-created gates; retirement rejects dispatch and keeps the DLL mapped
  until the stored callable is removed and destroyed.
- LevelEditor source-image decodes run in
  `SourceImageThumbnail.Decodes`. Workspace shutdown cancels and waits all
  retained task handles, destroys them before the manager's final async audit,
  and flushes accepted GPU uploads while the rendering thread exists.
  TextureEditor preview submissions and Renderer resource teardown likewise
  synchronously flush their module-authored render commands.
- Shutdown defers `VulkanRHI` from the general reverse-load unload pass. Only
  `RHIExit` unloads it, after render admission is closed, the RHI queue is
  flushed, its terminal shutdown marker completes, the RHI thread stops, and
  `GDynamicRHI` is deleted. A failed unload remains fail-closed.
- Engine asset-delete contributors and DurinEd editor/audit callbacks reside in
  process-resident libraries with no `IMPLEMENT_MODULE` entry. CoreDObject's
  ModuleManager callbacks are process-foundation registrations; GLFW callbacks
  are static ApplicationCore functions whose lifetime is bounded by their
  owning platform window. Search found no production timer or watcher
  registrations. Predicate/visitor callbacks remain synchronous stack values,
  and default unscoped source-thumbnail caches are limited to local native
  tests while production construction supplies the LevelEditor operation scope.
- `CoreUtilityTests` passed 75/75, `CoreConcurrencyTests` passed 138/138,
  `AssetImportCoreTests` passed 27/27, `AssetBuildCoreTests` passed 11/11,
  `EditorShellTests` passed 38/38, `ViewportTests` passed 95/95,
  `ThumbnailTests` passed 54/54, `RHIInitializationTests` passed 5/5,
  `RHIThreadTests` passed 10/10, `RendererResourceReloadVulkanTests` passed
  1/1, `TextureTests` passed 66 with two existing skips, and the full `all`
  target compiled.

#### Acceptance Gate

- No production process-retained Plugin callable lacks owner attribution,
  closed admission, and destruction proof.
- Render/executor work and external threads/watchers cannot reenter a retired
  module, and drain failure prevents unload.

### Stage 4: Regression, lasting contract, and qualification handoff

- [x] Run the frozen symbol/call-site audit and affected Runtime, Developer,
  Editor, build, import, rendering, and module-manager validations.
- [x] Publish the lasting specialized-registry and callback ownership contract.
- [x] Update the parent roadmap with Milestone 4 evidence and complete this
  plan only after every required disposition is closed.
- [x] Create `Documentation/Plans/DynamicDllUnloadQualification.md` only after
  the Milestone 4 exit gate passes.

#### Completion Evidence

- The frozen registration, render-command, task-root, and legacy authoring
  symbol searches leave only the documented process-resident, bounded,
  same-module, and native-test cases.
- [Modular Features and Module Retirement](../Runtime/Core/ModularFeaturesAndModuleRetirement.md)
  now owns the specialized-registry, escaped-instance, external execution,
  render/RHI ordering, and bounded-exemption contracts.
- `fast-all` passed all 55 selected contract, feature, and infrastructure
  targets. The focused Stage 3 matrix, full `all` build, changed-document
  validation, and all-plan validation also passed.
- [Dynamic DLL Unload Qualification](DynamicDllUnloadQualification.md) is the
  active Milestone 5 child plan with a closed fixture scope and explicit
  physical-unload, failure-injection, generation, and stress gates.

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
