# Material Render-Proxy Invalidation Plan

Summary: Replace object-wide material invalidation scans with stable render-proxy indirection, lazy parent-chain resolution, and explicit low-frequency structural rebuilds.

Last reviewed: 2026-07-30

Status: Active
Completed:

## Current Status

Material setters currently route through `FMaterialUpdateContext`, which
snapshots `GDObjectArray`, discovers dependent materials by testing parent
chains, finds concrete `DStaticMeshComponent` consumers, and pushes copied
`FMaterialRenderData` into each affected scene-proxy slot. This preserves live
updates, but ordinary parameter changes scale with the total loaded object
population and make the material subsystem responsible for discovering every
consumer type.

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
- Reduce `FMaterialUpdateContext` to an explicit structural/batch operation
  rather than the mandatory path for every setter.

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
  their scene proxies; `FMaterialUpdateContext` must not know their concrete
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

### Gaps

- `FStaticMeshSceneProxy` stores copied `FMaterialRenderData` and material
  versions per slot.
- Material content changes must therefore discover and push updates to every
  copied slot.
- `FMaterialUpdateContext` snapshots all objects and hard-codes
  `DStaticMeshComponent` as the only consumer.
- `GetLoadedDirectMaterialChildren` and `GetLoadedMaterialDependents` repeat
  object-wide dependency discovery outside the update context.
- Ordinary `MarkRenderDataDirty()` constructs and flushes a context
  synchronously, preventing natural coalescing.
- Current tests assert the copied-slot update mechanism rather than a stable
  proxy and lazy inherited-version contract.

## Implementation Stages

### Stage 0: Lock the Proxy and Invalidation Contract

- [ ] Add characterization tests for local instance updates, inherited base
  updates, parent reassignment, multi-level instance chains, multi-slot
  components, stale component revisions, and material destruction with queued
  render work.
- [ ] Record current `FMaterialUpdateCounters` for a fixed affected material
  graph while increasing unrelated `DObject` and component counts.
- [ ] Specify the immutable local layer independently from the resolved
  `FMaterialRenderData` representation so Stage 2 material-domain evolution
  does not require changing proxy lifetime rules.
- [ ] Specify the exact render-thread cache key:
  `(LocalVersion, ParentProxyIdentity, ParentResolvedVersion)`.
- [ ] Specify fallback behavior for missing parents, unavailable textures,
  unresolved shader maps, and a material destroyed before its proxy references.
- [ ] Decide which existing static dirty operations require an explicit
  structural rebuild because proxy publication cannot safely represent them.
  Default the undecided cases to the existing structural path until a focused
  test proves proxy-only publication safe.
- [ ] Update this plan with the selected structural dirty-operation table
  before Stage 1 changes runtime behavior.

#### Acceptance Gate

- Existing externally visible update behavior has characterization coverage.
- Proxy ownership, cache validity, failure behavior, and the boundary between
  proxy publication and structural reconstruction have one unambiguous
  contract.
- The baseline scaling test demonstrates the current unrelated-object
  sensitivity and can detect its removal later.

### Stage 1: Introduce Stable Material Render Proxies

- [ ] Add a ref-counted `FMaterialRenderProxy` whose render-thread state contains
  an immutable local layer, optional parent proxy reference, local version,
  cached resolved snapshot, observed parent resolved version, and resolved
  version.
- [ ] Give every `DMaterialInterface` one stable proxy identity without
  registering it in a material-consumer table.
- [ ] Publish initial proxy state after load and publish replacement local
  state from material mutation paths.
- [ ] Publish parent proxy changes from `SetParent`,
  `PostEditChangeProperty`, import-state exchange, duplication/load, and other
  direct parent-write paths.
- [ ] Implement parent-first render-thread resolution and cache reuse based on
  local and parent versions.
- [ ] Route proxy release through counted render-thread lifetime and existing
  shutdown/deferred-cleanup rules.
- [ ] Add unit coverage for version monotonicity, stale-publication rejection,
  lazy descendant observation, long parent chains, parent replacement, and
  owner destruction.

#### Acceptance Gate

- A material and material-instance chain can publish and resolve the same
  render values as the baseline `GetRenderData()` path.
- Updating a parent changes the next resolved descendant snapshot without
  enumerating children.
- No render-thread state retains a `DObject` pointer.
- Proxy destruction passes focused lifecycle and shutdown-audit tests.

### Stage 2: Bind Scene Proxies to Material Proxies

- [ ] Replace `FStaticMeshSceneProxy` material snapshots and parallel material
  version arrays with per-slot `FMaterialRenderProxyRef` bindings.
- [ ] Change scene-proxy construction to capture the effective material proxy
  for each stable mesh slot.
- [ ] Replace material-content `FMaterialRenderUpdate` packets with a
  binding-only update for operations that assign, clear, or replace the
  material occupying a component slot.
- [ ] Make renderer section draws resolve the slot proxy and consume the
  resulting immutable snapshot without reading a material object.
- [ ] Preserve current component revision ordering for slot-binding changes;
  remove material-version ordering from component updates once the proxy owns
  content versioning.
- [ ] Cover shared materials, duplicate material use across slots, fallback
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

### Stage 3: Remove Ordinary-Setter Global Discovery

- [ ] Change ordinary `MarkRenderDataDirty(DynamicParameters)` and proxy-safe
  static dirtiness to publish/coalesce proxy state directly.
- [ ] Stop constructing and synchronously flushing
  `FMaterialUpdateContext` from ordinary parameter setters.
- [ ] Ensure updates produced during world tick and application/editor event
  processing are submitted before frame rendering consumes the proxies.
- [ ] Preserve an explicit synchronous flush entry point for tests, import
  completion, save/preview boundaries, and operations that require immediate
  visibility.
- [ ] Remove component enumeration, per-slot material matching, handle sorting,
  and repeated handle resolution from the ordinary material update path.
- [ ] Replace the process-global reentrancy assertion with queue ownership and
  next-wave semantics.
- [ ] Replace scan-oriented counters with proxy publication, coalescing,
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

### Stage 4: Isolate Structural Updates and Dependency Queries

- [ ] Restrict `FMaterialUpdateContext` to the structural operations selected
  in Stage 0 and to explicit tool/import batches.
- [ ] Remove `DStaticMeshComponent` knowledge from the structural material
  context; structural render-state work must use the existing generic
  primitive/scene lifecycle surface or an explicit caller-provided component
  set.
- [ ] Centralize loaded-material parent-chain queries so
  `GetLoadedDirectMaterialChildren`, `GetLoadedMaterialDependents`, and rare
  structural updates do not carry separate implementations.
- [ ] Keep the centralized query scan scoped to loaded material interfaces
  when practical. Do not introduce a permanent parent-to-children index without
  new profiling evidence.
- [ ] Define editor hierarchy queries that require unloaded assets as asset
  registry operations rather than runtime object relationships.
- [ ] Delete obsolete affected-material/component handles, sorting helpers,
  global flush state, and scan-only counters after all call sites migrate.
- [ ] Add diagnostics identifying the operation that selected a structural
  fallback and the amount of work it performed.

#### Acceptance Gate

- No material subsystem source names a concrete material-consuming component
  type.
- Only explicitly classified structural operations may perform loaded-material
  traversal or generic render-state reconstruction.
- All runtime loaded-dependent queries share one implementation and preserve
  deterministic public results.
- Debug diagnostics make accidental use of the structural path by an ordinary
  parameter setter test-fatal.

### Stage 5: Complete Validation and Document the Runtime Contract

- [ ] Run the focused material, static-mesh, renderer, object-lifecycle, and
  shutdown test suites.
- [ ] Add a stress test covering long parent chains, many shared users, many
  slots, rapid update/binding interleaving, queued destruction, and a large
  unrelated object population.
- [ ] Add rendered-image or deterministic render-readback coverage proving
  local parameters, inherited parameters, textures, shader-map identities, and
  pipeline identities update through the proxy.
- [ ] Validate editor edits, Undo/Redo, asset reload, import-state exchange,
  preview/thumbnail updates, save/reload, and repeated clean exit.
- [ ] Complete the full build and editor runtime smoke validation required by
  `Documentation/Development/Build/BuildAndRun.md`.
- [ ] Move lasting proxy ownership, publication, parent-resolution, and
  structural-fallback contracts into the owning runtime rendering
  documentation.
- [ ] Update `Documentation/Plans/MaterialSystem.md` to reference the completed
  work and remove superseded scan-based claims.

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
- Lasting behavior is documented outside the active plan.

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
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialUpdateContext.h`
- `Engine/Source/Runtime/Engine/Public/Engine/PrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInterface.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInstance.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialUpdateContext.cpp`
- `Engine/Source/Runtime/Engine/Private/Components/StaticMeshComponent.cpp`
- `Engine/Source/Runtime/Engine/Private/Engine/PrimitiveSceneProxy.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
