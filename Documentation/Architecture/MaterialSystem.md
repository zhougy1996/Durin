# Material System

Durin's material architecture follows the useful ownership split from Unreal Engine while keeping the first implementation intentionally small.

## Current Architecture

- `DMaterialInterface` is the common asset/component-facing contract. It resolves named parameters and produces immutable `FMaterialRenderData` for rendering.
- `DMaterial` owns default scalar, vector, and texture parameter maps.
- `DMaterialInstance` references a parent material interface and stores only local overrides. Parent cycles are rejected.
- `DStaticMeshComponent` owns per-slot material overrides. Static mesh sections reference imported material slots, and the scene proxy snapshots one render-data value per slot.
- `FStaticMeshSceneProxy` receives compact render-data snapshots. Material and parent changes propagate through explicit render commands and update the existing proxy in place; the renderer never reads reflected material objects directly.
- The static mesh shader consumes `BaseColor`, `BaseColorTexture`, `Opacity`, `SpecularStrength`, and `Shininess`. It currently implements a fixed directional-light Blinn-Phong path plus an unlit viewport mode.

Built-in parameter names are `BaseColor` (vector), `BaseColorTexture` (texture), `Opacity` (scalar), `SpecularStrength` (scalar), and `Shininess` (scalar). Missing values preserve the orange fallback material and renderer-owned default textures.

## Static Mesh Vertex Contract

- Static meshes retain up to four UV channels. Missing channels are filled with `(0, 0)` while the LOD records how many source channels were present.
- Tangents are stored as `xyz` plus a handedness sign in `w`; the bitangent is reconstructed as `cross(N, T) * sign`.
- Missing normals and tangents are generated deterministically. Missing vertex colors use linear white, so they do not change the material base color.
- Each imported node-mesh instance becomes a section. Sections keep contiguous index ranges and reference a stable source material slot; source material assets are not created automatically.

## Design Rules

- Components and assets use `DMaterialInterface`; renderer code consumes only render data and shader maps.
- Instances override parameters without duplicating the parent's shader program.
- Static properties belong in shader-map/permutation keys; dynamic parameters belong in uniform/resource bindings.
- Material object mutation crosses to the rendering thread through explicit update commands. Replacing the material assigned to a component still rebuilds its scene proxy because the dependency binding changes.

The prioritized implementation backlog and current editor/rendering limitations are tracked in `Documentation/Plans/MaterialSystem.md`.
