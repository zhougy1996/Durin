# Material Render Representation Plan

Summary: Replace fixed material render fields with a validated, versioned representation while preserving the current StaticMesh output.

Last reviewed: 2026-08-04

Status: Active
Completed:

## Current Status

The current baseline is commit `a7e6d2650b2b9ea83610a3bc15f875ba914d588a`.
Material declarations, instance overrides, static properties, immutable
render snapshots, stable render-proxy publication, StaticMesh slot bindings,
and material shader-map and pipeline identities are already implemented.

`FMaterialRenderData` still exposes fixed `BaseColor`, `BaseColorTexture`,
`SpecularStrength`, and `Shininess` fields beside its pipeline identity.
Material resolution addresses the five canonical parameter GUIDs directly,
and `FStaticMeshRenderer` knows the resulting field layout. This is sufficient
for the first vertical slice but makes every additional material input a
coordinated Engine/Renderer structure edit and provides no explicit runtime
compatibility boundary for future layouts.

No implementation stage in this plan has started. The plan intentionally lands
before the PBR surface plan so that PBR inputs extend one established render
representation instead of creating another fixed structure.

## Goal

Establish one Engine-owned, immutable, versioned material render
representation that separates static shader/pipeline identity from dynamic
uniform and resource payloads, is validated before publication, and can evolve
without requiring renderer code to read reflected material objects or perform
GUID/name lookup per draw.

The migrated StaticMesh path must preserve the current Lit/Unlit Blinn-Phong
image, fallback behavior, scene-proxy lifetime, publication ordering, and
shader/pipeline cache selection.

## Scope

- Select and document the runtime layout model and its version authorities.
- Represent renderer-consumed uniform values and texture references without
  fixed public fields for every material input.
- Preserve material static properties in shader-map and pipeline identity.
- Validate layout, payload size, field/resource indices, types, finite numeric
  data, and supported versions before render-thread consumption.
- Migrate base-material and instance resolution, stable render proxies,
  StaticMesh scene-proxy bindings, and `FStaticMeshRenderer` to the new
  representation.
- Define compatibility rules for the transient render-layout version and for
  serialized material parameter-schema evolution.
- Preserve missing/invalid material and missing/not-ready texture fallbacks.
- Add focused unit, render-thread, Vulkan, and end-to-end regression coverage.

## Non-Goals

- Adding metallic/roughness PBR inputs or changing the current shader output.
- Implementing opaque, masked, translucent, culling, depth, shadow, or sorting
  policy.
- Introducing a material graph, shader-source generation, material functions,
  derived shader-map persistence, or asynchronous compilation.
- Adding transient gameplay material instances or update batching beyond the
  existing proxy publication coalescing.
- Generalizing material textures beyond the current `DTexture2D` contract.
- Introducing a public or runtime-polymorphic renderer/pass interface.

## Design Decisions and Invariants

- Runtime Engine owns the material render representation. Renderer owns shader,
  pipeline, descriptor, and draw interpretation for a supported representation
  version.
- Published render data is immutable and contains no `DObject`, reflected
  material, `FName`, or uncounted concrete texture-resource pointer.
- Persistent authoring identity remains the parameter GUID. The render layout
  may compile GUID-addressed declarations into validated compact indices; draw
  code does not perform GUID or name lookup.
- Static shader and pipeline properties remain outside the dynamic value
  payload. Dynamic-only edits must reuse the shader-map and pipeline identity.
- Texture values retain counted `FRHITextureReferenceRef` indirections. The
  renderer resolves null or unavailable resources through its owned defaults.
- Layout/version mismatch, malformed data, or unsupported fields fail to a
  deterministic fallback material and a diagnostic; they never permit an
  unchecked offset, index, type reinterpretation, or partial publication.
- Existing stable material proxies, parent-first lazy resolution, publication
  coalescing, startup replay, component binding revision, and render-thread
  ownership remain unchanged unless a recorded stage decision proves a
  conflict.
- The migration is visually neutral. Any deliberate output change belongs to
  the PBR Material Surface plan.
- Transient render-layout versioning and persistent asset-schema versioning are
  separate authorities. A render-layout version is never treated as an asset
  package migration version.

## Current Foundations and Gaps

### Foundations

- `FMaterialParameterDefinition` provides stable identity, type, default value,
  editor metadata, range, and texture-usage hint.
- `FMaterialLocalRenderLayer` publishes render-safe local parameters and
  optional static properties without retaining reflected objects.
- `FMaterialRenderProxy` resolves base and instance layers into one cached
  immutable render snapshot on the rendering thread.
- `FMaterialShaderMapIdentity` and `FMaterialPipelineIdentity` already contain
  schema and static-property inputs and key demand-created Renderer caches.
- StaticMesh scene proxies retain stable material-proxy bindings per slot.
- `FStaticMeshRenderer` has one concrete owner for material shader maps,
  pipelines, bindings, and draw submission.

### Gaps

- The schema version is attached to shader identity but does not describe or
  validate the dynamic uniform/resource payload layout.
- `FMaterialRenderData` is a fixed semantic structure rather than a versioned
  representation with an explicit validation boundary.
- Renderer binding code reaches named C++ fields, so adding inputs expands the
  cross-module coupling.
- Existing serialized materials have canonical definition validation but no
  selected forward evolution and migration policy for later parameter schemas.
- Focused tests assert current values and identities but do not exercise
  rejected layout versions, malformed compact payloads, or deterministic
  fallback after representation validation failure.

## Implementation Stages

### Stage 0: Freeze the Existing Contract and Select the Layout Model

Dependencies: current baseline and the completed material render-proxy and
Renderer modularization handoffs.

- [ ] Inventory every producer, publisher, consumer, serializer assumption,
  diagnostic, and test that depends on `FMaterialRenderData` or its fixed
  fields.
- [ ] Freeze the current parameter values, uniform packing, texture fallback,
  Lit/Unlit behavior, shader/pipeline identities, and rendered-output baseline.
- [ ] Compare bounded layout alternatives, including typed compact fields and a
  validated packed-uniform/resource table, and select one representation.
- [ ] Define layout identity, version ownership, supported-version behavior,
  limits, alignment, type metadata, resource indexing, and validation failure
  diagnostics.
- [ ] Define the separate persistent material-schema version and its rules for
  missing, renamed, added, removed, and type-changed declarations.
- [ ] Record the selected symbols, owners, migration order, and exact working
  set before implementation.

#### Acceptance Gate

- One representation and versioning model is selected with no unresolved
  ownership, alignment, validation, or compatibility choice required by Stage
  1.
- The frozen baseline can distinguish an accidental visual, identity, binding,
  or invalidation change from the intended structural migration.

### Stage 1: Introduce the Versioned Engine Representation

Dependencies: Stage 0.

- [ ] Add the Engine-owned layout/version and immutable uniform/resource
  payload types selected in Stage 0.
- [ ] Add checked construction and validation for version, counts, offsets,
  alignment, types, compact indices, finite values, and resource references.
- [ ] Keep static shader-map and pipeline identities distinct from dynamic
  uniform/resource data and define how the layout identity participates in
  cache selection.
- [ ] Implement deterministic empty/default/failure representations without
  retaining reflected objects.
- [ ] Add the selected serialized material-schema version and bounded upgrade
  or rejection behavior without conflating it with the runtime render layout.
- [ ] Add focused Engine tests for valid construction, stable layout identity,
  malformed data rejection, unsupported versions, defaults, and serialized
  compatibility cases.

#### Acceptance Gate

- Engine can build and validate the new representation independently of
  Renderer, and invalid input cannot become partially published render data.
- Persistent asset compatibility and transient render-layout compatibility
  have explicit, separately tested behavior.

### Stage 2: Migrate Material Resolution and Proxy Publication

Dependencies: Stage 1.

- [ ] Compile canonical base-material definitions into the selected compact
  layout and resolve instance overrides into layout-compatible values.
- [ ] Preserve orphan override exclusion, parent-cycle protection, static
  property inheritance, and deterministic fallback for missing parents or
  definitions.
- [ ] Publish the new immutable representation through the existing stable
  material proxy without weakening coalescing, startup replay, stale
  publication rejection, or parent resolution caching.
- [ ] Preserve counted texture-reference lifetime and current missing,
  unloaded, not-ready, replaced, and destroyed resource behavior.
- [ ] Update material and StaticMesh snapshot tests for the new representation,
  including long parent chains, repeated rapid updates, multiple bound slots,
  and fallback after validation failure.

#### Acceptance Gate

- Base materials and instances resolve to the same effective current values
  through the versioned representation.
- Proxy publications remain ordered, coalesced, leak-free, and visible to all
  current slot bindings without scene-proxy recreation.

### Stage 3: Migrate StaticMesh Renderer Consumption

Dependencies: Stage 2.

- [ ] Make `FStaticMeshRenderer` accept only validated supported layouts and
  obtain its current uniform values and base-color texture through the selected
  compact binding contract.
- [ ] Remove Renderer dependencies on the legacy fixed
  `FMaterialRenderData` fields and reject unsupported layouts through the
  deterministic fallback path and resource diagnostics.
- [ ] Preserve shader compilation options, shader-map and pipeline cache keys,
  solid/wireframe selection, descriptor bindings, and renderer-owned texture
  fallbacks.
- [ ] Preserve material thumbnail, preview, level viewport, and multi-slot
  rendering consumers through the same representation.
- [ ] Add focused Vulkan coverage for current uniform/texture binding, fallback
  switching, static identity changes, and shader/resource reload.

#### Acceptance Gate

- No Renderer draw path reads reflected material objects, performs parameter
  GUID/name lookup, or depends on a public fixed field per material input.
- Lit/Unlit, solid/wireframe, textured/untextured, multi-slot, thumbnail, and
  preview outputs match the Stage 0 baseline.

### Stage 4: Close Compatibility, Validation, and Documentation

Dependencies: Stage 3.

- [ ] Search production and test code for legacy fixed-field access, duplicate
  layout descriptors, unchecked payload access, and obsolete compatibility
  assumptions; remove or justify every remaining result.
- [ ] Run focused material, StaticMesh, texture, thumbnail, RenderCore, shader,
  renderer-resource reload, and Vulkan rendered-output coverage.
- [ ] Run the required full `all` build and hidden-window `DurinEditor` runtime
  smoke through the repository build entrypoint.
- [ ] Update Runtime material and shader-parameter contracts with the landed
  representation, ownership, versioning, fallback, and thread rules.
- [ ] Update the Material System Roadmap with completion evidence and re-review
  the PBR Material Surface plan against the final symbols and layout.

#### Acceptance Gate

- Required focused and aggregate validation passes, including real Vulkan
  uniform/texture binding and renderer resource recovery.
- Long-lived contracts live in Runtime documentation, this plan contains a
  compact final handoff, and the PBR plan has an evidence-backed starting
  baseline.

## Validation Matrix

| Area | Required coverage | Acceptance |
| --- | --- | --- |
| Layout | versions, identity, limits, alignment, offsets, types, resources | Invalid layouts fail before publication; valid identity is deterministic |
| Asset compatibility | current packages plus selected schema additions/removals/type changes | Explicit upgrade, default, or rejection behavior with diagnostics |
| Resolution | base, instance, parent chain, orphan, cycle, missing parent | Same effective current values and deterministic fallback |
| Publication | coalescing, startup replay, stale order, destruction, multiple slots | No lost latest update, stale overwrite, leak, or scene-proxy recreation |
| Renderer | Lit/Unlit, solid/wireframe, texture/fallback, static identities | Stage 0 output and cache behavior preserved |
| Vulkan | descriptor binding, fallback replacement, reload and device recovery | Rendered output and recovery tests pass |
| End to end | material editor, thumbnails, preview, level viewport | All consumers use the same representation |

## Definition of Done

- One validated, versioned Engine-owned representation replaces fixed public
  material render fields across all production consumers.
- Runtime render-layout and persistent asset-schema compatibility are explicit,
  separate, and tested.
- Material proxies and StaticMesh bindings preserve their established lifetime,
  invalidation, and thread contracts.
- Current rendered output and editor consumers show no unintended behavior
  change.
- Focused tests, full `all` validation, editor smoke, Runtime documentation,
  roadmap update, and the final stage handoff are complete.

## Deferred Follow-ups

- Metallic/roughness PBR inputs and visual output, owned by
  [PBR Material Surface](PBRMaterialSurface.md).
- Opaque, masked, translucent, culling, depth, shadow, and sorting policies.
- Material graph compilation, generated shader source/IR, derived shader maps,
  and asynchronous compilation.
- Dynamic material instances, update batching beyond existing publication
  coalescing, and render-command caching or batching.

## Related Documentation

- [Material System Roadmap](../Roadmaps/MaterialSystem.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Materials/MaterialTypes.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialRenderProxy.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialRenderProxy.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInterface.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/PrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Private/Engine/PrimitiveSceneProxy.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Shaders/Slang/StaticMesh.slang`
- `Engine/Tests/Native/EngineTests/Private/Materials/`
