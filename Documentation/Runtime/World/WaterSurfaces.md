# Finite Water Surfaces

Summary: The Water plugin renders finite Gerstner surfaces and provides matching CPU height and normal queries.
Modules: Water, WaterEditor, Engine

Last reviewed: 2026-11-23

## Enablement and authoring

Enable the Water plugin for the project and build its plugin inventory. The supported rendering path is above-water Win64 Vulkan deferred rendering. See the [plugin workflow](../../Workspace/Plugins.md).

`AWaterSurfaceActor` owns a `DWaterSurfaceComponent`. The component stores extent, elevation, speed and steepness. Use extent for independent XY dimensions. Transforms support translation, rotation and positive uniform scale.

## Reusable Water Waves assets

New components reference `/Plugins/Water/Waves/DefaultWaterWaves`, a 16-wave preset. Create or edit a **Water Waves** asset to change the generator parameters and assign it through **Waves Source**. Clearing the reference produces a flat surface at the component elevation. A missing assigned asset or an invalid resolved spectrum produces a component diagnostic and disables its surface queries and rendering.

A shared asset edit or undo refreshes every registered component using it. Keep the resolved waves, grid, bounds and render payload from the same settings snapshot. Each proxy retains its geometry until removal.

## Wave and query contract

`EvaluateWaterSurface` takes undisplaced local XY. `QueryWaterSurface` inverts the displaced XY map and returns an empty result outside the deformed rectangle. `DWaterSurfaceComponent::QuerySurface` projects along component-local Z and returns world-space position and normal. Use renderer material seconds for CPU/GPU agreement (`GetWaterMaterialTime` for the default clock).

The horizontal derivative bound protects inverse convergence. The shortest active wavelength determines grid density. Validation rejects grids above 256 cells per axis. Keep validation, displacement bounds and the CPU evaluator together in [WaterSurfaceTypes.cpp](../../../Engine/Plugins/Water/Source/Water/Private/WaterSurfaceTypes.cpp).

## Material resources

`DefaultWater` uses Single Layer Water and calls the plugin's `GerstnerWaves` material function. UV0 carries undisplaced local XY. The outputs are world-space displacement and a tangent-space normal. Optical defaults live in the [material authoring recipe](../../../Engine/Tests/Fixtures/WaterSurface/AuthorWaterMaterial.h). The renderer owns [Single Layer Water shading](../Rendering/Materials/MaterialSystem.md#single-layer-water).

The material uses Opaque routing, Opacity 0 and Pixel Normal Offset. Its
Refraction input decreases linearly from 1.15 at zero view depth to 1.02 at
50 metres, then stays at 1.02. View depth is the water surface position along
the camera's forward axis. The Gerstner normal drives distortion relative to
the mesh normal.

When changing the wave payload, update the CPU packers and the [material expression](../../../Engine/Plugins/Water/Source/Water/Private/MaterialExpressionGerstnerWaves.cpp) together. Primitive floats 0–3 carry amplitude, wavelength, speed and steepness. The next 28 floats carry seven direction-XY, amplitude-ratio and wavenumber-ratio records. The default instance carries the remaining nine as direction angle in radians and the same two ratios. Zero amplitude disables a record.

Saved components serialize the wave source and default material references for Cook. For runtime-only spawns, retain the material and selected wave asset through explicit Cook roots or project asset policy. The [validation fixture](../../../Engine/Tests/Fixtures/WaterSurface/README.md) covers CPU/material agreement, asset regeneration, Level persistence and cooked Vulkan rendering.
