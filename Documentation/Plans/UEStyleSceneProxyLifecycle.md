# UE-Style Scene Proxy Lifecycle Plan

Summary: Replace generic scene-publication metadata with UE-style Component-to-Proxy render-state ownership for Light, SkyBox, and VolumetricCloud.

Last reviewed: 2026-09-01

Status: Completed
Completed: 2026-09-01

## Current Status

All six stages are complete. Light, SkyBox, and VolumetricCloud components now
construct complete immutable proxies on the game thread, transfer ownership
through `FSceneInterface`, retain only a non-owning pointer token, and retire
that exact proxy. Renderer-private SceneInfo owns the attached proxy and each
registry is keyed by its exact pointer.

Validation evidence from 2026-09-01:

- `RendererSceneContractTests`: 40/40 passed.
- `VolumetricCloudSceneContractTests`: 7/7 passed.
- `SkyBoxTests`: 11/11 passed.
- `WorldTests`: 107/107 passed.
- `test affected`: 59/59 native test targets passed, including the mapped
  Renderer, lighting, SkyBox, VolumetricCloud, terrain, skeletal, and Vulkan
  integration coverage.
- Changed-document validation passed for four runtime documents; all-document
  and all-plan validation passed at final handoff.

## Goal

Adopt the useful UE scene-proxy ownership model without copying unrelated UE
implementation history:

- Component state remains game-thread-owned.
- SceneProxy construction copies a complete renderer-facing snapshot.
- After publication, the game thread never dereferences or mutates the proxy.
- `FScene` and its feature registry own the proxy on the rendering thread.
- The originating component retains only the exact proxy pointer needed to
  request retirement.
- Static/authored changes rebuild render state through remove-old/add-new;
  genuinely dynamic paths keep explicit feature-specific update commands.
- SceneInfo remains Renderer-private and owns scene membership, derived state,
  and the proxy-to-scene association.

## Scope

- `FLightSceneProxy`, `FSkyBoxSceneProxy`, and
  `FVolumetricCloudSceneProxy` construction and retirement.
- `FSceneInterface` Add/Remove contracts for those three families.
- `FLightSceneRegistry`, `FSkyBoxSceneRegistry`, and
  `FVolumetricCloudSceneRegistry` identity and ownership.
- Component render-state hooks, property-change rebuilding, scene teardown,
  tests, and runtime documentation.
- Cloud temporal-history invalidation after generic publication revision is
  removed.

The primitive pipeline is out of scope. It already has additional transform,
visibility, material, skeletal-pose, and spline-dynamic update contracts and
should receive a separate plan after this smaller vertical cut proves the
ownership model.

## Selected Decisions

### Ownership and thread boundary

- Proxy constructors execute on the game thread from a complete feature-owned
  Desc assembled from the component.
- Add accepts `std::unique_ptr<TProxy>` so ownership transfer is explicit and
  leak-safe. The component stores `Proxy.get()` before transfer as a non-owning
  lifecycle token.
- The render-command handoff may temporarily use the queue's required owning
  wrapper, but one Renderer-side SceneInfo remains the authoritative owner
  after attachment.
- Remove accepts the exact `TProxy*` token. The component clears its token
  immediately after submitting Remove and never dereferences it after Add.
- Add followed immediately by Remove is valid; FIFO command order must attach
  and retire the same proxy without a game-thread wait.
- Scene teardown retires every attached proxy on the rendering thread before
  registry and SceneInfo destruction completes.

### Identity and selection

- Remove `TSceneProxyPublication`, `TSceneProxyMetadata`, late
  `BindPublication`, scene-wide publication revision allocation, and generic
  registry revision tombstones.
- A proxy is complete and immutable when its constructor returns.
- Light identity used for ordering and diagnostics is copied into the light
  proxy Desc.
- SkyBox and VolumetricCloud retain persistent GUID, selection key, priority,
  and any runtime diagnostic identity directly in their feature proxy Desc.
- Registry membership is keyed by the exact proxy pointer. Persistent identity
  remains a candidate-selection value and is not used as an ownership key.
- Durin keeps multiple deterministic SkyBox and VolumetricCloud candidates;
  UE's unique-cloud product policy is not adopted.

### Updates and invalidation

- Authored property, visibility, and other complete-state changes retire the
  old proxy and publish a newly constructed proxy.
- Light transform changes initially rebuild the proxy. A specialized
  render-transform update is a follow-up only if profiling demonstrates that
  rebuild cost matters.
- No universal proxy revision is introduced. Independently ordered work must
  declare a feature-specific generation at that actual boundary.
- VolumetricCloud temporal history receives a dedicated immutable history key
  or lifetime serial in the cloud proxy Desc. It changes whenever cloud render
  state is rebuilt and does not participate in registry ownership.

### Failure behavior

- Invalid Desc or missing scene rejects publication before the component stores
  a live token.
- Add must define ownership for render-command admission failure: either the
  call retains the incoming `unique_ptr` until successful enqueue or returns a
  failure result. Silent loss or leaked raw ownership is forbidden.
- Remove of an unknown pointer is a safe no-op in non-validation builds and a
  diagnostic contract failure in validation builds.
- No implementation stage may add a game-thread flush to ordinary registration,
  property edits, visibility changes, or unregistration.

## Implementation Stages

### Stage 0: Freeze the render-state lifecycle seam

- [x] Add a small shared naming contract for component render-state hooks:
  create, destroy, dirty/rebuild, and optional dynamic update.
- [x] Decide whether `FSceneInterface::Add*` returns `bool` or an owning result
  on command-admission failure; record the ownership rule in
  `SceneInterface.h` before implementation.
- [x] Define feature Desc types containing every value read during proxy
  construction; constructors must not retain reflected components or actors.
- [x] Add compile-time or contract-test support proving that a published proxy
  cannot be rebound through `TSceneProxyPublication`.

Stage 0 is complete when the public ownership API, enqueue-failure behavior,
and Desc contents are selected with no unresolved raw-pointer ownership case.

### Stage 1: Convert VolumetricCloud as the vertical reference

- [x] Add `FVolumetricCloudSceneProxyDesc` and construct a complete immutable
  proxy before publication.
- [x] Store `FVolumetricCloudSceneProxy* SceneProxy` as a non-owning component
  token and split component create/destroy/dirty render-state paths.
- [x] Change `FSceneInterface` and `FScene` to Add by `unique_ptr` and Remove by
  exact proxy pointer.
- [x] Key `FVolumetricCloudSceneRegistry` membership by proxy pointer and make
  `FVolumetricCloudSceneInfo` the authoritative Renderer-side owner.
- [x] Establish and clear the Proxy-to-SceneInfo association only on the render
  thread.
- [x] Replace generic publication revision in the cloud history key with the
  selected cloud-specific history identity.
- [x] Preserve priority and stable-identity active selection, including
  deferred texture-reference readiness.
- [x] Cover immediate Add/Remove, repeated rebuild, scene teardown, hidden
  owner, invalid inputs, and history invalidation.

Stage 1 is complete when all Cloud contract and Vulkan integration targets pass
without generic publication metadata or component-side proxy dereference.

### Stage 2: Convert SkyBox

- [x] Add `FSkyBoxSceneProxyDesc` with persistent identity, selection key,
  texture reference, rotation, tint, and intensity.
- [x] Move SkyBox component registration, visibility, transform, and property
  changes onto the shared create/destroy/dirty render-state lifecycle.
- [x] Convert Add/Remove and registry membership to the exact proxy pointer.
- [x] Preserve deterministic candidate ordering by persistent GUID, selection
  key, and an explicit proxy/runtime tie-break value.
- [x] Preserve environment override precedence and counted texture-reference
  lifetime.

Stage 2 is complete when component, editor, rendering, and Vulkan SkyBox tests
pass with no Scene ID or revision argument in the publication API.

### Stage 3: Convert Light

- [x] Add a common light proxy Desc boundary plus family-specific directional,
  point, and spot payload construction.
- [x] Store a non-owning `FLightSceneProxy*` token in `DLightComponent` and
  rebuild render state for registration, hidden-owner, transform, and authored
  property changes.
- [x] Convert Add/Remove to exact proxy identity while retaining polymorphic
  family dispatch.
- [x] Key `FLightSceneRegistry` ownership by proxy pointer and preserve
  authoritative directional, point, and spot views.
- [x] Prove that a family change retires the old proxy before attaching the new
  proxy and leaves no stale typed membership.
- [x] Preserve stable light ordering through the identity copied into the
  proxy Desc rather than registry publication metadata.

Stage 3 is complete when Renderer scene, shadow, GBuffer, terrain, skeletal,
and Vulkan lighting coverage passes with explicit pointer-token lifecycle.

### Stage 4: Remove the generic publication layer

- [x] Delete `TSceneProxyPublication`, `TSceneProxyMetadata`,
  `BindPublication`, `NextPublicationRevision`, registry `Accept` helpers, and
  revision tombstone maps.
- [x] Remove transitional overloads and adapters; one Add/Remove contract must
  remain per converted feature.
- [x] Audit render commands for captured reflected objects, early proxy
  deletion, or post-publication game-thread proxy access.
- [x] Keep registries concrete; do not introduce a generic pointer registry
  unless the three completed implementations contain identical ownership code
  with identical failure semantics.
- [x] Update Scene snapshots so diagnostic identity is read from the immutable
  proxy while SceneInfo-derived values remain Renderer-owned.

Stage 4 is complete when repository search finds no generic publication
metadata in Light, SkyBox, or VolumetricCloud and no obsolete revision-based
removal API.

### Stage 5: Qualification and lasting contract handoff

- [x] Update the authoritative scene representation, forward lighting,
  SkyBox/cube-texture, and VolumetricCloud runtime documents.
- [x] Run changed-document and all-plan validation according to the repository
  documentation workflow.
- [x] Run the smallest feature targets during each stage, then one final
  `test affected` handoff validation according to the repository testing guide.
- [x] Verify shutdown and scene destruction with no pending proxy ownership,
  dangling component token, render-thread assertion, or forced game-thread
  flush.
- [x] Record exact validation evidence, complete the plan, and move lasting
  rules out of this plan before archival.

Stage 5 is complete when all acceptance gates below have evidence and the work
tree contains no transitional lifecycle path.

## Acceptance Gates

- [x] Proxy construction is complete before Add; no late binding or generic
  publication metadata remains.
- [x] Game-thread code retains only a non-owning pointer token after Add and
  never dereferences it.
- [x] Every successful Add has exactly one render-thread owner and every Remove
  destroys that owner only after prior commands for the proxy.
- [x] Add/Remove FIFO, immediate unregistration, repeated dirty rebuild, scene
  teardown, and command-admission failure are covered.
- [x] Multi-candidate SkyBox and Cloud selection remains deterministic.
- [x] Directional, point, and spot typed light membership remains atomic across
  rebuild and family change.
- [x] No ordinary render-state operation adds a synchronous rendering flush.
- [x] `test affected`, changed-document validation, and all-plan validation
  pass at final handoff.

## Related Code and Contracts

- [`SceneInterface.h`](../../Engine/Source/Runtime/Engine/Public/SceneInterface.h)
- [`SceneTypes.h`](../../Engine/Source/Runtime/Engine/Public/SceneTypes.h)
- [`Scene.h`](../../Engine/Source/Runtime/Renderer/Public/Scene.h)
- [`Scene.cpp`](../../Engine/Source/Runtime/Renderer/Private/Scene.cpp)
- [`SceneRegistry.h`](../../Engine/Source/Runtime/Renderer/Private/SceneRegistry.h)
- [`VolumetricCloudComponent.cpp`](../../Engine/Source/Runtime/Engine/Private/Components/VolumetricCloudComponent.cpp)
- [`SkyBoxComponent.cpp`](../../Engine/Source/Runtime/Engine/Private/Components/SkyBoxComponent.cpp)
- [`LightComponent.cpp`](../../Engine/Source/Runtime/Engine/Private/Components/LightComponent.cpp)
- [Renderer Scene Representation](../Runtime/Rendering/SceneRepresentation.md)
- [Forward Lighting](../Runtime/Rendering/ForwardLighting.md)
- [Volumetric Cloud Scene Contract](../Runtime/Rendering/VolumetricCloudSceneContract.md)
