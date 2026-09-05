# Terrain Removal Plan

Summary: Remove finite Terrain and Terrain World code, dedicated content, and feature integrations while preserving the core editor, rendering, asset, and physics workflows.

Last reviewed: 2026-09-05

Status: Completed
Completed: 2026-09-05

## Current Status

Implementation is complete. The finite Terrain feature, Terrain World data and
build paths, the TerrainBuild module, editor authoring integrations, renderer
branches, shader variant, Cook registrations, diagnostics, and dedicated tests
have been removed. The unfinished Terrain Runtime Tile Reference plan and
Terrain World System roadmap were cancelled and removed rather than marked as
completed work.

The pre-removal asset audit inspected all 23 mounted Engine and Sandbox packages
through asset catalog check and storage inventory. It found no Terrain assets,
products, descriptors, or retained package references, so the verified content
action was zero deletions and zero package rewrites. No local or untracked
Terrain content was found.

HeightField geometry had no production producer or consumer outside Terrain. It
was removed from PhysicsCore, Physics, Engine collision diagnostics, and tests.
`ECollisionGeometryKind` now assigns explicit values 0 through 3 to surviving
kinds, leaving the former value 4 unassigned instead of renumbering a serialized
domain.

## Goal

The maintained engine has no Terrain feature, import/placement UI, dedicated
build module, shader variant, Cook contributor, or Terrain-only content. Remaining
sample levels load and the editor, static meshes, materials, textures, physics,
and cooked runtime continue to work.

## Selected Decisions and Boundaries

- Remove both finite Terrain and Terrain World, including `TerrainBuild`; do not
  keep a disabled module, compatibility facade, or replacement terrain design.
- No migration or compatibility promise for old Terrain assets or cooked data.
  Remove Terrain objects/references from retained repository content before
  removing the reflected types. Generic unsupported-type diagnostics must remain
  usable; loading unrelated assets must not depend on Terrain registration.
- Delete dedicated packages, bulk sidecars, source files, and fixtures only after
  recording their ownership and incoming references. Preserve shared content.
  Git history is the source recovery point; verify recoverable LFS objects for
  deleted content. Do not rewrite history or prune the shared LFS store.
- Preserve generic grayscale16 image decoding, texture source/build support,
  asset transactions, DDC, shader infrastructure, collision queries, and import
  extension registration. Remove their Terrain-specific consumers and branches.
- Audit `PhysicsCore` / `Physics` HeightField geometry separately: remove it if
  Terrain is its only production consumer; retain it only with an identified
  independent use and tests. Resolve this in Stage 0 before physics edits.
- Preserve non-Terrain coverage in mixed test files. Replace Terrain fixtures in
  general rendering/picking/shadow tests with static meshes when they exercise
  a surviving contract, rather than deleting those tests wholesale.
- Keep each stage coherent and validated. Runtime declarations, their callers,
  and build/reflection membership must change together where needed to compile.

## Implementation Stages

### Stage 0: Resolve removal inventory and competing plans

- [x] Record exact dedicated files and cross-module integration sites, including
  module/reflection descriptors, tests, shader generation, Cook, and tool startup.
- [x] Inspect mounted package metadata and referencers using the existing asset
  catalog/tool workflow while Terrain types still exist. Record exact deletion
  paths and retained packages needing edits; include local/untracked findings
  separately and avoid inferring asset types from filenames or binary text search.
- [x] Identify all non-Terrain HeightField production consumers and record the
  selected physics scope and stable serialized-enum handling if applicable.
- [x] Retire `TerrainRuntimeTileReference.md` and `TerrainWorldSystem.md` as
  cancelled development, following owning documentation lifecycle rules without
  claiming their open gates passed. Repair active links and remove Terrain
  requirements from `ContentBrowserImportExtensions.md`, preserving its generic
  import extension work and recording the scope change.

Completion: the file/reference inventory, asset actions, physics decision, and
documentation disposition are explicit; no active work directs Terrain expansion.

### Stage 1: Remove dedicated content and editor authoring entry points

Depends on Stage 0 and requires existing Terrain serialization during content edits.

- [x] Remove Terrain actors/components/references from retained levels and save
  through supported package operations; delete inventoried exclusive assets,
  sidecars, sources, and demo content. Record a verified zero-deletion result if
  no dedicated content remains. Check references again after the transaction.
- [x] Remove LevelEditor import dialogs, placement, Details, thumbnails, picking
  adapters, viewport controls, and feature callbacks/registrations for Terrain.
- [x] Remove AssetForgeBuiltins Terrain import/factory/import-data registration,
  Terrain adapters, and DurinEd source-reference/thumbnail special cases; remove
  unused MainFrame registrations/dependencies where their callers are gone.
- [x] Update affected editor/import tests while retaining Scene, Texture, and
  Static Mesh workflows. Validate the stage with the applicable build/test scope.

Completion: retained content has no Terrain references; the editor exposes no
Terrain authoring action and still imports, places, selects, and saves other assets.

### Stage 2: Remove runtime, rendering, build, and physics dependencies

Depends on Stage 1. Treat removal of types and their remaining consumers as one
coordinated source change rather than leaving an intermediate broken checkout.

- [x] Remove Engine Terrain public/private APIs, actor/component, scene proxy,
  collision coordinator, heightmap/world recipes, providers, DDC orchestration,
  Cook contributors, and Terrain-specific World/Level/primitive interfaces.
- [x] Remove Renderer Terrain topology/LOD/draw preparation and all participation
  in scene visibility, forward/base passes, GBuffer, directional shadows, render
  graph composition, telemetry, and editor diagnostics.
- [x] Remove RenderCore Terrain vertex factory/resources/statistics and the Slang
  vertex factory; clean Terrain variants and bindings from material program
  generation/compilation and shared shaders without removing static-mesh paths.
- [x] Apply the Stage 0 HeightField decision to PhysicsCore, Physics, Engine
  collision diagnostics, and tests; preserve surviving query/shape semantics.
- [x] Delete `TerrainBuild` and remove project/profile module membership,
  `.dmodule` dependencies/reflection inputs, CMake entries, and DurinAssetTool
  startup/includes. Remove remaining Terrain-only AssetForgeBuiltins adapters.
- [x] Remove Terrain-only native tests and registrations; retain texture tests
  from `TextureAndTerrainTests.cmake`, and revise mixed Cook, thumbnail, shader,
  viewport, renderer, GBuffer, shadow, and physics tests/fixtures.

Completion: configured targets and generated reflection/shaders contain no Terrain
dependency; affected native tests pass and retained material/physics paths work.

### Stage 3: Qualify the reduced engine and reconcile documentation

Depends on Stage 2.

- [x] Remove obsolete Terrain runtime/editor contracts and guide; repair current
  domain documentation, module ownership, and direct references. Preserve archived
  implementation evidence without presenting it as current functionality.
- [x] Audit remaining `Terrain`, `Heightmap`, and `HeightField` references in
  maintained source, shaders, configuration, build/test metadata, and content.
  Classify legitimate generic or historical references; investigate unexplained
  matches rather than treating a literal zero-match search as the only gate.
- [x] Run a successful full `all` build for the user-visible editor change and
  `test affected`; discover additional bounded render/physics/Cook integration
  coverage from the test registry where affected selection is insufficient.
- [x] Smoke the retained Sandbox level: load, static-mesh selection/placement,
  material rendering and shadows, save/reload, and gameplay collision. Verify
  Texture/Scene/Static Mesh import actions and absence of Terrain UI.
- [x] Cook retained sample content and load it through the registered runtime
  workflow without TerrainBuild, Terrain types, or Terrain source/DDC products.
- [x] Run changed-document and all-plan validation, plus roadmap validation if
  lifecycle metadata changed. Record commands/results, remaining limitations,
  and content deletions; complete this plan only after required gates pass.

Completion: core editor and cooked-runtime workflows pass, current documentation
matches the reduced feature set, and no required removal or validation remains.

## Validation Evidence

- Asset catalog check and storage inventory succeeded for all 23 mounted
  packages before reflected Terrain types were removed; both reported no Terrain
  ownership or references.
- `./DevTool configure` and the final `./DevTool build` for target `all`
  succeeded on `MacOS-arm64-Debug-DurinEditor`.
- `./DevTool test affected` resolved to the complete native-test selection and
  passed. Focused cooked mesh and cooked shader-library targets also passed.
- Hidden Sandbox startup plus native gameplay lifecycle smoke passed for 120
  ticks. A visible Sandbox editor PIE lifecycle smoke also passed for 120 ticks.
  The complete native suite covers surviving import, placement, selection,
  save/reload, material, shadow, viewport, and gameplay collision contracts.
- A clean non-incremental Win64 Game Cook published five retained packages,
  including `/Game/Levels/GrayboxStage15`, without TerrainBuild or Terrain
  contributors (3,760,497 changed bytes, zero rollback).
- Maintained source, shaders, configuration, test metadata, and current domain
  documentation have no Terrain, Heightmap, or HeightField references. Remaining
  matches are this removal record, the sequential skeletal-removal plan, and
  historical archives.
- No repository content packages, sidecars, or LFS objects were deleted.
- Full documentation validation passed for 128 current files; lifecycle
  validation passed for all plans (3 active, 1 completed, 327 archived) and
  roadmaps (1 active, 25 archived).
- The two DurinDevTool Python test fixtures were updated to use a surviving
  renderer qualification example. Their standalone pytest suite was not run
  because neither available Python runtime provides pytest; no tool runtime
  implementation changed.

## Execution and Handoff

Follow [agent build/run](../Agents/BuildAndRun.md),
[agent testing](../Agents/Testing.md), and
[agent documentation](../Agents/Documentation.md) workflows. Before content
mutation, read [asset catalog and mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
and [asset packages](../Runtime/Assets/AssetPackages.md).
Use one checkout writer and never overlap build process trees. Each validated
implementation commit updates this plan with evidence and exact Plan/Stage
trailers. Planning validation does not count as code or runtime validation.

## Related Code

- `Engine/Engine.dproject`
- `Engine/Source/Runtime/Engine/{Public,Private}/Terrain`
- `Engine/Source/Runtime/Engine/{Public,Private}/{Actors,Components,Rendering}`
- `Engine/Source/Runtime/Renderer/Private/Renderers`
- `Engine/Source/Runtime/RenderCore/{Public,Private}/Terrain`
- `Engine/Source/Runtime/PhysicsCore` and `Engine/Source/Runtime/Physics`
- `Engine/Source/Developer/TerrainBuild`
- `Engine/Source/Editor/{LevelEditor,AssetForgeBuiltins,DurinEd,MainFrame}`
- `Engine/Source/Programs/DurinAssetTool`
- `Engine/Shaders/Slang/VertexFactory/TerrainVertexFactory.slang`
- `Engine/Tests/Native/{EngineTests,AssetTests,RenderCoreTests}`
- `Sandbox/Content` and `Engine/Content`
