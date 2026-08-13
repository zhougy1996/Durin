# Directional Shadows

## Ownership and selection

`FDirectionalShadowRenderer` is a private `FSceneRenderer` feature owner. The
first directional light selected by `FPreparedLightView` may own one shadow for
the current view when its detached `bCastShadows` value is enabled. The light
component, proxy, SceneInfo, and prepared-light path copy this value; render
code never reads a component. Missing, invalid, disabled, or failed shadow
state leaves the existing directional light fully unshadowed.

The feature owns one reusable 2048x2048 D32 texture, an exact sampled view, an
exact depth-attachment view, and a linear clamp-to-border opaque-white
`LessOrEqual` comparison sampler. Logical storage is 16,777,216 bytes. Backend
allocation bytes are reported independently. The target is cleared and
regenerated before Scene Color for every enabled main, auxiliary, preview,
present, or offscreen view; its dimensions do not follow viewport resize.

## Fitting and caster preparation

The fitted `FSceneView` supplies eight zero-to-one Vulkan clip corners. Receiver
depth is limited to the nearer camera far extent or 256 world units. The
normalized light direction is the light-space forward axis; world +Z is the
preferred up reference and world +Y is the parallel fallback. Receiver XY is
expanded by a two-texel guard and its center is snapped to whole shadow texels.
The caster volume shares that XY interval and extrudes 256 units opposite light
travel. Invalid inversion, basis, extents, or matrices disables the shadow.

Caster discovery starts from authoritative `FScene` primitive collections, not
camera visibility. Boundary contact and invalid finite bounds are included
conservatively and counted. Static/Spline LOD selection is evaluated against
the 2048 shadow view; SkeletalMesh remains LOD0 and Terrain remains single LOD.
The camera's prepared visibility and LOD values are not mutated.

## Pass and material contract

Opaque and Masked StaticMesh, SplineMesh, SkeletalMesh, and Terrain draws cast;
Translucent draws do not. Shadow preparation reuses normal material snapshots,
strict opacity-mask threshold behavior, vertex factories, spline deformation,
skeletal palettes, terrain height resources, two-sided state, and mirrored
winding. Skeletal shadow draws reuse the base view's matching frame-local
palette range rather than uploading a second palette.

The depth-only pass has no color attachment, clears D32 to 1.0, writes with
`Less`, and exits in `ShaderReadOnly`/`GraphicsShaderRead`. It forces filled
rasterization even for Wireframe camera views and uses constant/slope/clamp
depth bias 1.25/1.75/4.0. All shaders, PSOs, samplers, material resources,
geometry bindings, height resources, and palette ranges are prepared before
the pass begins.

## Forward sampling and failure

The reflected forward-lighting ABI contains one world-to-shadow matrix and
params `(enabled, receiver bias, texel world size xy)`. One shared Slang helper
rejects non-finite or out-of-range projected coordinates as fully lit and uses
a receiver bias of 0.0005. Only the selected directional direct-light term is
attenuated; local lights, environment/ambient, emissive, rim assistance, and
Unlit output are unchanged. Every prepared base draw carries an explicit
shadow texture/sampler pair or the complete white/default fallback, preventing
stale bindings across sequential views.

Target/view/sampler or draw-resource failure disables the feature for that view
and continues Scene Color unshadowed. Renderer release, device invalidation,
manual retry, and shader reload use the existing resource coordinator; the
feature introduces no `WaitIdle` or whole-device flush. Counters report light
and receiver selection, caster outcomes by family, resource outcomes, draws,
logical/backend target bytes, and preparation failure. Optional GPU timing
sinks expose Shadow Depth and Scene Color independently.
