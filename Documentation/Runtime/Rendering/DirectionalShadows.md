# Directional Shadows

Summary: Defines the selected three-cascade directional-light shadow path, deterministic PCF tiers, diagnostics, failure fallback, and qualification evidence.

Modules: RenderCore, Renderer, Engine, VulkanRHI

Last reviewed: 2026-08-16

## Ownership and selection

`FDirectionalShadowRenderer` is a private `FSceneRenderer` feature owner. The
first directional light selected by `FPreparedLightView` may own one shadow for
the current view when its detached `bCastShadows` value is enabled. The light
component, proxy, SceneInfo, and prepared-light path copy this value; render
code never reads a component. Missing, invalid, disabled, or failed shadow
state leaves the existing directional light fully unshadowed.

The selected representation is one reusable three-layer 2048x2048 D32
`Texture2DArray`, one sampled array view, three exact single-layer 2D depth
views, and one linear clamp-to-border opaque-white `LessOrEqual` comparison
sampler. Creation publishes only a complete target/view/sampler aggregate.
Logical and measured RTX 3090 backend storage are both 50,331,648 bytes. Each
enabled view regenerates all of its prepared layers before Scene Color; target
dimensions do not follow viewport resize. `SingleMap` remains a bounded
per-view comparison candidate and uses layer zero of the same complete array.

## Fitting and caster preparation

The fitted `FSceneView` supplies eight zero-to-one Vulkan clip corners. Receiver
depth is limited to the nearer camera far extent or 256 world units. Perspective
views use three practical split intervals with lambda 0.65; orthographic views
use three uniform intervals. Invalid, non-finite, inverted, or zero-width
intervals disable the shadow for that view. The
normalized light direction is the light-space forward axis; world +Z is the
preferred up reference and world +Y is the parallel fallback. Receiver XY is
expanded by the selected filter's guard and its center is snapped to whole
shadow texels. Low and Medium use two guard texels; High uses three. The
expansion is cascade- and tier-local, so one cascade or quality tier cannot
move another. Each slice independently fits and snaps its center to its own
whole-texel grid.
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

Three ordered depth-only passes have no color attachment. Each binds and clears
only its exact layer view to 1.0, writes with `Less`, and leaves the array ready
for `GraphicsShaderRead` before Scene Color. They force filled
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

Offsets derive at runtime from `Texture2DArray.GetDimensions`; viewport size,
nominal resolution constants, and world scale do not determine the sampling
step. Every hardware-linear tap is accepted only when its complete half-texel
footprint is valid. Invalid tap weight and an invalid receiver projection
contribute fully lit output rather than clamping inward.

The reflected forward-lighting ABI is 768 bytes. Its aligned 448-byte shadow
block starts at offset 64, carries control, view-depth transform, four split
boundaries, light/transition data, and three 128-byte cascade records. Each
cascade record contains one matrix plus texel/bias, raster-bias, filter, and
valid-region vectors. Local lights begin at offset 512.
C++ size/offset assertions and Slang compilation/reflection tests own this
packing.

One shared Slang helper consumes production world position and the final
mapped production normal. With normalized surface-to-light direction `l` and
`g = 1-saturate(abs(dot(n,l)))`, it computes receiver world bias
`R=clamp(t*(0.05+0.10g),0.0005,0.02)` and normal offset
`N=clamp(t*(0.05+0.70g),0,0.06)`. `R+N` is limited to
`min(0.75t,0.08)`, reducing `N` first. The helper transforms `p+lR+nN` through
the selected world-to-shadow matrix, preserving forward-depth `LessOrEqual`.
Non-finite bias input uses the old normalized-depth `0.0005` comparison with
no normal offset; invalid or outside projection remains fully lit. Only the
selected directional direct-light term is attenuated. Local lights,
environment/ambient, emissive, rim assistance, and Unlit output are unchanged.

## Screen-space contact supplement

The optional contact-shadow pass is a deferred visibility producer for valid
standard-Lit opaque and masked GBuffer receivers. It runs after the GBuffer is
complete and before deferred lighting, writes one on-demand `R8_UNORM` scalar
target, and never reads or writes Scene Color. Deferred lighting multiplies
only the selected directional term after cascade attenuation by this factor;
local lights, environment lighting, emissive output, retained-forward Unlit
and translucent surfaces, and already-shadowed contributions are unchanged.

The pass is an opt-in detail trace rather than a default shadow tier. It
decodes GBuffer material flags and geometric normals, reconstructs receiver
positions from D32 scene depth, and marches exactly 16 bounded line segments
toward the selected light. Each sampled depth texel represents a finite,
oriented surface element derived from its geometric normal and projected pixel
footprint. A hit requires the current ray segment to cross that surface plane
inside its footprint; spatial proximity to a depth point alone is not a hit.
The trace ends at 0.20 world units and stops at 48 screen pixels. Contribution
fades with world distance and across the final 25% of the screen-distance
budget, so crossing the screen bound does not introduce a full-strength hard
cut. A ray that leaves the viewport terminates without attempting to represent
off-screen geometry.

Only `GBufferStandardLitFlag` pixels participate. Backfacing receivers resolve
fully visible and the unstable grazing interval fades from dot(normal,
toward-light) 0.02 to 0.20. The ray origin moves along the receiver geometric
normal by half its dilated pixel footprint, with a 0.0005 minimum and a bound
of 5% of the trace distance. Parallel near-coplanar surfels are explicitly
classified as the receiver rather than blockers. The 1.5 footprint dilation
covers texel-center quantization without introducing a fixed world-space
thickness. Depth and normals are fetched with exact texel loads. Linear depth
filtering and one-sided unbounded device-depth tests are forbidden because
they create intermediate silhouette depths and detached false occlusion. The
pass does not reconstruct a receiver plane from neighboring depth, adaptively
increase its step count,
contract its world extent from endpoint projection, apply a viewport-edge mask,
or refine hits with extra binary searches. This keeps the worst-case depth
query budget explicit and avoids view-dependent classification heuristics.

Contact shadows default off in `FSceneViewSettings` and are enabled explicitly
by the viewport control or a caller-owned view setting. Missing targets,
invalid light or matrix input, or resource creation failure binds the complete
white fallback and continues the same deferred path. A view with no successful
deferred receiver draw records no target or pass. The viewport View menu exposes a
`Contact Shadows` checkbox. Its mutually exclusive `Shadow Debug Views >
Contact Shadow Contribution` mode enables the pass and displays the computed
contribution as a red mask; selecting another diagnostic clears that mode.
Per-view counters distinguish enabled passes from pass failures. The method
remains screen-space: off-screen casters are not
represented and contact shadows do not replace cascade coverage.

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

Q2 adds `CascadeIndex`, `CascadeTransition`, `CascadeCoverage`, and
`CascadeDifference`. Index uses fixed red/green/blue layer colors; transition
is a grayscale adjacent-layer weight; coverage reuses the selected layers'
coverage evidence; difference shows the signed attenuation difference between
neighboring cascade results. Candidate-versus-single comparisons remain an
offline qualification operation, avoiding a second production target solely
for a live diagnostic.

Receiver selection uses positive view-space depth. Outside a transition it
samples one layer; in the final 10% before an interior split it samples exactly
the two neighboring layers and blends their independently validated results.
Medium therefore has nine nominal comparisons outside overlap and 18 inside.
Invalid projections and taps contribute fully lit and never clamp into another
layer.

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

After rebasing the per-draw dynamic raster-bias fix, the frozen channel-
tolerance-two camera/light motion values are 71/117 pixels for Low, 48/88 for
Medium, and 32/49 for High against a 132-pixel limit. Medium remains the Q1
policy selected before Q2; the prerequisite fix did not reopen filter policy.
Medium also preserves the Q0 planar-acne output exactly,
keeps Masked/Opaque controls equal, retains distinct valid/defective geometry,
and matches the disabled reference. The original Q1 evidence rejected High by
motion; the later prerequisite fix improves those captures but does not reopen
the already selected Medium policy inside Q2.

On the RTX 3090, driver 591.86, Vulkan 1.4.325, 1920x1080 timing fixture with
30 warm-up and 120 measured frames, Low/Medium/High Scene Color medians are
11,872/12,640/13,504 ns and Shadow Depth medians are
9,600/10,240/10,816 ns. Medium adds 768 ns Scene Color over Low against a
200,000 ns budget; High adds 1,632 ns against 400,000 ns. Shadow Depth
regressions are 640 and 1,216 ns against 20,000 ns. The Medium filter-difference
diagnostic adds 1,568 ns over Medium Lit. Logical/backend bytes remain exactly
16,777,216 and failed frames remain zero. These results select Medium as the
production default while retaining Low fallback and bounded High availability.

## Q2 qualification

The selected production candidate is three independently fitted 2048x2048 D32
cascades, practical perspective splits at lambda 0.65, uniform orthographic
splits, a 256-unit maximum distance, 10% adjacent overlap, and the Medium 3x3
tent filter. CPU goldens own split ordering, clamp, overlap, selection, and
degenerate behavior. RHI and Vulkan tests own array creation, sampled-array and
exact layer views, layer transitions, comparison sampling, descriptor
completeness, injected creation failure, and release/retry behavior. Vulkan
captures exercise the three-pass candidate and cascade-index receiver path.

On the RTX 3090, driver 591.86, Vulkan 1.4.325, the 1920x1080 fixture uses 30
warm-up and 120 measured frames. SingleMap Medium records 19,328 ns combined
Scene Color plus Shadow Depth; ThreeCascades Medium records 31,936 ns, an
increment of 12,608 ns against the 1,000,000 ns gate. Logical/backend bytes are
50,331,648 against the 67,108,864-byte gate and measured-frame failures are
zero. Static/Spline, Skeletal, Terrain, Opaque/Masked, resource reload,
sequential-view, fully lit fallback, and Vulkan validation suites pass. These
results select `ThreeCascades` plus Medium as the production default.
