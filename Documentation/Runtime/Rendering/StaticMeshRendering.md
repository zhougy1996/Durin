# Static Mesh Rendering

Summary: Define static-mesh render data, scene proxies, materials, draw preparation, and pass participation.

Modules: Engine, Renderer, RenderCore

SkeletalMesh uses the same material/pass policy and combined Translucent order
through its dedicated geometry, vertex-factory, palette, and renderer owner; see
[Skeletal Mesh Rendering](SkeletalMeshRendering.md).
SplineMesh is a distinct primitive/deformation domain that borrows these
StaticMesh LOD resources and uses the same material/pass/LOD/lighting policy.

This document defines the implemented static-mesh render-resource ownership,
per-LOD lifecycle, vertex-factory boundary, and shader input contract.

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
immutable CPU ray-query acceleration allocation. It is built before render-data
publication from the same finite CPU positions and uint32 index triplets, so it
cannot outlive or mismatch the LOD generation that owns it. Replacement,
reimport, exchange, release, and destruction retire it with that render data;
component instances and editor viewports borrow the same allocation.

The flat binary hierarchy uses longest-axis centroid partitioning, triangle
ordinal as the deterministic split tie-break, and at most eight triangles per
leaf. Nodes contain local-space bounds plus child or triangle ranges. Exact
retained bytes and build nanoseconds are stored with the allocation. The asset
ceiling is 256 MiB and the measured layout allowance is 96 bytes per indexed
triangle with a 1 KiB small-asset floor. Invalid geometry, integer overflow, a
failed allocation, or either ceiling leaves acceleration absent; consumers
must use the complete reference geometry rather than a partial hierarchy.

LevelEditor currently queries only LOD 0 to preserve its semantic picking
contract. It transforms rays to local space, visits nearer child bounds first,
prunes against the current world-distance winner, and retains the double-sided
reference triangle test. This data is not serialized or added to derived data;
non-editor builds do not construct it.

`IsReadyForRendering()` requires the selected LOD's vertex buffers, index
buffer, and vertex factory to be ready and the same geometry validation to
pass. A populated RHI reference cannot make a malformed LOD renderable.

High-level render-data ownership, replacement, proxy recreation, release
fences, and deferred destruction are owned by the
[Static Mesh Render-Data Lifecycle Plan](../../Plans/Archive/2026-08/StaticMeshRenderDataLifecycle.md).

## Asset Lifecycle

`DStaticMesh` uniquely owns its current `FStaticMeshRenderData`. Detached
builders own unpublished candidates, and synchronous replacement temporarily
owns the displaced render data in a local `std::unique_ptr`. No scene proxy,
vertex factory, render command, or material/thumbnail consumer owns concrete
render data.

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

The editor's rendered-thumbnail cache queries `GetRenderResourceStatus()` and
`GetLOD0LocalBounds()` without waiting. A cold StaticMesh request loads the exact
asset class, initializes resources only from the unavailable state, and waits
across editor frames until a ready nonzero revision is published. The shared
preview scene then assigns one `DStaticMeshComponent`, mutually exclusive with
its Material sphere and TextureCube assignments.

Framing is derived deterministically from finite, non-degenerate LOD 0 bounds,
the frozen camera direction and field of view, output aspect ratio, and image
margin. The preview transform centers the local bounds; the returned camera and
clip planes contain every projected corner. Rendering normally uses the same
automatic per-view LOD policy as other scene views; test and comparison previews
can request forced LOD 0 explicitly. Material resolution uses the normal
positional default described below, including the shared default and
ErrorMaterial fallbacks. The clear region remains transparent while rendered
mesh pixels retain their coverage, allowing the card background to show through
without a fixed-color square.

The cache revalidates the captured render-resource revision immediately before
capture, after readback, and after PNG encoding before atomic publication. A
revision mismatch, cancellation, resource failure, invalid bounds, or shutdown
resets the shared component and view, releases the loaded reference, and leaves
the Content Browser fallback icon. Warm persistent hits decode and upload the
PNG without loading or initializing the StaticMesh and without creating or
mutating the preview scene.

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

Decode helpers are module-internal. Transform, material sampling, tangent-space
normal construction, environment sampling, and pass entry points remain in
`StaticMeshBasePass.slang`. `Lighting.PBRLighting` owns the pass-independent
Cook-Torrance GGX direct-light and split-sum environment-light evaluations.
The direct GGX distribution relies on the canonical minimum perceptual
roughness of `0.045` for finite evaluation and uses
`(1 - NoH^2) + NoH^2 * alpha^2` for its denominator term. It does not apply a
separate denominator floor that would reshape low-roughness highlights;
specular antialiasing remains a distinct future filtering concern.
Vulkan vertex fetch expands the normalized integer tangent and color streams to
floats before the vertex-factory module receives them.

`FLocalVertexFactory::GetShaderModuleName()` returns the stable import name
`VertexFactory.LocalVertexFactory`. The shader compiler links imported module
dependencies before code generation and fingerprints imported modules so a
change invalidates every dependent shader artifact.

### SplineMesh vertex deformation domain

Prepared mesh identity includes
`EVertexDeformationDomain::{Local,Spline,Skeletal}` in shader-map, effective
graphics-pipeline, and draw-sort keys. Material identity alone therefore cannot
alias Local and Spline vertex programs. `FSplineMeshSceneProxy` supplies the
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

For each `FSceneView`, `FStaticMeshRenderer` walks the authoritative visible
StaticMesh SceneInfo list once. It projects all eight authoritative world-AABB
corners into the fitted content viewport, selects the first transition threshold
satisfied by the normalized diameter, and validates readiness independently for
the requested LOD. A missing requested LOD searches toward lower detail first,
then higher detail; invalid projection or bounds math conservatively requests
LOD 0. `FSceneViewSettings::LODMode` selects automatic behavior or the qualified
forced-LOD-0 comparison path without process-global state.

Preparation stores transform, requested/selected indices, and the selected LOD
and vertex factory once in `FPreparedStaticMeshPrimitive`. Opaque, Masked, and
Translucent draw records reference that stable primitive by vector index, then
store only section-local geometry, resolved material/binding, pass, shader-map,
graphics-state, and sort facts. Execution resolves the index and binds the
selected vertex factory, index buffer, and section without rescanning scene
membership, resolving material identity, or reading an implicit LOD 0.

View-local counters conserve visible candidates against prepared plus rejected
primitives. Requested and selected LOD histograms each sum to prepared
primitives, while selected section and triangle totals reconcile with their
Opaque, Masked, and Translucent pass totals. Resource preparation and execution
separately conserve attempted draws against successful plus rejected draws, so a
failed shader, pipeline, sampler, or incomplete command remains attributable to
its rejection phase. Opaque/Masked input and final state-group counts plus
pipeline, material, vertex-factory, and geometry transitions describe the effective
ordering. Renderer emits one immutable `FViewRenderCounters` value through the
development counter-snapshot sink for every `RenderView` invocation; it retains
no view, target-size, or temporal counter cache.

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
exact v3 layout identified by `MaterialRenderLayoutV3Id` and frozen exact-v2
data for compatibility; it decodes the compact PBR constants, eight UV
transforms, per-role sampler states, and eight texture roles through the
version-matched binding decoder.

The draw path does not perform parameter GUID or `FName` lookup and does not
read reflected material objects or legacy fixed material fields. Dynamic
uniform/resource bytes are not part of shader-map or pipeline cache keys, so
dynamic edits reuse the existing identity while static-property edits select
the corresponding cached shader/pipeline pair. The effective pipeline key also
contains polygon, cull, front-face, depth, and color-blend values, so mirrored
winding, render mode, and every visible material policy select compatible PSOs.
Texture resources use role-specific white, black, or flat-normal fallbacks;
environment irradiance, prefilter, and BRDF-LUT resources are shared by the
scene renderer and fall back together to black.

If the representation identity or field table is unsupported, the Renderer
reports a `ShaderBinding` resource diagnostic and switches to a complete
asset-independent ErrorMaterial snapshot before shader-map or pipeline lookup.
The terminal contains the same validated v3 contract and is not recursively
validated as another fallback, so no partial payload can reach a draw.

## Payload Compatibility

DMSH schema 4 stores each LOD's `ScreenSize` beside its geometry and retains
the schema-3 bounded material-slot count rather than slot GUIDs.
Every decoded section index is validated against that count; package metadata
then restores editor/runtime slot names and imported source indices by stable
position. Schema 3 and older payloads are incompatible, and builder version 3
invalidates prior derived data while the derived-data key schema remains 1
because it already encodes both version values. Source-backed assets and stale
DDC entries rebuild; cooked/runtime-only schema-3 content must be recooked and
is never silently reinterpreted. Encode reads semantic data back from the named buffer resources;
decode constructs them from the payload's position, normal, tangent, UV,
color, index, and LOD-policy data. Decode and render-data reconstruction publish
only after the complete policy and geometry validate.

CPU storage is retained while editor and test consumers inspect LOD data.
`NeedsCPUAccess` is the explicit policy for a future discard path; this
refactor does not silently drop arrays after upload.

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
