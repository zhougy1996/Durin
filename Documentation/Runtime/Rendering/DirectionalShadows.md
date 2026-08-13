# Directional Shadows

Summary: Defines the selected directional-light shadow map, deterministic PCF tiers, diagnostics, failure fallback, and qualification evidence.

Modules: RenderCore, Renderer, Engine, VulkanRHI

Last reviewed: 2026-08-14

## Ownership and selection

`FDirectionalShadowRenderer` is a private `FSceneRenderer` feature owner. The
first directional light selected by `FPreparedLightView` may own one shadow for
the current view when its detached `bCastShadows` value is enabled. The light
component, proxy, SceneInfo, and prepared-light path copy this value; render
code never reads a component. Missing, invalid, disabled, or failed shadow
state leaves the existing directional light fully unshadowed.

The feature owns one reusable 2048x2048 D32 texture, an exact sampled view, an
exact depth-attachment view, and a linear clamp-to-border opaque-white
`LessOrEqual` comparison sampler. The complete depth/comparison pair is created
on first scene-view demand and remains the legal fully-lit binding when a
particular view has shadows disabled. Logical storage is 16,777,216 bytes.
Backend allocation bytes are reported independently. The target is cleared and
regenerated before Scene Color for every enabled main, auxiliary, preview,
present, or offscreen view; its dimensions do not follow viewport resize.

## Fitting and caster preparation

The fitted `FSceneView` supplies eight zero-to-one Vulkan clip corners. Receiver
depth is limited to the nearer camera far extent or 256 world units. The
normalized light direction is the light-space forward axis; world +Z is the
preferred up reference and world +Y is the parallel fallback. Receiver XY is
expanded by the selected filter's guard and its center is snapped to whole
shadow texels. Low and Medium use two guard texels; High uses three. The
expansion is tier-local, so preparing High cannot alter a later Low or Medium
view.
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
rasterization even for Wireframe camera views. For shadow texel world size
`t = max(texel.x, texel.y)`, raster constant/slope/clamp bias is
`clamp(1+2t,1,1.5)`, `clamp(1.25+t,1.25,2)`, and
`clamp(2+8t,2,4)` respectively. Non-finite input retains the bounded
1.25/1.75/4.0 fallback. Vulkan enables `depthBiasClamp` when the physical
device exposes it. Opaque casters use a no-output depth fragment entry point,
while Masked casters use a depth-only fragment entry point that performs
opacity-mask rejection without declaring a color output. All shaders, PSOs,
samplers, material resources,
geometry bindings, height resources, and palette ranges are prepared before
the pass begins.

## Filter tiers and forward sampling

`FSceneViewSettings` selects one immutable deterministic filter tier. Medium is
the production default. Low is numeric zero and remains the invalid-identity,
resource-failure, and optional-tier fallback.

- Low performs one linearly filtered comparison at offset `(0,0)`, has an
  effective radius of 0.5 texels, and uses a two-texel guard.
- Medium performs the Cartesian 3x3 offsets `{-1,0,1}` with per-axis tent
  weights `[1,2,1]`, nine comparison operations, row-major accumulation, exact
  `1/16` normalization, a 1.5-texel radius, and a two-texel guard.
- High performs the Cartesian 5x5 offsets `{-2,-1,0,1,2}` with per-axis weights
  `[1,2,3,2,1]`, 25 comparison operations, row-major accumulation, exact
  `1/81` normalization, a 2.5-texel radius, and a three-texel guard. High is
  available as a bounded tier but is not the default because it fails the
  frozen motion gate.

Offsets derive at runtime from `Texture2D.GetDimensions`; viewport size,
nominal resolution constants, and world scale do not determine the sampling
step. Every hardware-linear tap is accepted only when its complete half-texel
footprint is valid. Invalid tap weight and an invalid receiver projection
contribute fully lit output rather than clamping inward.

The reflected forward-lighting ABI is 464 bytes. Its aligned 144-byte shadow
block contains the world-to-shadow matrix followed by control, texel/bias,
raster-bias, light/bounds, and filter `float4` values at offsets 64, 80, 96,
112, and 128.
C++ size/offset assertions and Slang compilation/reflection tests own this
packing.

One shared Slang helper consumes production world position and the final
mapped production normal. With normalized surface-to-light direction `l` and
`g = 1-saturate(abs(dot(n,l)))`, it computes receiver world bias
`R=clamp(t*(0.05+0.10g),0.0005,0.02)` and normal offset
`N=clamp(t*(0.20+0.55g),0,0.10)`. `R+N` is limited to
`min(0.75t,0.10)`, reducing `N` first. The helper transforms `p+lR+nN` through
the selected world-to-shadow matrix, preserving forward-depth `LessOrEqual`.
Non-finite bias input uses the old normalized-depth `0.0005` comparison with
no normal offset; invalid or outside projection remains fully lit. Only the
selected directional direct-light term is attenuated. Local lights,
environment/ambient, emissive, rim assistance, and Unlit output are unchanged.

## Causal diagnostics

`EDirectionalShadowDiagnosticMode` is copied from `FSceneViewSettings` into the
immutable prepared shadow value. `Lit` is zero and follows ordinary production
output. Development modes visualize stored depth/coverage, unbiased receiver
comparison, receiver-bias comparison, final normal-offset comparison, texel
grid and guard, normalized bias contributions, or final classification.
Missing coverage, outside/invalid coordinates, failed comparison, recovered
comparison, excessive clamped displacement, lit, and shadowed results use
distinct frozen colors recorded by the Q0 fixture package.

Diagnostics execute only in the existing Scene Color fragment path and add no
global mutable camera/light state, render target, pass, descriptor, or device
wait. Disabled/failed shadows and Unlit views ignore diagnostic requests and
match their Lit references exactly. Per-view counters report selected mode and
prepared bias fallback/clamp state; exact diagnostic image statistics own
fragment-level comparison and classification evidence without GPU atomics.

Q1 adds three filter modes. `FilterFootprint` encodes bounded tier identity,
effective radius, and normalized valid comparison weight. `FilterTapValidity`
encodes invalid weight in red and valid weight in green. `FilterDifference`
compares the selected candidate against exact Low in the same fragment: red is
darker, blue is lighter, and green is unchanged. Only the difference diagnostic
performs the additional Low comparison; diagnostic-disabled production uses
only the selected tier's nominal 1, 9, or 25 comparisons.

Target/view/sampler or draw-resource failure disables the feature for that view
and continues Scene Color unshadowed. Renderer release, device invalidation,
manual retry, and shader reload use the existing resource coordinator; the
feature introduces no `WaitIdle` or whole-device flush. Counters report light
and receiver selection, caster outcomes by family, resource outcomes, draws,
logical/backend target bytes, and preparation failure. Optional GPU timing
sinks expose Shadow Depth and Scene Color independently.

## Q0 qualification

The checked-in `DirectionalShadowQ0` package contains the 13-image fixed-bias
entry baseline, 13 selected-policy Lit images, seven causal diagnostic images,
and exact disabled/Unlit diagnostic references. On the RTX 3090, driver 591.86,
Vulkan 1.4.325 fixture, Masked and Opaque controls are byte-identical, valid
and intentionally defective modular boundaries remain distinct, and the
0.12-world-unit contact case is no longer identical to the fully lit fallback.
Tolerance-2 sub-texel motion changes 22, 18, and 58 of 66,049 pixels against a
132-pixel limit.

The 1920x1080 timing fixture uses 30 warm-up and 120 measured frames. The Lit
shadow tier records 7,936 ns disabled Scene Color, 10,464 ns enabled Scene
Color, 9,248 ns Shadow Depth, and an 11,776 ns combined median increment.
Classification records 10,528 ns Scene Color, a 64 ns median increment over
Lit. Logical and backend shadow storage both remain 16,777,216 bytes.

## Q1 qualification

The checked-in `DirectionalShadowQ1` package preserves all Low hashes and adds
complete Medium/High edge, diagonal, thin-caster, Masked, guard, camera/light
motion, Q0 correctness-parity, and filter-diagnostic evidence. The shadow-only
radial high-frequency fractions are 0.018361, 0.015372, and 0.013730 for Low,
Medium, and High. Medium/Low is 0.837 against a maximum 0.85; High/Medium is
0.893 against a maximum 0.90. Maximum transition width remains 40 pixels for
all tiers.

At channel tolerance two, the dedicated Q1 camera/light motion comparisons are
76/125 pixels for Low, 51/76 for Medium, and 487/563 for High against a
132-pixel limit. Medium also preserves the Q0 planar-acne output exactly,
keeps Masked/Opaque controls equal, retains distinct valid/defective geometry,
and matches the disabled reference. High is therefore rejected by motion even
though its edge and performance evidence pass.

On the RTX 3090, driver 591.86, Vulkan 1.4.325, 1920x1080 timing fixture with
30 warm-up and 120 measured frames, Low/Medium/High Scene Color medians are
11,872/12,640/13,504 ns and Shadow Depth medians are
9,600/10,240/10,816 ns. Medium adds 768 ns Scene Color over Low against a
200,000 ns budget; High adds 1,632 ns against 400,000 ns. Shadow Depth
regressions are 640 and 1,216 ns against 20,000 ns. The Medium filter-difference
diagnostic adds 1,568 ns over Medium Lit. Logical/backend bytes remain exactly
16,777,216 and failed frames remain zero. These results select Medium as the
production default while retaining Low fallback and bounded High availability.
