# UE Sky Rendering Alignment Plan

Summary: Move visible SkyBox and Procedural Sky to material-backed backgrounds while preserving scene-owned Sky Light capture and existing levels.

Last reviewed: 2026-11-25
Status: Active
Completed:

## Current Status

Stage 0 selected a fullscreen material draw in the existing scene-color passes.
The legacy renderer is still active. The inward-facing Unlit sphere prototype
produces the expected HDR emissive color from inside the sphere, but requires a
mesh proxy, finite geometry and ordinary mesh depth. The background draw will
take its sky role from the selected provider, reconstruct each view's direction
using the double-precision camera-relative inverse transform and keep the
existing far-depth convention (0 reversed, 1 forward). Keep the hybrid bootstrap
depth-equal test with depth writes off, the forward background order, and the
current position upstream of water and cloud composition. Each submitted view,
including previews and auxiliary views, supplies its own projection and override.

Stage 1 has an authored `/Engine/Materials/DefaultCubeSky` asset. Its Unlit
emissive graph samples in the world-position-minus-camera-position direction
and multiplies linear radiance by `SkyTint` and `SkyIntensity`. It is an
Engine built-in Cook root. The Win64 Debug Sandbox Game receipt, 10-package
Cook and source-independent stage export passed. The Vulkan dome test loads
the saved asset, binds the HDR texture and compares its center pixel to the
legacy draw. This test removes the scene sky and supplies matching view,
projection and combined matrices so the background cannot satisfy the check.
The original Stage 0 mesh-interior image included the scene background and
is superseded by this isolated check. The fullscreen cubemap now evaluates the material in both scene-color passes.
The Win64 Debug Vulkan fixture passed the six-face, HDR, override, transparent
clear and occlusion probes, including an authored material instance with its own
texture and intensity. All 16 SkyBox feature tests passed, including level
reload and transaction replay. Cooked runtime rendering remains open.

**Consumer inventory.** LevelEditor placement creates or transactionally
replaces a visible SkyBox and rejects ambiguous placement. The scene accepts
one cubemap proxy. A hidden actor publishes none. View preparation first
resolves an explicit cube override, returning `RequiredEnvironmentUnavailable`
for a missing or non-cube target. Otherwise the highest-priority eligible
procedural provider wins, with ascending persistent ID and selection key
breaking ties. The scene cubemap comes last and uses the renderer's default cube
while its texture is unavailable. An empty scene preserves the view clear color.
TextureEditor passes a retained TextureCube snapshot through thumbnail draw and
capture. The studio preview's Specified Cube Sky Light is independent. Captured
Sky reads the selected analytic proxy and Specified Cube uses the light's own
texture. Existing Cook inputs are the reflected level/component references and
texture platform data. Sandbox and RoadWeaver have no direct source-level
consumers of the four inventoried types.

**Reference evidence.** At runtime revision `512b31950` the expanded
`SkyBoxVulkanIntegrationTests` passed twice on Win64 Debug DurinEditor,
GTX 1060 6 GB, Vulkan 1.4.312, in the user-confirmed quiet window.
Set `DURIN_TEST_KEEP_WORK=1` to retain the test's `SkyParity` directory.
The two runs produced 20 byte-identical image buffers each, including six HDR
faces with tint (0.5, 0.25, 1) and intensity 2, both projection forms,
billion-unit translation, transparent clear, view override, procedural horizon,
sun and positive-Z capture. Compare the 17×17 sRGB face centers within 12 byte
values per channel and procedural horizon/sun within 4. Transparent pixels
and the override comparison require exact agreement. The HDR capture uses
the existing half-float radiance, irradiance and mip assertions.
`WaterVolumeMatchesThreeLinearHDRBedProbes` passed and retained refraction
on/off buffers at 320×240 and 640×480 under `Build/SingleLayerWater/Stage3`.

Raw samples and images from both sky runs are under
`Build/UESkyRenderingAlignment/512b31950/{29984,16512}/SkyParity`.
At 17×17, four warm-up pairs and 32 alternating measured pairs gave incremental
cube draw median/p95 values of 6.016/6.848 and 6.400/6.944 microseconds.
Two warm-up captures and 16 measured updates per run gave median/p95 values of
0.705/1.261 and 0.709/2.175 milliseconds. Use the same fixture size and paired
subtraction for comparisons. These figures characterize this GPU and workload.

## Goal

Use ordinary `Surface` / `Opaque` / `Unlit` materials to author visible skies.
Keep sky placement, visibility and draw order in the renderer. Preserve the
current analytic Procedural Sky model and share its evaluator between the default
visible material and Sky Light capture. Keep visible background selection and
environment lighting as separate scene responsibilities.

UE 5.8 provides two references. Its [skydome material path](https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine#rendering-the-sky-using-a-skydome-mesh)
uses an ordinary Unlit material with explicit sky treatment. Its atmosphere
renderer owns atmospheric evaluation and exposes inputs to sky materials. Durin
will adopt that ownership split with its current analytic sky. Physical atmosphere
LUTs, aerial perspective and UE-style general scene capture are outside this plan.

## Implementation Stages

### Stage 0: Record existing behavior and choose sky draw routing

- [x] Inventory the Engine, Sandbox and RoadWeaver consumers of `DSkyBoxComponent`,
  `DProceduralSkyComponent`, `FSkyBoxRenderer` and `FViewEnvironmentOverride`.
  Include level placement, editor previews, TextureCube thumbnails, Cook and
  Sky Light capture. Record source priority and missing-resource behavior for
  views with multiple sky providers and an explicit environment override.
- [x] Record reference captures for all six cube faces, HDR intensity and tint,
  procedural horizon and sun direction, no sky, view override, water refraction
  and Sky Light capture. Include camera rotation, large translation, perspective
  and orthographic views, and transparent offscreen output. Record an incremental
  GPU cost baseline for the existing draw and capture paths.
- [x] Prototype an inward-facing sky mesh with existing Unlit material features.
  Decide between a mesh with sky-specific depth and view state and a background
  draw that evaluates a material over a fullscreen primitive. Use one explicit
  sky role in the scene. Start with role assignment on the sky provider and
  evaluate an `Is Sky` material flag only if ordinary meshes need to opt in
  independently. Record how the chosen route preserves far depth, camera-relative
  sampling, stereo or auxiliary view isolation where supported, and existing
  water and cloud composition. Completion requires a recorded choice and
  reproducible parity fixtures for both sky sources.

### Stage 1: Render cubemap SkyBox with an Unlit material

- [x] Ship a default HDR cubemap sky material with direction sampling, rotation,
  tint and intensity. Route it through the chosen sky draw using the material
  compiler, resource binding and pass preparation. Keep scene or view ownership
  of the retained inputs and reuse the existing view-environment override
  behavior while its callers migrate. Keep opaque geometry, depth and GBuffer
  writes independent of the sky background draw.
- [ ] Preserve existing `SkyBox` actor and component data. Resolve the stored
  texture, tint, intensity and transform into the default material at load and
  edit time. Provide an explicit authored material override and document how it
  interacts with legacy settings. Confirm save/reopen, undo/redo, texture
  replacement, shader readiness and source-free Cook on representative assets.
- [x] Verify stage 0 parity for cubemap scenes, missing textures, preview
  overrides and HDR output. Keep the old background route available for
  Procedural Sky until Stage 2 passes.

### Stage 2: Render Procedural Sky through the material path

- [ ] Keep `DProceduralSkyComponent` as the provider for the analytic parameters
  and publish them through the existing retained scene snapshot. Bind that
  selected snapshot and the sky direction to the default Unlit sky material.
  Expose the shared `EvaluateProceduralSky` calculation through a sky-specific
  material expression or intrinsic with defined behavior when the provider is
  absent. Use the same [radiance evaluator](../../Engine/Shaders/Slang/ProceduralSky.slang)
  in the visible draw and the [Sky Light capture](../Runtime/Rendering/Features/SkyLighting.md).
- [ ] Keep `Captured Sky` scoped to the analytic provider and `Specified Cube`
  scoped to its explicit asset. Preserve the existing capture scheduling and
  publication lifetimes. Define whether any authored visual customization also
  affects capture before exposing it. Verify capture and visible output against
  their existing radiance contract, including any intentionally excluded terms.
- [ ] Verify old level settings, source priority, parameter edits, disabled or
  removed providers, capture refresh and source-free Cook. Completion requires
  the same reference directions and transforms to agree with the baseline
  radiance in both paths within the tolerance set in Stage 0.

### Stage 3: Migrate consumers and retire the legacy visible sky shader

- [ ] Migrate all registered view and content consumers. Check preview scenes,
  TextureCube thumbnails and the thumbnail cache contract. Verify that ordinary
  material thumbnails keep their existing output. Include a `DefaultWater`
  capture over a known opaque background and inspect rendered pixels to verify
  that the sphere differs from the background and stays visible. Track the
  choice of default water thumbnail background independently of the sky migration.
- [ ] Verify sky color reaches the water snapshot with the expected far depth,
  opaque geometry occludes the background, clouds composite in the existing
  order and transparent offscreen views remain transparent when no background
  is requested. Verify cubemap and procedural lighting with their current
  Sky Light source modes.
- [ ] Remove `FSkyBoxRenderer` and the old visible `SkyBox.slang` shader entry
  when all consumers use the material-backed route. Keep reusable procedural
  evaluation and capture filtering. Update the owning rendering, material and
  editor contracts after behavior is established.
- [ ] Run bounded Engine, Sandbox and RoadWeaver consumer builds where shared
  interfaces change. Use the SkyBox feature and Vulkan integration targets,
  affected material and shader tests, water rendering, thumbnail and cooked
  coverage. Verify the changed sky and capture paths on supported Vulkan and
  Metal configurations. Follow the [build](../Agents/BuildAndRun.md) and
  [test](../Agents/Testing.md) workflows. Record tested revisions, configurations,
  image comparisons, GPU cost and any outstanding coverage in Current Status.
