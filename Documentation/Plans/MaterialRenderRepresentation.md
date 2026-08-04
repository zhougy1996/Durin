# Material Render Representation Plan

Summary: Replace fixed material render fields with a validated, versioned representation while preserving the current StaticMesh output.

Last reviewed: 2026-08-04

Status: Active
Completed:

## Current Status

Stage 0 is complete against baseline commit
`919401389d259c6da9c391cc39a7a2e6e4ed080b`, and Stage 1 is now complete against
the Stage 0 handoff commit
`e72bb5fe0be6b1b9bb9135d5cea6e53c491c8ac7` (the baseline is recorded below).
Material declarations, instance overrides, static properties, immutable render
snapshots, stable render-proxy publication, StaticMesh slot bindings, material
shader-map and pipeline identities, and the validated Engine representation
foundation are implemented.

`FMaterialRenderData` still exposes fixed `BaseColor`, `BaseColorTexture`,
`SpecularStrength`, and `Shininess` fields beside its pipeline identity.
Material resolution addresses the five canonical parameter GUIDs directly,
and `FStaticMeshRenderer` knows the resulting field layout. Stage 0 froze that
contract and selected the validated packed uniform/resource table described
below. Stage 1 added and tested the Engine representation and serialized
schema boundary; Stage 2 is the next implementation stage and will compile
resolved material layers into this representation.

The plan intentionally lands before the PBR surface plan so that PBR inputs
extend one established render representation instead of creating another fixed
structure.

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

- [x] Inventory every producer, publisher, consumer, serializer assumption,
  diagnostic, and test that depends on `FMaterialRenderData` or its fixed
  fields.
- [x] Freeze the current parameter values, uniform packing, texture fallback,
  Lit/Unlit behavior, shader/pipeline identities, and rendered-output baseline.
- [x] Compare bounded layout alternatives, including typed compact fields and a
  validated packed-uniform/resource table, and select one representation.
- [x] Define layout identity, version ownership, supported-version behavior,
  limits, alignment, type metadata, resource indexing, and validation failure
  diagnostics.
- [x] Define the separate persistent material-schema version and its rules for
  missing, renamed, added, removed, and type-changed declarations.
- [x] Record the selected symbols, owners, migration order, and exact working
  set before implementation.

#### Selected Representation and Version Authorities

- The selected model is a validated packed uniform/resource table. A
  `FMaterialRenderRepresentation` owns one immutable `FMaterialRenderLayout`
  descriptor, a byte-packed uniform payload, and counted texture-reference
  resources. `FMaterialRenderData` retains the representation beside the
  existing `FMaterialPipelineIdentity`; it no longer grows one public C++ field
  for each material input.
- The layout is Engine-owned and contains compact field descriptors. Each
  descriptor records a stable parameter GUID for Engine-side compilation and
  diagnostics, a render value type, storage class, compact field/resource
  index, byte offset, and byte size. Renderer draw code consumes compact
  indices and typed accessors only; it never searches a GUID or `FName`.
- Uniform storage uses little-endian native float lanes with a 16-byte payload
  alignment. Supported v1 numeric field types are `Scalar` (4 bytes),
  `Vector3` (12 bytes), and `Vector4` (16 bytes). Scalar offsets are 4-byte
  aligned; vector offsets are 16-byte aligned; the total payload size is a
  non-zero multiple of 16 bytes. Padding is zero-filled and validated. The
  current v1 StaticMesh payload is 32 bytes: BaseColor at bytes `[0, 12)`,
  Opacity at `[12, 16)`, SpecularStrength at `[16, 20)`, Shininess at
  `[20, 24)`, and zero padding at `[24, 32)`. The renderer preserves the
  existing `MaterialUniform` ABI when it adds the viewport Lit/Unlit mode in
  its local draw binding.
- Texture storage is a separate compact resource table. v1 supports only
  `DTexture2D`/`FRHITextureReferenceRef` entries; null references are valid
  absent values and are resolved by Renderer-owned defaults. Every published
  entry is counted through `FRHITextureReferenceRef`; no concrete texture
  pointer is retained without ownership.
- `FMaterialRenderLayoutIdentity` is the pair `(Version, Id)`. Version is the
  transient layout validation/interpretation authority and is currently `1`.
  `Id` is an Engine-assigned stable `FGuid` for the exact field order, type,
  packing, resource table, and shader binding contract. A packing or binding
  change receives a new Id; a change to the interpretation rules also
  increments Version. Neither value is a package migration version.
- `FMaterialShaderMapIdentity` carries the supported render-layout identity in
  addition to static blend/shading/mask inputs. `FMaterialPipelineIdentity`
  continues to nest the shader identity and pipeline-only properties. Dynamic
  bytes and resource references are excluded from both identities, so a
  dynamic-only edit reuses shader-map and pipeline cache entries.
- The Engine factory validates layout version/identity, count and size limits,
  contiguous compact indices, offset and alignment rules, non-overlapping
  ranges, supported types, finite float data, zero padding, and resource-index
  bounds before it returns a representation. The selected limits are 256
  fields, 64 resources, and 16 KiB of uniform data; these are hard rejection
  bounds, not renderer hints. The factory returns either a complete immutable
  representation or a deterministic v1 fallback and an Engine-owned
  validation diagnostic. It never publishes a partially filled payload.
- v1 is the only supported render-layout version and current layout identity
  during this plan. Engine may describe a newer version only after its
  validator and Renderer adapter land; Renderer rejects an unknown version or
  identity before reading payload bytes, reports a material-binding diagnostic,
  and uses the same orange/default-texture fallback representation. Missing or
  not-ready texture resources remain valid representation values and continue
  through the existing white fallback path.

#### Frozen Baseline and Inventory

- The default effective values are BaseColor `(0.95, 0.62, 0.22, 1.0)`,
  SpecularStrength `0.35`, Shininess `32.0`, and no texture. Opacity clamps to
  `[0, 1]`, SpecularStrength to `[0, 1]`, Shininess to `[1, 256]`, and vector
  channels to `[0, 1]`. Null, unloaded, replaced, not-ready, and destroyed
  texture resources resolve through the Renderer white texture fallback.
- The StaticMesh shader binding baseline is Transform binding `0`, Lighting
  binding `1`, Material binding `2`, BaseColorTexture binding `3`, and
  BaseColorSampler binding `4`. Lit and Unlit differ only through the existing
  local mode value; solid and wireframe use the same material values and their
  existing pipeline pair. Multi-slot, thumbnail, preview, and level-viewport
  draws consume the same resolved snapshot.
- Material producers are `DMaterialInterface::GetRenderData`,
  `DMaterial::BuildMaterialLocalRenderLayer`, and
  `DMaterialInstance::BuildMaterialLocalRenderLayer`. Publication is owned by
  `DMaterialInterface::SubmitMaterialRenderProxyState`; render-thread
  inheritance, caching, static-property application, fallback construction,
  and counted resource retention are owned by `FMaterialRenderProxy`.
- Scene-proxy binding and empty-slot fallback are owned by
  `FStaticMeshSceneProxy::ResolveMaterialRenderData_RenderThread`. The only
  production field consumer is `FStaticMeshRenderer::DrawProxy_RenderThread`,
  which currently builds `FStaticMeshMaterialUniform` and resolves the
  texture reference before typed shader-parameter submission. Editor previews
  and thumbnails reach this path through the scene proxy.
- `DMaterial::ParameterDefinitions`, `DMaterialInstance::ParameterOverrides`,
  and `FMaterialStaticProperties` are reflected serialized state. The render
  representation is transient and is not a reflected/serialized object. The
  existing field-table loader skips incompatible legacy scalar/vector/texture
  maps with a warning; `DMaterial::PostLoad`, `DMaterialInstance::PostLoad`,
  and the canonical/static validators remain the asset boundary.
- Existing diagnostics are the canonical/static validation `OutError` strings,
  material proxy publication/coalescing/resolution/stale/binding counters,
  and Renderer resource diagnostics for shader/pipeline creation. Stage 1 adds
  a typed Engine representation-failure code and message; Stage 3 maps
  unsupported render bindings to the existing Renderer diagnostic channel.
- The focused test inventory is `MaterialRenderProxyTests`,
  `MaterialRenderingTests`, `MaterialSchemaAndEditingTests`,
  `MaterialInstanceTests`, `StaticMeshMaterialTests`, `StaticMeshUpdateTests`,
  `MaterialAssetThumbnailTests`, `EditorTextureSmokeTests`, and the shared
  `MaterialTestSupport` snapshot helpers. `RenderedAssetThumbnailFixtureTests`,
  `SceneImportVulkanTests`, and `RenderCoreTests/ShaderReflectionTests` freeze
  thumbnail/import/Vulkan shader binding behavior. Their fixed-field assertions
  will be translated to layout/index/value assertions during Stages 1–3.

#### Persistent Material-Schema Compatibility

- Persistent material authoring uses a separate reflected
  `FMaterialParameterSchemaVersion`, currently `1`, on base and instance
  material state. It describes the serialized declaration/override encoding;
  it is never copied into or compared as the transient render-layout version.
- A missing or zero schema version is accepted only as the current legacy
  package shape after the existing canonical/type validation succeeds; it is
  normalized to v1 with a warning before render publication. A known older
  version uses an explicit bounded migration. A version newer than the
  supported version, malformed field data, duplicate/invalid identities, or an
  ambiguous legacy shape rejects the asset and publishes no partial render
  state.
- Renaming a declaration preserves its GUID and therefore preserves values and
  instance overrides; the new name/metadata is adopted during migration.
  Adding a declaration supplies the base default and leaves instances without
  an override. Removing a declaration drops it from a migrated base schema,
  while an instance override with that GUID remains an explicit orphan and is
  ignored until removed. No rule matches by display name or vector position.
- Changing the type of an existing GUID is a hard compatibility error. Values
  are never reinterpreted between scalar, vector, and texture encodings. An
  explicit future migration may replace the value, but that migration must be
  versioned and validated before publication.

#### Stage 0 Acceptance Evidence

- Targeted symbol/file searches found every production `FMaterialRenderData`
  access, the reflected material fields and PostLoad validators, the proxy and
  scene-proxy fallback paths, renderer diagnostics, shader bindings, and the
  focused test groups listed above. No serializer owns the transient render
  structure.
- The selected v1 byte/resource contract and identity split account for the
  current shader ABI, texture fallback, parent/proxy lifetime, static cache
  keys, and dynamic invalidation behavior. Stage 1 therefore has no open
  alignment, ownership, supported-version, or schema-compatibility choice.

#### Stage 0 Handoff

- Baseline: `919401389d259c6da9c391cc39a7a2e6e4ed080b` before the Stage 0
  documentation commit.
- Working set: `MaterialTypes.h/.cpp`, `MaterialRenderProxy.h/.cpp`,
  `MaterialInterface.h/.cpp`, `Material.h/.cpp`, `MaterialInstance.h/.cpp`,
  `PrimitiveSceneProxy.h/.cpp`, `StaticMeshRenderer.h/.cpp`,
  `StaticMesh.slang`, the focused Engine material tests and support helpers,
  RenderCore shader-reflection coverage, and the Runtime material/shader
  contract documents updated by later stages.
- Key symbols: `FMaterialRenderData`, the new
  `FMaterialRenderRepresentation`/`FMaterialRenderLayout` family,
  `FMaterialRenderProxy::Resolve_RenderThread`,
  `DMaterialInterface::GetRenderData`,
  `FStaticMeshSceneProxy::ResolveMaterialRenderData_RenderThread`,
  `FStaticMeshRenderer::DrawProxy_RenderThread`,
  `FMaterialShaderMapIdentity`, and `FMaterialPipelineIdentity`.
- Migration order: Engine layout/value/validation types and schema version;
  base/instance compilation and proxy publication; scene-proxy and StaticMesh
  compact binding; shader/Vulkan and end-to-end regressions; then Runtime
  contract and roadmap evidence.
- Open questions: none blocking Stage 1. PBR may introduce a new layout Id or
  Version after this plan lands, but it must preserve the selected table and
  compatibility rules and re-review this handoff before implementation.
- Validation: Stage 0 inventory and targeted searches completed; no build or
  runtime test was required because this stage changed only the executable
  plan contract.

#### Acceptance Gate

- One representation and versioning model is selected with no unresolved
  ownership, alignment, validation, or compatibility choice required by Stage
  1.
- The frozen baseline can distinguish an accidental visual, identity, binding,
  or invalidation change from the intended structural migration.

### Stage 1: Introduce the Versioned Engine Representation

Dependencies: Stage 0.

- [x] Add the Engine-owned layout/version and immutable uniform/resource
  payload types selected in Stage 0.
- [x] Add checked construction and validation for version, counts, offsets,
  alignment, types, compact indices, finite values, and resource references.
- [x] Keep static shader-map and pipeline identities distinct from dynamic
  uniform/resource data and define how the layout identity participates in
  cache selection.
- [x] Implement deterministic empty/default/failure representations without
  retaining reflected objects.
- [x] Add the selected serialized material-schema version and bounded upgrade
  or rejection behavior without conflating it with the runtime render layout.
- [x] Add focused Engine tests for valid construction, stable layout identity,
  malformed data rejection, unsupported versions, defaults, and serialized
  compatibility cases.

#### Acceptance Gate

- Engine can build and validate the new representation independently of
  Renderer, and invalid input cannot become partially published render data.
- Persistent asset compatibility and transient render-layout compatibility
  have explicit, separately tested behavior.

#### Stage 1 Acceptance Evidence

- `FMaterialRenderLayout`, `FMaterialRenderRepresentation`, the v1 identity,
  compact field descriptors, finite/padding/resource validation, and the
  deterministic fallback are implemented in Engine. `FMaterialRenderData` retains
  the legacy fields only as a migration bridge; no new material input requires
  another public fixed field.
- `FMaterialShaderMapIdentity` now carries `FMaterialRenderLayoutIdentity`;
  dynamic payload bytes/resources are not part of shader or pipeline identity.
  The Renderer diagnostic text includes the layout version and Id.
- Base and instance assets carry the reflected parameter-schema version.
  Missing v0 state upgrades to v1, unsupported future versions reject, and
  instance override type metadata rejects incompatible schema changes without
  reinterpreting values.
- `MaterialRenderRepresentationTests` covers stable v1 layout and defaults,
  complete valid construction, unsupported versions, misalignment, non-finite
  values, non-zero padding, and separate schema-version upgrade/rejection.
  The focused filter passed 4/4 tests; the full `MaterialTests` target passed
  55/55 tests through `DevTool.bat`.

#### Stage 1 Handoff

- Baseline: Stage 0 commit `e72bb5fe0be6b1b9bb9135d5cea6e53c491c8ac7` before
  Engine implementation.
- Working set: `MaterialTypes.h/.cpp`, `Material.h/.cpp`,
  `MaterialInstance.h/.cpp`, `StaticMeshRenderer.cpp`,
  `MaterialRenderRepresentationTests.cpp`, and the EngineTests CMake source
  list. Generated reflection output remains build-owned and untracked.
- Key symbols: `FMaterialRenderLayoutIdentity`, `FMaterialRenderField`,
  `FMaterialRenderRepresentation`, `ValidateMaterialRenderLayout`,
  `UpgradeMaterialParameterSchemaVersion`, `FMaterialShaderMapIdentity`, and
  `FMaterialParameterOverride::Type`.
- Decisions carried forward: v1 is the only supported runtime layout; the
  32-byte current payload and 16-byte alignment are fixed; invalid factory
  input returns a fallback output and diagnostic; asset schema v0 upgrades to
  v1 and future/type-incompatible data rejects before publication.
- Open questions: none blocking Stage 2. Stage 2 must compile base/instance
  GUID layers into compact indices and populate the representation while
  preserving the existing proxy publication/coalescing contract.
- Validation: focused representation tests and the complete `MaterialTests`
  target passed; no full `all` build or runtime smoke was required for this
  Engine-only stage.

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
