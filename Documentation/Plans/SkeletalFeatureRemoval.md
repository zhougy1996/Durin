# Skeletal Feature Removal Plan

Summary: Remove Skeleton, SkeletalMesh, AnimationClip, playback, GPU skinning, dedicated authoring tools, and exclusive sample content while preserving static scene workflows.

Last reviewed: 2026-09-05

Status: Active
Completed:

## Current Status

Planning only; no skeletal code or assets have been removed. The user selected
complete skeletal feature removal and explicitly retained volumetric clouds and
spline deformation. Terrain removal remains a separate selected task under the
[Terrain Removal Plan](TerrainRemoval.md).

Inspected implementation includes immutable Skeleton/mesh/clip assets, single-clip
playback and pose publication, GPU skinning, skeletal scene participation in base,
GBuffer and shadow passes, SkeletalBuild recipes, Scene import integration, and
SkeletalMeshEditor. These are a connected removal scope, not just a playback API.
The Sandbox player currently constructs a static-mesh visual. This does not prove
that serialized levels are free of skeletal references.

Tracked candidates under `Sandbox/Content/Characters/RiggedSimple` include
`Animations/Animation_0.dasset`, `SkeletalMeshes/Cylinder.dasset`,
`Skeletons/Armature.dasset`, `Meshes/RiggedSimple.dasset`, and
`Materials/Material_001_effect.dasset`. The mesh and material require referencer
inspection before deletion; directory membership alone is not ownership evidence.

## Goal and Boundaries

- Remove `DSkeleton`, `DSkeletalMesh`, `DAnimationClip`, skeletal actor/component,
  animation binding/evaluation/playback, pose palettes, render resources/proxies,
  skinning shaders, build providers, derived-data orchestration, and Cook/load
  specializations. Remove the `SkeletalBuild` and `SkeletalMeshEditor` modules.
- Remove skeletal import products, UI filters/actions, inspectors, previews,
  thumbnails, picking cases, telemetry, tests, and exclusive demo/source assets.
  Do not leave disabled modules, skeletal stubs, or a bind-pose fallback renderer.
- Preserve volumetric clouds, their assets/rendering, spline curves and mesh
  deformation, their collision/picking, and their tests. Preserve static meshes,
  materials, textures, Scene import, generic asset/thumbnail/build infrastructure,
  math interpolation, ticking, and render-resource lifecycle mechanisms.
- No compatibility or conversion promise for old skeletal packages or cooked
  data. Clean retained repository content while old reflected types still load.
  Keep generic unsupported-type diagnostics and unrelated package loading intact.
- Preserve generic glTF/GLB static-scene import. A source requiring skeletal
  import must fail clearly before publication, with no partial assets or silent
  conversion of skinned meshes to static meshes. Animation-only channels with no
  supported output must receive an explicit diagnostic. Preserve any existing
  independent static-scene behavior; do not introduce a new static animation system.
- Do not renumber persistent identifiers belonging to surviving asset/primitive
  families when removing enum members. Audit serialization before altering them.
- Delete source assets and bulk sidecars only when exclusive ownership and
  recoverability are established. Preserve shared materials/textures and LFS
  objects; do not prune shared storage, rewrite history, or purge entire caches.

## Implementation Stages

### Stage 0: Establish asset and integration inventory

- [ ] Record exact skeletal-only source files and shared integration sites in
  Engine, Renderer, RenderCore, AssetForgeBuiltins, editor modules, DurinAssetTool,
  shaders, module/reflection metadata, CMake, and native-test registrations.
- [ ] Inspect mounted package types/dependencies and incoming references using
  supported asset tools before type removal. Record retained level edits, exact
  package/sidecar/source deletion paths, and local/untracked content separately.
- [ ] Resolve ownership of the RiggedSimple mesh/material and locate referenced
  source files even when their filenames do not mention skeletal content. Record
  the pre-removal Git revision and recoverable LFS data for selected deletions.
- [ ] Check current plans/roadmaps and contracts for skeletal requirements. Record
  necessary scope/link changes without marking abandoned requirements as passed.
- [ ] Verify the selected import rejection boundary in Scene capture/publication
  and identify existing transaction tests that can check no partial publication.
- [ ] Record baseline fixtures and registered validation selections for static
  scenes, volumetric clouds, and spline deformation. Reconcile already completed
  Terrain removal edits rather than restoring deleted Terrain branches.

Completion: removal files, asset actions, shared contracts, import behavior, and
preservation coverage are explicit enough to execute without guessing ownership.

### Stage 1: Remove content and skeletal authoring support

Depends on Stage 0; retained packages must be edited while skeletal types exist.

- [ ] Remove skeletal actors/components/references from retained levels using
  supported package operations, then save/reload and recheck referencers.
- [ ] Delete inventoried exclusive skeletal packages, bulk payloads, sources,
  and demo content. Retain shared assets and record reasons for retained candidates.
- [ ] Remove SkeletalMeshEditor, its module startup/dependencies, inspectors,
  preview and thumbnail registrations, Content Browser filters/actions, DurinEd
  preview cases, and LevelEditor skeletal picking/diagnostic UI.
- [ ] Remove GltfSkeletalDecoder and skeletal Scene import capture, staged products,
  peer publication, feature registration, and associated public import structures.
  Enforce the selected explicit unsupported-content behavior before publication.
- [ ] Adapt import/editor tests to verify surviving static-scene behavior,
  cancellation/rollback, and unsupported skeletal input without partial assets.

Completion: retained content has no skeletal references, the editor offers no
skeletal workflow, and static Scene import still publishes valid assets atomically.

### Stage 2: Remove runtime assets, playback, rendering, and build code

Depends on Stage 1. Remove types and remaining callers together so each validated
handoff compiles; do not split declarations from consumers into broken commits.

- [ ] Delete Engine Skeleton/SkeletalMesh/AnimationClip implementations, playback,
  actor/component, scene proxy, resources/vertex factory, build keys/providers,
  asset compilation and Cook/load cases. Audit cooked mesh load manager branches
  and preserve static-mesh residency and asynchronous completion behavior.
- [ ] Remove Renderer skeletal storage, visibility, preparation, palette transport,
  base/scene-color, GBuffer, shadow, graph, cleanup/recovery, and telemetry branches.
  Keep shared static-mesh, spline and cloud frame paths intact.
- [ ] Remove `SkeletalMeshVertexFactory.slang` and skeletal material/shader
  generation, reflection, bindings, statistics, and primitive-family cases across
  Engine, RenderCore, shared Slang, and any concrete RHI consumer found in Stage 0.
- [ ] Delete SkeletalBuild and remove project/profile membership, module dependencies,
  reflection inputs, CMake/test deployment, and DurinAssetTool startup/includes.
- [ ] Remove skeletal-only tests and fixtures; replace skeletal fixtures in mixed
  world/lifetime, renderer, Cook, thumbnail, GBuffer, shadow, and picking tests
  where they verify surviving behavior. Preserve generic assertions and coverage.

Completion: runtime and editor targets compile without skeletal modules/types or
shader variants; affected tests pass and retained primitive families still render.

### Stage 3: Verify remaining workflows and reconcile documentation

Depends on Stage 2.

- [ ] Remove obsolete skeletal runtime/editor contracts and guide; update module
  ownership, Scene import, thumbnails, rendering/asset contracts, and current plan
  requirements. Preserve archived evidence and repair current direct links.
- [ ] Audit maintained source, shaders, configuration, build/test registrations,
  package references, and active documentation for skeletal remnants. Classify
  generic/historical references rather than deleting by keyword alone.
- [ ] Complete a full `all` build and `test affected`. Use the registry to select
  additional bounded integration coverage for static Scene import, asset Cook/load,
  rendering/shadows, clouds and spline deformation when affected coverage omits it.
- [ ] Smoke the retained Sandbox level: load, select/place static meshes, render
  materials and shadows, save/reload, and run basic gameplay collision. Verify
  static glTF/GLB import and explicit failure for unsupported skeletal input.
- [ ] Exercise volumetric-cloud rendering and spline mesh deformation, picking,
  and collision with their existing qualified fixtures; verify their assets and
  editor entry points remain available. Record correctness, not unqualified GPU
  performance claims.
- [ ] Cook retained content and load it through the registered runtime workflow
  without SkeletalBuild, skeletal reflection, or source/DDC fallback for removed
  products. Verify surviving static-mesh/material/texture output.
- [ ] Validate changed documentation and all plans; run roadmap lifecycle validation
  if applicable. Record exact commands/results and asset deletions, and close this
  plan only when all required acceptance gates have passed.

Completion: skeletal functionality is absent end to end, preserved features pass
their checks, and current documentation describes the reduced engine accurately.

## Execution and Handoff

Follow [agent build/run](../Agents/BuildAndRun.md),
[agent testing](../Agents/Testing.md), and
[agent documentation](../Agents/Documentation.md) workflows. Before asset edits,
read [asset catalog and mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
and [asset packages](../Runtime/Assets/AssetPackages.md).

Use one checkout writer, with no overlapping build process trees. Terrain removal
and skeletal removal touch shared renderer/import/test files: execute sequentially
in this checkout and recheck the actual tree before each stage. Neither plan
authorizes removing volumetric clouds or spline deformation. Each validated
implementation commit updates status/evidence and includes the exact Plan and
Stage trailers. Documentation validation alone does not validate implementation.

## Related Code

- `Engine/Engine.dproject`
- `Engine/Source/Runtime/Engine/Public/SkeletalMesh` and corresponding private code
- `Engine/Source/Runtime/Engine/Public/Animation` and corresponding private code
- `Engine/Source/Runtime/Engine/Public/Components/SkeletalMeshComponent.h`
- `Engine/Source/Runtime/Engine/Private/Rendering/SkeletalMeshSceneProxy.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderer.cpp`
- `Engine/Source/Runtime/RenderCore/Public/ViewRenderStatistics.h`
- `Engine/Source/Developer/SkeletalBuild`
- `Engine/Source/Editor/SkeletalMeshEditor`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/SceneImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/GltfSkeletalDecoder.cpp`
- `Engine/Source/Programs/DurinAssetTool`
- `Engine/Shaders/Slang/VertexFactory/SkeletalMeshVertexFactory.slang`
- `Engine/Tests/Native/EngineTests` and `Engine/Tests/Native/AssetTests`
- `Sandbox/Content/Characters/RiggedSimple`
