# PBR Material Surface Plan

Summary: Replace the fixed Blinn-Phong material slice with a validated metallic/roughness PBR surface contract.

Last reviewed: 2026-08-04

Status: Active
Completed:

## Current Status

This plan remains prepared and implementation-blocked until its own Stage 0
starts, but its predecessor is now complete through the Stage 4 handoff of
[Material Render Representation](MaterialRenderRepresentation.md). The next
implementation baseline is the predecessor's completion commit, whose parent
is `81392097`; the old scope-only baseline
`a7e6d2650b2b9ea83610a3bc15f875ba914d588a` is no longer a valid code baseline.
Stage 0 must still revalidate every symbol and assumption before selecting the
PBR layout.

The current StaticMesh surface combines vertex color, `BaseColor`, and
`BaseColorTexture`, then applies a directional-light Blinn-Phong calculation
controlled by `SpecularStrength` and `Shininess`. `Opacity` is carried in output
alpha, but the current pipeline remains opaque. These five canonical values
are now compiled into the immutable v1 `FMaterialRenderRepresentation` and
consumed through `FMaterialRenderV1Binding`; `FMaterialRenderData` no longer
publishes one fixed field per input. StaticMesh render data already provides
normals, tangent handedness, vertex color, and up to four UV channels.
The texture pipeline already distinguishes Color, Normal, and Data/Mask usage,
builds usage-appropriate mips and desktop formats, and provides renderer-owned
white, black, and flat-normal fallbacks.

No PBR implementation stage may begin until Stage 0 records the exact
predecessor completion commit, revalidates `MaterialRenderLayoutV1Id`,
`FMaterialRenderRepresentationBuilder`, `TryGetMaterialRenderV1Binding`, and
the proxy/resource recovery tests, then explicitly chooses a compatible PBR
layout identity.

## Goal

Define and implement an initial metallic/roughness PBR surface for StaticMesh
materials with constant and texture-backed base color, normal, metallic,
roughness, ambient occlusion, emissive, opacity, and opacity-mask inputs;
correct color space and fallback behavior; tangent-space normal mapping;
per-input UV channel
selection and basic UV transform; and a selected environment-lighting baseline.

The result must be authorable through the schema-driven material domain,
render consistently in level viewports, previews, and thumbnails, and remain a
bounded surface-contract step rather than a material compiler or render-pass
rewrite.

## Scope

- Select exact PBR parameter identities, types, ranges, defaults, grouping,
  texture-usage hints, UV selection, and transform semantics.
- Define a linear metallic/roughness workflow and energy-conserving direct-light
  response.
- Add constant and texture-backed base color, tangent-space normal, metallic,
  roughness, ambient occlusion, emissive, opacity, and opacity-mask inputs.
- Use Color/sRGB sampling for base color and emissive and linear data sampling
  for normal, metallic, roughness, ambient occlusion, opacity, and opacity mask.
- Decode tangent-space normals with the existing packed tangent basis and
  mirrored-transform handedness.
- Support UV channels available from the local vertex factory, defaulting to
  UV0, with validated scale and offset per texture-backed input.
- Select and implement an initial environment-lighting baseline sufficient to
  judge roughness, metallic response, normals, and emissive consistently.
- Preserve material inheritance, stable proxy updates, texture readiness and
  fallback, thumbnails, previews, and shader/resource recovery.
- Define and test compatibility behavior for the existing BaseColor,
  BaseColorTexture, Opacity, SpecularStrength, and Shininess schema.

## Non-Goals

- Actual opaque, masked, or translucent pass policy; alpha blending, mask
  discard, culling, depth-write selection, translucent sorting, depth-only, or
  shadow-depth passes.
- Deferred shading, a GBuffer, clustered lighting, multiple light types, shadow
  maps, or a general lighting architecture rewrite.
- A material graph, arbitrary expressions, generated shader source/IR,
  material functions, or asynchronous compilation.
- Advanced lobes such as clear coat, sheen, subsurface, anisotropy,
  transmission, refraction, iridescence, or displacement.
- Per-material sampler assets, virtual textures, texture streaming, or generic
  texture dimensions.
- Dynamic gameplay material instances or performance-driven draw-command,
  descriptor, or uniform caching.

## Design Decisions and Invariants

- The surface model uses a metallic/roughness workflow. Metallic, roughness,
  ambient occlusion, opacity, and opacity mask are scalar linear inputs; base
  color and emissive are linear values authored or sampled with Color/sRGB intent;
  tangent-space normals are linear data.
- Base color, normal, metallic, roughness, ambient occlusion, emissive, opacity,
  and opacity mask receive stable parameter GUIDs. Renames or editor regrouping
  do not change persistent identity.
- Every input has a deterministic constant and texture fallback. Missing,
  unloaded, failed, replaced, or destroyed textures must not change descriptor
  shape or expose an invalid RHI resource.
- Normal decoding uses the established tangent `xyz`, handedness `w`, and local
  transform determinant sign. Missing normal data resolves to a flat normal.
- UV channel indices are validated against the supported vertex-factory range.
  Missing requested channels resolve by the Stage 0 selected fallback; they
  never read outside the vertex payload.
- Dynamic surface values and texture choices do not create shader
  permutations. Only static properties selected by the material shader-map
  contract may affect shader identity.
- Opacity and opacity-mask values are produced by the surface contract, but
  actual coverage, blending, sorting, and depth behavior remain owned by the
  later Material Render Pass plan.
- Renderer consumes only the completed versioned material render
  representation through its selected binding contract. It does not read
  reflected definitions, fixed public material fields, or resolve GUIDs and
  names per draw.
- Level rendering, material preview, and thumbnails use the same surface shader
  and fallback semantics; preview-specific lighting may differ only through an
  explicit view/environment input.
- Existing materials receive selected, deterministic compatibility behavior.
  Silent coincidental remapping by collection order is forbidden.

## Current Foundations and Gaps

### Foundations

- StaticMesh vertices provide position, color, normal, tangent handedness, and
  up to four UV channels through `FLocalVertexFactory`.
- Texture build settings provide Color, Normal, and Data/Mask usage, explicit
  sRGB policy, usage-appropriate mip filters, BC formats, DDC restoration, and
  counted texture-reference publication.
- Renderer owns white, black, and flat-normal defaults and a shared
  linear-repeat sampler.
- Material declarations provide stable GUID, type, default value, presentation
  metadata, range, and texture-usage hint.
- Material instances inherit values and publish changes through stable proxies
  without scene-proxy recreation.
- StaticMesh shader and pipeline identities already distinguish static material
  properties even though most policies remain visually fixed. The v1 layout
  identity is separate from those static properties and must not be mutated in
  place when PBR inputs are added.

### Gaps

- The current shader implements Blinn-Phong rather than a metallic/roughness
  BRDF.
- Only base color has a texture-backed renderer binding; normal and data texture
  usages are not consumed by materials.
- There is no selected environment-lighting contract for assessing metallic
  reflection and roughness.
- UV choice and transform are not material inputs; the shader always uses UV0.
- Existing `SpecularStrength` and `Shininess` parameters remain in v1 with
  their current Blinn-Phong meaning; the PBR plan must explicitly choose a
  migration/default rule rather than silently reinterpret them as metallic or
  roughness.
- Rendered-image coverage does not yet validate PBR constants, texture roles,
  color-space behavior, tangent normals, mirrored transforms, or fallback
  substitution.

## Implementation Stages

### Stage 0: Freeze the PBR and Compatibility Contract

Dependencies: completed Material Render Representation plan and its final
handoff.

- [ ] Replace this plan's provisional baseline with the predecessor completion
  commit and validate the landed representation, layout/version APIs, renderer
  consumption, tests, and documentation with targeted inspection.
- [ ] Select permanent parameter GUIDs, names, types, ranges, defaults, texture
  usage, grouping, UV selection, and transform semantics for every PBR input.
- [ ] Select the direct-light BRDF terms, numeric clamping, epsilon behavior,
  normal convention, ambient-occlusion scope, emissive units, and output color
  assumptions.
- [ ] Select the initial environment-lighting implementation and asset/resource
  ownership, including its fallback when no environment is available.
- [ ] Select compatibility behavior for existing canonical material parameters
  and packages, including whether SpecularStrength/Shininess are migrated,
  retained as legacy schema, or replaced with documented defaults.
- [ ] Select behavior for an unavailable requested UV channel and define the
  maximum supported UV index without expanding the vertex-factory contract.
- [ ] Select whether scalar data textures use fixed channels, per-input channel
  selection, or a documented packed map convention; record fallback channels
  and compatibility behavior.
- [ ] Freeze representative CPU values and rendered reference scenes covering
  dielectric, metallic, roughness extremes, normal direction, emissive, AO,
  vertex color, UV transforms, and texture fallbacks.

#### Acceptance Gate

- No unresolved parameter, BRDF, environment, compatibility, UV, color-space,
  or fallback decision is required by Stage 1.
- Reference values and scenes can detect a sign, channel, gamma, handedness,
  energy, binding, or migration regression.

### Stage 1: Extend the Material Domain and Render Layout

Dependencies: Stage 0.

- [ ] Add the selected stable PBR declarations and editor metadata to the
  canonical material schema.
- [ ] Implement validated constant, texture, UV-channel, scale, and offset
  values through the versioned render representation without reintroducing
  fixed Renderer-facing fields.
- [ ] Apply finite checks, scalar clamping, safe normal defaults, texture-usage
  validation, and deterministic missing-input behavior before publication.
- [ ] Implement the selected compatibility upgrade/default/rejection behavior
  for current serialized materials and instance overrides.
- [ ] Preserve inheritance, orphan handling, dirty classification, proxy
  coalescing, and shader/pipeline identity behavior.
- [ ] Add focused domain, serialization, inheritance, compatibility, and render
  layout tests for every input and override form.

#### Acceptance Gate

- Base materials and instances can author and resolve the complete PBR input
  set through one validated versioned layout.
- Existing packages follow the selected compatibility rule without wrong-GUID,
  wrong-type, or collection-order assignment.

### Stage 2: Implement Direct-Lit Metallic/Roughness Shading

Dependencies: Stage 1.

- [ ] Replace the Blinn-Phong fragment calculation with the selected
  energy-conserving metallic/roughness direct-light BRDF.
- [ ] Implement constant base color, metallic, roughness, ambient occlusion,
  emissive, opacity, and opacity-mask consumption before adding texture
  modulation.
- [ ] Define and preserve Lit/Unlit behavior under the new surface contract,
  including emissive and output alpha.
- [ ] Clamp or stabilize degenerate roughness, view/light vectors, normals, and
  BRDF denominators to finite deterministic output.
- [ ] Add CPU reference tests for deterministic BRDF helpers where practical
  and focused shader/rendered-output tests for selected reference values.

#### Acceptance Gate

- Constant-input reference surfaces match the Stage 0 numeric and rendered
  expectations across dielectric/metallic and rough/smooth extremes.
- The shader produces finite output for every validated material value and
  preserves the selected Lit/Unlit contract.

### Stage 3: Add Texture Roles, UV Transforms, and Normal Mapping

Dependencies: Stage 2 and the required Texture Support capabilities.

- [ ] Bind texture-backed base color, normal, metallic, roughness, ambient
  occlusion, emissive, opacity, and opacity-mask inputs through the versioned resource
  table and stable RHI texture references.
- [ ] Apply Color/sRGB versus Normal/Data/Mask linear sampling expectations and
  verify the selected platform formats through real Vulkan sampling.
- [ ] Add per-input UV channel selection and scale/offset using only supported
  local vertex-factory channels and the selected missing-channel fallback.
- [ ] Reconstruct the tangent basis and apply tangent-space normal maps with
  correct handedness under ordinary and mirrored transforms.
- [ ] Preserve white, black, flat-normal, and selected scalar fallbacks during
  missing, unloaded, not-ready, failed, replaced, and destroyed texture states.
- [ ] Add focused multi-texture descriptor, fallback replacement, UV channel,
  transform, normal direction, color-space, mip, and mirrored-transform tests.

#### Acceptance Gate

- Each texture role affects only its selected PBR input with correct channel,
  gamma, UV, fallback, and descriptor lifetime behavior.
- Tangent-space normal output remains correct under mirrored transforms and
  missing tangent/UV source data.

### Stage 4: Establish Environment Lighting and Shared Preview Output

Dependencies: Stage 3.

- [ ] Implement the Stage 0 environment-lighting baseline with explicit
  Renderer resource ownership, generation invalidation, fallback, and
  render-thread release.
- [ ] Combine direct and environment lighting without double-counting ambient
  energy and apply the selected ambient-occlusion scope.
- [ ] Make level viewports, Material Preview, and material thumbnails consume
  the same PBR surface contract and deterministic environment fallback.
- [ ] Preserve shader/resource reload, device invalidation, stale-ready
  fallback, and multi-view behavior for any new environment resources.
- [ ] Add rendered reference coverage for metallic reflection, roughness,
  normal mapping, emissive, AO, environment absence, preview, thumbnail, and
  level viewport parity.

#### Acceptance Gate

- Metallic and roughness response can be judged under a stable environment in
  every material rendering surface.
- New environment resources obey established Renderer ownership, recovery,
  multi-view, and shutdown contracts.

### Stage 5: Close Validation and Documentation

Dependencies: Stage 4.

- [ ] Search production and test code for legacy Blinn-Phong-only fields,
  wrong texture-usage assumptions, direct reflected-material access, duplicate
  layout definitions, and obsolete compatibility paths.
- [ ] Run focused material, StaticMesh, texture, thumbnail, preview, shader,
  RenderCore, renderer-resource reload, and Vulkan rendered-output coverage.
- [ ] Run the required full `all` build and hidden-window `DurinEditor` runtime
  smoke through the repository build entrypoint.
- [ ] Update Runtime material, texture, shader-parameter, StaticMesh, and
  viewport contracts with the landed PBR, color-space, UV, normal, environment,
  fallback, and ownership rules.
- [ ] Update the Material System Roadmap with completion evidence and create no
  render-pass plan until the exact landed surface outputs and limitations have
  been reviewed.

#### Acceptance Gate

- Required focused and aggregate validation passes on real Vulkan coverage,
  and reference images detect surface, gamma, binding, normal, UV, fallback, or
  environment regressions.
- Long-lived rules live in Runtime documentation and this plan ends with a
  compact baseline, working set, decisions, open questions, and validation
  handoff for the future render-pass plan.

## Validation Matrix

| Area | Required coverage | Acceptance |
| --- | --- | --- |
| Schema | identities, types, defaults, ranges, grouping, compatibility | Stable GUID behavior and deterministic old-package handling |
| Resolution | base/instance values, texture overrides, orphans, parent updates | Correct complete PBR payload without scene-proxy recreation |
| BRDF | dielectric/metallic, roughness extremes, finite edge cases, Lit/Unlit | Matches selected numeric and rendered references |
| Textures | every role, sRGB/linear, channels, mips, readiness and replacement | Only intended input changes; fallback and lifetime are safe |
| Geometry | UV0-UV3 selection, scale/offset, tangent basis, mirrors, missing data | No out-of-range read or handedness regression |
| Environment | present/absent, roughness response, invalidation, multi-view | Stable lighting and established Renderer lifecycle behavior |
| Editor output | material values, instances, thumbnails, previews, level viewport | Same surface contract and deterministic fallbacks |
| End to end | save/reload, shader reload, device retry, full build and smoke | Production path remains recoverable and validated |

## Definition of Done

- The fixed Blinn-Phong slice is replaced by the selected metallic/roughness
  PBR surface across StaticMesh level rendering, preview, and thumbnails.
- Every scoped constant and texture input has stable identity, validated layout,
  correct color-space/UV/fallback behavior, and inheritance coverage.
- Tangent-space normal mapping and mirrored-transform handedness are correct.
- A deterministic environment-lighting baseline makes metallic and roughness
  output assessable and follows Renderer resource lifecycle rules.
- Existing materials follow the selected compatibility contract.
- Focused tests, reference rendering, full `all` validation, editor smoke,
  Runtime documentation, roadmap update, and the final handoff are complete.

## Deferred Follow-ups

- Opaque, masked, translucent, culling, depth-write, depth-only, shadow-depth,
  sorting, platform, and quality permutations.
- Additional light types, shadows, deferred shading, clustered lighting, and
  advanced PBR lobes.
- Material graph authoring, generated shader source/IR, material functions,
  derived shader-map caching, and asynchronous compilation.
- Texture streaming, virtual textures, per-material samplers, and generic
  texture dimensions.
- Dynamic material instances, update batching, uniform/resource reuse, draw
  sorting, and command caching.

## Related Documentation

- [Material System Roadmap](../Roadmaps/MaterialSystem.md)
- [Material Render Representation Plan](MaterialRenderRepresentation.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Texture Support Plan](TextureSupport.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Materials/MaterialTypes.h`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialRenderProxy.h`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialTypes.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialInterface.cpp`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/DefaultTextureResources.cpp`
- `Engine/Shaders/Slang/StaticMesh.slang`
- `Engine/Tests/Native/EngineTests/Private/Materials/`
- `Engine/Tests/Native/EngineTests/Private/Texture/`
