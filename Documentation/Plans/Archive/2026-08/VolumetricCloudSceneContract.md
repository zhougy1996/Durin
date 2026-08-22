# Volumetric Cloud Scene Contract Plan

Summary: Add the reflected volumetric-cloud component, stable scene snapshot, and generic Details authoring boundary

Last reviewed: 2026-08-21

Status: Archived
Completed: 2026-08-21

## Current Status

P2 activated on 2026-08-21 after Volumetric Cloud Spatial Rendering completed
its RTX 3090 qualification. P1 froze the flat world-Z slab, immutable parameter
block, base/detail/optional-weather texture roles, scene-depth binding,
compute/fragment/disabled fallback, composition order, and resource ownership.

P2 completed on 2026-08-21. The reflected actor and component publish one
revisioned immutable candidate; the scene selects one active cloud
deterministically; Renderer translates it into the P1 handoff; and generic
property transactions plus real Vulkan output pass. The final gate passed the
focused contracts, both Vulkan executors, `fast-all`, the default native
aggregate, the complete Debug Editor build, documentation validation, and a
validation-enabled 120-tick hidden-window Debug Editor smoke.

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

### Reflected authoring surface

`DVolumetricCloudComponent` derives from `DSceneComponent`. Properties use the
generic reflection-driven Details surface; the initial reflection system does
not provide category or numeric-range metadata, so the names and declaration
order below define the P2 presentation groups and setters plus
`PreEditChangeProperty` enforce the stated ranges.

| Group | Reflected property | Type | Default | Accepted authored range / rule |
| --- | --- | --- | --- | --- |
| Activation | `bEnabled` | `bool` | `true` | Disabled candidates remain registered but are ineligible. Owner hidden state is an additional nonserialized disable. |
| Activation | `Priority` | `int32` | `0` | Clamp to `[-1000, 1000]`; greater values win. |
| Density Textures | `BaseDensityTexture` | `TObjectPtr<DVolumeTexture>` | null | Required; must have valid platform data and a counted texture reference. |
| Density Textures | `DetailDensityTexture` | `TObjectPtr<DVolumeTexture>` | null | Required; same validation as base. |
| Density Textures | `WeatherTexture` | `TObjectPtr<DTexture2D>` | null | Optional; when absent Renderer binds the P1 white weather fallback. |
| Layer | `MinimumZ` | `double` | `1500.0` | Finite, clamped to `[-10'000'000, 10'000'000]`; eligibility also requires `MinimumZ < MaximumZ`. |
| Layer | `MaximumZ` | `double` | `3500.0` | Finite, clamped to `[-10'000'000, 10'000'000]`; eligibility also requires `MinimumZ < MaximumZ`. |
| Layer | `MaximumDistance` | `double` | `100000.0` | Finite, clamped to `[1.0, 10'000'000.0]`. |
| Density Mapping | `BaseFrequency` | `FVector3f` | `(0.00008)` | Each component finite and clamped to `[0.00000001, 1.0]`; publication copies the value directly. |
| Density Mapping | `DetailFrequency` | `FVector3f` | `(0.00032)` | Each component finite and clamped to `[0.00000001, 1.0]`; publication copies the value directly. |
| Density Mapping | `WindOffset` | `FVector3f` | `(0.0)` | Each component finite and clamped to `[-1'000'000, 1'000'000]`; P3 may add time-derived motion without changing this spatial offset. |
| Density Mapping | `WeatherFrequency` | `FVector2f` | `(0.00004)` | Each component finite and clamped to `[0.00000001, 1.0]`; publication copies the value directly. |
| Density Mapping | `WeatherOffset` | `FVector2f` | `(0.0)` | Each component finite and clamped to `[-1'000'000, 1'000'000]`; publication copies the value directly. |
| Optical | `Coverage` | `float` | `0.55` | Finite and clamped to `[0, 1]`. |
| Optical | `DetailErosion` | `float` | `0.30` | Finite and clamped to `[0, 1]`. |
| Optical | `Extinction` | `float` | `0.0015` | Finite and clamped to `[0, 1]`. |
| Optical | `LightExtinction` | `float` | `0.0020` | Finite and clamped to `[0, 1]`. |
| Optical | `Ambient` | `float` | `0.12` | Finite and clamped to `[0, 1]`. |

Non-finite edited or setter input is rejected without mutating the component;
finite out-of-range input is clamped. Cross-property invalidity such as an
inverted slab is preserved as authored intent for undo/serialization but makes
the candidate ineligible until corrected. Setters mark the package dirty and
publish one complete replacement only when the effective value changes.

The serialized `VolumetricCloudSceneId` is a persistent `FGuid`. A
nonserialized `VolumetricCloudInstanceId` and `PublicationRevision` distinguish
runtime instances and their queued mutations. Class-default/template objects
allocate none of these runtime identities. Duplication preserves the persistent
GUID but receives a new instance ID, matching existing component graph rules.

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

The immutable `FVolumetricCloudSceneData` value contains only:

- selection metadata: `PersistentId`, `SelectionKey`, `InstanceId`,
  `PublicationRevision`, `Priority`, and effective `bEnabled`;
- `FRHITextureReferenceRef` values for base density, detail density, and
  optional weather;
- the authored slab, distance, frequency, offset, coverage, erosion,
  extinction, light-extinction, and ambient values listed above; and
- a precomputed `bEligible` bit produced by shared GPU-free validation.

`FVolumetricCloudSceneProxy` owns exactly one such value. Scene storage owns the
proxy through `FVolumetricCloudSceneInfo`; public render-thread observation
copies the active value so no container, reflected object, or uncounted texture
pointer escapes. Texture eligibility checks platform data and a non-null
counted reference on the game thread; the eventual referenced RHI texture may
still be temporarily unavailable, in which case P1 disables that view without
discarding the scene snapshot.

### Renderer translation table

| Prepared-view field | Source |
| --- | --- |
| `bVolumetricCloudRequested` | Selected snapshot exists and is eligible. |
| Base/detail/weather bindings | Resolve the selected counted texture references on the render thread; weather may remain null for the Renderer white fallback. |
| `MinimumZ`, `MaximumZ`, `MaximumDistance` | Same-named snapshot values. |
| `BaseFrequency`, `DetailFrequency`, `WindOffset`, `WeatherFrequency`, `WeatherOffset` | Same-named snapshot values. |
| `Coverage`, `DetailErosion`, `Extinction`, `LightExtinction`, `Ambient` | Same-named snapshot values. |
| `LightDirection`, `LightColor` | Existing prepared directional-light state, with P1 defaults when no eligible directional light exists. |
| `PrimarySampleCount`, `LightSampleCount`, `TransmittanceCutoff` | Renderer-owned P1 reference policy; not component or scene fields. |
| `SceneDepth`, `DensitySampler` | Renderer-owned per-view/default resources. |
| `bVolumetricCloudForceFragmentForQualification` | Qualification seam only; never copied from scene or serialized. |

Translation is a pure function for values and references. Resolving references,
scene depth, samplers, route choice, target allocation, fallback resources, and
all GPU ownership remain in Renderer.

### Stable selection and mutation

- Every registered cloud receives the existing stable scene identity. Eligible
  candidates sort by `Priority` descending, then `PersistentId`,
  `SelectionKey`, and `InstanceId` ascending. `SelectionKey` is the component
  object path and deterministically separates duplicated content that shares a
  persistent GUID; the instance ID is only the final same-path runtime
  tie-break. Registration order is not observable policy.
- Add, replace, remove, and selection recomputation execute on the render thread.
  Mutation publishes one complete replacement snapshot; failure retains the
  previous committed scene state until removal is explicitly committed.
- Unregister, level/world replacement, garbage collection, and scene shutdown
  remove the matching identity only and cannot remove a newer replacement.

The game thread increments `PublicationRevision` before each add/replace and
passes that revision to removal. The render-thread registry applies an
add/replace only when its revision is newer than the stored revision and
applies a remove only when its expected revision matches the stored revision;
duplicate or stale commands are no-ops. A successfully queued replacement with
missing required assets, disabled state, hidden owner, or invalid authored
values commits an ineligible entry and recomputes selection so another layer
can become active. Only inability to enqueue or construct the complete proxy is
a publication failure that retains the previous committed entry.

Scene release clears the registry and active pointer on the render thread.
Commands retain their counted references until execution. A component whose
world or render scene changes removes its last committed `(InstanceId,
Revision)` from the old endpoint before publishing to the new one.

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

- [x] Freeze reflected property names, defaults, clamps, categories, and the
  authored-versus-quality-policy boundary.
- [x] Freeze immutable scene data, texture-reference roles, validation, and the
  Renderer translation table.
- [x] Freeze stable identity/priority selection and add/replace/remove failure
  behavior across registration, mutation, and world replacement.
- [x] Add GPU-free `VolumetricCloudSceneContractTests` coverage for class-default
  identity, runtime defaults, finite/clamp validation, invalid slab/assets,
  priority and all identity tie-breaks, registration-order independence,
  duplicate/stale add-replace-remove commands, and exact P1 translation.

#### Acceptance Gate

- Component ownership, snapshot shape, deterministic selection, failure policy,
  and P1 translation are explicit and pass without GPU initialization.

### Stage 1: Publish one stable Engine scene snapshot

- [x] Add `FVolumetricCloudSceneId`, immutable scene data, proxy/scene-info,
  registry storage, cached active selection, and revision-aware render-thread
  add/replace/remove operations to Engine/Renderer scene boundaries.
- [x] Extend `IScene` and `FScene` with cloud-specific mutation and observation;
  keep primitive, light, and SkyBox storage and policy unchanged.
- [x] Extend the GPU-free contract target to prove duplicate registration,
  priority/identity ties, ineligible replacement fallback, stale removal,
  scene release, endpoint replacement, and counted-reference lifetime.

#### Acceptance Gate

- One complete immutable active snapshot is selected deterministically and no
  reflected object or uncounted texture/resource pointer crosses threads.

### Stage 2: Add the reflected component and actor

- [x] Implement `DVolumetricCloudComponent` with the frozen reflected order,
  getters/setters, shared validation, template/runtime identity allocation,
  registration, complete replacement, visibility, and edit notifications.
- [x] Add `AVolumetricCloudActor` (`DisplayName = "Volumetric Cloud Actor"`)
  with one default root `VolumetricCloudComponent` and no extra render state.
- [x] Add Engine/editor lifecycle cases for memory serialization, package
  save/load, duplication, mutate/undo/redo, enable/disable, asset replacement,
  unregister, level removal, garbage collection, and world reopen.

#### Acceptance Gate

- Authored cloud state round-trips and every lifecycle transition publishes or
  removes exactly the intended stable scene identity.

### Stage 3: Replace the development seam and qualify generic authoring

- [x] Add the pure snapshot-to-P1 adapter, resolve texture references during
  prepared-view creation, derive light inputs from the existing prepared light,
  and stop production tests from depending on
  `SetVolumetricCloudPreparationSink`; retain route forcing only as a narrow
  qualification hook.
- [x] Prove real component output through compute/fragment fallback, offscreen,
  Present, resize, reload/retry, invalid assets, and clean shutdown.
- [x] Prove generic Details editing, undo/redo, save/reload, duplication, and
  world reopen without adding specialized cloud UI.
- [x] Run focused Engine/Renderer/editor/Vulkan targets, native aggregate, full
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

### Planned validation selections

- Register `VolumetricCloudSceneContractTests` as a direct, GPU-free feature
  target owned by Engine/Renderer scene contracts; use it for Stages 0-2.
- Extend `VolumetricCloudSceneVulkanTests` so a real serialized component drives
  compute, fragment fallback, offscreen, Present, resize, invalid-asset, and
  reload/retry routes in both command executors.
- Extend the closest editor lifecycle target for reflection discovery,
  property editing, undo/redo, duplication, save/reload, and world reopen; do
  not add a specialized widget target.
- At the final gate run the focused contract, scene Vulkan, cloud Vulkan,
  editor lifecycle, and renderer contract targets, then `fast-all`, the native
  aggregate, a full build, documentation validators, and the validation-enabled
  Debug Editor smoke required by the plan.

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

- [Volumetric Cloud Rendering roadmap](../../../Roadmaps/VolumetricCloudRendering.md)
- [Volumetric cloud scene contract](../../../Runtime/Rendering/VolumetricCloudSceneContract.md)
- [Volumetric cloud spatial rendering](../../../Runtime/Rendering/VolumetricCloudSpatialRendering.md)
- [Volume textures](../../../Runtime/Assets/VolumeTextures.md)
- [Build and run](../../../Agents/BuildAndRun.md)
- [Testing](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Components/ActorComponent.h`
- `Engine/Source/Runtime/Engine/Public/Components/SkyBoxComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/SkyBoxComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Actors/SkyBoxActor.h`
- `Engine/Source/Runtime/Engine/Private/Actors/SkyBoxActor.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/SkyBoxSceneProxy.h`
- `Engine/Source/Runtime/Engine/Public/IScene.h`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Renderer/Public/Scene.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PreparedSceneView.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudSpatialRenderer.h`
- `Engine/Tests/Native/EngineTests/Private/SkyBox/SkyBoxComponentTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudSceneVulkanTests.cpp`
