# Renderer Scene Proxy and Info Contract Plan

Summary: Split every production scene-resident rendering item into an Engine-facing SceneProxy and Renderer-private SceneInfo, detach directional-light state, and replace whole-scene RTTI discovery with typed FScene storage.

Last reviewed: 2026-08-08

Status: Completed
Completed: 2026-08-08

## Current Status

M1 implementation and Stage 4 qualification are complete. StaticMesh,
TextureCube preview, directional light, and SkyBox now use paired SceneProxy and
SceneInfo ownership, strong family identities, FIFO render-command mutation,
and authoritative typed `FScene` views. Rendering retains no directional-light
component pointer, the SkyBox revision map is gone, and feature renderers no
longer rediscover primitive families through whole-scene RTTI scans.

The lasting contract is documented in
[Renderer Scene Representation](../Runtime/Rendering/SceneRepresentation.md).
Focused scene-contract, StaticMesh, SkyBox, TextureCube thumbnail, material,
and editor-rendering suites pass, as do SkyBox and scene-import Vulkan
integration. All-plan validation and a full `all` build pass under
`Win64-Debug-DurinEditor-Tests`.

## Goal

Give every production scene-resident primitive, light, and SkyBox a consistent
two-layer renderer representation:

- a SceneProxy containing detached family-specific rendering state; and
- a Renderer-private SceneInfo containing scene membership, stable identity,
  generic classification, bounds or other scene facts, and internal lookup
  state.

After the plan, `FScene` owns every Proxy through its paired SceneInfo, feature
renderers consume typed collections without whole-scene RTTI scans, and no
render-thread path reads a component, actor, reflected asset, or other game-
thread object.

## Scope

- Rename the primitive base to the repository-standard
  `FPrimitiveSceneProxy` and pair primitive proxies with
  `FPrimitiveSceneInfo`.
- Align `FStaticMeshSceneProxy` and `FTextureCubePreviewSceneProxy` with the
  primitive SceneInfo and typed-collection contract.
- Add `FLightSceneProxy`, `FDirectionalLightSceneProxy`, and
  `FLightSceneInfo`, then replace every directional-light component pointer in
  `FScene` with detached proxy state.
- Add `FSkyBoxSceneProxy` and `FSkyBoxSceneInfo` instead of treating the
  SkyBox value snapshot as an unpaired special case.
- Define strong scene identities, ownership transfer, ordered add/update/
  replace/remove semantics, release behavior, and complete-or-null insertion.
- Add primitive-family classification, local/world bounds, visibility facts,
  and a non-RTTI material-binding update route needed by later roadmap stages.
- Replace untyped primitive scans with Renderer-private typed collections or
  typed views maintained by `FScene`.
- Preserve existing StaticMesh render-data borrows, material-proxy retention,
  SkyBox texture retention, viewport behavior, resource invalidation, and
  image output.
- Add focused ownership, ordering, classification, replacement, removal, and
  component-retirement coverage before broad rendering validation.

## Non-Goals

- Opaque, masked, or translucent pass behavior; that belongs to M2.
- Frustum culling, LOD selection, prepared draw lists, or sorting; those belong
  to M3.
- Point lights, spot lights, multi-light GPU payloads, or light culling; those
  belong to M5.
- Shadow resources, caster preparation, or shadow sampling; those belong to
  M6.
- A public renderer, pass, SceneProxy, or SceneInfo registration API.
- Converting `FSceneRenderer` feature owners such as post-process, editor
  assistance, or default-texture resources into SceneProxy/SceneInfo pairs.
- Replacing the render-command pipe with a new task system, forcing a scene-
  specific per-frame batch, or adding synchronization waits to ordinary scene
  mutation.
- Removing owner-specific material, texture, or resource revisions that guard
  genuinely independent asynchronous work.

## Design Decisions and Invariants

### Scene residents use a two-layer representation

The selected family pairs are:

| Scene family | Engine-facing proxy | Renderer-private scene state |
| --- | --- | --- |
| Primitive | `FPrimitiveSceneProxy` with `FStaticMeshSceneProxy` and `FTextureCubePreviewSceneProxy` derivatives | `FPrimitiveSceneInfo` |
| Light | `FLightSceneProxy` with `FDirectionalLightSceneProxy` | `FLightSceneInfo` |
| SkyBox | `FSkyBoxSceneProxy` | `FSkyBoxSceneInfo` |

A SceneProxy contains detached rendering facts and retained renderer-facing
resources for one family. A SceneInfo contains the scene identity, owning
`FScene`, collection membership, classification and generic scene facts needed
without invoking feature-specific rendering code. A SceneInfo owns exactly one
Proxy. SceneInfo objects are not exposed through `IScene` and are never created
or mutated by components.

`FPrimitiveSceneInfo` owns transform, local/world bounds, visibility flags,
primitive-family classification, and internal typed-list membership.
Primitive proxies own family-specific render data, vertex-factory inputs, and
material bindings. Light and SkyBox SceneInfo objects follow the same split for
their relevant generic membership and selection state without forcing every
family into one universal base class.

### Ownership crosses once

- A component constructs a detached SceneProxy candidate from copied values
  and retained renderer-facing resources on the game thread.
- `IScene` accepts unique ownership plus a strong scene identity and enqueues
  that ownership through the existing render-command pipe.
- The rendering thread creates the corresponding SceneInfo and becomes the
  sole owner and mutator of both objects.
- After transfer, the component may retain only its strong scene identity. It
  must not dereference or mutate the Proxy or SceneInfo.
- Removal destroys typed membership, SceneInfo, and Proxy on the rendering
  thread. Borrowed StaticMesh render data remains protected by the existing
  component-removal and asset-release fence protocol.
- Failed or null candidates never publish partial SceneInfo membership.

### Identity and ordering are separate concerns

Primitive, light, and SkyBox identities are strong, family-specific types.
They remain stable across render-state recreation for the lifetime of their
originating component and are not raw component or proxy pointers.

Ordinary scene mutations have one game-thread producer and execute through the
existing FIFO render-command pipe. Their order is the mutation order; they do
not carry a universal per-entry revision. Remove followed by add for the same
identity creates a new entry deterministically, and an update after removal is
ignored unless a later add has already executed.

A revision or generation is allowed only where Stage 0 identifies a genuine
reordering or reuse boundary, such as independently completed asynchronous
work or a reclaimed slot. Existing material/resource revisions remain owned by
their current protocols. Stage 0 must determine whether the current SkyBox
revision guards such a boundary or merely duplicates FIFO ordering before it
is retained or removed.

### Classification is explicit and private

Primitive family is one explicit axis and is not combined with material pass,
shading model, vertex factory, view mode, or output policy. `FScene` maintains
authoritative typed collections or typed views when it attaches and detaches a
SceneInfo. Feature renderers request only their corresponding typed entries;
they do not scan a shared vector, call `dynamic_cast`, or maintain a second
registry.

The exact representation of typed membership—family-specific SceneInfo
derivatives or one SceneInfo with type-safe family views—is frozen in Stage 0.
Either representation must provide complete attach/detach rollback, stable
iteration for one render command, and no public runtime registration surface.

### Mutation never reaches reflected objects

Transform, bounds, visibility, light values, SkyBox values, and material-
binding changes cross as copied payloads or uniquely transferred proxy
candidates. `FScene`, SceneInfo, and SceneProxy code may retain counted render
resources and explicitly bounded render-data borrows, but rendering must not
call a component getter or dereference a `DObject`, actor, component, or asset.

### Existing rendering composition remains intact

`FSceneRenderer` continues to compose StaticMesh, SkyBox, TextureCube preview,
post-process, and editor-assistance feature owners explicitly. This plan
changes how scene-resident work reaches those owners; it does not introduce a
pass framework or move shader, pipeline, retry, invalidation, and release
ownership out of the existing feature renderers.

## Current Foundations and Gaps

| Area | Foundation | Gap owned by this plan |
| --- | --- | --- |
| Primitive identity | `FPrimitiveSceneId` is stable and component mutations already enqueue render commands. | The base name is inconsistent, no SceneInfo exists, and replacement/removal have no common typed-membership owner. |
| Primitive proxies | StaticMesh and TextureCube preview already use detached `PrimitiveSceneProxy` derivatives. | Transform lives on the proxy, bounds and visibility are absent, and feature renderers rediscover types with whole-array RTTI scans. |
| StaticMesh lifetime | Proxy render-data borrows and material proxy references have focused lifecycle coverage. | The lifetime must remain unchanged when SceneInfo becomes the owning scene entry. |
| Directional light | `FDirectionalLightSceneData` can represent copied light values. | `FScene` stores `DDirectionalLightComponent*`, mutates that array on the game thread, and calls `GetSceneData()` while rendering. |
| SkyBox | Stable instance identity, retained texture references, ordered commands, selection, and focused/Vulkan tests exist. | `FSkyBoxSceneData` is stored directly with a separate revision map instead of a Proxy/SceneInfo pair. |
| Scene storage | `FScene` owns primitive proxies and SkyBox snapshots and exposes them to feature renderers. | Storage is inconsistent across families and permits untyped discovery and type-specific mutation branches. |
| Command ordering | The render-command pipe serializes accepted commands and supports targeted fences. | Scene APIs do not state their single-producer/order contract, while lights bypass the pipe entirely. |

## Implementation Stages

### Stage 0: Freeze the scene-entry contract and baseline

- [x] Record every primitive, directional-light, and SkyBox add, update,
  replace, remove, scene-release, and consumer call site.
- [x] Record the owning thread and lifetime boundary for every payload and
  retained render resource crossing those calls.
- [x] Select the exact typed-membership representation and document its attach,
  replacement, rollback, detach, and iteration behavior.
- [x] Freeze Proxy versus SceneInfo field ownership, including transforms,
  bounds, visibility, family kind, material updates, light values, and SkyBox
  selection state.
- [x] Confirm and assert the single game-thread producer contract for ordinary
  scene mutations, identifying any real asynchronous exception.
- [x] Decide with evidence whether SkyBox revisioning remains necessary and
  record any retained revision at its actual reordering boundary.
- [x] Add or identify focused baseline coverage for FIFO mutation, remove/add
  recreation, update-after-remove, component retirement, scene release, and
  complete-or-null insertion.
- [x] Record the baseline commit, working set, symbols, decisions, open
  questions, and validation result in the stage handoff.

#### Acceptance Gate

- Every current scene mutation and consumer is assigned to one selected Proxy,
  SceneInfo, typed collection, and thread owner.
- No unresolved storage, ownership, ordering, or failure choice remains before
  the first C++ migration.
- Baseline focused tests pass or any pre-existing failure is recorded without
  being attributed to this plan.

### Stage 1: Introduce primitive SceneInfo and typed classification

- [x] Rename `PrimitiveSceneProxy` to `FPrimitiveSceneProxy` and migrate all
  constructors, ownership declarations, and call sites atomically.
- [x] Add `FPrimitiveSceneInfo` with stable identity, transform, bounds,
  visibility, primitive kind, owning scene, and selected typed-membership
  state.
- [x] Make `FPrimitiveSceneInfo` the sole scene owner of each
  `FPrimitiveSceneProxy` and preserve complete-or-null replacement.
- [x] Publish local bounds from StaticMesh and TextureCube preview proxies and
  derive finite world bounds under transform updates.
- [x] Replace StaticMesh-specific material mutation in `FScene` with the
  selected proxy or typed-entry contract.
- [x] Replace the shared primitive vector and `dynamic_cast` scans in
  StaticMesh and TextureCube preview rendering with typed SceneInfo access.
- [x] Preserve StaticMesh render-data borrow, material-proxy, replacement,
  invalidation, and release-fence behavior.
- [x] Add focused primitive classification, membership, transform/bounds,
  material update, replacement, removal, and scene-release tests.
- [x] Record the stage handoff and validation evidence.

#### Acceptance Gate

- Every StaticMesh and TextureCube preview entry has exactly one
  `FPrimitiveSceneInfo` and one corresponding proxy while attached.
- Feature renderers perform no whole-scene `dynamic_cast` discovery, and
  `FScene` contains no StaticMesh-only mutation branch.
- Replacement and removal leave no dangling typed entry or proxy, including
  asset replacement and shutdown paths.
- Existing StaticMesh lifecycle, material update, thumbnail, main/auxiliary,
  and offscreen behavior remains unchanged.

### Stage 2: Add light Proxy and SceneInfo ownership

- [x] Add strong light scene identity issuance and component lifetime storage.
- [x] Add `FLightSceneProxy`, `FDirectionalLightSceneProxy`, and
  `FLightSceneInfo` with Renderer-private typed light membership.
- [x] Build directional-light proxy candidates from copied component state and
  transfer ownership through `IScene` without retaining the component.
- [x] Route registration, visibility, transform, color, intensity, and removal
  through ordered render commands and update only the Renderer-owned entry.
- [x] Preserve the current deterministic single-directional-light selection
  policy without implementing M5 multi-light shading.
- [x] Add focused light add/update/remove, remove/add recreation, hidden-state,
  component-retirement, scene-release, and multi-view snapshot tests.
- [x] Record the stage handoff and validation evidence.

#### Acceptance Gate

- `IScene`, `FScene`, SceneInfo, proxies, and feature renderers retain no
  `DDirectionalLightComponent*` for rendering.
- Every directional-light read occurs from a render-thread-owned proxy through
  its SceneInfo, and no render path calls `GetSceneData()` on a component.
- Ordered mutation and retirement tests prove that a destroyed or removed
  component cannot affect a later render.
- Existing lit StaticMesh output and no-light fallback remain unchanged.

### Stage 3: Align SkyBox with Proxy and SceneInfo

- [x] Add `FSkyBoxSceneProxy` and `FSkyBoxSceneInfo` and move retained texture,
  rotation, tint, intensity, identity, and selection responsibilities to the
  Stage 0-selected owners.
- [x] Transfer SkyBox proxy candidates through the same complete-or-null scene
  ownership protocol used by primitive and light entries.
- [x] Replace direct `FSkyBoxSceneData` storage and the separate revision map
  with the selected ordered mutation contract, retaining a version only if
  Stage 0 proved an independent reordering boundary.
- [x] Adapt `FSkyBoxRenderer` to consume the typed SkyBox SceneInfo collection
  without changing active-SkyBox selection or viewport behavior.
- [x] Preserve texture reference, component visibility, resource reload,
  editor, and Vulkan coverage.
- [x] Record the stage handoff and validation evidence.

#### Acceptance Gate

- Every attached SkyBox has exactly one SceneInfo and Proxy and no parallel raw
  snapshot entry remains in `FScene`.
- Active-SkyBox selection, remove/add recreation, component retirement, scene
  release, and texture lifetime are deterministic under the documented order
  contract.
- Main, auxiliary, window-backed, offscreen, and fixed-aspect SkyBox output is
  unchanged.

### Stage 4: Consolidate contracts and qualify M1

- [x] Remove obsolete pointer, raw-snapshot, untyped-vector, RTTI-discovery,
  and duplicate revision paths after all consumers migrate.
- [x] Add diagnostics or focused assertions for invalid identity, wrong-thread
  mutation, missing typed membership, duplicate attach, and incomplete detach.
- [x] Update Runtime Rendering documentation with the lasting Proxy/SceneInfo,
  identity, ordering, ownership, and typed-collection contracts.
- [x] Update the roadmap M1 status and retain this plan as implementation
  provenance.
- [x] Run the focused native suites, Renderer integration coverage, relevant
  Vulkan rendering tests, plan validation, and a successful full `all` build
  following the repository build guidance.
- [x] Record the final handoff with baseline commit, working set, symbols,
  decisions, open questions, and validation results.

#### Acceptance Gate

- StaticMesh, TextureCube preview, directional light, and SkyBox all use the
  selected Proxy/SceneInfo ownership model with authoritative typed storage.
- Rendering reads no component or other reflected object, and ordinary scene
  mutation depends on one documented FIFO order instead of universal
  revisions.
- Existing images, multi-view behavior, resource lifecycle, failure recovery,
  and Vulkan validation remain clean.
- The roadmap M1 exit gate is satisfied and M2, M3, and M5 can build on the
  resulting contracts without another scene-ownership refactor.

## Stage Handoffs

### Stage 0

- Baseline: `cd14bdd3`.
- Working set: `IScene`, primitive/light/SkyBox components and proxies,
  `FScene`, three scene feature consumers, and their focused native tests.
- Decision: one `FPrimitiveSceneInfo` plus explicit-kind typed views; family-
  specific strong `TSceneId` values; ordinary mutation follows the existing
  single-producer FIFO render-command order.
- Ordering evidence: SkyBox add/replace/remove all enqueue onto that same pipe;
  no independently completed SkyBox work can overtake it, so the revision map
  was redundant and selected for removal. Material/resource revisions remain
  with their independent owners.
- Validation: existing StaticMesh and SkyBox focused baselines passed after the
  contract migration; no pre-existing failure was observed. Open questions:
  none.

### Stage 1

- Baseline: `cd14bdd3`; key symbols: `FPrimitiveSceneProxy`,
  `EPrimitiveSceneProxyKind`, `FPrimitiveSceneInfo`,
  `FScene::GetStaticMeshSceneInfos`, and
  `FScene::GetTextureCubePreviewSceneInfos`.
- Decision: SceneInfo owns transform, local/world AABB, visibility, identity,
  classification, and typed membership; proxies retain family render data and
  material bindings. Material updates dispatch through the base proxy contract.
- Validation: `RendererSceneContractTests` covers classification, transformed
  bounds, complete-or-null replacement, removal, update-after-remove,
  remove/add recreation, and release. `FStaticMeshUpdateTests.*` passes. Open
  questions: none.

### Stage 2

- Baseline: `cd14bdd3`; key symbols: `FLightSceneProxy`,
  `FDirectionalLightSceneProxy`, `FLightSceneInfo`, and
  `FScene::AddOrReplaceDirectionalLight`.
- Decision: components keep only `FLightSceneId`, publish copied proxy
  candidates for registration, property, visibility, and transform changes,
  and never cross a component pointer into Renderer storage.
- Validation: `RendererSceneContractTests` proves copied state outlives its
  publisher scope and FIFO replace/remove/re-add behavior. Open questions:
  point/spot and multi-light payload policy remain intentionally deferred to
  M5.

### Stage 3

- Baseline: `cd14bdd3`; key symbols: `FSkyBoxSceneProxy`,
  `FSkyBoxSceneInfo`, and `FScene::GetActiveSkyBoxSceneInfo_RenderThread`.
- Decision: SceneInfo owns runtime/persistent identities and selection keys;
  the proxy owns retained texture and visual values. FIFO order replaces the
  duplicate SkyBox revision map.
- Validation: `FSkyBoxTests.*` passes selection, FIFO recreation, component
  synchronization/retirement, auxiliary-scene isolation, and release. Open
  questions: none.

### Stage 4

- Baseline: `cd14bdd3`; lasting contract:
  [Renderer Scene Representation](../Runtime/Rendering/SceneRepresentation.md).
- Working set and key decisions are unchanged from Stages 1-3.
- Validation: `RendererSceneContractTests` 2/2, `StaticMeshTests` 49/49,
  `SkyBoxTests` 10/10, `TextureThumbnailTests` 6/6, `MaterialTests` 78/78,
  `EditorRenderingTests` 33/33, `SkyBoxVulkanIntegrationTests` 1/1, and
  `SceneImportVulkanTests` 1/1 passed. All-plan validation passed, followed by
  a successful full `all` build for `Win64-Debug-DurinEditor-Tests`. Open
  questions: none; M2, M3, and M5 may use this contract as their baseline.

## Validation Matrix

| Contract | Focused validation | Integration outcome |
| --- | --- | --- |
| Thread ownership | Wrong-thread assertions; component retirement before queued removal completes; scene release with pending entries | No render command or view dereferences a game-thread object. |
| Identity and order | Invalid identity, duplicate attach, update-after-remove, remove/add recreation, scene release, retained async version boundary | FIFO mutations produce deterministic final membership without a universal revision. |
| Primitive membership | StaticMesh and TextureCube attach, replace, transform, material update, hide/show, remove | Typed renderer inputs contain each live entry exactly once and never require RTTI discovery. |
| Bounds and visibility facts | Finite local bounds, transformed bounds, invalid candidates, hidden entries | Facts required by M3 are stable without enabling culling or changing output. |
| Light detachment | Direction, transform, color/intensity, visibility, remove/re-add, component destruction | Lit and no-light images are unchanged and all reads come from `FDirectionalLightSceneProxy`. |
| SkyBox alignment | Selection, replace/remove, visibility, texture retention, component destruction | Main, auxiliary, offscreen, fixed-aspect, editor, and Vulkan SkyBox results are unchanged. |
| StaticMesh lifetime | Render-data replacement, material rebinding, proxy replacement/removal, asset retirement, engine shutdown | Existing borrowed and counted resources release through their owning fences with no dangling SceneInfo. |
| Failure and cleanup | Null proxy, incomplete resources, scene release, renderer invalidation/retry | Failed publication leaves no partial map or typed-list membership. |
| Documentation and build | All-plan validator, focused native tests, relevant Vulkan tests, full `all` build | Lasting contracts are documented and M1 is ready for dependent plans. |

## Definition of Done

- Every stage passes its acceptance gate with recorded handoffs and validation
  evidence.
- Every production scene-resident StaticMesh, TextureCube preview,
  directional-light, and SkyBox entry has a SceneProxy/SceneInfo pair.
- `FScene` owns authoritative typed collections and contains no render-facing
  component pointer, repeated whole-scene RTTI discovery, or StaticMesh-only
  mutation branch.
- Strong identities address entries without raw proxy pointers; ordinary
  mutation uses the documented FIFO command order, and every retained revision
  is justified by a named asynchronous or reuse boundary.
- StaticMesh render-data/material lifetimes, SkyBox texture lifetimes,
  multi-view behavior, output images, resource invalidation, and failure
  recovery remain qualified.
- Lasting behavior is moved into Runtime Rendering documentation, the roadmap
  records M1 completion, plan validation passes, and the required build/test
  evidence is recorded.

## Deferred Follow-ups

- M2 material pass classification and execution.
- M3 prepared per-view visibility, LOD, sorting, and counters.
- M4 integration of the selected second production primitive family.
- M5 point/spot proxies, bounded GPU light data, and scalable light selection.
- M6 shadow SceneInfo state, caster preparation, and shadow resources.
- Scene-mutation batching or coalescing if profiling shows command overhead is
  material after the ownership migration.
- Reusable packed handles with generations if non-reused strong identities or
  current collection indices become a measured limitation.
- Public registration only after a named external runtime module supplies a
  concrete lifetime and ordering requirement.

## Related Documentation

- [Rendering Capability Expansion Roadmap](../Roadmaps/RenderingCapabilityExpansion.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [RHI Command Execution](../Runtime/Rendering/RHICommandExecution.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/IScene.h`
- `Engine/Source/Runtime/Engine/Public/Engine/FPrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Private/Engine/PrimitiveSceneProxy.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/PrimitiveComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/DirectionalLightComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/DirectionalLightComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/SkyBoxComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/SkyBoxComponent.cpp`
- `Engine/Source/Runtime/Renderer/Public/Scene.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TextureCubeThumbnailRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkyBoxRenderer.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/StaticMeshUpdateTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/StaticMeshRenderDataLifetimeContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkyBox/SkyBoxComponentTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkyBox/SkyBoxRenderingTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkyBox/SkyBoxVulkanTests.cpp`
