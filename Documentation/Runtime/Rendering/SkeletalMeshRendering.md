# Skeletal Mesh Rendering

Summary: Define production GPU skinning, animated bounds, scene preparation, palette transport, passes, counters, and recovery.

Modules: Engine, GeometryBuild, AssetForge, Renderer, RenderCore, RHI, VulkanRHI

Last reviewed: 2026-08-23

Durin renders `DSkeletalMeshComponent` through the ordinary scene and viewport
pipeline. The component evaluates a complete immutable `FSkeletalPosePalette`
on the game thread and publishes a detached `FSkeletalMeshSceneProxy`; the
rendering thread never reads the component, mesh asset, skeleton, clip, or
actor.

GeometryBuild registers `Durin.GeometryBuild.SkeletalMesh@1`; its synchronous
session validates complete DSKM values against Skeleton bone count, material
slot count, target, and request identity. Scene parsing and detached candidate
construction remain in AssetForge, which also owns the atomic asset
graph publication and hard Skeleton relationship. A valid cache hit skips
payload encoding and another store.

## Geometry and Skinning

`FSkeletalMeshRenderData` owns one explicit LOD 0 with positions, packed tangent
basis, four UV channels, color, four exact `uint16` joint indices, four float
weights, uint32 indices, sections, material slots, palette metadata, and
per-palette-entry influence bounds. Its dedicated
`FSkeletalMeshVertexFactory` binds ten attributes from five streams. Geometry,
influence, index, declaration, and stream resources initialize and retire under
the asset render-resource fence; incomplete candidates never replace complete
render data.

The vertex shader performs four-influence linear-blend skinning. Position uses
the weighted affine result. Normal and tangent use the weighted linear result
followed by deterministic normalization, and tangent handedness is preserved.
The pose matrix count must equal the render-data palette count. Invalid,
non-finite, empty, oversized, or incompatible palettes reject the primitive;
there is no identity-palette or CPU-skinning fallback.

## Palette Transport and Bounds

One prepared primitive carries one immutable palette pointer and revision. The
renderer uploads its exact matrix bytes to a frame-local dynamic storage range,
then transitions that exact range from `HostWrite` to
`GraphicsShaderRead` before Scene Color. Admission uses the RHI-published
`MinStorageBufferOffsetAlignment` and `MaxStorageBufferRange`, the asset maximum
of 65,535 matrices, and a 64 MiB per-view palette budget. Allocation never
truncates a palette or exposes a native handle.

Render data precomputes a bind-space bound for the vertices influenced by each
palette entry. Pose evaluation transforms the non-empty influence-bound corners
and publishes their finite union atomically with the matrices and revision.
FIFO scene mutation replaces pose and local bound together, then recomputes the
world bound before later visibility preparation.

## Scene Preparation and Passes

`FScene` maintains authoritative typed StaticMesh and SkeletalMesh membership.
Visibility classifies every primitive once per view. `FPreparedSkeletalMeshView`
then freezes identity, transform, render data, LOD 0, pose/revision, palette
range, material binding, section, pass, caster eligibility, and complete value
sort keys before the render pass.

`FSkeletalMeshRenderer` owns skeletal shader maps, pipelines, retry state,
palette preparation, execution, invalidation, and release. `FSceneRenderer`'s
surface-material service owns the generation-aware material sampler slots and
canonical surface uniform, role fallbacks, fragment programs, and parameter
binding shared with StaticMesh and Terrain. It is a
private feature owner composed by `FSceneRenderer`, not a parallel frame
renderer. Opaque and Masked work remains state-grouped within each geometry
family. One prepared combined list orders all StaticMesh and SkeletalMesh
Translucent draws by distance descending and then by complete stable value ties.
Execution only consumes resource-complete records.

Skeletal sections use the same PBR material snapshots, Opaque/Masked/
Translucent meaning, Lit/Unlit behavior, Solid/Wireframe raster state, depth
policy, straight-alpha blending, texture defaults, post-process, output, and
editor-assistance composition as StaticMesh. After skeletal transform/palette
and geometry binding, the shared mesh surface-pass executor owns the
opaque-shadow fast path, masked role-7 binding, complete forward binding, and
single draw submission. GBuffer consumes the same canonical eight-role packet
through its existing binder. Opaque and Masked sections participate in the
directional-shadow path without moving palette, raster-bias, or draw counters
out of the skeletal owner.

## Diagnostics and Recovery

Per-view counters conserve visible/rejected primitives, sections, triangles,
state transitions, requested/uploaded/reused/rejected palettes, matrix and byte
totals, resource attempts, and draw outcomes. Geometry, palette, shader,
binding, sampler, or PSO failure rejects the smallest complete draw or primitive
and preserves unrelated features.

Shader reload and device invalidation advance renderer resource generations;
the skeletal owner recreates stale shader/pipeline slots and the shared surface
owner recreates stale samplers on the next prepared view. Scene
removal precedes borrowed render-data retirement. Shutdown removes membership,
drains render commands, releases renderer slots, crosses asset fences, and
destroys each RHI resource exactly once.

## Validation

The deterministic skeletal fixture freezes reference, interpolated, key, loop,
and clamp CPU skinning/bound goldens for equivalent glTF and GLB inputs.
Vulkan qualification covers reflected storage binding, aligned allocation,
transitions, multiple influences, nonuniform deformation, all surface modes,
sequential views, debug editor and Shipping game profiles, readback, reload,
failure/retry, and shutdown. The current numeric CPU/GPU tolerance is `1e-5`.

## Related Documentation

- [Renderer Scene Representation](SceneRepresentation.md)
- [Static Mesh Rendering](StaticMeshRendering.md)
- [Material System](MaterialSystem.md)
- [RHI Command Execution](RHICommandExecution.md)
- [Viewport Rendering](ViewportRendering.md)
- [Skeletal Animation Playback](../Animation/SkeletalAnimationPlayback.md)
