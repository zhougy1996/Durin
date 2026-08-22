# Surface Material Pass Execution Plan

Summary: Unify renderer-private surface material resource resolution and forward/shadow pass execution without merging geometry-family renderers.

Last reviewed: 2026-08-23

Status: Completed
Completed: 2026-08-23

## Current Status

Stages 1 through 5 are complete. `FSceneRenderer` owns one Renderer-private
surface-material service with the canonical 256-byte uniform, eight-role
fallback table, complete-state generation-aware sampler slots, short-lived
resolved packets, common forward/opaque-shadow/masked-shadow fragment types,
and typed binders. StaticMesh and SkeletalMesh use one bounded mesh surface-pass
executor after their distinct vertex and geometry setup. Terrain uses the same
fragment programs and packets while retaining its indexed-instancing and
batching path. Opaque shadow resolves no material resources, masked shadow
resolves only role 7 plus its uniform, and forward/GBuffer resolve all roles.

Focused contract, Material/Static/Skeletal/Terrain Vulkan, resource reload,
directional-shadow qualification, Terrain qualification, GBuffer qualification,
Renderer target, full `all` build, and `fast-all` gates pass on 2026-08-23.
The final isolated GBuffer qualification passes 1/1 in 7.37 seconds; its
half-resolution GTAO resolve reports 116,304/117,376 ns median/p95 against the
150,000/250,000 ns gates and reproduces the frozen 116,416/117,600 ns baseline.
The rebase also resolves the prior `EditorAssetWorkflowTests` source-relocation
failure.

The active
[Directional Shadow Caster Preparation Plan](DirectionalShadowCasterPreparation.md)
owns receiver/cascade preparation-fact sharing. This plan does not introduce a
second prepared-draw cache or move cascade-local resource-readiness outcomes,
LOD, bias, ordering, or counters. Both plans touch the same Renderer files, so
their production stages must be serialized in one writer checkout. Any shared
prepared material fact is added by the shadow plan and may call the APIs
established here; this plan must not independently change that ownership.

Production migration, lasting documentation, and all selected acceptance gates
are complete.

## Goal

Establish one surface-material execution contract for production geometry
families so a new texture role, fallback rule, environment input, shadow
binding, or material uniform field is implemented and validated once while
geometry-family specialization remains explicit.

## Scope

- Canonical PBR surface uniform construction and role fallback metadata.
- Short-lived resolved surface-resource packets for forward, masked-shadow,
  and GBuffer consumers.
- One generation-aware Renderer-owned material sampler resource service shared
  by StaticMesh, SkeletalMesh, and Terrain.
- Geometry-independent forward, opaque-shadow, and masked-shadow fragment
  shader types and parameter binding.
- A common mesh surface-pass execution skeleton used by StaticMesh and
  SkeletalMesh after family-specific pipeline, vertex, and stream setup.
- Terrain and GBuffer adoption of the canonical surface data/resource layer
  without changing Terrain batching or GBuffer ownership.
- Symmetric render-thread, render-pass, nullability, resource-completeness, and
  failure-accounting contracts at feature-renderer entry points.
- Focused contract, Vulkan integration, reload, and qualification coverage.

## Non-Goals

- Merging `FStaticMeshRenderer`, `FSkeletalMeshRenderer`, or
  `FTerrainRenderer`, or adding a runtime-polymorphic renderer hierarchy.
- Moving vertex shaders, deformation domains, transform/palette/spline data,
  vertex factories, index buffers, Terrain topology, or draw batching into the
  surface-material layer.
- Sharing receiver/cascade prepared geometry or material facts; that remains
  owned by the Directional Shadow Caster Preparation plan.
- Adding persistent cross-frame resolved material packets or retaining raw
  texture pointers beyond the owning prepared submission.
- Bindless descriptors, descriptor indexing, GPU-driven submission, render
  graphs, shader graphs, asynchronous material compilation, or dynamic-uniform
  reuse.
- Changing the authored material schema, v3 renderer-facing representation,
  shader-visible PBR ABI, pass classification, blend/depth policy, or rendered
  output.
- Generalizing unrelated feature renderers that do not consume the canonical
  eight-role PBR surface contract.

## Design Decisions and Invariants

### Surface ownership

- `FSceneRenderer` owns one `FSurfaceMaterialResources`-equivalent service and
  injects it into the three production surface renderers, alongside the
  existing default-texture and environment-lighting owners.
- The service owns generation-aware sampler creation slots keyed only by the
  complete `FMaterialSamplerState`. Feature renderers retain their own
  resource-attempt/result counters and reject the smallest affected draw or
  batch when resolution fails.
- Device invalidation and renderer shutdown invalidate or release the shared
  sampler service once. Shader maps, graphics pipelines, Terrain topology and
  height resources remain family-owned.
- `FMaterialRenderBinding` retains counted texture-resource references.
  Resolved RHI texture and sampler pointers are command/submission-local and
  are never cached across frames or material publication.

### Canonical surface data

- Replace the mesh-named uniform implementation with one
  `FSurfaceMaterialUniform`-equivalent type while preserving its exact size,
  alignment, field order, defaults, and shader ABI. Add compile-time layout
  assertions where the ABI permits them.
- One ordered fallback table owns the eight roles: White, FlatNormal, White,
  White, White, Black, White, White. Forward and GBuffer resolution consume
  this table rather than restating named assignments.
- `MakeSurfaceMaterialUniform(Binding, bLit)` is the only production builder
  for the surface uniform. Lit selection remains the conjunction of view
  `ERenderMode::Lit` and material `EMaterialShadingModel::Lit`.
- Environment irradiance, prefilter, BRDF LUT, and sampler resolve as an
  all-or-nothing set. An incomplete set uses the existing black/cube fallback
  set. Directional-shadow texture and sampler each retain their current
  deterministic fallback.

### Pass-aware resolution

- Opaque shadow execution resolves no material uniform, role texture, material
  sampler, environment resource, or receiver-shadow resource.
- Masked shadow execution resolves the material uniform plus only the
  OpacityMask texture and sampler at role 7.
- Forward and GBuffer execution resolve all eight material roles. Only forward
  execution adds lighting, environment, and directional-shadow inputs.
- Resource preparation requests the role set required by the selected pass.
  Its success remains recorded by the owning prepared view/cascade; the shared
  service does not own view phases or counters.
- The first implementation adoption must preserve current behavior before the
  masked-only role reduction is enabled and measured. No stage may leave two
  selectable production resolution paths.

### Shader and execution boundary

- StaticMesh, SkeletalMesh, and Terrain use common surface forward,
  opaque-shadow, and masked-shadow fragment shader types compiled in their
  existing shader maps. Vertex shader types and compile options remain
  geometry-family-specific.
- The common mesh executor starts only after the caller has selected a complete
  graphics pipeline and prepared family-specific vertex parameters. It may set
  common depth bias, invoke caller-provided geometry binding, bind the selected
  surface fragment pass, and invoke a bounded draw-submission callable.
- Deformation domain is not an input to surface binding or fragment execution.
  It remains part of shader-map/pipeline identity and vertex setup.
- Terrain may reuse the same fragment programs and binding functions without
  using the mesh executor; indexed instancing, scalar Translucent submission,
  patch batching, and dynamic-allocation counters remain explicit Terrain
  code.
- Public renderer/phase entry points assert render-thread and render-pass state
  symmetrically. Shared helpers validate only their own immediate preconditions
  and do not replace feature-owner phase/nullability checks.

### Failure and ordering

- Resource creation remains outside render passes. Execution consumes only
  resources admitted by the owning preparation phase and returns a recoverable
  failure if a generation changed or a required packet is incomplete.
- The order remains pipeline and vertex binding, then opaque-shadow fast path
  or material allocation/resource binding, then exactly one draw submission.
- A rejected resource or command never produces a partial draw. Existing
  attempted = successful + rejected conservation and Terrain batch-level
  rejection remain unchanged.
- Opaque/Masked/Translucent ordering, combined cross-family Translucent order,
  mirrored winding, culling, depth policy, and shadow raster bias remain
  byte-for-byte/effectively identical.

## Current Foundations and Gaps

| Area | Existing foundation | Selected gap |
| --- | --- | --- |
| Material decoding | `ResolveMaterialBinding` already serves StaticMesh, SkeletalMesh, and Terrain. | Execution converts the same binding into uniforms and RHI resources independently. |
| Uniform construction | `MakeStaticMeshMaterialUniform` is used by Static/Skeletal GBuffer draws. | Both forward paths and Terrain manually reproduce the complete ABI. |
| Texture fallback | `MakeGBufferFragmentParameters` centralizes the eight GBuffer roles. | Forward Static/Skeletal/Terrain each restate all eight roles and defaults. |
| Samplers | Shared key/creation helpers exist. | StaticMesh, SkeletalMesh, and Terrain own three caches and repeat lookup/binding. |
| Fragment programs | StaticMesh and SkeletalMesh already share mesh fragment types. | Terrain declares byte-for-byte equivalent forward and shadow fragment types. |
| Pass execution | `MeshRendererExecution.h` centralizes bucket iteration and counter conservation. | Per-draw opaque-shadow, masked-shadow, forward binding, and submission branching is duplicated. |
| Contracts | Prepared phases and resource-complete execution are explicit. | Static and Skeletal entry-point thread/pass/null checks have drifted. |
| Shadow preparation | The active shadow plan owns shared receiver/cascade facts. | This plan must supply reusable resolution APIs without competing for prepared-fact ownership. |

## Implementation Stages

### Stage 0: Freeze the surface execution boundary

- [x] Inventory StaticMesh, SkeletalMesh, Terrain, GBuffer, shared material,
  sampler, fallback, shader, and execution code.
- [x] Select a geometry-independent surface layer and retain explicit
  geometry-family renderer ownership.
- [x] Define pass-aware resource requirements and short-lived pointer lifetime.
- [x] Define the non-overlapping boundary with Directional Shadow Caster
  Preparation Stage 2 and require serialized source ownership.
- [x] Select focused build/test/qualification gates from the configured test
  registry.

#### Acceptance Gate

- Scope, ownership, ordering, failure, lifetime, coordination, and validation
  decisions are explicit; no unresolved design choice blocks Stage 1.

### Stage 1: Canonicalize surface data and resolution

- [x] Add a Renderer-private surface-material header/source containing the
  canonical uniform type, builder, fallback-role table, texture resolver, and
  short-lived resolved material packet.
- [x] Replace Static/Skeletal GBuffer construction with the canonical packet
  adapter while retaining the existing GBuffer pipeline/binder boundary.
- [x] Replace manual Static/Skeletal forward uniform and texture construction;
  keep current sampler ownership and draw branching temporarily so this stage
  is behavior-preserving and independently reviewable.
- [x] Replace Terrain's duplicate uniform type/builder and texture fallback
  assignments with the canonical surface layer without changing batching.
- [x] Add focused contract tests for exact uniform bytes, Lit/Unlit selection,
  all eight fallback roles, missing referenced textures, and incomplete
  environment fallback.

#### Acceptance Gate

- One production uniform builder and fallback table serve StaticMesh,
  SkeletalMesh, Terrain, and GBuffer; focused contract tests pass; the Renderer
  target builds; no shader-visible ABI, draw count, or output contract changes.

### Stage 2: Establish one sampler resource owner

- [x] Add the generation-aware shared sampler service under the Renderer
  resource ownership boundary and compose it in `FSceneRenderer`.
- [x] Inject the service into StaticMesh, SkeletalMesh, and Terrain renderers;
  migrate ensure/resolve operations while preserving feature-local resource
  accounting and diagnostic attribution.
- [x] Remove all three family-local material sampler caches and release paths;
  retain unrelated base resources and family caches.
- [x] Add exact key/create/reuse/failure conservation and prove that identical
  sampler states create one renderer-generation resource rather than one per
  family.
- [x] Cover shader reload, device invalidation, retry after creation failure,
  multi-view reuse, and ordered shutdown without double release.

#### Acceptance Gate

- One shared owner creates and releases material samplers by complete state and
  generation; all families preserve their failure boundaries and counters;
  reload/invalidation integration coverage passes with no leaked or duplicate
  sampler resources.

### Stage 3: Share fragment programs and mesh pass execution

- [x] Promote mesh-named forward and shadow fragment shader types to canonical
  surface fragment programs and migrate StaticMesh/SkeletalMesh shader-map and
  pipeline payloads without changing vertex permutations.
- [x] Add typed forward and masked-shadow parameter builders/binders over the
  resolved packet; remove named eight-role assignments from production draw
  functions.
- [x] Add the common Static/Skeletal surface-pass executor with explicit
  geometry binding and one bounded draw-submission callback. Preserve the
  opaque-shadow fast path and exact depth-bias ordering.
- [x] Migrate StaticMesh and SkeletalMesh draw paths, remove their duplicated
  fragment execution bodies, and make render-thread/render-pass/null contracts
  symmetric at preparation, execution, and release entry points.
- [x] Enable pass-aware role requests: no material resources for opaque shadow,
  role 7 only for masked shadow, and all roles for forward/GBuffer. Reconcile
  preparation and execution rejection counters.

#### Acceptance Gate

- StaticMesh and SkeletalMesh retain distinct geometry execution but use one
  surface fragment/pass skeleton; opaque/masked/forward paths request exactly
  their selected resources; focused Static/Skeletal and directional-shadow
  Vulkan coverage passes with unchanged ordering, draws, images, and failure
  conservation.

### Stage 4: Migrate Terrain and complete pass parity

- [x] Replace Terrain's duplicate fragment shader types with the canonical
  surface programs while retaining its vertex shader and pipeline ownership.
- [x] Bind Terrain forward, masked-shadow, opaque-shadow, and GBuffer resources
  through the surface packet/binders; retain indexed instancing, scalar
  Translucent submission, batching compatibility, and allocation counters.
- [x] Remove obsolete mesh/terrain material execution declarations and narrow
  `MeshRendererShared.h`/`MeshRendererExecution.h` to their remaining vertex,
  geometry, bucket, and counter responsibilities.
- [x] Add cross-family parity cases for constants, authored/missing textures,
  independent samplers, Lit/Unlit, Opaque/Masked/Translucent, missing
  environment, missing receiver shadow, and material/sampler creation failure.
- [x] Verify Static/Skeletal/Terrain output and resource counters across
  receiver forward, GBuffer/deferred, and directional-shadow paths.

#### Acceptance Gate

- All production PBR geometry families use the canonical fragment programs,
  surface packet, fallback policy, and sampler owner; Terrain batching and
  GBuffer contracts remain unchanged; integration and qualification lanes pass.

### Stage 5: Qualify and publish the lasting contract

- [x] Run the bounded contract, integration, reload, and qualification matrix;
  isolate and resolve every regression before broader validation.
- [x] Run `fast-all` and a full `all` build after the final cross-renderer
  migration, following repository build/test guidance.
- [x] Record structural before/after evidence: production builders, fallback
  tables, sampler owners, fragment program types, material role resolutions,
  and draw submissions by pass/family.
- [x] Update Material System, Static Mesh Rendering, Skeletal Mesh Rendering,
  and Terrain Rendering with the implemented surface ownership, pass-aware
  resolution, invalidation, failure, and geometry-family boundary.
- [x] Complete this plan only after all selected gates pass and no duplicate
  production surface execution path remains.

#### Acceptance Gate

- All required tests and builds pass; output, draw ordering/counts, counters,
  fallback and failure behavior match the established contracts; lasting
  runtime documentation owns the final design; the plan records exact evidence.

## Implementation Evidence

Structural inspection on 2026-08-23 records one
`MakeSurfaceMaterialUniform`, one `SurfaceTextureFallbacks` table, one
`TRendererResourceSlotCache<FMaterialSamplerState, FSamplerRHIRef>` owner, and
exactly three canonical surface fragment types in `SurfaceMaterial.*`.
`MeshRendererShared.h` contains no material uniform, fallback resolver,
sampler creator, or fragment type. StaticMesh, SkeletalMesh, and Terrain contain
no family-local material sampler cache or named eight-role texture/sampler
assignment. StaticMesh and SkeletalMesh each enter
`ExecuteMeshSurfacePass_RenderThread`; Terrain retains its explicit indexed
instanced submission. GBuffer's existing binder remains the only named adapter
from packet arrays to GBuffer shader fields.

Passing local gates:

- `RendererSceneContractTests`: 27/27, including exact surface-uniform bytes,
  Lit/Unlit selection, canonical fallbacks, and pass role masks.
- `TerrainRenderPrimitiveTests`: 12/12.
- `MaterialVulkanTests`, `StaticMeshRenderPreparationVulkanTests`,
  `SkeletalMeshRenderResourcesVulkanTests`, `TerrainRenderVulkanTests`, and
  `RendererResourceReloadVulkanTests`: all selected cases pass.
- `DirectionalShadowBaselineVulkanTests --mode qualification` and
  `TerrainRenderQualificationTests --mode qualification`: pass.
- `GBufferQualificationTests --mode qualification`: 1/1 passes; the final
  isolated run reports half-resolution GTAO resolve 116,304/117,376 ns
  median/p95 against the 150,000/250,000 ns gates.
- `build --target Renderer` and the final full `build` (`all`): pass.
- After rebasing `dev`, `EditorAssetWorkflowTests` and the complete `fast-all`
  selection pass.

## Validation Matrix

| Change | Minimum validation | Final/cross-cutting validation |
| --- | --- | --- |
| Pure uniform/fallback/packet helpers | Focused cases in `RendererSceneContractTests`; Renderer target build | `MaterialVulkanTests` and `StaticMeshRenderPreparationVulkanTests` |
| Shared sampler owner/lifecycle | Focused resource contract tests | `RendererResourceReloadVulkanTests` plus family integration lanes |
| Static/Skeletal fragment execution | `StaticMeshRenderPreparationVulkanTests`; `SkeletalMeshRenderResourcesVulkanTests` | `DirectionalShadowBaselineVulkanTests` |
| Terrain adoption | `TerrainRenderPrimitiveTests` | `TerrainRenderVulkanTests` and `TerrainRenderQualificationTests` |
| GBuffer parity | Focused Renderer contract cases | `GBufferQualificationTests` |
| Final repository gate | Documentation changed-scope and all-plan validation | `fast-all`, full `all` build, and changed runtime-document validation |

Every stage starts with the smallest named target that covers its changed
behavior. Vulkan and qualification lanes are run only when their integration
boundary is touched. Test and build commands follow the repository agent
guides rather than being duplicated here.

## Definition of Done

- One canonical surface uniform type/builder and one fallback-role table serve
  StaticMesh, SkeletalMesh, Terrain, forward, shadow, and GBuffer consumers.
- One `FSceneRenderer`-owned generation-aware service owns all canonical
  material sampler resources; no family-local material sampler cache remains.
- Geometry-independent fragment shaders and binding functions are declared
  once. StaticMesh and SkeletalMesh use one surface-pass execution skeleton;
  Terrain reuses the same surface layer without surrendering batching.
- Opaque shadow resolves zero material resources, masked shadow resolves only
  role 7 plus its uniform, and forward/GBuffer resolve exactly eight roles.
- Renderer classes remain explicit family owners for vertex deformation,
  pipelines, streams, geometry, batching, phases, counters, and diagnostics.
- Static/Skeletal thread, pass, phase, nullability, invalidation, and release
  contracts are symmetric and covered at the narrowest practical boundary.
- Receiver/cascade prepared-fact ownership remains consistent with the
  Directional Shadow Caster Preparation plan; no competing cache or duplicate
  production path exists.
- Required contract, integration, qualification, reload, documentation, and
  build gates pass with recorded evidence and unchanged rendered behavior.

## Deferred Follow-ups

- Material uniform/resource interning and dynamic-update batching await measured
  Runtime Materials and Scalability work.
- Bindless or descriptor-indexed material tables require an RHI capability and
  profiling plan; the surface packet is not a disguised bindless abstraction.
- Persistent cross-frame material packets require explicit representation,
  texture-generation, sampler-generation, and residency identities.
- Public pass/renderer registration remains deferred until another runtime
  module supplies a concrete ownership and lifecycle requirement.
- GPU-driven or indirect submission may consume the canonical surface contract
  later but does not alter this plan's geometry-family ownership.

## Related Documentation

- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Skeletal Mesh Rendering](../Runtime/Rendering/SkeletalMeshRendering.md)
- [Terrain Rendering](../Runtime/Rendering/TerrainRendering.md)
- [Directional Shadows](../Runtime/Rendering/DirectionalShadows.md)
- [Material System Roadmap](../Roadmaps/MaterialSystem.md)
- [Directional Shadow Caster Preparation Plan](DirectionalShadowCasterPreparation.md)
- [Agent Build And Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/MaterialBindingResolution.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/MaterialBindingResolution.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/MeshRendererShared.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/MeshRendererExecution.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Resources/DefaultTextureResources.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/EnvironmentLightingResources.h`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshRenderPreparationVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalMeshRenderResourcesVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/TerrainRenderPrimitiveTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/TerrainRenderVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceReloadVulkanTests.cpp`
