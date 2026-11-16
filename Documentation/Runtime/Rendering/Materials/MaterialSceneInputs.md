# Material Scene Inputs

Summary: Define logical material scene reads, viewport/depth conversion and Renderer-owned RDG resource lifetimes.

Modules: Engine, Renderer, RenderCore

Last reviewed: 2026-11-16

## Authoring and Compilation

`DMaterialExpressionSceneTexture` follows UE's scene-input boundary: a logical
`SceneTextureId`, optional Float2 `Coordinates`, and `bFiltered`. Coordinates
are viewport UV; an absent connection/default uses ScreenPosition. Input0
supports Point and Linear, with Linear as the expression default. SceneDepth
requires Point; a filtered depth declaration rejects before shader compilation.
SceneTexture returns RGBA for Input0 and replicated device depth for SceneDepth.
`DMaterialExpressionSceneDepth` returns Float3 `(device depth, positive view
depth, validity)`, using the shared analytic projection reconstruction helper.

`ValidateMaterialConfiguration` owns shared domain/model/blend admission.
Scene reads are Pixel-only and execute only in opaque Unlit PostProcess
evaluation. Domain legality is checked after static selection and inactive
property removal. Functions inherit the executing caller's legality. Retained
inactive authoring data does not create a scene binding. Mesh UVs, primitive
data, vertex interpolation and object/tangent transforms cannot supply a
PostProcess execution context.

The minimal PostProcess compilation contract requests only
`PostProcessMaterial / PostProcessMaterialMain`. It has no Vertex Factory,
lighting or mesh shader request. Its root is Emissive Color. Output RGB is
finite-clamped to nonnegative linear HDR through 65504; alpha comes from Input0.
The pass applies no exposure or display transfer.

Each entry owns sorted, unique `(InputId, Filter)` requirements. All selected
reads are required. Input0/Linear is also required for output alpha, including
constant effects. These values are separate from authored texture parameter
IDs and compact material texture indices. MIR/canonical identities include the
read payload. Detached translation and source-free DMAT retain requirements,
domain and typed entry identities; they contain no Renderer resource pointers.
Cooked admission verifies the package root's domain as well as its parameters.

## Viewport and Depth Conversion

Renderer's `FScreenPassTexture` couples a graph-local texture handle with
`ViewMin` and `ViewSize`, the active integer rectangle. The texture description
comes from `FRDGBuilder::GetTextureDesc`, an owned snapshot that rejects foreign
handles. Zero/negative extents, out-of-bounds rectangles, multisampling,
non-2D resources and incompatible formats reject during planning.

For viewport UV `v`, the input texel coordinate is `ViewMin + v * ViewSize`.
Clamp centers to `[ViewMin + 0.5, ViewMin + ViewSize - 0.5]`, then divide by the
texture extent. Point and Linear have separate shader helpers, so reflection
does not retain an unrequested sampler through a runtime filtering branch.
Backend orientation is already supplied by fullscreen/projection conventions;
the scene-input adapter adds no second Y flip.

Depth reconstruction uses the actual point-sampled texel center in the depth
viewport, rather than an unclamped/interpolated ray. Projection rows support
NormalZ and ReversedZ, including oblique projections. The declared clear value
is 0 or 1. Clear depth within `1e-7`, nonfinite depth or failed reconstruction
produces view depth 0 and validity 0. Positive view depth is the magnitude of
the reconstructed view Z. Projection rows must be finite and nonsingular.

## RDG Planning and Binding

`PlanMaterialSceneTextures` consumes one PostProcess entry and the current
frame's inputs. Input0 is a read-only RGBA16F image; depth is the completed
geometry depth and excludes depthless translucency. The caller supplies the
composed Input0 snapshot. The plan creates a distinct RGBA16F output, sized to
the active Input0 viewport, with an origin of zero. It never writes an input.

`FMaterialSceneTexturePassParameters` declares optional typed Input0/depth reads
and an ordinary `FRDGColorAttachmentBinding` output. Submit it through
`AddRasterPass`; RDG owns attachment transitions, ordering, allocation and
retirement. The output is fully overwritten. Input0 and depth use base mip,
one layer, and their exact color/depth aspect. Sampling views match these
declared ranges. Scene input lifetime follows the graph and counted RHI
references, without a persistent raw texture cache across frames or resizing.

`ResolveMaterialSceneTextureBindings` runs inside the pass callback with that
pass's resolver authority. It atomically builds counted views, samplers and an
owned uniform range from reflected scene bindings. Set 0 reserves binding 40
for scene uniforms, 41 for Input0, 42 for depth, and 43/44 for Point/Linear.
The existing MaterialView and set-1 material/collection bindings stay separate.

Invalid requirements, missing input, incompatible viewport or binding allocation
returns a local error; the caller retains Input0 and bypasses the effect. A
failed RDG allocation records no callback and publishes no extracted output.
Retry uses a newly planned graph/frame. No incomplete output or previous
frame's scene binding is substituted. The per-view material slot and its
placement in the display chain are separate from this input/planning API.

See [Render Graph](../Pipeline/RenderGraph.md) for resolver authority and backing
retirement, and [Material System](MaterialSystem.md) for material publication.
