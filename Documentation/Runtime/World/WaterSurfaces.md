# Finite Water Surfaces

Summary: The opt-in Water plugin renders finite Gerstner surfaces through ordinary StaticMesh and material facilities and exposes matching CPU queries.
Modules: Water, Engine

Last reviewed: 2026-11-13

## Enablement and authoring

Enable `"Plugins": {"Water": true}` in the project's `.dproject`, build its
`<Project>PluginBuild` inventory, and restart through the ordinary
[plugin workflow](../../Workspace/Plugins.md). Unselected Water does not compile
or admit the module; ordinary plugin dependencies can also enable it. The first
qualified platform is Win64; the descriptor excludes other platforms.

`AWaterSurfaceActor` owns a `DWaterSurfaceComponent` root. Generic reflected
editing exposes its `Settings` and optional `Material`. Full XY dimensions,
local elevation, amplitude, base wavelength, speed and steepness persist with
the Level. No Water editor module is required. Positive uniform scale,
translation and rotation are supported; use Extent for independent dimensions.
Nonuniform, zero, negative or mirrored scale produces a local diagnostic and
no render proxy or component query surface.

The component generates one regular grid using ordinary StaticMesh resources.
UV0 preserves undisplaced local XY. Both section and primitive bounds include
the complete horizontal and vertical displacement envelope plus floating-point
padding. Old proxies retain their own geometry until scene removal; resource
initialization and retirement follow render-command order. Query-only worlds
need no RHI resources. Water creates no collision or physics body.

## Wave and query contract

For undisplaced local coordinates p, each wave has a unit horizontal direction
D, amplitude A, wavenumber k, angular frequency omega and horizontal ratio Q:

```text
theta = k * dot(D, p) - omega * t
P.xy = p + sum(Q * A * D * cos(theta))
P.z  = elevation + sum(A * sin(theta))
N    = normalize(cross(dP/dp.x, dP/dp.y))
```

The three directions are (1,0), (0.6,0.8), (-0.8,0.6). Amplitude ratios are
0.6/0.3/0.1 and wavenumber ratios are 1/1.37/0.73. The base wavenumber is
2*pi/wavelength, omega is speed*sqrt(9.81*k), and Q is Steepness. Analytic
derivatives produce the visible and queried normal. Edges move freely.

| Setting | Default | Admission range |
| --- | --- | --- |
| Extent, full local XY dimensions | 32, 32 | 1..128 on each axis |
| Elevation, local Z | 0 | -10000..10000 |
| Amplitude | 0.15 | 0..2 |
| Wavelength | 8 | 2..512 |
| Speed | 1 | 0..4 |
| Steepness | 0.3 | 0..1 |

All inputs must be finite. Additionally, sum(abs(Q*A*k)) must not exceed 0.5.
Grid spacing is at most base wavelength*0.73/32 (approximately the shortest
wavelength/32), and each axis has at most
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

The default appearance is two-sided opaque Lit water with analytic normals and
WPO. Its source is the ordinary typed-expression material asset
`/Plugins/Water/Materials/DefaultWater.DefaultWater`. The plugin content mount
contains authored data only; no compiled Water binary is checked in.

The component serializes a soft reference to this default asset. Ordinary Cook
reachability retains it with a saved water Level. Projects that create water only
at runtime must retain the material through an explicit Cook root or their
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

An assigned material must implement the same wave convention. Primitive custom
data slots 0..3 are reserved for amplitude, wavelength, speed and steepness.
Read local p from UV0; transform the computed local WPO direction to World.
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

Stage, packaged Game startup and packaged asset/Shader qualification remain
deferred, as do refraction, underwater rendering, buoyancy, spline rivers,
terrain modification and infinite ocean meshing. The plugin is not qualified
for release deployment.
