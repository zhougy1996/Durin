# Material System Roadmap

Summary: Long-term sequencing for material authoring, surface models, render passes, compilation, and runtime scalability.

Last reviewed: 2026-08-08

Status: Active
Completed:

## Current Status

The material foundation and first textured, forward-lit StaticMesh vertical
slice are complete. Material and material-instance assets provide declared
parameters, inheritance, static properties, serialization, dependency
tracking, stable render-proxy publication, mesh-slot binding, texture fallback,
and shader-map and pipeline identities. The Material Editor supports creation,
save, parent selection, built-in parameter editing, instance overrides, and
rendered thumbnails.

The visible StaticMesh surface now uses the validated v3 metallic/roughness PBR
contract across level, preview, and thumbnail rendering. It includes eight
texture roles with explicit color-space, UV transform, and independent sampler
behavior, tangent-space normal
mapping, Cook-Torrance GGX direct lighting, and a shared pre-baked studio IBL
asset. Material identities now drive distinct opaque, masked, and translucent
passes with authored culling, depth policy, mask coverage, and deterministic
translucent ordering. Material graph compilation, transient
runtime instances, and renderer scalability work have not landed.

This roadmap records ordering and activation gates only. Executable decisions,
working sets, stages, and acceptance evidence belong to the linked active
plans. Later milestones receive their own plans only after their dependencies
are complete and the current implementation can supply an evidence-backed
baseline.

## Active Plans

| Roadmap milestone | Execution plan | State |
| --- | --- | --- |
| 2. Versioned renderer-facing material representation | [Material Render Representation](../Plans/Archive/2026-08/MaterialRenderRepresentation.md) | Complete; final handoff recorded |
| 3. Metallic/roughness PBR surface contract | [PBR Material Surface](../Plans/Archive/2026-08/PBRMaterialSurface.md) | Complete; final handoff recorded |
| 4. Static permutations and render passes | [Material Render Pass Policies](../Plans/MaterialRenderPassPolicies.md) | Complete; M2 qualification recorded |

Milestones 2 through 4 are complete. The pass execution contract is recorded in
[Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md); M3
visibility/LOD and M6 shadow-depth work extend it through the
[Rendering Capability Expansion Roadmap](RenderingCapabilityExpansion.md).

## Completed Foundations

- `DMaterialInterface`, `DMaterial`, and inherited `DMaterialInstance` assets.
- Stable declared-parameter GUIDs, scalar/vector/`DTexture2D` values, instance
  overrides, parent-cycle rejection, serialization, dependency tracking, and
  garbage-collection reachability.
- Base-material static properties for blend mode, shading model, two-sided
  state, depth-write policy, and masked-opacity threshold.
- Immutable renderer-facing snapshots and stable material render proxies with
  coalesced publication, lazy parent resolution, startup replay, and stale
  update rejection.
- Versioned, Engine-owned material render layouts with validated compact
  uniform/resource payloads, separate persistent asset-schema compatibility,
  complete deterministic fallbacks, and stable layout identity in shader-map
  cache keys.
- StaticMesh Renderer consumption through the v1 compact binding contract;
  unsupported layouts report ShaderBinding diagnostics before shader-map or
  pipeline selection, while texture fallback and Vulkan resource reload remain
  covered by integration tests.
- Stable positional StaticMesh material slots with separate user/import names,
  reserved removed indices, mesh defaults, positional component overrides,
  dormant cross-mesh entries, and compact slot-ordered scene-proxy bindings.
- Base-color texture sampling, counted RHI texture references, renderer-owned
  white/black/flat-normal fallbacks, and per-role sampler ownership.
- StaticMesh shader-map and pipeline caches keyed by versioned material static
  identity while retaining the existing scene proxy across material changes.
- A dedicated Material Editor and persistent rendered material thumbnails.
- A local StaticMesh vertex factory that owns the declaration, physical vertex
  streams, up to four UV channels, and packed tangent handedness.
- Concrete Renderer-private feature owners, including `FStaticMeshRenderer`,
  under one `FSceneRenderer` orchestration boundary.
- Canonical PBR v3 constants and eight texture roles, per-role UV0-UV3
  transforms and sampler state, usage/color-space validation, and deterministic
  role fallbacks.
- Tangent-space RNM normal composition with mirrored-transform handedness,
  Cook-Torrance GGX direct lighting, and split-sum environment lighting from a
  pre-baked internal Engine asset shared across scene and preview output.

Milestone 2 completion evidence is recorded in the linked plan. The landed
contract is `FMaterialRenderRepresentation` v1 identified by
`MaterialRenderLayoutV1Id`, with Engine-side GUID compilation, exact compact
binding validation, separate persistent asset-schema versioning, deterministic
fallback, stable proxy publication, and no fixed material-value fields in
`FMaterialRenderData`. StaticMesh draws preserve the existing uniform ABI and
texture fallback, and the aggregate native/Vulkan/reload coverage plus full
`all` build and editor smoke passed before this roadmap was marked complete.

Milestone 3 completion evidence is recorded in the linked PBR plan. The landed
surface writes finite scene-linear RGB and carries effective Opacity in alpha.
Milestone 4 adds visible mask coverage, straight-alpha translucency, authored
culling/depth policy, and deterministic per-view ordering. Depth-only/shadow
passes remain M6. The hidden studio IBL is an
Engine-content asset with asset-owned Cook and an independent bake tool, not a
SkyBox or material parameter.

## Remaining Editor Workflow

The following usability work is intentionally not assigned a new plan yet:

- Synchronize open Material Editor documents after asset rename, move, or
  deletion.
- Show the complete resolved parent chain and the ancestor supplying each
  inherited value.
- Add Content Browser drag/drop for texture parameters and instance parents.
- Add an interactive lit preview with mesh selection, camera controls, and
  live updates.
- Add end-to-end editor coverage for create, edit, save/reload, mesh-slot
  assignment, inherited updates, and deletion/reference diagnostics.

This backlog may be selected independently when editor workflow becomes the
priority. It must not redefine the renderer-facing representation or PBR
surface contract owned by milestones 2 and 3.

## Future Milestones

### 4. Static Permutations and Render Passes (Complete)

Milestone 4 is complete through the
[Material Render Pass Policies Plan](../Plans/MaterialRenderPassPolicies.md).
The selected M2 boundary owns actual opaque, masked, and translucent base-pass
behavior; culling and depth-write policy; deterministic translucent sorting;
and the minimum pass, shader, pipeline, and RHI state identity needed to make
those policies visible. M3 later extends the prepared work with visibility and
LOD, while M6 owns depth-only/shadow-depth resources and execution. The lasting
contract is recorded in
[Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md).

### 5. Material Compilation

Plan graph-domain ownership, typed expressions, validation, source or IR
generation, dependency-derived keys, shader-map persistence, diagnostics,
asynchronous compilation, cancellation, and fallback behavior only after the
runtime surface and render-pass contracts are stable. Split this milestone
into more than one plan if graph authoring, compiler/IR, and derived-data
lifecycles cannot retain bounded working sets and independent acceptance gates.

### 6. Runtime Materials and Scalability

Plan transient dynamic material instances, update batching, resource lifetime
rules, uniform/resource reuse, diagnostics, and stress coverage after profiling
the compiled material path. Streaming and broader texture residency remain
owned by the texture roadmap and plans rather than this milestone.

## Sequencing Rules

1. Complete and validate the Material Render Representation plan.
2. Update this roadmap with its completion evidence and the resulting concrete
   render contract.
3. Re-review the prepared PBR plan against that landed contract before starting
   its first implementation stage; the review and implementation are complete.
4. Use the landed PBR output and limitation review above when milestone 4 is
   explicitly selected; do not infer an active render-pass plan from PBR
   completion.
5. Do not create detailed milestone 5 or 6 plans until the preceding milestone
   exposes the decisions and performance evidence they require.

## Cross-Roadmap Dependencies

- Texture usage, color-space selection, mip generation, compression, platform
  data, failure states, and residency are owned by
  [Texture Support](../Plans/TextureSupport.md).
- StaticMesh stream layout, UV availability, tangent handedness, section
  topology, and vertex-factory ownership are defined by
  [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md).
- Feature-renderer ownership, fixed render ordering, and resource recovery are
  defined by [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md).
- Public or runtime-polymorphic renderer/pass registration remains deferred
  until a second module has a concrete feature-registration requirement.
- Opaque, masked, and translucent pass execution, visibility preparation, and
  the second production primitive-family proof are coordinated by the
  [Rendering Capability Expansion Roadmap](RenderingCapabilityExpansion.md).
  This roadmap continues to own material asset schema, static identity, and
  compilation sequencing.

## Related Documentation

- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
