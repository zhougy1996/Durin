# UE Sky Rendering Alignment Plan

Summary: Move visible SkyBox and Procedural Sky to material-backed backgrounds while preserving scene-owned Sky Light capture and existing levels.

Last reviewed: 2026-11-24
Status: Active
Completed:

## Current Status

Implementation has not started. Durin draws both visible sky sources through the
same [SkyBox shader](../../Engine/Shaders/Slang/SkyBox.slang). The procedural
radiance evaluator also feeds the separate Sky Light capture path. The material
system already supports Surface Unlit, HDR emissive output and directional
TextureCube sampling. The open design decision is how the material draw will
preserve the existing background depth, projection and composition behavior.

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

- [ ] Inventory the Engine, Sandbox and RoadWeaver consumers of `DSkyBoxComponent`,
  `DProceduralSkyComponent`, `FSkyBoxRenderer` and `FViewEnvironmentOverride`.
  Include level placement, editor previews, TextureCube thumbnails, Cook and
  Sky Light capture. Record source priority and missing-resource behavior for
  views with multiple sky providers and an explicit environment override.
- [ ] Record reference captures for all six cube faces, HDR intensity and tint,
  procedural horizon and sun direction, no sky, view override, water refraction
  and Sky Light capture. Include camera rotation, large translation, perspective
  and orthographic views, and transparent offscreen output. Record an incremental
  GPU cost baseline for the existing draw and capture paths.
- [ ] Prototype an inward-facing sky mesh with existing Unlit material features.
  Decide between a mesh with sky-specific depth and view state and a background
  draw that evaluates a material over a fullscreen primitive. Use one explicit
  sky role in the scene. Start with role assignment on the sky provider and
  evaluate an `Is Sky` material flag only if ordinary meshes need to opt in
  independently. Record how the chosen route preserves far depth, camera-relative
  sampling, stereo or auxiliary view isolation where supported, and existing
  water and cloud composition. Completion requires a recorded choice and
  reproducible parity fixtures for both sky sources.

### Stage 1: Render cubemap SkyBox with an Unlit material

- [ ] Ship a default HDR cubemap sky material with direction sampling, rotation,
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
- [ ] Verify stage 0 parity for cubemap scenes, missing textures, preview
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
