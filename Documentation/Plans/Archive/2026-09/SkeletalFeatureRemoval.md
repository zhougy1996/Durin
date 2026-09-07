# Skeletal Feature Removal Plan

Summary: Remove Skeleton, SkeletalMesh, AnimationClip, playback, GPU skinning, dedicated authoring tools, and exclusive sample content while preserving static scene workflows.

Last reviewed: 2026-09-05

Status: Archived
Completed: 2026-09-05

## Current Status

Complete skeletal feature removal is implemented. Skeleton, SkeletalMesh,
AnimationClip, playback, GPU skinning, build/editor modules, import publication,
runtime/render paths, tests, fixtures, documentation, and exclusive RiggedSimple
content are gone. Static Scene import rejects sources containing `skins` or
`animations` before staging outputs. Volumetric clouds, spline deformation,
static meshes, materials, textures, shadows, Cook, and Sandbox startup remain
qualified. Terrain removal remains reconciled under the completed
[Terrain Removal Plan](TerrainRemoval.md).

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

- [x] Record exact skeletal-only source files and shared integration sites in
  Engine, Renderer, RenderCore, AssetForgeBuiltins, editor modules, DurinAssetTool,
  shaders, module/reflection metadata, CMake, and native-test registrations.
- [x] Inspect mounted package types/dependencies and incoming references using
  supported asset tools before type removal. Record retained level edits, exact
  package/sidecar/source deletion paths, and local/untracked content separately.
- [x] Resolve ownership of the RiggedSimple mesh/material and locate referenced
  source files even when their filenames do not mention skeletal content. Record
  the pre-removal Git revision and recoverable LFS data for selected deletions.
- [x] Check current plans/roadmaps and contracts for skeletal requirements. Record
  necessary scope/link changes without marking abandoned requirements as passed.
- [x] Verify the selected import rejection boundary in Scene capture/publication
  and identify existing transaction tests that can check no partial publication.
- [x] Record baseline fixtures and registered validation selections for static
  scenes, volumetric clouds, and spline deformation. Reconcile already completed
  Terrain removal edits rather than restoring deleted Terrain branches.

Completion: removal files, asset actions, shared contracts, import behavior, and
preservation coverage are explicit enough to execute without guessing ownership.

### Stage 1: Remove content and skeletal authoring support

Depends on Stage 0; retained packages must be edited while skeletal types exist.

- [x] Remove skeletal actors/components/references from retained levels using
  supported package operations, then save/reload and recheck referencers.
- [x] Delete inventoried exclusive skeletal packages, bulk payloads, sources,
  and demo content. Retain shared assets and record reasons for retained candidates.
- [x] Remove SkeletalMeshEditor, its module startup/dependencies, inspectors,
  preview and thumbnail registrations, Content Browser filters/actions, DurinEd
  preview cases, and LevelEditor skeletal picking/diagnostic UI.
- [x] Remove GltfSkeletalDecoder and skeletal Scene import capture, staged products,
  peer publication, feature registration, and associated public import structures.
  Enforce the selected explicit unsupported-content behavior before publication.
- [x] Adapt import/editor tests to verify surviving static-scene behavior,
  cancellation/rollback, and unsupported skeletal input without partial assets.

Completion: retained content has no skeletal references, the editor offers no
skeletal workflow, and static Scene import still publishes valid assets atomically.

### Stage 2: Remove runtime assets, playback, rendering, and build code

Depends on Stage 1. Remove types and remaining callers together so each validated
handoff compiles; do not split declarations from consumers into broken commits.

- [x] Delete Engine Skeleton/SkeletalMesh/AnimationClip implementations, playback,
  actor/component, scene proxy, resources/vertex factory, build keys/providers,
  asset compilation and Cook/load cases. Audit cooked mesh load manager branches
  and preserve static-mesh residency and asynchronous completion behavior.
- [x] Remove Renderer skeletal storage, visibility, preparation, palette transport,
  base/scene-color, GBuffer, shadow, graph, cleanup/recovery, and telemetry branches.
  Keep shared static-mesh, spline and cloud frame paths intact.
- [x] Remove `SkeletalMeshVertexFactory.slang` and skeletal material/shader
  generation, reflection, bindings, statistics, and primitive-family cases across
  Engine, RenderCore, shared Slang, and any concrete RHI consumer found in Stage 0.
- [x] Delete SkeletalBuild and remove project/profile membership, module dependencies,
  reflection inputs, CMake/test deployment, and DurinAssetTool startup/includes.
- [x] Remove skeletal-only tests and fixtures; replace skeletal fixtures in mixed
  world/lifetime, renderer, Cook, thumbnail, GBuffer, shadow, and picking tests
  where they verify surviving behavior. Preserve generic assertions and coverage.

Completion: runtime and editor targets compile without skeletal modules/types or
shader variants; affected tests pass and retained primitive families still render.

### Stage 3: Verify remaining workflows and reconcile documentation

Depends on Stage 2.

- [x] Remove obsolete skeletal runtime/editor contracts and guide; update module
  ownership, Scene import, thumbnails, rendering/asset contracts, and current plan
  requirements. Preserve archived evidence and repair current direct links.
- [x] Audit maintained source, shaders, configuration, build/test registrations,
  package references, and active documentation for skeletal remnants. Classify
  generic/historical references rather than deleting by keyword alone.
- [x] Complete a full `all` build and `test affected`. Use the registry to select
  additional bounded integration coverage for static Scene import, asset Cook/load,
  rendering/shadows, clouds and spline deformation when affected coverage omits it.
- [x] Smoke the retained Sandbox level: load, select/place static meshes, render
  materials and shadows, save/reload, and run basic gameplay collision. Verify
  static glTF/GLB import and explicit failure for unsupported skeletal input.
- [x] Exercise volumetric-cloud rendering and spline mesh deformation, picking,
  and collision with their existing qualified fixtures; verify their assets and
  editor entry points remain available. Record correctness, not unqualified GPU
  performance claims.
- [x] Cook retained content and load it through the registered runtime workflow
  without SkeletalBuild, skeletal reflection, or source/DDC fallback for removed
  products. Verify surviving static-mesh/material/texture output.
- [x] Validate changed documentation and all plans; run roadmap lifecycle validation
  if applicable. Record exact commands/results and asset deletions, and close this
  plan only when all required acceptance gates have passed.

Completion: skeletal functionality is absent end to end, preserved features pass
their checks, and current documentation describes the reduced engine accurately.

## Validation Evidence

- Pre-removal revision was `8b3a7bda99e7db9c6d346968f0ba21f7ecc670fb`.
  The initial asset catalog contained 25 packages. Referencer inspection found
  no incoming reference from retained content into RiggedSimple, so no retained
  level required mutation. Its Skeleton, SkeletalMesh, AnimationClip,
  StaticMesh, and MaterialInstance packages were exclusive and recoverable from
  Git; all five were deleted. Shared storage, LFS objects, and DDC caches were
  not pruned. The post-removal asset catalog check succeeds.
- `./DevTool configure` and the final `./DevTool build` for target `all`
  succeeded on `MacOS-arm64-Debug-DurinEditor`.
- `./DevTool test affected` resolved to the complete routine native-test set and
  passed. Qualification selections for `renderer`, `asset-import`, `viewport`,
  `spline`, and `static-mesh` passed, covering retained GBuffer/shadow/cloud,
  static Scene import, picking, deformation/collision, cooked mesh residency,
  and GPU resource recovery paths.
- Parser and publication tests reject both skeletal and animation-only glTF
  roots with `UnsupportedFeature`, empty outputs, and no partially published
  material. Static glTF/GLB Scene import remains covered by routine and Vulkan
  integration tests.
- A hidden Sandbox editor run completed 120 ticks and shut down cleanly. The
  routine suite covers static placement/selection, material and shadow paths,
  save/reload, gameplay lifecycle, and collision contracts.
- A clean non-incremental Win64 Game Cook published five retained packages,
  including `/Game/Levels/GrayboxStage15` and both volumetric-cloud texture
  inputs, with 3,760,497 changed bytes and zero rollback.
- Maintained source, shaders, project/build/test metadata, and current domain
  documentation contain no removed type/module path. The only maintained source
  keyword is the intentional unsupported-input diagnostic; the Terrain removal
  record links this plan, and historical archives remain unchanged.
- Changed-document validation and all-plan lifecycle validation pass. Roadmap
  lifecycle validation also passes after reconciling the active Material System
  roadmap with StaticMesh/SplineMesh execution.

## Execution and Handoff

Follow [agent build/run](../../../Agents/BuildAndRun.md),
[agent testing](../../../Agents/Testing.md), and
[agent documentation](../../../Agents/Documentation.md) workflows. Before asset edits,
read [asset catalog and mutation](../../../Runtime/Assets/AssetCatalogAndMutation.md)
and [asset packages](../../../Runtime/Assets/AssetPackages.md).

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
