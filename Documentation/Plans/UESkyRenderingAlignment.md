# UE Sky Rendering Alignment Plan

Summary: Move visible SkyBox and Procedural Sky to material-backed backgrounds while preserving scene-owned Sky Light capture and existing levels.

Last reviewed: 2026-11-26
Status: Active
Completed:

## Current Status

The Win64 Vulkan editor uses reversed Z. The sky mesh pass pins viewport depth
to the clear value. Preserving equal depth bounds restored the GrayboxStage15
sky in the working tree based on `bace3af42`. With its then-current MI_PureSky
material, the center sample changed from RGB (0, 0, 0) to (214, 216, 220),
matching a direct render of the same HDR cubemap and view. The Vulkan sky
suite also exercises reversed-Z lit rendering with its analytical HDR cube.

The Stage 4 component follow-up uses authored mesh geometry for Surface materials
marked `Is Sky`. Cubemap drops create a static mesh with a dynamic material
instance. The scene-wide cube proxy has been removed. GrayboxStage15 uses a
regular static mesh actor with the built-in sphere and DefaultCubeSky. Its
SkyTexture defaults to `/Engine/Renderer/DefaultSkyCube`. The former instance's
only override referenced that same texture. The level reloaded with the parent
material in slot 0 after removal of the instance. A fresh Win64
Vulkan material thumbnail had center RGBA (127, 140, 176, 255) and a transparent
corner at 256 × 256 pixels. Per-view
cubemaps and the procedural environment still use a camera-centered sphere. On
Win64 Vulkan, a marked triangle covered only its projected area and submitted
one draw in both forward and hybrid views. The component-generated cube
material passed the HDR readback on the built-in sphere (working tree based
on `3523c8609`).

Stage 4 replaced the Stage 0 fullscreen geometry with a camera-centered
skydome for environment backgrounds. Each view supplies its own projection
and override. The indexed sphere provides spherical UV0 and interpolated sky
direction. Forward and hybrid render the same dome at far depth before water
and clouds. Sky Light continues to capture the analytic provider independently.

On Win64 Debug Vulkan (GTX 1060, working tree based on `ee86e27fa`), the
authored UV probe distinguished opposite longitudes at equal latitude. All
six cube and HDR face probes passed. Against Stage 3 run 4608, the largest
full-image channel differences were 16/255 for the LDR cube, 1/255 for the HDR
cube display and 1/255 for the procedural sun. The procedural horizon, empty
view and half-float capture matched exactly. The dome draw measured 6.784
microseconds median and 9.120 p95 at 17 x 17 over 32 paired draws. Readbacks
and raw samples are under `Build/UESkyRenderingAlignment/Skydome/26948/SkyParity`.

The same revision passed the Win64 SkyBox component and Vulkan suites,
water composition, material compiler, source-free shader Cook, Vulkan thumbnail
and volumetric cloud scene tests (166 cases in total). The
Debug Sandbox export included both default sky materials and passed packaged
startup verification under `Build/UESkyRenderingAlignment/Skydome/SandboxExport`.
Metal GPU qualification remains open.

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
reload and transaction replay. The source-free Game checks below cover both sky assets.

Stage 2 now loads the authored `/Engine/Materials/DefaultProceduralSky` asset
and evaluates the selected component snapshot in its emissive graph. The Win64
Debug Vulkan integration passed after loading the saved package. Procedural sun
and horizon samples matched the Stage 0 images exactly. Captured Sky retains its
independent analytic capture and excludes direct sun-disc radiance.

Stage 3 removed the legacy visible fragment shader. On Win64 Vulkan the 19
comparable Stage 0 buffers (including the half-float capture face) matched
byte for byte in runs 4608 and 21484. The mesh prototype uses the corrected
fixture described above. Captures and raw timer pairs are under
`Build/UESkyRenderingAlignment/MaterialSky/{4608,21484}/SkyParity`.
Incremental cube draw median/p95 was 7.584/8.384 and 7.104/8.288 microseconds.
Capture update median/p95 was 0.704/0.931 and 1.181/1.189 milliseconds.
Both runs used the vertex-only global sky shader, verified black emissive output
without a procedural provider, and collected 32 draw pairs
and 16 capture samples on the Stage 0 GPU. The default water material previously
divided by a zero wavelength on ordinary preview spheres. Guarding that
denominator restored visible sphere pixels over an opaque background. All 16
Vulkan thumbnail tests passed. Default water backdrop policy is tracked in
[Asset Thumbnails](../Editor/Architecture/Assets/AssetThumbnails.md).

The Win64 Debug stage-3 working tree based on `fadb58341` passed all 296
tests across SkyBox, SkyBox Vulkan, WaterSurfaceRendering Vulkan, volumetric
cloud scenes, MaterialCompiler, MaterialRuntime, MaterialCook, shader Cook and
the three thumbnail targets. The two depth conventions transmitted sky pixels
through water while occluding a translucent layer behind the water. Sandbox
and RoadWeaver Game receipts, RoadWeaverEditor, DurinAssetTool and
DurinExportTool built successfully. A clean Sandbox Cook exported 11 packages.
A DurinGame content probe loaded DefaultCubeSky, DefaultProceduralSky and
GrayboxStage15 with no ShaderBuild module. The staged Sandbox executable
loaded the default level and exited normally after 12 ticks. The Cook, staged
export and logs are under `Build/UESkyRenderingAlignment/MaterialSky`.
Metal visible-background and capture qualification still require a macOS GPU
runner. The plan remains active for this acceptance gate.

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
- [x] Preserve existing `SkyBox` actor and component data. Resolve the stored
  texture, tint, intensity and transform into the default material at load and
  edit time. Provide an explicit authored material override and document how it
  interacts with legacy settings. Confirm save/reopen, undo/redo, texture
  replacement, shader readiness and source-free Cook on representative assets.
- [x] Verify stage 0 parity for cubemap scenes, missing textures, preview
  overrides and HDR output. Keep the old background route available for
  Procedural Sky until Stage 2 passes.

### Stage 2: Render Procedural Sky through the material path

- [x] Keep `DProceduralSkyComponent` as the provider for the analytic parameters
  and publish them through the existing retained scene snapshot. Bind that
  selected snapshot and the sky direction to the default Unlit sky material.
  Expose the shared `EvaluateProceduralSky` calculation through a sky-specific
  material expression or intrinsic with defined behavior when the provider is
  absent. Use the same [radiance evaluator](../../Engine/Shaders/Slang/ProceduralSky.slang)
  in the visible draw and the [Sky Light capture](../Runtime/Rendering/Features/SkyLighting.md).
- [x] Keep `Captured Sky` scoped to the analytic provider and `Specified Cube`
  scoped to its explicit asset. Preserve the existing capture scheduling and
  publication lifetimes. Define whether any authored visual customization also
  affects capture before exposing it. Verify capture and visible output against
  their existing radiance contract, including any intentionally excluded terms.
- [x] Verify old level settings, source priority, parameter edits, disabled or
  removed providers, capture refresh and source-free Cook. Completion requires
  the same reference directions and transforms to agree with the baseline
  radiance in both paths within the tolerance set in Stage 0.

### Stage 3: Migrate consumers and retire the legacy visible sky shader

- [x] Migrate all registered view and content consumers. Check preview scenes,
  TextureCube thumbnails and the thumbnail cache contract. Verify that ordinary
  material thumbnails keep their existing output. Include a `DefaultWater`
  capture over a known opaque background and inspect rendered pixels to verify
  that the sphere differs from the background and stays visible. Track the
  choice of default water thumbnail background independently of the sky migration.
- [x] Verify sky color reaches the water snapshot with the expected far depth,
  opaque geometry occludes the background, clouds composite in the existing
  order and transparent offscreen views remain transparent when no background
  is requested. Verify cubemap and procedural lighting with their current
  Sky Light source modes.
- [x] Remove `FSkyBoxRenderer` and the old visible `SkyBox.slang` shader entry
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

### Stage 4: Unify visible skies on a skydome mesh

- [x] Route cubemap, procedural and override materials through one indexed
  camera-centered sphere with spherical UV0 and per-view far depth. Retire
  the fullscreen vertex shader and its screen-space material coordinates.
- [x] Validate material UVs, HDR, depth and water composition on Win64 Vulkan.
- [x] Route authored sky material sections through ordinary static mesh components
  and give cubemap placement a mesh-backed default.
- [x] Remove meshless scene publication and migrate GrayboxStage15 to a regular
  static mesh actor with a persistent cube material instance.
