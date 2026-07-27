# Material System Plan

Summary: Material editing, surface models, shader maps, and runtime materials.

Last reviewed: 2026-07-27

## Current Status

The material system has completed its foundation and a first textured,
forward-lit static-mesh vertical slice. It is suitable for proving asset
serialization, inheritance, component binding, render-thread updates, and
basic shader resource binding, but it is not yet a complete artist-facing
material workflow or a general material compiler.

The current surface model is fixed in `StaticMesh.slang`: vertex color,
`BaseColor`, and `BaseColorTexture` feed a simple directional-light
Blinn-Phong calculation controlled by `SpecularStrength` and `Shininess`.
`Opacity` is carried in the output alpha, but the static-mesh pipeline has
alpha blending disabled, so it does not yet define translucent rendering.
All static meshes share one shader map, one opaque pipeline policy, and one
linear-repeat base-color sampler.

The Content Browser can now create material and material-instance assets and
open them in a dedicated Material Editor. The first editor slice exposes the
current built-in scalar, color, and texture parameters, parent selection,
per-parameter instance overrides, reset-to-parent behavior, and save support.
Material and material-instance cards now have persistent rendered sphere
thumbnails with invalid and failure states. The editor still does not provide
an interactive rendered preview, drag/drop, arbitrary declared parameters, or
complete rename/delete synchronization for open documents.

Static-mesh material assignment now uses the landed mesh-slot architecture:
meshes own persistent reconciled slot GUIDs and optional defaults, components
store sparse GUID overrides and explicit orphans, Details presents fixed rows,
and scene proxies consume compact slot-ordered snapshots. The completed design
and compatibility boundary are preserved in
`Documentation/Plans/Archive/2026-07/StaticMeshMaterialSlots.md`.

## Implemented

- [x] `DMaterialInterface`, `DMaterial`, and inherited `DMaterialInstance`
  assets.
- [x] Named scalar, vector, and `DTexture2D` parameters with local instance
  overrides and explicit override clearing.
- [x] Parent-cycle rejection during editing and asset loading.
- [x] Reflection, serialization, dependency tracking, and garbage-collection
  reachability for material and texture references.
- [x] Immutable `FMaterialRenderData` snapshots; renderer code does not read
  reflected material objects.
- [x] Material-to-instance and material-to-component dependency propagation.
- [x] Render-thread material updates that preserve the existing scene proxy
  and reject stale component revisions.
- [x] Mesh-owned persistent material-slot identities, reimport reconciliation,
  sparse GUID component overrides, mesh defaults, explicit orphans, and
  fixed-row Details editing.
- [x] Base-color texture sampling with renderer-owned fallback textures and a
  shared linear-repeat sampler.
- [x] Basic lit/unlit static-mesh rendering using base color, opacity,
  specular strength, shininess, vertex color, normals, tangents, and UV0.
- [x] Focused tests for inheritance, cycles, texture overrides, serialization,
  garbage collection, multi-slot snapshots, and live proxy updates.

## Stage 1: Complete the Usable Editor Workflow

This is the highest-value next step because the runtime vertical slice exists
but cannot be authored normally.

- [x] Add Content Browser creation actions for `DMaterial` and
  `DMaterialInstance`, including unique naming, initial save, reveal, and
  automatic opening.
- [ ] Keep open Material Editor documents synchronized when their assets are
  renamed, moved, or deleted through the Content Browser.
- [x] Register material and material-instance asset editors instead of routing
  double-clicks to the current "no editor" error.
- [x] Build a dedicated editor for the current built-in scalar, color, and
  texture values without exposing raw reflected `unordered_map` storage.
- [x] Show inherited values, local override state, and reset-to-parent actions
  for material instances.
- [ ] Show the complete resolved parent chain and identify which ancestor
  supplies each inherited value.
- [x] Add searchable asset pickers for texture parameters and instance parents,
  with cycle and incompatible-type diagnostics.
- [ ] Add Content Browser drag/drop for texture parameters and instance parents.
- [ ] Add a lit preview scene, mesh selection, camera controls, and live updates
  without requiring reassignment on a level component.
- [x] Add material and material-instance thumbnails with invalid/compiling
  states.
- [ ] Add editor tests covering create, edit, save/reload, assign to a mesh
  slot, inherited update, and deletion/reference diagnostics.

## Stage 2: Define the Material Domain Model

The parameter identity, declaration, GUID override, and schema-driven editor
slice is complete and preserved in
`Documentation/Plans/Archive/2026-07/MaterialParameterDomainRefactor.md`. Static
properties and the compiled render representation remain in this stage.

- [x] Introduce explicit parameter declarations with stable identifiers,
  display metadata, type, default value, grouping, range, and optional color or
  texture-usage hints.
- [x] Separate declared parameters from resolved instance values so instances
  cannot silently accumulate misspelled or type-incompatible overrides.
- [ ] Define material static properties: blend mode, shading model, two-sided
  state, depth-write policy, and masked-opacity threshold.
- [ ] Split dynamic parameter dirtiness from static shader/pipeline dirtiness;
  static changes must rebuild the correct shader map and pipeline identity.
- [ ] Replace the ad-hoc fixed `FMaterialRenderData` fields with a versioned
  render representation that can grow without coupling every material feature
  directly to `RendererModule.cpp`.
- [ ] Define compatibility/versioning rules for existing serialized materials
  as the parameter schema evolves.

## Stage 3: Establish a PBR Surface Contract

- [ ] Replace the fixed Blinn-Phong surface with an initial metallic/roughness
  PBR contract.
- [ ] Add material inputs for base color, normal, metallic, roughness, ambient
  occlusion, emissive color, and opacity/opacity mask.
- [ ] Support constant and texture-backed values for each input with correct
  white, black, and flat-normal fallbacks.
- [ ] Implement tangent-space normal mapping using the existing packed tangent
  basis and mirrored-transform handedness.
- [ ] Define color-space expectations per input: color/emissive in sRGB and
  normal/data/mask inputs in linear space.
- [ ] Add per-input UV channel selection and a basic UV scale/offset transform.
- [ ] Coordinate texture usage presets, mip generation, compression, and
  residency work with `Documentation/Plans/TextureSupport.md` rather than
  duplicating that pipeline in the material system.
- [ ] Add image-based lighting or another environment-lighting baseline before
  treating the PBR output as production-ready.

## Stage 4: Add Static Permutations and Render Passes

- [ ] Give each material a shader-map identity derived from static properties,
  vertex-factory requirements, pass type, platform, and quality level.
- [ ] Add pipeline-state keys and caching for culling, blend, depth, render-pass
  layout, vertex declaration, and shader permutation.
- [ ] Implement opaque, masked, and translucent policies. Opacity must affect
  actual coverage/blending rather than only the stored scene-color alpha.
- [ ] Make two-sided rendering a material choice; the current static-mesh
  pipeline disables back-face culling for every material.
- [ ] Add depth-only and shadow-depth material passes, including masked
  materials.
- [ ] Add deterministic translucent sorting and document the initial
  limitations before adding more advanced order-independent techniques.
- [ ] Decide whether the renderer remains forward or introduces deferred paths,
  then define the material outputs required by that choice.
- [ ] Add platform and quality-level permutation controls without allowing an
  unbounded shader variant explosion.

## Stage 5: Material Compilation

- [ ] Introduce a material graph asset with typed expression pins and stable
  node identifiers.
- [ ] Implement core expressions: constants, parameters, texture sample, UV,
  arithmetic, interpolation, normal utilities, and material output.
- [ ] Validate types, missing connections, cycles, and unsupported platform
  features with source-located diagnostics.
- [ ] Generate Slang/HLSL source or an intermediate representation without
  exposing renderer-private bindings to graph nodes.
- [ ] Track graph, function, texture, shader include, compiler-version, and
  platform dependencies in derived-data keys.
- [ ] Cache compiled shader maps and restore them without recompiling on every
  editor or runtime load.
- [ ] Add material functions/subgraphs only after the base graph compiler has
  deterministic serialization and diagnostics.
- [ ] Support asynchronous compilation, cancellation, hot reload, and a safe
  fallback material while compilation is pending or failed.

## Stage 6: Runtime Materials and Scalability

- [ ] Add a transient dynamic material instance API for gameplay changes. It
  must not mark content packages dirty like the current asset setters do.
- [ ] Batch or coalesce repeated parameter updates before they cross to the
  render thread.
- [ ] Define lifetime and synchronization rules for texture/resource changes
  while material updates are queued.
- [ ] Add material uniform/resource caching so unchanged values are not packed
  and rebound independently for every section draw.
- [ ] Add material, shader-map, pipeline, uniform, texture, and compile-time
  statistics to renderer/editor diagnostics.
- [ ] Add stress coverage for long instance chains, many bound components,
  multi-slot updates, rapid mutation, destruction with queued commands, and
  large material libraries.

## Validation Gaps

- [ ] Test clamping and defaults for opacity, specular strength, and shininess.
- [x] Test stale component-revision rejection explicitly, including multiple
  rapid updates to different slots; either enforce `MaterialVersion` ordering
  or remove it from the update contract if it remains diagnostic-only.
- [ ] Test missing, unloaded, not-ready, replaced, and destroyed texture
  resources through the real render-thread path.
- [ ] Add rendered-image tests for lit/unlit output, vertex color modulation,
  UV sampling, mirrored transforms, and normal/tangent correctness.
- [ ] Add Vulkan smoke coverage for material descriptor updates and switching
  between fallback and imported textures.
- [ ] Add visual tests for opaque, masked, and translucent behavior when those
  pipeline modes land.
- [ ] Run a full `all` build and `DurinEditor` runtime smoke test after each
  change that crosses Engine, RHI/VulkanRHI, Renderer, shader, and editor
  boundaries.

## Recommended Implementation Order

1. Deliver material/material-instance creation, editing, preview, and save/load
   as an end-to-end editor workflow.
2. Formalize declared parameters and static material properties while keeping
   existing assets loadable.
3. Add the PBR input contract and texture-backed inputs, coordinated with the
   texture build pipeline.
4. Add opaque/masked/translucent permutations, pipeline caching, depth, and
   shadow passes.
5. Build graph compilation and derived-data caching on top of the stable domain
   and shader-map model.
6. Add dynamic runtime instances, update batching, statistics, and advanced
   rendering features after profiling identifies concrete limits.

## Related Documentation

- `Documentation/Runtime/Rendering/MaterialSystem.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Plans/Archive/2026-07/StaticMeshMaterialSlots.md`
- `Documentation/Plans/Archive/2026-07/MaterialParameterDomainRefactor.md`
- `Documentation/Plans/TextureSupport.md`
