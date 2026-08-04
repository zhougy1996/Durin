# Material Render-Proxy Invalidation Plan

Summary: Replace object-wide material invalidation scans with stable render-proxy indirection, lazy parent-chain resolution, and explicit low-frequency structural rebuilds.

Last reviewed: 2026-08-04

Status: Completed
Completed: 2026-08-04

## Current Status

Stages 0 through 5 are complete. Every material interface owns one stable
counted `FMaterialRenderProxy`, and static-mesh scene proxies now retain those
proxy references per stable slot instead of copied `FMaterialRenderData`,
material versions, and dirty flags. Renderer section draws resolve the bound
proxy on the rendering thread; null slot bindings retain the existing default
material fallback.

Material content publication and component-slot binding are now independent.
Parameter, static-property, and inherited changes update the stable proxy
without changing component revision or sending slot updates. Assigning,
clearing, or replacing a slot material sends a binding-only packet ordered by
component revision, while structural mesh/default/bulk changes still recreate
the scene proxy. The unused global `FMaterialUpdateContext` discovery pass has
been removed; structural work stays with the initiating primitive/scene
lifecycle owner.

The broader integration, editor, rendered-output, and shutdown validation
matrix is complete. The runtime contract now also covers replaying a retained
publication after render-command admission restarts, which keeps preview and
scene-proxy creation correct when assets were edited before renderer startup.

The ordinary setter path now publishes immutable state through each material's
stable proxy. One pending publication wave per proxy is coalesced before the
render command applies it, so ordinary parameter and inherited changes do not
snapshot `GDObjectArray`, enumerate components, or push copied material data
into scene-proxy slots. No global material update context remains on the
ordinary or structural material-content path.

The selected replacement is stable render-proxy indirection. Scene proxies
will retain a render-thread-safe `FMaterialRenderProxyRef` for each material
slot instead of a copied material snapshot. A material publishes its local
render state and parent-proxy reference once; the proxy resolves and caches the
inherited state from local and parent versions. Material content changes then
update the proxy without enumerating components. Component work remains
necessary only when the component changes which material proxy is bound to a
slot.

This plan intentionally does not add a persistent
`Material -> Component/Slot` reverse index. Existing object and component
lifecycle paths remain the canonical owners of material objects and scene
primitives rather than feeding a parallel registration system.

Baseline commit: `f443868fbd46902fa6339c8a7c31de1ae4af8ea2`.

## Goal

- Make ordinary material parameter updates independent of the number of loaded
  objects, material instances, and material-consuming components.
- Let all present and future primitive types consume materials through the same
  stable render-proxy contract without adding their concrete types to the
  material subsystem.
- Preserve inherited material behavior without maintaining a permanent
  parent-to-children or material-to-consumer registration graph.
- Separate frequent data publication from rare shader-layout or render-state
  reconstruction.
- Remove the unused global material update context after ordinary publication
  no longer has any production caller of that path.

## Scope

- Stable material render-proxy ownership and render-thread lifetime.
- Immutable publication of local material parameters, static properties,
  shader-map identity, pipeline identity, and referenced render resources.
- Parent-proxy linkage and version-stamped lazy resolution for material
  instances.
- Static-mesh scene-proxy slots retaining material proxy references.
- Slot-binding updates when a component changes its mesh, default material, or
  material override.
- Dirty-path classification for local data, inherited data, binding, and
  structural changes.
- Replacement of ordinary-setter object scans and concrete component
  discovery.
- Focused diagnostics, correctness tests, scaling tests, full-build
  validation, and editor runtime smoke coverage.

## Non-Goals

- A universal object dependency graph or generic producer-consumer
  registration framework.
- A persistent `Material -> Primitive` or `Material -> Slot` reverse index.
- A persistent `Parent Material -> Child Instances` reverse index unless later
  profiling establishes a separate, unresolved structural-update bottleneck.
- Material graph compilation, new PBR inputs, new render passes, or new shader
  permutation features.
- Changing serialized material, material-instance, static-mesh, or component
  formats.
- Making render-thread code dereference `DObject` pointers.
- Eliminating every global operation in rare editor-only shader compilation or
  render-state reconstruction paths before those paths have measured costs.

## Design Decisions and Invariants

### Canonical ownership

- Each live `DMaterialInterface` owns one stable game-thread material proxy
  facade and one ref-counted render-thread proxy state.
- Scene-proxy material slots retain `FMaterialRenderProxyRef`; they do not own
  or retain `DMaterialInterface`.
- Material-instance proxy states retain parent proxy references, not parent
  `DObject` pointers, across the render-thread boundary.
- The owner releases its proxy reference from ordinary object teardown. Parent
  and scene references keep proxy storage alive until accepted render work and
  dependent scene proxies release it.
- Proxy storage and any immutable published snapshots obey the existing render
  command-admission, deferred cleanup, and shutdown audit contracts.

### Published state

- The game thread builds an immutable local render layer containing only
  render-safe values and counted resource references.
- Publication receives a monotonically increasing local version and is ordered
  through the existing render-command stream.
- A material instance publishes local overrides plus its parent proxy
  reference. It does not publish a permanently flattened copy of all inherited
  values.
- The render thread resolves a proxy parent-first. A cached resolved snapshot
  is reusable only while both its local version and observed parent resolved
  version match.
- Rebuilding a resolved snapshot increments that proxy's resolved version.
  Descendants discover the change when they next resolve; no child enumeration
  is required.
- Parent-cycle rejection remains a game-thread material-domain invariant.
  Render-thread resolution treats a cycle as a debug-fatal contract violation,
  not as a recoverable graph search.

### Locked local-layer and cache contract

- `FMaterialLocalRenderLayer` is distinct from resolved
  `FMaterialRenderData`. It contains a sorted immutable set of render parameter
  entries keyed by parameter GUID and type. Each entry stores only its active
  scalar/vector value or a counted `FRHITextureReferenceRef`; it stores no
  `DObject`, `TObjectPtr`, editor metadata, display name, range, or resolved
  source object.
- A base material local layer contains the complete canonical render parameter
  values plus one render-safe static-property value. An instance local layer
  contains only active, non-orphan overrides and no static-property value.
  Static properties therefore resolve from the first ancestor layer that owns
  them.
- Shader-map and pipeline identities are derived while producing the resolved
  snapshot. They are not flattened into descendant local layers. The resolved
  snapshot remains the current `FMaterialRenderData` shape so renderer
  consumption can migrate independently from local publication.
- Parent linkage is proxy metadata beside the local layer, not a field inside
  `FMaterialRenderData`. Root proxies use a null parent identity and parent
  resolved version zero.
- Resolution first resolves the retained parent proxy, then tests the exact
  cache key `(LocalVersion, ParentProxyIdentity, ParentResolvedVersion)`.
  Reusing a cache requires all three values to match. Rebuilding a cache
  publishes a new nonzero resolved version even when the resulting values
  compare equal, so every accepted parent/local publication becomes observable
  lazily by descendants.
- A proxy accepts a publication only when its local version is newer than the
  currently published version. Equal or older publications are stale no-ops
  and increment the stale-publication diagnostic.

### Locked fallback contract

- A null or missing parent proxy is treated as an empty parent layer. Local
  instance overrides still resolve; every missing render field uses the
  existing default `FMaterialRenderData`.
- A null or unavailable texture reference remains null in the resolved
  snapshot and selects the existing renderer-owned default texture. Resolution
  never reacquires a texture through a `DTexture2D` pointer.
- An unresolved shader-map or pipeline identity remains the requested identity
  in the resolved snapshot. Renderer lookup may use its existing fallback
  shader/pipeline until the requested cache entry is available; it does not
  rewrite the material proxy to the fallback identity.
- Destroying the owning material prevents new game-thread publication but does
  not invalidate accepted render commands or the last published local layer.
  Parent and scene-proxy references keep proxy storage and counted render
  resources alive until those references and accepted work are released.

### Structural dirty-operation table

| Operation | Target path | Structural fallback |
| --- | --- | --- |
| Base scalar, vector, or texture value setter; reflected value edit; Undo/Redo | Publish/coalesce a replacement local layer | No |
| Instance override set/clear; reflected override value edit; Undo/Redo | Publish/coalesce instance local overrides | No |
| Base `FMaterialStaticProperties` change | Publish the local static value; resolution derives new shader-map/pipeline identities | No |
| `SetParent`, reflected parent edit, Undo/Redo, or instance import-state exchange | Publish the retained parent proxy identity and a new local version; explicit import completion may synchronously flush visibility | No |
| Initial load, duplication, or reload with the current fixed canonical schema | Publish initial/replacement local state and parent proxy | No |
| Material parameter declaration count, GUID, type, or renderer binding-layout change | Explicit structural update context until a compiled layout contract represents the transition safely | Yes |
| Shader resource-layout or root-signature transition not representable by the current identity values | Explicit structural update context named by the initiating operation | Yes |
| Unknown or newly added static dirty operation not listed above | Existing structural path until focused tests and this table classify it as proxy-safe | Yes |
| Component mesh/default/override assignment | Component-owned slot-binding or scene-proxy path, not material-content invalidation | Not a material structural update |

### Update classes

- `DynamicParameters` publishes a new local layer. It must not enumerate
  `GDObjectArray`, material instances, or components and must not recreate a
  scene proxy.
- Static properties publish a new local layer and invalidate the proxy's
  resolved shader-map and pipeline identities. Renderer caches select or build
  the resulting identities when the proxy is resolved.
- Changing a material instance parent publishes a new parent proxy reference
  and local version. Descendants observe the new resolved version lazily.
- Changing the material assigned to a component slot updates that slot's proxy
  reference through the component/scene path. This is a binding change, not a
  material-content invalidation.
- A shader resource-layout transition that cannot be represented safely by
  proxy publication uses an explicit structural update context. Structural
  fallback must be selected by a specific dirty flag or operation; it must not
  be the default for parameter setters.

### No redundant registration

- The implementation must not introduce register/unregister calls for every
  material consumer solely to support material invalidation.
- New primitive types become material consumers by storing proxy references in
  their scene proxies; the material subsystem must not know their concrete
  component types.
- If later profiling requires reverse lookup, the first candidate is a
  renderer-owned index derived from canonical scene-proxy state. A second
  gameplay-side mirror requires a separately reviewed plan and evidence.

### Threading, ordering, and failure

- Material object mutation, parent changes, and proxy publication originate on
  the game thread.
- Rendering reads and resolves published proxy state only on the render thread.
- Multiple game-thread publications may be coalesced before render-thread
  submission when no externally visible intermediate state is required.
- Accepted publications preserve command order. A stale local version must
  never replace a newer version.
- Update processing uses queued waves rather than a process-global
  non-reentrancy failure flag. Work submitted while a wave is being prepared is
  assigned to the next wave.
- A destroyed material object may prevent new publication, but already
  accepted proxy state remains valid until its counted render references are
  released.
- Invalid or unavailable render resources resolve to the existing fallback
  material/resource policy without retaining dead object pointers.

## Current Foundations and Gaps

### Foundations

- `FMaterialRenderData` is already an immutable render-facing value and does
  not expose reflected material objects to renderer code.
- Material instances already validate parent cycles and resolve inherited
  parameters through the parent chain.
- Material dirty flags already distinguish dynamic parameter, shader-map,
  pipeline, and parent-chain effects.
- Static-mesh components already use stable mesh slot GUIDs and component
  revisions for binding changes.
- Scene proxies and renderer draws already have a single material access point
  per static-mesh section.
- Render resources, render commands, fences, and deferred cleanup already have
  explicit engine shutdown contracts.

### Historical Gaps Resolved by Stages 1-4

- [x] Static-mesh scene proxies no longer retain copied material snapshots or
  material versions per slot.
- [x] Material content changes publish through stable proxies without finding
  every copied slot or consuming component.
- [x] Loaded direct-child and transitive-dependent queries share one
  deterministic implementation with explicit diagnostics.
- [x] Ordinary `MarkRenderDataDirty()` publishes a coalesced proxy wave rather
  than constructing and flushing a global context.
- [x] Tests cover stable proxy identity and lazy inherited-version resolution.

## Implementation Stages

### Stage 0: Lock the Proxy and Invalidation Contract

- [x] Add characterization tests for local instance updates, inherited base
  updates, parent reassignment, multi-level instance chains, multi-slot
  components, stale component revisions, and material destruction with queued
  render work.
- [x] Record current `FMaterialUpdateCounters` for a fixed affected material
  graph while increasing unrelated `DObject` and component counts.
- [x] Specify the immutable local layer independently from the resolved
  `FMaterialRenderData` representation so Stage 2 material-domain evolution
  does not require changing proxy lifetime rules.
- [x] Specify the exact render-thread cache key:
  `(LocalVersion, ParentProxyIdentity, ParentResolvedVersion)`.
- [x] Specify fallback behavior for missing parents, unavailable textures,
  unresolved shader maps, and a material destroyed before its proxy references.
- [x] Decide which existing static dirty operations require an explicit
  structural rebuild because proxy publication cannot safely represent them.
  Default the undecided cases to the existing structural path until a focused
  test proves proxy-only publication safe.
- [x] Update this plan with the selected structural dirty-operation table
  before Stage 1 changes runtime behavior.

#### Acceptance Gate

- Existing externally visible update behavior has characterization coverage.
- Proxy ownership, cache validity, failure behavior, and the boundary between
  proxy publication and structural reconstruction have one unambiguous
  contract.
- The baseline scaling test demonstrates the current unrelated-object
  sensitivity and can detect its removal later.

#### Stage 0 Handoff

- Baseline commit:
  `f443868fbd46902fa6339c8a7c31de1ae4af8ea2`.
- Working set:
  `Documentation/Plans/MaterialRenderProxyInvalidation.md` and
  `Engine/Tests/Native/EngineTests/Private/Materials/MaterialUpdateContextTests.cpp`.
- Key symbols and evidence:
  `FMaterialUpdateCounters`,
  `FMaterialUpdateContextTests.ScanCostGrowsWithUnrelatedLoadedObjectsAndComponents`,
  `FMaterialUpdateContextTests.AcceptedRenderUpdateSurvivesMaterialDestruction`,
  `FMaterialTests.BoundMaterialAndParentChangesUpdateProxyInPlace`,
  `FMaterialTests.ParentTransactionsRenderFromCurrentCanonicalStorage`,
  `FMaterialTests.MultiLevelResolutionReportsSupplyingSourceAndCurrentOverrideState`,
  `FMaterialTests.StaticMeshProxyResolvesPrecedenceAndUpdatesEverySharedMaterialSlot`,
  and
  `FMaterialTests.StaticMeshProxyOrdersRapidCrossSlotUpdatesAndRejectsStaleRevisions`.
- Decisions: local layers are sparse render-safe publications, resolved
  snapshots retain the current renderer-facing shape, parent linkage is proxy
  metadata, the three-part cache key is exact, and declaration/layout changes
  remain structural by default.
- Open questions: none block Stage 1. Concrete container and allocation choices
  may vary if they preserve the locked observable contract.
- Validation: focused `MaterialUpdateContext` characterization tests pass under
  the `Win64-Debug-DurinEditor-Tests` Agent Build Profile.

### Stage 1: Introduce Stable Material Render Proxies

- [x] Add a ref-counted `FMaterialRenderProxy` whose render-thread state contains
  an immutable local layer, optional parent proxy reference, local version,
  cached resolved snapshot, observed parent resolved version, and resolved
  version.
- [x] Give every `DMaterialInterface` one stable proxy identity without
  registering it in a material-consumer table.
- [x] Publish initial proxy state after load and publish replacement local
  state from material mutation paths.
- [x] Publish parent proxy changes from `SetParent`,
  `PostEditChangeProperty`, import-state exchange, duplication/load, and other
  direct parent-write paths.
- [x] Implement parent-first render-thread resolution and cache reuse based on
  local and parent versions.
- [x] Route proxy release through counted render-thread lifetime and existing
  shutdown/deferred-cleanup rules.
- [x] Add unit coverage for version monotonicity, stale-publication rejection,
  lazy descendant observation, long parent chains, parent replacement, and
  owner destruction.

#### Acceptance Gate

- A material and material-instance chain can publish and resolve the same
  render values as the baseline `GetRenderData()` path.
- Updating a parent changes the next resolved descendant snapshot without
  enumerating children.
- No render-thread state retains a `DObject` pointer.
- Proxy destruction passes focused lifecycle and shutdown-audit tests.

#### Stage 1 Handoff

- Baseline commit:
  `72bb03cc7bffd17bddea2106b1a784ca33dbf7f6`.
- Working set:
  `MaterialRenderProxy.h/.cpp`, `MaterialInterface.h/.cpp`,
  `Material.h/.cpp`, `MaterialInstance.h/.cpp`,
  `MaterialRenderProxyTests.cpp`, the `MaterialTests` source registration, and
  this plan.
- Key symbols:
  `FMaterialLocalRenderParameter`, `FMaterialLocalRenderLayer`,
  `FMaterialRenderProxyPublication`, `FMaterialRenderProxy`,
  `DMaterialInterface::GetMaterialRenderProxy`,
  `DMaterialInterface::PublishMaterialRenderProxyState`, and
  `ReleaseMaterialRenderProxy_GameThread`.
- Decisions: stable proxy storage uses intrusive atomic reference counting;
  game-thread publication sorts and captures a render-safe immutable layer;
  render-thread state owns the retained parent proxy and resolved cache; owner
  teardown releases through an ordered render command when admission is open.
- Open questions: none block Stage 2. Scene proxies still store copied
  `FMaterialRenderData`, so ordinary setters continue to publish the new proxy
  and execute the characterized legacy scan until slot bindings migrate.
- Validation: all four `FMaterialRenderProxyTests` and the complete
  `MaterialTests` target pass under
  `Win64-Debug-DurinEditor-Tests`.

### Stage 2: Bind Scene Proxies to Material Proxies

- [x] Replace `FStaticMeshSceneProxy` material snapshots and parallel material
  version arrays with per-slot `FMaterialRenderProxyRef` bindings.
- [x] Change scene-proxy construction to capture the effective material proxy
  for each stable mesh slot.
- [x] Replace material-content `FMaterialRenderUpdate` packets with a
  binding-only update for operations that assign, clear, or replace the
  material occupying a component slot.
- [x] Make renderer section draws resolve the slot proxy and consume the
  resulting immutable snapshot without reading a material object.
- [x] Preserve current component revision ordering for slot-binding changes;
  remove material-version ordering from component updates once the proxy owns
  content versioning.
- [x] Cover shared materials, duplicate material use across slots, fallback
  bindings, material replacement, component unregister, scene-proxy
  replacement, and rapid binding changes.

#### Acceptance Gate

- Material parameter changes appear in every bound static-mesh draw without
  sending a component-slot update.
- Component material assignment still updates only the intended stable slot and
  rejects stale binding updates.
- Adding a future primitive consumer requires retaining material proxy
  references in its scene proxy, not modifying material update code.
- Existing rendered output remains unchanged for the current material feature
  set.

#### Stage 2 Handoff

- Baseline commit:
  `610e43d2de210491e780e81572a2a9f7027cb010`.
- Working set: `PrimitiveSceneProxy`, primitive/static-mesh component
  render-state dispatch, `IScene`/`FScene`, renderer static-mesh section draws,
  the transitional material update context, and native material, thumbnail,
  and editor-rendering tests.
- Key symbols: `FMaterialRenderProxyBindingUpdate`,
  `EPrimitiveRenderStateDirtyFlags::MaterialBinding`,
  `BuildMaterialRenderProxyBindingUpdate`,
  `UpdatePrimitiveMaterialBinding`,
  `FStaticMeshSceneProxy::GetMaterialRenderProxy`, and
  `ResolveMaterialRenderData_RenderThread`.
- Decisions: a slot owns either one counted material proxy or a null fallback
  binding; only binding changes advance component revision; content changes
  resolve through the stable proxy and never enqueue component-slot updates;
  structural mesh/default/bulk edits keep the existing proxy-recreation path;
  explicit structural work remains outside ordinary material publication.
- Open questions: none block Stage 4. The explicit `FMaterialUpdateContext`
  path still owns transitional loaded-object/component diagnostics until the
  structural cleanup stage removes those scan-only details.
- Validation: `MaterialTests` 51/51, `ThumbnailTests` 45/45, and
  `EditorRenderingTests` 10/10 pass under
  `Win64-Debug-DurinEditor-Tests`; the material rendered-thumbnail Vulkan case
  confirms unchanged current output.

### Stage 3: Remove Ordinary-Setter Global Discovery

- [x] Change ordinary `MarkRenderDataDirty(DynamicParameters)` and proxy-safe
  static dirtiness to publish/coalesce proxy state directly.
- [x] Stop constructing and synchronously flushing
  `FMaterialUpdateContext` from ordinary parameter setters.
- [x] Ensure updates produced during world tick and application/editor event
  processing are submitted before frame rendering consumes the proxies.
- [x] Preserve an explicit synchronous flush entry point for tests, import
  completion, save/preview boundaries, and operations that require immediate
  visibility.
- [x] Remove component enumeration, per-slot material matching, handle sorting,
  and repeated handle resolution from the ordinary material update path.
- [x] Replace the process-global reentrancy assertion with queue ownership and
  next-wave semantics.
- [x] Replace ordinary-path scan diagnostics with proxy publication, coalescing,
  resolution-cache hit/miss, structural fallback, stale-publication, and
  binding-update counters.

#### Acceptance Gate

- Repeating a parameter setter in one frame coalesces to the documented number
  of proxy publications.
- Ordinary local and inherited parameter changes perform zero object snapshots
  and zero component enumeration.
- Scaling counters and CPU measurements remain invariant when unrelated object
  and component counts increase.
- Synchronous editor preview and test paths retain explicit, deterministic
  visibility.

#### Stage 3 Handoff

- Baseline commit: `551cbfe1` (`feat(material): bind scene proxies to material proxies`).
- Working set: material proxy publication/lifetime, material interface setter
  invalidation, explicit material update context, primitive proxy binding
  diagnostics, material runtime documentation, and native material tests.
- Key symbols: `FMaterialRenderProxy::QueuePublication_GameThread`,
  `FMaterialRenderProxy::ApplyPendingPublication_RenderThread`,
  `FMaterialRenderProxyCounters`,
  `DMaterialInterface::MarkRenderDataDirty`, and
  `RecordMaterialStructuralFallback`.
- Decisions: each proxy owns one pending game-thread publication wave; later
  versions replace that wave until render-thread ownership begins; ordinary
  setters increment the material's own revision and publish only proxy state;
  `FMaterialUpdateContext` is explicit structural/batch compatibility work;
  the obsolete material-interface context adapter and global reentrancy flag
  were removed.
- Open questions: Stage 4 still needs to make structural traversal generic and
  remove its transitional scan-only counters.
- Validation: focused proxy/update-context tests pass 9/9 and the complete
  `MaterialTests` target passes 54/54 under `Win64-Debug-DurinEditor-Tests`.
  The same profile completes `build --target all` and a hidden five-tick
  `DurinEditor` runtime smoke.

### Stage 4: Isolate Structural Updates and Dependency Queries

- [x] Remove the unused global `FMaterialUpdateContext` after confirming that
  no production caller remains; structural render-state work stays with the
  initiating generic primitive/scene lifecycle surface.
- [x] Remove `DStaticMeshComponent` knowledge from the material subsystem;
  structural work is owned by the generic primitive/scene lifecycle.
- [x] Centralize loaded-material parent-chain queries so
  `GetLoadedDirectMaterialChildren`, `GetLoadedMaterialDependents`, and rare
  structural updates do not carry separate implementations.
- [x] Keep the centralized query scan scoped to loaded material interfaces
  when practical. Do not introduce a permanent parent-to-children index without
  new profiling evidence.
- [x] Define editor hierarchy queries that require unloaded assets as asset
  registry operations rather than runtime object relationships.
- [x] Delete obsolete affected-material/component handles, the global flush
  state, the dedicated sorting helper, and scan-only counters with the removed
  context.
- [x] Add loaded-query diagnostics identifying the operation and the amount of
  snapshot/material/result work it performed.

#### Acceptance Gate

- No material subsystem source names a concrete material-consuming component
  type.
- Only explicitly classified structural operations may perform loaded-material
  traversal or generic render-state reconstruction.
- All runtime loaded-dependent queries share one implementation and preserve
  deterministic public results.
- Debug diagnostics make accidental loaded-relationship traversal visible in
  ordinary parameter setter tests, which require zero loaded-query work.

#### Stage 4 Handoff

- Baseline commit: `c2ea71fd` (`feat(material): coalesce ordinary proxy publications`).
- Working set: loaded material relationship queries, material proxy diagnostics,
  the removed material update context and scan tests/build references, runtime
  rendering documentation, and dependency-query tests.
- Key symbols: `QueryLoadedMaterialHandles`,
  `GetMaterialLoadedQueryDiagnostics`,
  `EMaterialLoadedQueryOperation`, and
  `DMaterialInterface::GetMaterialRenderProxy`.
- Decisions: no production caller justified retaining the global material
  update context; material relationship queries share one loaded-object scan
  helper and remain deterministic; unloaded hierarchy queries belong to the
  asset registry; proxy counters no longer expose an unreachable structural
  fallback counter.
- Open questions: Stage 5 still needs broad editor, renderer readback,
  lifecycle, and stress coverage.
- Validation: focused dependency/proxy tests pass 10/10; complete
  `MaterialTests` passes 50/50; the same `Win64-Debug-DurinEditor-Tests`
  profile completes `build --target all` and a hidden five-tick `DurinEditor`
  runtime smoke.

### Stage 5: Complete Validation and Document the Runtime Contract

- [x] Run the focused material, static-mesh, renderer, object-lifecycle, and
  shutdown test suites.
- [x] Add a stress test covering long parent chains, many shared users, many
  slots, rapid update/binding interleaving, queued destruction, and a large
  unrelated object population.
- [x] Add rendered-image or deterministic render-readback coverage proving
  local parameters, inherited parameters, textures, shader-map identities, and
  pipeline identities update through the proxy.
- [x] Validate editor edits, Undo/Redo, asset reload, import-state exchange,
  preview/thumbnail updates, save/reload, and repeated clean exit.
- [x] Complete the full build and editor runtime smoke validation required by
  `Documentation/Development/Build/BuildAndRun.md`.
- [x] Move lasting proxy ownership, publication, parent-resolution, and
  structural-fallback contracts into the owning runtime rendering
  documentation.
- [x] Update `Documentation/Plans/MaterialSystem.md` to reference the completed
  work and remove superseded scan-based claims.

#### Stage 5 Handoff

- Baseline commit: `ad4d5bb2` (`refactor(material): remove unused global update scan`).
- Working set: proxy publication retry behavior, the combined material stress
  test, Vulkan inherited-parameter readback coverage, runtime material
  documentation, the Material System plan reference, and this completed plan.
- Key symbols: `FMaterialRenderProxy::QueuePublication_GameThread`,
  `FMaterialRenderProxyTests.StressSharedUsersSlotsInterleavedPublicationAndDestruction`,
  `FMaterialTests.RenderedThumbnailPreviewSceneCapturesResolvedMaterialDifferences`,
  and `DMaterialInterface::GetMaterialRenderProxy`.
- Decisions: a retained pending wave is replayed after render-command
  admission restarts; proxy publication and loaded-query diagnostics remain
  separate; rendered output proves local, inherited, and texture changes while
  deterministic proxy snapshots prove shader-map and pipeline identity changes.
- Open questions: none block completion. Future surface policies, shader graph
  compilation, and dynamic runtime material APIs remain in the separate
  material-system backlog.
- Validation: `MaterialTests` 51/51, `StaticMeshTests` 44/44,
  `WorldTests` 61/61, `EditorRenderingTests` 31/31, `ThumbnailTests` 45/45,
  `RendererResourceReloadVulkanTests` 1/1, `EditorPropertyTests` 25/25, and
  `EditorAssetWorkflowTests` 43/43 passed under
  `Win64-Debug-DurinEditor-Tests`; the focused stress and inherited readback
  cases also passed. The same profile completed `build --target all` and two
  hidden five-tick `DurinEditor` runs with clean exit.

#### Acceptance Gate

- The validation matrix passes without relying on the removed global material
  consumer scan.
- A successful full `all` build and `DurinEditor` runtime smoke test are
  recorded from the same Agent Build Profile.
- Runtime documentation owns the final contract, and this plan contains a
  compact completed-stage handoff with commit, working set, decisions, open
  questions, and validation evidence.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Unit | Local/resolved versions, cache keys, parent-chain resolution, fallback state, stale publication, and cycle contract |
| Material integration | Base and instance parameter changes, parent replacement, import exchange, serialization reload, and texture reference changes |
| Component integration | Initial slot capture, override/default changes, multi-slot/shared material use, stale binding revisions, unregister, and proxy recreation |
| Renderer | Shader-map and pipeline identity selection, immutable snapshot lifetime, fallback resources, and no `DObject` access |
| Threading/lifecycle | Ordered publication, next-wave updates, owner destruction, parent proxy lifetime, scene release, render admission close, and repeated clean shutdown |
| Performance | Ordinary setters perform no object/component enumeration; cost does not grow with unrelated objects or users |
| Editor | Details edits, Undo/Redo, preview, thumbnail, asset reload, save/reload, and parent reassignment |
| End to end | Focused suites, full `all` build, and `DurinEditor` runtime smoke test using the documented workflow |

## Definition of Done

- Static-mesh scene proxies bind stable material render proxies rather than
  copied material snapshots.
- Ordinary material parameter and inherited parameter changes reach rendering
  without scanning `GDObjectArray`, enumerating material instances, or finding
  components.
- Material slot assignment remains an explicit component binding update and is
  not confused with material-content invalidation.
- No persistent material-consumer or material-child reverse registry was added.
- Structural traversal is explicit, rare, generic over consumer types, and
  instrumented.
- Obsolete scan, sorting, repeated resolve, hard-coded component, reentrancy,
  and scan-counter code is removed.
- Required unit, integration, renderer, lifecycle, performance, editor, full
  build, and runtime smoke validation passes.
- Lasting behavior is documented outside this completed plan.

## Deferred Follow-ups

- Renderer-owned material-to-draw indexing, only if profiling later shows a
  concrete structural-update use case that proxy indirection cannot handle.
- A direct parent-to-children material index, only if rare loaded-material
  structural traversal becomes a measured editor bottleneck.
- Transient dynamic material instances and gameplay-specific mutation APIs.
- Asynchronous shader compilation and fallback transitions.
- Generalization of stable render-proxy publication to textures, meshes, or
  other render assets through their own domain plans.

## Related Documentation

- `Documentation/Plans/MaterialSystem.md`
- `Documentation/Runtime/Rendering/MaterialSystem.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Development/Build/BuildAndRun.md`

## Related Code

- `Engine/Source/Runtime/Engine/Public/Materials/MaterialInterface.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialTypes.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialRenderProxy.h`
- `Engine/Source/Runtime/Engine/Public/Engine/PrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInterface.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInstance.cpp`
- `Engine/Source/Runtime/Engine/Private/Components/StaticMeshComponent.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/PrimitiveSceneProxy.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
