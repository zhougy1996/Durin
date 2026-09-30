# Material Vertex Evaluation

Summary: Material programs evaluate World Position Offset in the vertex stage and expose explicit bounded vertex-to-pixel interpolators.
Modules: Engine, RenderCore, Renderer, MaterialEditor

## Authoring and semantics

The Surface material terminal exposes a `World Position Offset` Float3 input
with zero default. Its connection and retained literal remain independent of
the eight pixel Surface attributes and Material Attributes mode. Graph commands,
transactions, working copies, Apply and package persistence use the ordinary
terminal input contract.

WPO accepts vertex-legal non-spatial Float3 values or World Direction/Normal
values. Position values are rejected: subtract positions to form a direction.
Object, View and Tangent offsets require an explicit transform to World.
The root selects Vertex evaluation; `Both` remains a legality mask, without
automatic hoisting or interpolation.

`Vertex Interpolator` is an independent adaptive numeric expression. Its input
must allow Vertex evaluation; its output preserves width and spatial semantics
and is Pixel-only. Pixel-only inputs, implicit texture sampling and nested
interpolators are rejected. A reachable interpolator used inside a material
function obeys the same semantic checks after expansion.

Up to eight reachable interpolators receive deterministic TEXCOORD locations
after the four existing mesh UV channels. Interpolator inputs execute once per
vertex, and rasterization interpolates their values for pixel consumers. They
also support authored customized UV calculations by connecting the interpolated
Float2 to a texture sample's UV input. Nonlinear math before interpolation can
produce results that vary with tessellation; interpolate inputs first when the
operation requires per-pixel precision.

`WorldPosition` is Both/World/Position: the vertex stage reads the position after
vertex-factory deformation and before material WPO, while the pixel stage reads
the interpolated displaced position. `VertexNormal` remains Vertex/World/Normal.
WPO does not reconstruct normals or update collision geometry. Existing geometry
bounds remain unchanged; displacement beyond them requires conservative authored
bounds for visibility and shadow culling.

## Compilation and execution

MIR keeps WPO as a separate vertex root. Reachability, parameter and collection
discovery, normalization and canonical identity include it regardless of Surface
attribute mode or shading model. Canonical operand sorting also permutes scalar
broadcast metadata. Pixel evaluation stops traversal at each interpolator;
vertex evaluation traverses its input and the WPO root. Shared expressions may
execute separately when both stages consume them.

Accepted programs contain four fragment artifacts plus `VertexMain`,
`SplineVertexMain` and `GPUCullingVertexMain`. The generated dependency closure
includes the Local and Spline vertex factories. All seven entries, their exact
frequencies and allowed resource reflection are validated together. MIR version
7, generator version 10, compiler envelope 11, pass contract 7 and cooked payload
schema 10 invalidate incompatible cache and cooked artifacts.

Local and Spline vertices apply WPO after vertex-factory deformation. GPU-culling
vertices use the selected instance transform. Clip displacement transforms the
world offset as a homogeneous direction, preserving the active view projection.
Forward, GBuffer, opaque/masked shadows, hit proxies, material previews and
thumbnails use the accepted vertex artifact with their selected fragment.

Vertex parameter batches combine geometry bindings with reflected material,
view and collection bindings. Their reuse key includes primitive, material,
pass and shader so sections with distinct WPO parameters cannot share stale
vertex bindings. Opaque shadows retain a fixed resource-free fragment but now
prepare material resources for the vertex stage. Hit proxies use world collection
snapshots with the same schema checks as mesh passes. The independent fixed
error-material path remains available without authored graphs or compiler sources.
