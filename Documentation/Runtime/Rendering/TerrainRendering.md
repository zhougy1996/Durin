# Terrain Rendering

Summary: Defines the finite single-LOD Terrain Actor, scene proxy, exact height resource, patch visibility, material, and lifecycle contracts.

Modules: Engine, RenderCore, Renderer, LevelEditor

Last reviewed: 2026-08-12

## Runtime ownership

`ATerrainActor` owns one `DTerrainComponent`. The component persists a
`DTerrainHeightmap`, positive finite X/Y sample spacing, finite signed height
scale, finite height offset, and one optional PBR material. A null material uses
the Engine default-material proxy. Assets larger than 1025 samples on either
axis remain valid heightmap assets but do not publish a render proxy.

One proxy creation captures a `shared_ptr<const FTerrainHeightmapPayload>` and
its revision. `FTerrainSceneProxy` contains only that immutable snapshot,
copied numeric values, copied patch descriptors and bounds, and a counted
material proxy. It never retains a reflected Actor, Component, or mutable
sample pointer. Heightmap publication temporarily removes registered consumers
and recreates them in object-handle order after the complete new revision is
visible. Failed and semantic no-op reimports do not replace the published
render generation.

## Coordinates and patches

Sample `(X,Y)` maps to component-local position
`(X * SpacingX, Y * SpacingY, HeightOffset + Sample / 65535 * HeightScale)`.
Source rows are top-to-bottom and become increasing local Y. No flip,
transpose, half-texel offset, or observed-range normalization occurs. Negative
height scale is supported; bounds order the converted minimum and maximum.
UV0 is `(X / (Width - 1), Y / (Height - 1))`.

Terrain uses one sample per vertex and one 64x64-cell maximum topology. Patch
origins are emitted Y-major then X-major. Right and bottom patches use their
exact remaining cell dimensions, adjacent patches repeat identical border
sample coordinates, and no triangle extends beyond the authored sample plane.
Each patch queries the heightmap's exact inclusive-vertex rectangle through
the half-open min/max API. The renderer transforms its eight bound corners and
classifies every patch once after the primitive visibility gate. Finite outside
patches do not prepare resources or draw; invalid bounds remain visible and
increment the fallback counter.

## GPU and shading contract

The Renderer shares a one-mip `R16_UINT` texture by immutable payload identity
and dimensions. Upload pitch is exactly `Width * sizeof(uint16)`. Revisions are
never updated in place. The cache admits at most 64 retained height revisions;
an additional distinct revision is rejected without disturbing complete
entries. Topology is shared by exact cell dimensions and admits at most 256
variants.

`FTerrainVertexFactory` binds one `UShort2` patch-local grid coordinate. The
Terrain shader performs integer texel loads and divides by 65535. Interior
normals use central differences; terrain edges use the available one-sided
span. XY spacing and signed height scale participate in the derivative before
the normal is transformed by the inverse-transpose matrix. The two triangles
of each cell are `(A,B,C)` and `(B,D,C)`; the existing determinant-based front
face policy handles mirrored component transforms.

Terrain compiles the existing material base pass with the Terrain vertex
contract. Material v3 and v2 compatibility, ErrorMaterial fallback, texture
fallbacks, Opaque/Masked/Translucent classification, two-sided state, depth
policy, Lit/Unlit lighting, Solid/Wireframe rasterization, environment data,
and Present/offscreen render-target layouts remain shared Renderer policy.
Terrain translucent patches enter the same back-to-front geometry list as
StaticMesh and SkeletalMesh draws.

## Visibility, counters, and lifecycle

The scene owns one authoritative typed Terrain SceneInfo list in addition to
the all-primitive list. Command-local counters report visible Terrain
primitives, candidate/visible/culled patches, invalid-bound fallbacks,
pass-classified patches, triangles, resource attempts/results, draw
attempts/results, exact height uploads/reuses/bytes, and topology
creations/reuses/bytes. A 1025x1025 heightmap produces 256 patches, 2,097,152
triangles, a 2,101,250-byte height upload, and at most 66,052 bytes for the
shared 64x64 topology. A measured 4097x4097 candidate with 4,096 draws did not
complete the Debug validation-layer gate within 300 seconds on the named GTX
1060 adapter, so T1 deliberately rejects it. This is the correct single-LOD
baseline for the later LOD plan, not a scalability target.

The frozen qualification profile is Win64 Debug DurinEditor, threaded Vulkan
with validation enabled, NVIDIA GeForce GTX 1060 6GB, Vulkan API 1.3.280,
17x17 offscreen output, two warm-up frames, and seven measured frames. For the
1025x1025 ceiling it recorded CPU command preparation median 3096.58 ms / p95
3150.25 ms and Scene Color GPU median 26.7722 ms / p95 37.8943 ms. The ceiling
therefore uses explicit Debug characterization budgets of 5000 ms CPU and 50 ms
GPU on this profile. These intentionally loose finite gates expose the urgent
T3 draw-submission problem; they are not shipping performance targets.

Device invalidation and renderer shutdown release Terrain vertex factories,
topology buffers, height textures, samplers, shader maps, and pipelines through
the existing ordered resource coordinator. Recorded RHI commands retain their
resource references independently. Scene removal first detaches the typed
SceneInfo, while immutable payload ownership keeps an accepted revision alive
until its proxy and upload are retired.

## Editor and cooked runtime

Terrain Actor and Component properties are reflected and serialize through the
ordinary Actor/Component path. The World Outliner can construct the reflected
actor, and dropping a Terrain Heightmap into the scene viewport creates a
Terrain Actor, assigns the asset, positions it through the existing viewport
ray path, marks the level dirty, and selects it. Editor picking uses the same
conservative component-local bounds.

Cooked runtime obtains the exact payload from the heightmap companion. Proxy
creation needs neither source data nor DDC and uses the same patch and GPU
contracts as editor-loaded data.
