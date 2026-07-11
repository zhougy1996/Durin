# Material System

Durin's material architecture follows the useful ownership split from Unreal Engine while keeping the first implementation intentionally small.

## Current Architecture

- `DMaterialInterface` is the common asset/component-facing contract. It resolves named parameters and produces immutable `FMaterialRenderData` for rendering.
- `DMaterial` owns default scalar and vector parameter maps.
- `DMaterialInstance` references a parent material interface and stores only local overrides. Parent cycles are rejected.
- `DStaticMeshComponent` owns the material assignment. The current static mesh format has one section, so the component currently exposes one material slot.
- `FStaticMeshSceneProxy` receives a compact render-data snapshot. The renderer never reads reflected material objects directly.
- The static mesh shader consumes `BaseColor` and `Opacity` through its existing transform uniform.

Built-in parameter names are `BaseColor` (vector) and `Opacity` (scalar). Missing parameters preserve the engine's orange fallback material.

## Implementation Roadmap

1. **Foundation (implemented):** material interface, base material, instances, named scalar/vector parameters, inheritance, component binding, render proxy snapshot, serialization tests.
2. **Textures and samplers:** add texture parameters, default textures, sampler policy, asset residency, and fragment-shader bindings.
3. **Static permutations:** add blend mode, shading model, two-sided state, vertex-factory keys, shader-map identity, and pipeline-state caching.
4. **Material graph compilation:** introduce graph assets, typed expressions, HLSL/Slang generation, dependency tracking, diagnostics, and derived-data caching.
5. **Runtime/editor workflow:** dynamic material instances, render-thread update commands, material/instance editors, thumbnails, hot reload, and statistics.
6. **Advanced rendering:** mesh sections and multiple slots, depth/shadow passes, deferred/PBR inputs, decals, translucent sorting, and platform quality levels.

## Design Rules

- Components and assets use `DMaterialInterface`; renderer code consumes only render data and shader maps.
- Instances override parameters without duplicating the parent's shader program.
- Static properties belong in shader-map/permutation keys; dynamic parameters belong in uniform/resource bindings.
- Material object mutation must eventually cross to the rendering thread through explicit update commands. Until that stage, reassigning the component material rebuilds its scene proxy.
