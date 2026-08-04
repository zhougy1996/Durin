# Material System Roadmap

Summary: Long-term sequencing for material authoring, surface models, render passes, compilation, and runtime scalability.

Last reviewed: 2026-08-04

## Current State

The material foundation and first textured, forward-lit StaticMesh vertical
slice are complete. Material and material-instance assets provide declared
parameters, inheritance, static properties, serialization, dependency
tracking, stable render-proxy publication, mesh-slot binding, texture fallback,
and shader-map and pipeline identities. The Material Editor supports creation,
save, parent selection, built-in parameter editing, instance overrides, and
rendered thumbnails.

The visible surface remains the fixed `StaticMesh.slang` Blinn-Phong path, now
fed by the validated, versioned Engine render representation and compact v1
binding contract. Cached material identities still do not make blend, culling,
depth, masking, or shading policies visibly different. The representation
implementation and its final aggregate-validation/documentation handoff are
complete; PBR, material graph compilation, transient runtime instances, and
renderer scalability work have not landed.

This roadmap records ordering and activation gates only. Executable decisions,
working sets, stages, and acceptance evidence belong to the linked active
plans. Later milestones receive their own plans only after their dependencies
are complete and the current implementation can supply an evidence-backed
baseline.

## Active Plans

| Roadmap milestone | Execution plan | State |
| --- | --- | --- |
| 2. Versioned renderer-facing material representation | [Material Render Representation](../Plans/Archive/2026-08/MaterialRenderRepresentation.md) | Complete; final handoff recorded |
| 3. Metallic/roughness PBR surface contract | [PBR Material Surface](../Plans/PBRMaterialSurface.md) | Planned; implementation waits for milestone 2 |

Only the first plan is the current implementation priority. The PBR plan is
checked in now to freeze its scope and dependency boundary, not to authorize
overlapping implementation.

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
- Persistent StaticMesh material-slot identities, mesh defaults, sparse
  component overrides, explicit orphans, and compact slot-ordered scene-proxy
  bindings.
- Base-color texture sampling, counted RHI texture references, renderer-owned
  white/black/flat-normal fallbacks, and shared sampler ownership.
- StaticMesh shader-map and pipeline caches keyed by versioned material static
  identity while retaining the existing scene proxy across material changes.
- A dedicated Material Editor and persistent rendered material thumbnails.
- A local StaticMesh vertex factory that owns the declaration, physical vertex
  streams, up to four UV channels, and packed tangent handedness.
- Concrete Renderer-private feature owners, including `FStaticMeshRenderer`,
  under one `FSceneRenderer` orchestration boundary.

Milestone 2 completion evidence is recorded in the linked plan. The landed
contract is `FMaterialRenderRepresentation` v1 identified by
`MaterialRenderLayoutV1Id`, with Engine-side GUID compilation, exact compact
binding validation, separate persistent asset-schema versioning, deterministic
fallback, stable proxy publication, and no fixed material-value fields in
`FMaterialRenderData`. StaticMesh draws preserve the existing uniform ABI and
texture fallback, and the aggregate native/Vulkan/reload coverage plus full
`all` build and editor smoke passed before this roadmap was marked complete.

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

### 4. Static Permutations and Render Passes

Create a dedicated plan only after the PBR surface contract is complete. It
will own actual opaque, masked, and translucent behavior; culling and
depth-write policy; depth-only and shadow-depth passes; translucent sorting;
pass, platform, quality, and vertex-factory permutation identity; and the
forward-versus-deferred decision. Until then, existing static properties remain
identity inputs without a promise that each policy differs on screen.

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
   its first implementation stage; amend its baseline and any invalidated
   assumptions before code changes. This re-review is now recorded in
   `PBRMaterialSurface.md`; PBR implementation remains unstarted.
4. Complete and validate the PBR plan, then decide the exact scope and number
   of plans required for milestone 4.
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

## Related Documentation

- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
