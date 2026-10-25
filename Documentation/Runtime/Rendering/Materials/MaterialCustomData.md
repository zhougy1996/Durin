# Primitive and Instance Material Data

Summary: Runtime numeric channels vary shared materials per primitive and stable StaticMesh instance without publishing new material owners.
Modules: Engine, RenderCore, Renderer, MaterialEditor

Last reviewed: 2026-10-16

## Numeric contract

`FPrimitiveCustomData` contains 32 finite float channels, initially zero.
`DPrimitiveComponent::SetCustomPrimitiveData(First, Values)` writes a checked
contiguous range. Empty ranges, out-of-range indices and nonfinite values reject
the entire update. Signed zero is canonicalized. Unchanged updates publish no
new state. These APIs belong to the game thread.

`DMaterialExpressionPrimitiveCustomData` and
`DMaterialExpressionInstanceCustomData` expose `DataIndex` and `ResultType`.
Float, Float2, Float3 and Float4 reads consume consecutive channels; the complete
range must fit within 0-31, including reads that cross a four-component slot.
Unsupported types and indices fail graph/MIR validation. Both reads are legal
in vertex and pixel evaluation. Instance data has independent zero defaults;
it does not fall back to primitive data.

Runtime values live outside material parameter declarations, render proxies and
shader identities. Changing a node's index or type changes material code;
changing an object's values does not compile or publish a material. The node
types use the normal reflected graph catalog, package persistence and Cook
pipeline. Runtime component/instance values themselves are transient and are
not serialized as authored component defaults in this slice.

## Stable StaticMesh instances

`DStaticMeshComponent::SetRenderInstances` copies an immutable snapshot of at most
4096 `FStaticMeshRenderInstance` records. Each record has a unique nonzero uint32
`Id`, an invertible finite affine `LocalToPrimitive` transform, and 32 custom
float channels. Invalid records reject the whole snapshot. The instance
transform composes after the component transform in local-to-world evaluation.

`SetInstanceCustomData(Id, First, Values)` and `RemoveRenderInstance(Id)` address
the stable ID, not the current vector index. Reordering a replacement snapshot
preserves data association and render-cache identity. A non-null empty snapshot
emits no geometry. `ClearRenderInstances()` restores the ordinary single-mesh
behavior; its Instance channels are zero. Instance updates copy the bounded
snapshot; this is not an incremental GPU-scene upload API.

StaticMesh proxies retain the snapshot and expose aggregate transformed local
bounds. Ordered scene updates refresh those bounds and subsequent collection
emits one batch per stable ID. Selected asset LOD, geometry records and materials
remain shared. LOD selection currently belongs to the aggregate component, not
to independent per-instance distance selection. Instanced physics bodies,
asset serialization and an instance-painting editor are outside this API.

Custom primitive data also reaches SplineMesh through the ordinary mesh
collection context. Other mesh providers must copy the context's custom
channels into their published `FMeshBatch` fields. Batch admission rejects
nonfinite custom values, including the retained-geometry fast path.

## Rendering and ownership

Preparation copies both channel banks into immutable per-batch facts. The
primitive-state cache comparison includes both banks; changed values rebuild
dependent uniform/binding packets while prepared older frames retain their
previous storage. Geometry descriptions, shader maps and material parameters
remain shareable. Removal, replacement and data publication obey the same scene
command queue ordering as transform and material updates.

`MaterialPrimitiveUniform` and its CPU counterpart now occupy 512 bytes: the
existing 256-byte transform/origin prefix, 128 Primitive bytes, then 128 Instance
bytes. Direct, Spline and shadow vertices consume that uniform. GPU-instanced
vertices select the same structure through the existing visible-instance index
and `InstanceTransforms` buffer. Generated flat varyings carry only referenced
four-component slots to the fragment stage. Consequently a grouped fragment
reads its selected instance, independently of the representative draw's uniform.
The transport uses no additional descriptor bindings.

Material vertex shaders classify the optional Material uniform as dynamic, just
as material fragments do. Geometry preparation leaves that binding to material
composition. This permits parameter-driven WPO and pixel parameters to share the
same buffer without conflicting pipeline descriptor types.

Per-view GPU binding canonicalization includes compatible packets restored from
persistent commands. Mixed cache hits and misses therefore share one packet
when their full material group, shader and primitive-dependency key agrees.

Existing [GBuffer GPU grouping](../Pipeline/GBuffer.md#pass-and-resource-lifecycle) still requires compatible
Local indexed draws and at least 64 candidates. Eligible instances can share one
indirect draw despite different numeric values. Forward, shadows, small groups
and other fallbacks retain their existing direct submission behavior; custom
data alone does not guarantee fewer draw calls. The 4 MiB generated-buffer limit
charges the enlarged 512-byte records and can admit fewer candidates at the same
budget. These costs must accompany any draw-count comparison.

WPO reads use the same selected data before clip transformation; masked shadows,
hit proxies and GBuffer share the accepted vertex product. WPO does not expand
bounds or change collision geometry. Authors must keep bounds conservative for
their maximum displacement, as required by [vertex evaluation](MaterialVertexEvaluation.md).

MIR/normalization revision 8 introduced these numeric reads. Current producer
and Cook compatibility versions are owned by [shader stage selection](MaterialShaderStages.md).
Cook retains compiled stages and typed graph identity; incompatible pass
contracts or container schemas require recooking. The material parameter layout
version remains independent of primitive/instance channels.

## Design references

The ownership model follows UE's
[Custom Primitive Data](https://dev.epicgames.com/documentation/unreal-engine/storing-custom-data-in-unreal-engine-materials-per-primitive?lang=en-US)
and [ISM custom data](https://dev.epicgames.com/documentation/unreal-engine/instanced-static-mesh-component-in-unreal-engine)
separation. Durin selects a fixed 32-float bound for each bank and stable-ID
snapshot APIs; UE also exposes a configurable per-instance float count and
[ID-based updates](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/UInstancedStaticMeshComponent/SetCustomDataById).
