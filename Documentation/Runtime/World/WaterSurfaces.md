# Finite Water Surfaces

Summary: The opt-in Water plugin renders finite Gerstner surfaces through ordinary StaticMesh and material facilities and exposes matching CPU queries.
Modules: Water, WaterEditor, Engine

Last reviewed: 2026-11-23

## Enablement and authoring

Enable `"Plugins": {"Water": true}` in the project's `.dproject`, build its
`<Project>PluginBuild` inventory, and restart through the ordinary
[plugin workflow](../../Workspace/Plugins.md). Unselected Water does not compile
or admit the module; ordinary plugin dependencies can also enable it. The first
qualified platform is Win64; the descriptor excludes other platforms.

`AWaterSurfaceActor` owns a `DWaterSurfaceComponent` root. Generic reflected
editing exposes the surface dimensions, local elevation, speed, steepness,
Waves Source and optional material. Each Level saves the component settings and
wave asset reference. Positive uniform scale, translation and rotation are
supported; use Extent for independent dimensions.
Nonuniform, zero, negative or mirrored scale produces a local diagnostic and
no render proxy or component query surface.

The component generates one regular grid using ordinary StaticMesh resources.
UV0 preserves undisplaced local XY. Both section and primitive bounds include
the complete horizontal and vertical displacement envelope plus floating-point
padding. Old proxies retain their own geometry until scene removal; resource
initialization and retirement follow render-command order. Query-only worlds
need no RHI resources. Water creates no collision or physics body.

## Reusable Water Waves assets

In the Content Browser, choose **Create → Water Waves**. Open the asset
to edit the simple Gerstner generator. Defaults follow UE 5.8: 16 waves,
seed 0, wavelength 5.21..60 m, amplitude 0.04..0.8 m, quadratic wavelength
and amplitude falloffs, randomness 0, dominant wind angle 0 degrees and angular
spread 1325 degrees. Wave zero follows the wind. Later directions use the
seeded angular spread. At index i the falloffs use alpha = 1 - i/NumWaves,
plus optional seeded jitter up to Randomness/NumWaves, clamped to 0..1.

Durin accepts 0..16 waves, wavelengths inside 2..512 m with a maximum at
most 32 times the minimum, and per-wave amplitude inside 0..2 m. A group with
no waves or zero amplitude is flat. Horizontal displacement uses the component
steepness. The inverse query and grid limits below apply to the result.

New components reference the plugin preset **/Plugins/Water/Waves/DefaultWaterWaves**.
Select a different asset in **Waves Source** to change the spectrum. Clearing
the field gives a flat surface at the component elevation with zero Gerstner
displacement and a local up normal. An assigned asset that fails to load
produces a diagnostic. Multiple components can share a wave asset.

Asset edits and transaction replay refresh every registered component that
references the asset. Each component publishes its effective wave parameters
with its replacement geometry and uses that same snapshot for CPU queries.
If a shared asset change exceeds a particular surface's mesh or steepness
limits, that surface reports a local diagnostic and becomes unavailable for
rendering and querying until the settings are compatible. Other referencing
surfaces can continue to use the asset. The saved references retain the asset
through ordinary Cook reachability.

## Wave and query contract

For undisplaced local coordinates p, each wave has a unit horizontal direction
D, amplitude A, wavenumber k, angular frequency omega and horizontal ratio Q:

```text
theta = k * dot(D, p) - omega * t
P.xy = p + sum(Q * A * D * cos(theta))
P.z  = elevation + sum(A * sin(theta))
N    = normalize(cross(dP/dp.x, dP/dp.y))
```

An empty group gives a flat surface. Each direction is normalized for
evaluation, A is the surface amplitude times the wave amplitude ratio, and
k is the wave wavenumber ratio times 2*pi/wavelength. Omega is
speed*sqrt(9.81*k), and Q is Steepness. Analytic derivatives produce the visible
and queried normal. Edges move freely.

| Setting | Default | Admission range |
| --- | --- | --- |
| Extent, full local XY dimensions | 32, 32 | 1..128 on each axis |
| Elevation, local Z | 0 | -10000..10000 |
| Amplitude (sum of generated amplitudes) | 5.08125 | 0..32 |
| Wavelength (base for wave ratios) | 8 | 2..512 |
| Speed | 1 | 0..4 |
| Steepness | 0.3 | 0..1 |

All inputs must be finite. Directions must be nonzero, amplitude ratios must
be in 0..1 and sum to at most 1 (within Float32 rounding tolerance), and
wavenumber ratios must be in 0.125..4. The sum of abs(Q*A*k) must not exceed 0.5.
Grid spacing is approximately the shortest nonzero-amplitude wavelength/32,
with one cell per axis when all weights are zero. Each axis has at most
256 cells (at most 66049 vertices). Extent/wavelength combinations exceeding
that limit are rejected before allocation. Setters, reflected edit proposals
and loaded object graphs enforce the same settings contract.

`EvaluateWaterSurface` evaluates validated parameters at undisplaced XY.
`QueryWaterSurface` inverts the displaced XY map by bounded contraction, then
checks the recovered coordinates against the finite rectangle. It returns no
surface outside the deformed footprint or for invalid settings/coordinates/time.
`DWaterSurfaceComponent::QuerySurface` projects a world point along component
local Z and returns world position and normal. This is a vertical height query
for an upright body. For a rotated body it follows that body's local up axis.

Supply renderer material seconds explicitly. `GetWaterMaterialTime` returns
`FTime::Seconds() - GStartTime`, the renderer's default process clock, including
Editor and paused frames. Simulation/World time is not interchangeable. Frame
tests use explicit `MaterialTimeSeconds`. Component access follows owner-thread
object rules; workers can query an independently owned settings value through
the pure functions. Parameter updates publish custom data and replacement
geometry together, and queries read the current settings rather than a cached
snapshot.

The qualification fixture checks representative Float32 material-recipe/CPU
samples through one hour with tolerances of 5 mm in local position and 0.02 in
normal-vector distance. CPU height against sampled piecewise linear grid
triangles has a 2 cm local tessellation tolerance. Uniform world
scale multiplies position tolerances. Longer uptime remains supported but is
outside this Float32 precision qualification.

## Material resources

The default appearance uses two-sided Single Layer Water with analytic normals
and WPO. Its source is the ordinary typed-expression material asset
`/Plugins/Water/Materials/DefaultWater.DefaultWater`. The plugin content mount
contains authored data only; no compiled Water binary is checked in.

The material uses Opaque routing, Opacity 0 and refraction derived from
dielectric F0. RGB scattering is (0.00035, 0.00065, 0.0009) per cm and
absorption is (0.004, 0.0012, 0.00065) per cm. PhaseG is 0.2 and the
behind-water color scale is white. See the [material contract](../Rendering/Materials/MaterialSystem.md#single-layer-water).

The component serializes a soft reference to this default asset. Ordinary Cook
reachability retains it with a saved water Level. Projects that create water only
at runtime must retain the material and any wave preset through explicit Cook roots or their
existing asset policy. Reflected default construction does not load content.
Rendering retains the loaded material through a transient reflected reference.

Editor loads and compiles the authored graph through the normal material services.
Game loads the same logical object path from its configured Cook generation;
the ordinary material Cook hooks prepare target variants and payloads. Cook
publishes package data and `Shaders/ShaderLibrary.dslb` under the selected output
directory. No Water-specific build target, cross-preset compiler, absolute build
path or runtime shader compilation is required. See [asset cooking](../Assets/Build/Cooking.md)
and the [validation fixture](../../../Engine/Tests/Fixtures/WaterSurface/README.md).

This follows UE's asset boundary: [Water Body Component](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Plugins/Water/UWaterBodyComponent)
references material assets, and [material Cook preparation](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/Materials/UMaterial/BeginCacheForCookedPlatformData?application_version=5.5)
prepares platform data before serialization. This is an architectural comparison
against public Epic documentation, not a claim of source equivalence.

The Water plugin's `GerstnerWaves` material function owns GPU wave evaluation.
Its two outputs are world-space displacement and a tangent-space analytic normal.
DefaultWater connects these to World Position Offset and Normal. Other water
appearances can call the same function to match component queries.

The Water expression reads the component's per-draw coefficients.
Primitive custom data slots 0..3 hold amplitude, wavelength, speed and
steepness. Slots 4..31 hold seven records of normalized direction X and Y,
amplitude ratio and wavenumber ratio. The default instance payload holds the
remaining nine records as direction angle in radians, amplitude ratio and
wavenumber ratio. Inactive records have zero amplitude. The scene proxy
publishes both sets with its grid and uses the ordinary retained mesh path.
Read local p from UV0 and transform the computed local WPO direction to World.
The flat mesh tangent basis permits the analytic local normal to feed the
Tangent/Normal terminal through a spatially classified identity swizzle. See
[material vertex evaluation](../Rendering/Materials/MaterialVertexEvaluation.md).
Arbitrary replacement materials are responsible for preserving this contract.
Missing or incompatible resources produce a surface-local diagnostic.

## Qualification boundary

Focused Vulkan scenes cover the independent rendering path and the actual
component in Editor and development Game. Native probes cover opt-in startup,
disabled admission, CPU/material agreement and Level save/reopen, including
Game loading of the saved water Level. The component Game scene consumes the
ordinary Cook output. The independent proof retains isolated test material
programs; with Water enabled, both Game scenes use the normal Cook Shader
library. Without Water, the independent proof uses its own prepared library.
The packaged Win64 Debug fixture also starts the cooked water Level through
the normal Game export verification path.

The supported rendering scope is above-water Win64 Vulkan deferred rendering.
Underwater rendering, buoyancy, spline rivers, terrain modification, infinite
ocean meshing and release deployment remain outside this qualification.
