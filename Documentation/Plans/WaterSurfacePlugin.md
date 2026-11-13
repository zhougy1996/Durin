# Water Surface Plugin Plan

Summary: Add an opt-in Engine water plugin for finite, animated water surfaces using the existing material and mesh systems.

Last reviewed: 2026-11-13

Status: Active
Completed:

## Current Status

Planning complete; implementation has not started. The first release targets finite pond and lake surfaces in the Editor and development Game runtime. Cook, Stage, packaged Game validation, refraction, underwater rendering, buoyancy, spline rivers, and camera-following oceans are deferred.

## Goal

An enabled project can place a water surface in a level, edit its dimensions and wave parameters, save and reopen it, and see the same animated surface during development runtime. Gameplay can query water height and normal from the same wave definition used by the visible surface. A project that does not enable the plugin has no Water module in its build or startup selection.

## Selected Design

- Put `Water.dplugin` and a `Water` Runtime module under `Engine/Plugins/Water/`. Projects opt in through their `.dproject` plugin selection. Add an Editor module only when a concrete editor feature needs one; ordinary reflected properties and component editing are the starting point.
- Keep water geometry, authored parameters, conservative bounds, and CPU surface queries in the plugin. Use the existing material system for appearance and vertex displacement. Do not add a parallel water rendering pipeline.
- Start with one finite rectangular water body. A regular grid provides enough vertices for displacement; the component owns its extent and material. Use a small, deterministic Gerstner wave set with bounded amplitudes. Define coordinate space, time source, edge behavior, and CPU/GPU agreement before implementation.
- Use the existing Lit/translucent, `Time`, `WorldPosition`, and World Position Offset capabilities where qualified. A missing material expression or grid asset path is a Stage 0 dependency, not a reason to introduce a general renderer extension API.
- Keep the first implementation independent of terrain modification, physics simulation, and specialized water shading. Record any future renderer API requirement as a separate design decision with a concrete visual case.

## Implementation Stages

### Stage 0: Prove the rendering path

- [ ] Identify the smallest existing grid mesh and material authoring path that can express wave displacement and a usable surface normal. Build a minimal development scene without plugin-specific renderer changes.
- [ ] Verify time-driven WPO, transparent or opaque Lit appearance, conservative displacement bounds, and editor selection in a focused renderer/component test or controlled visual smoke.
- [ ] Record the wave equations, parameter limits, time source, world/local space convention, and CPU/GPU tolerance. If an essential capability is missing, specify and implement only that bounded foundation before Stage 2.

Completion: a finite animated grid is visible and stable in Editor and development Game; the required lower-level work is known and bounded.

### Stage 1: Establish the plugin boundary

- [ ] Add the Engine plugin descriptor and Runtime module with the minimal dependency set. Keep it disabled by default in existing projects.
- [ ] Verify project opt-in selects and builds Water, while an unselected project does not compile or admit it. Cover plugin descriptor discovery and module startup with focused tests.

Completion: Water can be enabled and loaded through the ordinary plugin workflow without changes to unrelated project roots.

### Stage 2: Author and render a finite water surface

- [ ] Add a reflected, placeable water surface actor/component with extent, elevation, material, and bounded wave parameters.
- [ ] Provide the regular grid and default water material through plugin-owned resources. Apply the wave definition in WPO, produce a consistent visible normal, and enlarge bounds for maximum displacement.
- [ ] Validate property edits, registration/unregistration, level save/reopen, editor viewport rendering, and development Game rendering. Keep failure diagnostics local to the surface or plugin.

Completion: a level author can place and persist a water body and see stable animation in Editor and development Game.

### Stage 3: Expose surface queries

- [ ] Implement deterministic CPU height and normal queries using the Stage 0 wave convention, including points outside the finite extent.
- [ ] Test CPU queries at representative positions and times against the material wave definition within the recorded tolerance.
- [ ] Verify parameter edits update visible and query state together without stale snapshots.

Completion: gameplay queries agree with the rendered water within the stated tolerance.

## Validation and Deferred Work

Use the repository [build](../Agents/BuildAndRun.md) and [test](../Agents/Testing.md) workflows to select the smallest owning-module and consumer coverage. Validate both enabled and disabled project selection and one Editor/development Game scene. A Game preset is required for the runtime-variant boundary; a repository-wide `all` build is needed only if bounded targets cannot cover a shared renderer or build-graph change.

Cook, Stage, packaged Game startup, and packaged asset/shader validation are **not acceptance gates for this plan**. Before claiming the plugin is release-ready, open a follow-on stage or plan for those paths. The same applies to refraction, underwater effects, buoyancy, spline rivers, terrain interaction, and infinite ocean meshing.

Related contracts: [plugin metadata and selection](../Workspace/Plugins.md), [material vertex evaluation](../Runtime/Rendering/Materials/MaterialVertexEvaluation.md), and [forward lighting](../Runtime/Rendering/Features/ForwardLighting.md).
