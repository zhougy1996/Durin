# Volumetric Cloud Scene Contract Plan

Summary: Add the reflected volumetric-cloud component, stable scene snapshot, and generic Details authoring boundary

Last reviewed: 2026-08-21

Status: Active
Completed:

## Current Status

P2 activated on 2026-08-21 after Volumetric Cloud Spatial Rendering completed
its RTX 3090 qualification. P1 froze the flat world-Z slab, immutable parameter
block, base/detail/optional-weather texture roles, scene-depth binding,
compute/fragment/disabled fallback, composition order, and resource ownership.

Engine already provides reflected actor components, game-thread registration,
render-thread scene proxies, texture-reference indirection, serialization,
duplication, mutation notifications, world replacement, and generic Details
editing. The remaining work is to apply those contracts to one stable active
global cloud without moving GPU-resource or quality-policy ownership out of
Renderer.

## Goal

Provide one reflected volumetric-cloud actor/component whose authored state
serializes and edits normally, publishes one immutable render-thread snapshot,
selects one active global layer deterministically, and feeds the frozen P1
Renderer input without exposing reflected objects or owning Renderer resources.

## Scope

- `DVolumetricCloudComponent` plus a convenience
  `AVolumetricCloudActor` with reflected enablement, selection priority,
  base/detail volume assets, optional weather texture, slab bounds, distance,
  density mapping, wind, coverage, erosion, and extinction properties.
- Immutable Engine scene data/proxy storage with texture-reference indirection
  and stable scene identity.
- Deterministic one-active-layer selection independent of registration order.
- Registration, replacement, removal, property mutation, duplication,
  serialization, level/world replacement, and scene release.
- A Renderer preparation adapter that translates the selected snapshot into the
  already-frozen P1 parameter and texture-binding contract.
- Generic Details editing and focused runtime/editor validation.

## Non-Goals

- GPU targets, shaders, samplers, histories, transitions, route selection, or
  Vulkan handles in Engine objects.
- Authored sample counts, dispatch sizes, target scale, temporal policy, or
  platform quality tiers; P3 owns those implementation policies.
- Production scattering, directional self-shadowing, or receiver cloud shadows;
  P4 owns lighting behavior.
- Purpose-built cloud panels, previews, debug views, presets, import/generation,
  or asset workflow; P5 owns specialized UI.
- Multiple simultaneous layers, local volumes, spherical shells, fog, or a
  general volumetric-media component hierarchy.

## Design Decisions and Invariants

### Authored and render-thread ownership

- The component owns reflected `TObjectPtr` asset references and physical
  authoring intent on the game thread. It never stores RHI targets, pipelines,
  histories, or backend handles.
- Scene publication copies plain values and counted texture references into an
  immutable proxy. Renderer sees no `DObject`, actor, component, or mutable
  container.
- Base and detail must resolve to sampled 3D textures. Weather is optional and
  resolves through the P1 white fallback. Invalid required assets publish no
  eligible active cloud rather than failing a view.

### Stable selection and mutation

- Every registered cloud receives the existing stable scene identity. The
  active layer is the enabled valid candidate with highest authored priority;
  equal priority selects the lowest stable identity. Registration order is not
  observable policy.
- Add, replace, remove, and selection recomputation execute on the render thread.
  Mutation publishes one complete replacement snapshot; failure retains the
  previous committed scene state until removal is explicitly committed.
- Unregister, level/world replacement, garbage collection, and scene shutdown
  remove the matching identity only and cannot remove a newer replacement.

### Renderer handoff and authoring policy

- The adapter populates `FPreparedSceneView` immediately after scene
  preparation using the frozen P1 fields. Scene depth and Renderer-owned
  defaults remain Renderer responsibilities.
- Authored properties exclude primary/light sample counts and route forcing.
  P2 uses the P1 reference policy until P3 introduces named quality tiers.
- Generic reflection-driven Details is the only required UI. Specialized cloud
  visualization remains deferred and must not block the runtime component.

## Current Foundations and Gaps

| Area | Foundation | P2 gap |
| --- | --- | --- |
| Component lifecycle | `DActorComponent` registration, render-state dirtiness, serialization, duplication, and world replacement | Add cloud-specific reflected state and complete replacement semantics |
| Scene storage | Primitive, light, and SkyBox proxy/scene-info patterns | Add a non-primitive global-cloud registry and stable active selection |
| Texture assets | `DVolumeTexture`, `DTexture2D`, and counted texture references | Validate dimensions and publish required/optional cloud roles |
| Renderer input | P1 immutable parameters, texture bindings, fallbacks, and preparation seam | Replace the development seam with selected scene-snapshot translation |
| Editor | Generic Details and asset property editors | Register reflected properties and prove edit/save/reload workflows |

## Implementation Stages

### Stage 0: Freeze the component, snapshot, and selection contract

- [ ] Freeze reflected property names, defaults, clamps, categories, and the
  authored-versus-quality-policy boundary.
- [ ] Freeze immutable scene data, texture-reference roles, validation, and the
  Renderer translation table.
- [ ] Freeze stable identity/priority selection and add/replace/remove failure
  behavior across registration, mutation, and world replacement.
- [ ] Add pure contract tests for defaults, validation, selection ties, stale
  removal, and parameter translation.

#### Acceptance Gate

- Component ownership, snapshot shape, deterministic selection, failure policy,
  and P1 translation are explicit and pass without GPU initialization.

### Stage 1: Publish one stable Engine scene snapshot

- [ ] Add cloud scene data, proxy/scene-info, stable registry storage, active
  selection, and render-thread add/replace/remove operations.
- [ ] Extend Engine scene interfaces without making generic primitive or light
  registries own cloud semantics.
- [ ] Prove duplicate registration, priority/identity ties, replacement,
  stale removal, invalid assets, scene release, and world replacement.

#### Acceptance Gate

- One complete immutable active snapshot is selected deterministically and no
  reflected object or uncounted texture/resource pointer crosses threads.

### Stage 2: Add the reflected component and actor

- [ ] Implement `DVolumetricCloudComponent` reflected properties, validation,
  registration, render-state mutation, serialization, and duplication.
- [ ] Add `AVolumetricCloudActor` with one default cloud component and preserve
  construction-script/component hierarchy contracts.
- [ ] Prove save/load, duplicate, mutate, enable/disable, asset replacement,
  unregister, level removal, garbage collection, and world reopen.

#### Acceptance Gate

- Authored cloud state round-trips and every lifecycle transition publishes or
  removes exactly the intended stable scene identity.

### Stage 3: Replace the development seam and qualify generic authoring

- [ ] Translate the selected scene snapshot into the frozen P1 prepared-view
  input and remove the development-only injection dependency from production
  qualification.
- [ ] Prove real component output through compute/fragment fallback, offscreen,
  Present, resize, reload/retry, invalid assets, and clean shutdown.
- [ ] Prove generic Details editing, undo/redo, save/reload, duplication, and
  world reopen without adding specialized cloud UI.
- [ ] Run focused Engine/Renderer/editor/Vulkan targets, native aggregate, full
  build, and validation-enabled Debug Editor smoke.

#### Acceptance Gate

- A real reflected component drives the P1 renderer through one immutable
  snapshot, generic authoring works, and all lifecycle/output gates pass.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Defaults and validation | Finite bounded physical properties; no authored implementation-quality policy | Pure Engine tests |
| Stable selection | Priority then stable identity; registration order irrelevant | Scene contract tests |
| Required/optional assets | Invalid base/detail disables eligibility; missing weather uses P1 white fallback | Engine/Renderer tests |
| Mutation | Complete replacement, stale removal rejection, no partial snapshot | Scene lifecycle tests |
| Persistence | Save/load, duplicate, undo/redo, world reopen preserve authored intent | Engine/editor tests |
| Thread boundary | Render thread receives values and counted references only | Contract and lifecycle tests |
| Renderer output | Component drives P1 compute/fallback/offscreen/Present routes | Vulkan integration tests |
| Shutdown/recovery | Reload, world replacement, scene release, and shutdown release every snapshot | Runtime validation |

## Definition of Done

- One reflected cloud component/actor serializes, duplicates, edits, registers,
  replaces, and removes correctly.
- One deterministic active scene snapshot reaches Renderer without reflected
  objects, mutable game-thread state, GPU ownership, or backend handles.
- The selected snapshot maps to the frozen P1 input and produces qualified
  output across supported view routes and failures.
- Generic Details authoring and persistence pass; specialized UI remains owned
  by P5.
- Lasting Engine scene/component and Renderer handoff contracts are documented,
  allowing P3 temporal reconstruction to activate.

## Deferred Follow-ups

- P3 owns named quality tiers, low-resolution targets, jitter, reconstruction,
  typed history, and temporal diagnostics.
- P4 owns production lighting and cloud shadows; P5 owns specialized editor UI
  and asset workflow.

## Related Documentation

- [Volumetric Cloud Rendering roadmap](../Roadmaps/VolumetricCloudRendering.md)
- [Volumetric cloud spatial rendering](../Runtime/Rendering/VolumetricCloudSpatialRendering.md)
- [Volume textures](../Runtime/Assets/VolumeTextures.md)
- [Build and run](../Agents/BuildAndRun.md)
- [Testing](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Components/ActorComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/SkyBoxComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/SkyBoxComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
