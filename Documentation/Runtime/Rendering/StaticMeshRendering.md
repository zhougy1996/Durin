# Static Mesh Rendering

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
`FRawStaticIndexBuffer`, `Sections`, `LocalBounds`, `NumTexCoords`, and
`bHasColorVertexData`. It exposes no raw RHI references and no writable legacy
vector fields.

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

`BeginDestroy()` queues release for initialized resources and starts the
asset's one destruction fence. `IsReadyForFinishDestroy()` remains false until
that fence and the normal `DObject` lifecycle are complete; `FinishDestroy()`
then destroys the aggregate. Engine termination drains ordinary DObject and
render-command ownership while the asset keeps this same release contract—no
StaticMesh-specific shutdown registry or global render flush is required.

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
Vulkan vertex fetch expands the normalized integer tangent and color streams to
floats before the vertex-factory module receives them.

`FLocalVertexFactory::GetShaderModuleName()` returns the stable import name
`VertexFactory.LocalVertexFactory`. The shader compiler links imported module
dependencies before code generation and fingerprints imported modules so a
change invalidates every dependent shader artifact.

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
the corresponding cached shader/pipeline pair. The current uniform ABI,
solid/wireframe choice and typed descriptor submission remain unchanged.
Texture resources use role-specific white, black, or flat-normal fallbacks;
environment irradiance, prefilter, and BRDF-LUT resources are shared by the
scene renderer and fall back together to black.

If the representation identity or field table is unsupported, the Renderer
reports a `ShaderBinding` resource diagnostic and switches to a complete
asset-independent ErrorMaterial snapshot before shader-map or pipeline lookup.
The terminal contains the same validated v3 contract and is not recursively
validated as another fallback, so no partial payload can reach a draw.

## Payload Compatibility

DMSH schema 3 stores a bounded material-slot count rather than slot GUIDs.
Every decoded section index is validated against that count; package metadata
then restores editor/runtime slot names and imported source indices by stable
position. Schema 2 is incompatible, and builder version 2 invalidates prior
derived data. Encode reads semantic data back from the named buffer resources;
decode constructs them from the payload's position, normal, tangent, UV,
color, and index arrays.

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
