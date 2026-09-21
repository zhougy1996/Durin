# Static Mesh Rendering

Summary: Define static-mesh render data, scene proxies, materials, draw preparation, and pass participation.

Modules: Engine, Renderer, RenderCore

Last reviewed: 2026-09-18

SplineMesh is a distinct primitive/deformation domain that borrows these
StaticMesh LOD resources and uses the same material/pass/LOD/lighting policy.

This document defines the implemented static-mesh render-resource ownership,
per-LOD lifecycle, vertex-factory boundary, and shader input contract.

Engine keeps one `DStaticMesh` contract while separating its implementation by
responsibility: `StaticMesh.cpp` owns lifecycle and publication,
`StaticMeshCook.cpp` owns authored/Cooked transitions,
`StaticMeshCollision.cpp` and `StaticMeshCollisionQuery.cpp` own collision
state and CPU query acceleration, and `StaticMeshRenderResources.cpp` owns
vertex/index/RHI resource mechanics. These are implementation partitions, not
additional owners or forwarding layers.

Cook serializes the same schema-5 render value used by DDC into the lazy
`RenderData` BulkData field and BodySetup collision into `CollisionData`.
Cooked metadata load attaches both fields without range reads. Render and
physics publication independently lock, decode, validate, and transactionally
publish only the required field; cooked runtime has no authored or DDC fallback.

## Resource Ownership

`FStaticMeshRenderData` owns parallel per-LOD arrays. `LODResources` owns
geometry and CPU metadata; `LODVertexFactories` owns the vertex-fetch policy
that reads those buffers:

```text
FStaticMeshRenderData
  LODResources[LODIndex]       -> FStaticMeshLODResources
  LODVertexFactories[LODIndex] -> FStaticMeshVertexFactories
```

`FStaticMeshLODResources` owns `FStaticMeshVertexBuffers`,
`FRawStaticIndexBuffer`, `Sections`, `LocalBounds`, `ScreenSize`, `NumTexCoords`, and
`bHasColorVertexData`. It exposes no raw RHI references and no writable legacy
vector fields.

Each renderable LOD set owns one validated transition policy. `ScreenSize` is
finite and normalized to `[0, 1]`; values are strictly descending from LOD 0,
and the final LOD is exactly zero. The first threshold satisfying
`projectedSize >= ScreenSize` wins, so equality selects the higher-detail LOD.
Builders without authored values generate `2^-(LODIndex + 1)` and force the
final value to zero; a single-LOD mesh therefore uses `[0]`. Invalid policies
are rejected before render-data publication rather than clamped per view.
Engine validation returns `FStaticMeshLODPolicyResult`, owning the failed LOD
index/count and current/previous thresholds, including NaN or signed zero. It has
no diagnostic-output overload. Candidate publication returns
`FStaticMeshPublicationResult`, retaining LOD-policy or collision-build causes and
distinguishing missing render data from resource initialization failure. Failed
preparation leaves the live product unchanged. Authored application retains
PublicationCause; pending cooked-load contracts format explicitly.

The UE-named buffer resources have these responsibilities:

| Type | Responsibility |
| --- | --- |
| `FPositionVertexBuffer` | Position CPU storage and vertex-buffer RHI allocation. |
| `FStaticMeshVertexBuffer` | Semantic aggregate of tangent-basis and texture-coordinate storage. |
| `FStaticMeshVertexBuffer::FTangentsVertexBuffer` | Independently bindable packed tangent-basis stream. |
| `FStaticMeshVertexBuffer::FTexcoordVertexBuffer` | Independently bindable texture-coordinate stream. |
| `FColorVertexBuffer` | Vertex-color CPU storage and RHI allocation. |
| `FStaticMeshVertexBuffers` | Aggregate of position, static-mesh attribute, and color buffers. |
| `FRawStaticIndexBuffer` | uint32 index storage and index-buffer RHI allocation. |
| `FVertexFactory` | RenderCore base for declaration lifetime and draw-facing streams. |
| `FLocalVertexFactory` | Local-space vertex-fetch policy, declaration, and shader module identity. |
| `FStaticMeshVertexFactories` | Per-LOD container of static-mesh vertex factories. |

## Per-LOD Lifecycle

Initialization is render-thread-only and follows a fixed order:

1. Validate every LOD's geometry, index ranges, sections, and material-slot
   references.
2. Initialize position, tangent, texcoord, color, then index buffers for every
   LOD.
3. Initialize each LOD's `FLocalVertexFactory` after its buffers are ready.

Release runs in reverse order: all vertex factories first, then each LOD's
index buffer and vertex buffers. Initialization is idempotent, and any partial
failure releases all resources already initialized so a later retry starts
clean.

## Editor Ray-Query Acceleration

In editor builds each published `FStaticMeshLODResources` may carry one
immutable CPU ray-query acceleration allocation built from the same validated
positions and indices. It shares the LOD generation's lifetime, is neither
serialized nor stored in derived data, and is omitted from non-editor builds.
Invalid or over-budget geometry leaves acceleration absent rather than
publishing a partial hierarchy. Level Editor traversal, fallback, budgets, and
semantic picking behavior are owned by
[Viewport Editing Architecture](../../Editor/Architecture/ViewportEditing.md).

`IsReadyForRendering()` requires the selected LOD's vertex buffers, index
buffer, and vertex factory to be ready and the same geometry validation to
pass. A populated RHI reference cannot make a malformed LOD renderable.

High-level replacement, proxy borrows, and destruction fences follow the
[asset lifecycle](#asset-lifecycle) below. The linked lifecycle plan preserves
historical implementation evidence.

## Asset Lifecycle

`DStaticMesh` uniquely owns its current `FStaticMeshRenderData`. Detached
builders own unpublished candidates, and synchronous replacement temporarily
owns the displaced render data in a local `std::unique_ptr`. No scene proxy,
vertex factory, render command, or material/thumbnail consumer owns concrete
render data.

Direct `ReplaceRenderData`/`ReplaceSourceRenderData` return typed CPU replacement
results and retain `FStaticMeshReplacementError` on the object. They invalidate
old derived data first; failed replacement preserves the existing no-rollback
contract. Errors own rejected source/slot/UV facts and payload/publication causes.
Collision errors are stored separately as `FStaticMeshCollisionError`, retaining
mode/policy and derived-data causes; collision failure can leave CPU data usable.
`ApplyStaticMeshBuildResult` returns typed render/collision causes and preserves
its existing dirtying behavior. Status checks never depend on diagnostic text.
`RenameMaterialSlot` returns `FStaticMeshSlotRenameResult`, owning the rejected
name, index/count, conflicting slot index and object key. Failure changes no slot
or dirty state; same-name success is a no-op. Successful changes still update the
live render slot, notify compilation and dirty the package.

Blocking cooked CPU loading returns `FCookedMeshBlockingResult` with typed
`FCookedMeshLoadError`, independent of CPU/GPU lifecycle status. Synchronous
failures retain the object key and complete resource-read, product-decoding or
candidate-publication cause. Unavailable residency has its own reason. Causes
remain valid after retry and object retirement; preview/thumbnail adapters format
them at presentation. Async workers and publisher callbacks return typed errors;
terminal callbacks retain field indices, resource/product/publication causes,
task state and completion-mailbox byte limits. A current object keeps the terminal
error for blocking observers. Cancellation can preserve an underlying failure;
terminal lifecycle status remains separate. Cancelled and stale results do not
publish, and accepted retries clear the previous observation. Manager `Submit`
returns `FCookedMeshAdmissionResult`, preserving requested and conflicting accepted
identities, field/callback facts and flight/pending byte/count limits. Rejected
replacement admission leaves accepted work intact and starts no resource read;
the StaticMesh adapter retains AdmissionCause and preserves synchronous fallback.

Replacement follows one ordered protocol:

1. Build and CPU-validate a detached candidate.
2. Initialize the candidate and wait on its targeted fence before publication.
3. Remove component render state through
   `FStaticMeshRenderStateRecreateContext`.
4. Publish the candidate and release the local old render data in reverse
   child-resource order.
5. Wait on the targeted release fence before destroying old C++ storage.
6. Recreate component render state against the new current data.

Scene proxies retain only const, non-owning render-data borrows between
component render-state creation and removal. The renderer does not lazily
initialize StaticMesh resources during scene preparation; components request
asset initialization before proxy creation.

Simple runtime collision is separate from this render lifecycle.
`DStaticMesh::BodySetup` owns reusable authored geometry, while each colliding
component owns its own `FBodyInstance`; render visibility, proxy readiness, LOD
selection, and editor picking never enable collision or become its geometry
authority. See [Runtime Collision](../Physics/Collision.md).

`BeginDestroy()` queues release for initialized resources and starts the
asset's one destruction fence. `IsReadyForFinishDestroy()` remains false until
that fence and the normal `DObject` lifecycle are complete; `FinishDestroy()`
then destroys the aggregate. Engine termination drains ordinary DObject and
render-command ownership while the asset keeps this same release contract—no
StaticMesh-specific shutdown registry or global render flush is required.

## Editor Thumbnail Contract

StaticMesh exposes nonblocking render-resource status, a nonzero resource
revision, and validated LOD 0 local bounds to the editor thumbnail provider.
Provider ownership, framing, material resolution, revision revalidation,
persistence, failure, and shutdown behavior are owned by
[Asset Thumbnails](../../Editor/Architecture/AssetThumbnails.md).

## Vertex Streams and Declaration

`FLocalVertexFactory::FDataType` describes the four physical streams. The
declaration and stream indices are private to the factory; renderer call sites
do not reconstruct them.

| Stream | Data | Format | Stride | Attribute locations |
| --- | --- | --- | --- | --- |
| 0 | Position | `Float3` | 12 | 0 |
| 1 | Packed normal and tangent basis | `Short4N` x2 | 16 | 1, 2 |
| 2 | Four UV channels | `Float2` x4 | 32 | 3-6 |
| 3 | Vertex color | `UByte4N` | 4 | 7 |

Import generates missing normals and tangents deterministically. Each imported
node-mesh instance becomes a section with contiguous indices and a stable source
material slot; import does not automatically create source material assets.

Missing UV channels are zero-filled and missing colors are linear white,
materialized CPU-side before upload. The tangent stream stores normal and
tangent `xyz` plus handedness in `w`; the bitangent is reconstructed in the
shader as `cross(N, T) * sign`.

`FLocalVertexFactory::SetData()` validates matching vertex counts and stores
buffer references. `InitRHI()` builds the declaration from the data and binds
all four streams. `BindStreams()` binds the complete vertex-factory set,
`BindPositionStream()` binds stream 0 alone for position-only passes, and
`GetDeclaration()` supplies the PSO declaration.

The static-mesh renderer selects `FRawStaticIndexBuffer` independently with
`BindIndexBuffer` and uses the factory declaration and streams for every
static-mesh draw. It contains no static-mesh vertex declaration construction
and no hard-coded static-mesh stream selection.

## Shader Module Boundary

`StaticMeshBasePass.slang` imports `VertexFactory.LocalVertexFactory` and
`Lighting.PBRLighting`. The vertex factory module owns:

- `FLocalVertexFactoryInput`, the pass-facing vertex input structure;
- `FLocalVertexFactoryIntermediates`, pass-neutral decoded vertex data;
- `GetLocalVertexFactoryIntermediates()`, the decode entry function.

Decode helpers are module-internal. `StaticMeshBasePass.slang` retains geometry
transform, vertex entry points, and the material-resource-free opaque-shadow
fragment. A material's accepted generated module owns forward, GBuffer, and
masked-shadow fragment entry points and imports the same surface, specular-AA,
directional-shadow, forward-lighting, and PBR helpers. `Lighting.PBRLighting` owns the pass-independent
Cook-Torrance GGX direct-light and split-sum environment-light evaluations.
The direct GGX distribution relies on the canonical minimum perceptual
roughness of `0.045` for finite evaluation and uses
`(1 - NoH^2) + NoH^2 * alpha^2` for its denominator term. It does not apply a
separate denominator floor that would reshape low-roughness highlights;
the shared surface material module instead derives a bounded normal variance
from screen derivatives of the final world-space shading normal and folds it
into effective perceptual roughness. Opaque and masked StaticMesh and SplineMesh
records publish that value through the existing
GBuffer roughness channel; retained Lit forward surfaces use the same value for
directional, local, and environment lighting. Unlit and shadow/depth-only
paths do not evaluate the filter. Authored roughness and material identities
remain unchanged, and the default-enabled per-view switch is a development A/B
seam rather than an editor or material setting.
Vulkan vertex fetch expands the normalized integer tangent and color streams to
floats before the vertex-factory module receives them.

`FLocalVertexFactory::GetShaderModuleName()` returns the stable import name
`VertexFactory.LocalVertexFactory`. The shader compiler links imported module
dependencies before code generation and fingerprints imported modules so a
change invalidates every dependent shader artifact.

### SplineMesh vertex deformation domain

Prepared mesh identity includes factory type and binding-layout keys in
shader-map, effective graphics-pipeline and draw-sort keys. Pipeline identity
also includes vertex declaration and topology. Local/Spline labels remain in
diagnostics and ordering; registered factory implementations select and bind
the vertex programs. Material identity alone cannot alias those programs. `FSplineMeshSceneProxy` supplies the
same selected LOD buffers, indices, declaration, sections, material proxies,
and world transform as StaticMesh plus one immutable deformation uniform.
There is no per-component position, tangent, UV, color, or index-buffer copy.

`VertexFactory.SplineMeshVertexFactory` translates the normalized CPU Hermite,
attribute interpolation, forward-axis mapping, frame fallback, roll, scale, and
offset equations. `StaticMeshBasePass.slang` selects it only for the Spline
domain; UV/color/material behavior, masked discard, tangent-space normal
mapping, winding, front face, Lit/Unlit behavior, prepared lights, and
environment lighting remain common. Deformation-only FIFO updates replace the
uniform and bounds without recreating the primitive or source GPU resources.

Vulkan qualification renders opaque, masked, and translucent sections, checks
straight identity pixels against StaticMesh, and checks curved Spline shader
pixels against a StaticMesh built from CPU-deformed positions and tangent bases.
The two curved images are byte-identical on the qualified adapter.

## View-Local Base-Pass Preparation

`FSceneViewSettings` defaults to authored visibility plus conservative frustum
culling and automatic projected-size LOD selection. The explicit
`FrustumCullingDisabled` and `ForceLOD0` settings remain comparison and
diagnostic policies carried by the immutable submitted view; disabling frustum
culling never overrides an authored-hidden primitive.

`FSceneRenderer` fits the view to the output before one centralized visibility
walk. Every live primitive receives exactly one hidden, outside, inside,
intersecting, invalid-bounds fallback, invalid-view fallback, or
culling-disabled classification. Only typed visible family lists feed feature
preparation. Invalid bounds or frustum inputs stay conservatively visible and
increment their named fallback counters; finite fully outside bounds never
reach StaticMesh preparation or a base-pass draw.

The visibility result is preparation-local and is destroyed before resource
preparation and the first scene render pass. `FPreparedSceneView` retains only
the fitted immutable view, copied lighting/sky facts, family-prepared work, and
value counters needed by execution. Sequential main, auxiliary, present,
offscreen, fixed-aspect, thumbnail, and preview invocations construct distinct
prepared values; no SceneInfo list, prepared result, target-size semantic
cache, or temporal state is shared between views.

For each `FSceneView`, Renderer walks the visible mesh candidates and projects
their authoritative world-AABB corners into the fitted content viewport. It
supplies scalar projection/LOD policy through `FMeshCollectionContext`. Engine
providers select the first transition threshold satisfied by the normalized
diameter and validate readiness independently for the requested LOD. A missing requested LOD searches toward lower detail first,
then higher detail; invalid projection or bounds math conservatively requests
LOD 0. `FSceneViewSettings::Mode.LODMode` selects automatic behavior or the qualified
forced-LOD-0 comparison path without process-global state.

Preparation stores transform, diagnostic requested/selected indices, and a
retained `FVertexFactoryInputBinding` in `FPreparedStaticMeshPrimitive`. Opaque,
Masked and Translucent records reference that primitive by vector index and own
checked draw ranges, buffer views, material, pass, shader-map, graphics-state and
sort facts. The input snapshot retains vertex declarations and streams instead
of borrowing LOD/section/factory objects. Execution binds those resources and
forwards direct indexed/non-indexed RHI arguments without rescanning scene
membership or reading an implicit LOD 0.

View-local telemetry conserves visible candidates against prepared plus rejected
primitives. Requested and selected LOD histograms each sum to prepared
primitives, while selected section and triangle totals reconcile with their
Opaque, Masked, and Translucent pass totals. Resource preparation and execution
separately conserve attempted draws against successful plus rejected draws, so a
failed shader, pipeline, sampler, or incomplete command remains attributable to
its rejection phase. Opaque/Masked input and final state-group counts plus
pipeline, material, vertex-factory, and geometry transitions describe the effective
ordering. Renderer emits one immutable `FViewRenderTelemetry` value through the
development telemetry-snapshot sink for every `RenderView` invocation; it retains
no view, target-size, or temporal telemetry cache.

Opaque and Masked execute first after deterministic value-based grouping. Their
keys compare effective pass and pipeline state, material/shader identity and
validated uniform bytes, vertex-factory declaration facts, section geometry,
then primitive id, selected LOD, and section index. Pointer addresses and
unordered-container iteration
never break ties. Grouping may reduce or preserve state groups but cannot move a
draw across pass order. Both passes use blending disabled, depth test `Less`,
and automatic depth writes enabled. Translucent executes last with straight-alpha
color factors `SrcAlpha`/`OneMinusSrcAlpha`, alpha factors
`One`/`OneMinusSrcAlpha`, and automatic depth writes disabled. Explicit Enabled
or Disabled depth-write policy overrides the blend-mode default.

Masked coverage is `saturate(OpacityMask constant * OpacityMask texture red)`.
The statically identified threshold discards only a strictly lower value, so
equality is covered; Opacity and BaseColor alpha do not enter the mask.
One-sided materials cull back faces, two-sided materials cull none, and negative
local-to-world determinant parity changes the effective front face from
clockwise to counter-clockwise. Solid/Wireframe and Lit/Unlit remain orthogonal
pipeline/shader choices.

Translucent items sort independently per view by descending squared distance
from the camera to the transformed section-bounds center. Invalid section bounds
fall back to primitive world bounds and then transformed local origin. Equal
distance uses the same complete value key, including selected LOD and section
facts, without weakening distance-first order. This deterministic center metric
does not provide per-triangle ordering for intersecting geometry.

## Material Binding Contract

`ValidateStaticMeshMaterialOverrides` returns `FStaticMeshMaterialOverrideResult`.
Slot-limit errors retain actual/maximum counts; incompatible-object errors own
the failing index, object path and type. Validation does not mutate overrides.
`FormatStaticMeshMaterialOverrideError` accepts the consumer name only at the
presentation boundary; both StaticMesh and SplineMesh components use this contract.

Each StaticMesh section carries one stable positional material-slot index and
resolves that slot's material proxy snapshot. Asset import preserves matched
indices, retains removed positions, appends new slots, and maps imported
sections explicitly. Component resolution at the same index is override, mesh
default, then the shared Engine `DefaultMaterial` proxy; no slot GUID or source
metadata crosses the render boundary. An empty slot is normal and emits no
warning. A missing proxy after binding resolution is structural failure and
selects ErrorMaterial. The
snapshot carries an Engine-owned `FMaterialRenderRepresentation` and a
separate static shader/pipeline identity. `FStaticMeshRenderer` accepts the
exact v3 layout identified by `MaterialRenderLayoutV3Id`; it decodes the compact PBR constants, eight UV
transforms, per-role sampler states, and eight texture roles through the
version-matched binding decoder.

The draw path does not perform parameter GUID or `FName` lookup and does not
read reflected material objects or legacy fixed material fields. Dynamic
uniform/resource bytes are not part of shader-map or pipeline cache keys, so
dynamic edits reuse the existing identity while static-property edits select
the corresponding cached shader/pipeline pair. The normalized program digest
is part of shader-map, pipeline, diagnostic, and draw-sort identity; identical
programs may reuse a candidate while distinct programs cannot alias. The effective pipeline key also
contains polygon, cull, front-face, depth, and color-blend values, so mirrored
winding, render mode, and every visible material policy select compatible PSOs.

StaticMesh and SplineMesh vertex stages are registered
`FMeshMaterialShader` types. Their exact typed map identity combines the
Material identity with the stable Local or Spline Vertex Factory type and the
forward, GBuffer, or shadow mesh pass. The pipeline retains strong typed vertex
and fragment refs plus their merged layout; no draw-time raw shader-map lookup
or cast is permitted. Forward and shadow map caches remain independent and are
bounded to 256 exact maps, while their dependent pipeline caches retain at most
512 entries.
Texture resources use role-specific white, black, or flat-normal fallbacks;
environment irradiance, prefilter, and BRDF-LUT resources are shared by the
scene renderer and fall back together to black.

After StaticMesh or SplineMesh selects its family pipeline, allocates transform
and optional spline uniforms, and binds its geometry streams, it enters the
shared mesh surface-pass executor. The executor preserves
pipeline/vertex-before-fragment ordering and submits exactly one bounded draw:
opaque shadow takes the zero-material fast path, masked shadow binds only the
canonical role-7 packet, and forward binds the complete surface packet.
GBuffer retains its pipeline/binder ownership while consuming the same uniform,
eight textures, and shared sampler pointers. Renderer entry points assert the
render thread, owning render-pass state, and prepared phase symmetrically.

If the representation identity or field table is unsupported, the Renderer
reports a `ShaderBinding` resource diagnostic and switches to a complete
asset-independent ErrorMaterial snapshot before shader-map or pipeline lookup.
The terminal contains the same validated v3 contract and is not recursively
validated as another fallback, so no partial payload can reach a draw.

## Source and Payload Compatibility

StaticMesh builds publish a complete detached render, ray, and collision product
on the owner thread. Render consumers retain the accepted mesh until publication;
bounds queries and scene preparation do not finish authored work. Source ownership,
payload validation, cancellation, and schema compatibility are defined in
[Static mesh source and building](../Assets/StaticMeshBuilding.md).

## Related Documentation

- [Material System](MaterialSystem.md)
- [Shader Cache](ShaderCache.md)
- [Viewport Rendering](ViewportRendering.md)
- [Static Mesh Render-Data Lifecycle Plan](../../Plans/Archive/2026-08/StaticMeshRenderDataLifecycle.md)
- [Static Mesh LOD Resources Refactor Plan](../../Plans/Archive/2026-08/StaticMeshLODResourcesRefactor.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshResources.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/LocalVertexFactory.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/LocalVertexFactory.cpp`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/RenderCore/Public/VertexFactory.h`
- `Engine/Source/Runtime/RenderCore/Public/RenderResource.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Shaders/Slang/StaticMeshBasePass.slang`
- `Engine/Shaders/Slang/Lighting/PBRLighting.slang`
- `Engine/Shaders/Slang/VertexFactory/LocalVertexFactory.slang`
