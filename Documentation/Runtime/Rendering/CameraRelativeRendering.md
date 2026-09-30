# Camera Relative Rendering

Summary: GPU spatial calculations use per-view translated world space, with subtraction performed in CPU double precision before float conversion.
Modules: Engine, RenderCore, Renderer, MaterialEditor

Last reviewed: 2026-10-01

## Coordinate boundary

CPU world locations, scene bounds, transforms, visibility, gameplay, and persisted
assets use absolute double world coordinates. `FSceneView::ViewLocation` is the
render origin for one view. GPU World positions are `absolute - ViewLocation`;
the camera is at zero. Translation retains world axis orientation and units.
Each viewport, preview, and capture prepares its own translated uniforms.

`FSceneView` owns double translation helpers. Local-to-world GPU matrices use
`Translation(-origin) * LocalToWorld`; their inverses use
`WorldToLocal * Translation(origin)`. World-to-view uses
`ViewMatrix * Translation(origin)` and world-to-clip composes projection with
that translated view matrix. Narrow only after these operations. Cached CPU
inverse transforms remain double and view-independent. Direct and GPU-instanced
mesh draws share primitive uniform construction; GPU culling bounds and its
view-projection matrix use the same translated space.

Directions, normals, scales, and World Position Offset are translation-invariant.
CPU projected simple elements and gizmos may compose their complete projection
in double before float conversion. Editor grid phases remain double-derived;
sky directions remove view translation in double before narrowing the final matrix.

## Material positions

Material `WorldPosition`, `CameraPosition`, `ObjectPosition`, and the World
coordinate space in `TransformPosition` refer to translated world space.
`CameraPosition` is zero; `ObjectPosition` is the translated primitive bounds
center. Vertex WorldPosition is pre-WPO; pixel WorldPosition is displaced and
interpolated. Object and View coordinates retain their local meanings.
`CameraVector`, lighting, picking distance, and shadow receiver calculations
consume translated positions directly.

There is no implicit absolute-world reconstruction or legacy material position
mode. World-space position constants and collection parameters authored on the
CPU must use the matching view origin before entering a positional calculation;
spatial vectors and offsets do not need translation. Absolute/world-anchored
procedural effects need an explicit origin or periodic phase contract rather
than silently adding a large float world origin. Generated material cache and
pass-contract versions invalidate the previous position convention.

## Lighting and history

Local lights subtract the render origin in double. Deferred depth reconstruction,
contact shadows, and AO use translated transforms. Shadow fitting and caster
visibility stay in absolute CPU double space. Caster views inherit the receiver
origin; receiver shadow matrices compose that origin before narrowing, and
cascade receiver-depth rows adjust their constant term by the origin dot product.

Cloud layer heights subtract the origin Z. Base, detail, and weather sampling
preserve their absolute periodic pattern using double-computed fractional phases,
with separate base and detail phases. Cloud shadows use the same convention.
The CPU cloud reference integrator follows this boundary as well.

Temporal metadata remains absolute double. Cloud reprojection composes the
previous absolute view-projection matrix with `Translation(current origin)` in
double, mapping current translated positions directly into previous clip space.
An origin change must not be handled by adding a large absolute float coordinate.

## Validation

CPU regression tests cover fractional mesh/light offsets at large origins,
independent views, inverse transforms, GPU culling bounds, shadow receiver depth,
cloud periodic phases, and moving-origin reprojection. GPU correctness coverage
also exercises generated materials and the spatial/temporal cloud routes; build
and CPU results alone do not establish pixel equivalence on every backend.
