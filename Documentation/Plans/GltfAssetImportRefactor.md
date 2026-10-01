# glTF Asset Import Refactor Plan

Summary: Introduce a complete GLTFImporter asset workflow over shared format-neutral source data, with multiple mesh outputs, dependency-aware planning, and compatible reimport.

Last reviewed: 2026-10-02

Status: Active
Completed:

## Current Status

Planning only. No implementation stages have started. The selected direction is
ordinary asset import for glTF/GLB, sharing normalized data and asset execution
with FBX where their semantics actually coincide. Scene or Level object creation
is outside this plan. Existing FBX behavior and saved import records must remain
usable during migration.

## Goal

Import one glTF or GLB source into independently usable StaticMesh, Material,
MaterialInstance, and Texture2D assets, with explicit output selection, accurate
resource relationships, useful diagnostics, and repeatable updates. Users should
not need to choose a Scene Source workflow to obtain materials and textures.

## Scope and Selected Decisions

- GLTFImporter names the format-specific capability; it does not name the shared
  import framework. Internal identifiers follow existing `Gltf` casing.
- glTF and FBX share normalized resource data, output planning, and execution.
  They retain separate parsing, format settings, element matching, and material
  interpretation. Do not introduce a generic extensible node framework merely
  to support these two formats.
- Replace the shared `FImportedSceneData` boundary with `FImportedDocument`.
  Preserve source transform nodes without treating them as runtime scene objects.
- Use `FAssetImportPlan` and `FAssetImportSession` for shared multi-asset output
  planning and execution. Avoid collisions with existing AssetTools and DurinEd
  concepts; confirm exact names and ownership in Stage 0.
- Keep the implementation in AssetForgeBuiltins unless the dependency inventory
  demonstrates a concrete need for a separate module. Expose the import UI through
  ContentBrowser extension registration, without requiring a LevelEditor workspace.
- Default to one StaticMesh per source mesh; source primitives become mesh
  sections and material slots. Multiple source nodes referencing one mesh reuse
  the same mesh asset. Combining is an explicit option.
- Import selected content and its dependency closure by default. Include unused
  resources only when requested. Unsupported source content must produce an
  attributable diagnostic rather than an apparent promise of complete support.
- Reuse captured source snapshots, detached mesh/texture builders, PBR material
  conversion, compilation ownership, and protected package persistence.
- Retain current per-package save semantics initially. Report partial publication
  explicitly; cancellation does not imply rollback of already committed packages.

## Data and Responsibility Boundaries

### Format adapters and normalized document

GltfDecoder/GLTFImporter owns GLB chunks, glTF JSON, buffers/accessors, images,
samplers, extensions, and exact primitive/material correlation. The FBX adapter
owns FBX interpretation and the Assimp boundary. Assimp may remain an implementation
detail for geometry decoding; source resource identity must not depend on its
incidental mesh/material ordering.

The shared document contains source mesh resources, their primitives, normalized
materials, images, texture bindings, transform nodes, dependencies, and diagnostics.
Primitive data references materials; texture bindings reference images and retain
sampler/UV semantics. Nodes reference meshes and retain hierarchy and transforms.
Do not represent each node instance as a new mesh resource.

Adapters output a documented canonical coordinate and unit convention, applied
exactly once to geometry and transforms. Individual asset import preserves local
mesh geometry. Combined import expands instances using node transforms within an
explicitly selected source scene/root set. Preserve origin semantics; do not
silently recenter meshes.

Normalize only supported common material semantics. Preserve adapter-owned
provenance/settings for format-specific interpretation and emit diagnostics for
unsupported or lossy mappings. A shared PBR representation must not imply exact
equivalence between FBX material models and glTF metallic/roughness materials.

### Output plan and execution

Each planned output carries a source element reference, output identity, asset
type, destination, dependencies, build policy, and create/update/preserve/conflict
disposition. Preview and execution consume the same resolved plan. Revalidate
source identity and destination occupancy before publication because either can
change after preview.

Texture output reuse considers image identity, usage/color space, and derivation
settings. The same encoded image can require distinct color and data products.
Turning material or texture creation off must select an explicit existing-asset
mapping or documented fallback and cannot leave dangling dependencies.

Separate product building, candidate materialization, dependency binding, and
publication behind the shared session. Keep source work detached and asset
mutation on its existing owning thread. Report warnings before import and show
saved, failed, and unexecuted outputs at completion.

### Import records and updates

Separate relocatable source file hints, source element identity, and destination
asset identity. A source filename must not remain the sole authority for matching
an existing output. glTF element indices and names are not guaranteed stable;
define matching and ambiguity handling rather than claiming universal stable IDs.

Use a Gltf-specific import record derived from existing `DAssetImportData` where
format settings are needed, plus a common source/output association contract.
Integrate material provenance with that association. Determine persistence of
the import-group mapping in Stage 0 before implementing reimport.

Support updating an individual output and refreshing the complete source output
set. Individual updates still decode the source and resolve required dependencies.
Report added, removed, and unmatched elements. Preserve outputs removed from the
source by default; do not automatically delete referenced assets.

Maintain compatible reads and explicit migration for `DSceneImportData`, material
receipts, importer identifiers, existing combined mesh outputs, and old settings.
Renaming reflected types or changing serialized identity requires a migration,
not a source-only rename.

## Implementation Stages

### Stage 0: Confirm ownership and migration contracts

- [ ] Inventory current glTF/FBX adapters, Scene import, standalone geometry import,
  import records, reimport handlers, and ContentBrowser registrations.
- [ ] Search all projects in `Durin.dworkspace` for consumers of shared types and
  APIs; record affected owning and consumer targets.
- [ ] Confirm shared type names, file/module ownership, importer boundaries, and
  canonical coordinate/unit conventions from current runtime contracts.
- [ ] Decide the source element matching policy and persistent import-group mapping.
- [ ] Define old-record migration, combined-output preservation, and the transition
  between legacy Scene Source and new glTF asset entrypoints.
- [ ] Select existing fixtures and add the missing fixture matrix required by the
  acceptance gates below; characterize FBX behavior before changing shared data.

Completion: contracts and migration choices are recorded here, affected consumers
are known, and no unresolved identity or geometry semantics block Stage 1.

### Stage 1: Separate source resources and instances

Depends on Stage 0.

- [ ] Introduce `FImportedDocument` with mesh resources/primitives distinct from nodes.
- [ ] Adapt glTF decoding with exact primitive/material mapping, local geometry,
  reusable resources, and retained source transforms.
- [ ] Adapt FBX and all other existing shared-data consumers, preserving their
  externally visible behavior through explicit compatibility paths where needed.
- [ ] Verify coordinate conversion, units, winding, normals, tangents, UVs, and
  mirrored/nonuniform transforms without double conversion.

Completion: repeated instances do not duplicate source resources, primitives
retain correct materials, and affected consumers pass their selected checks.

### Stage 2: Resolve and execute multiple asset outputs

Depends on Stage 1.

- [ ] Replace fixed `scene:mesh:combined` planning with per-source-mesh outputs and
  explicit combined output plans.
- [ ] Resolve selected resource dependencies, material policies, texture reuse,
  destinations, existing output matches, and conflicts before product building.
- [ ] Extract shared build/materialize/bind/publish responsibilities from the
  existing Scene execution while preserving asynchronous and save ownership.
- [ ] Keep FBX import working and preserve legacy combined-output update semantics.
- [ ] Provide a complete result with diagnostics and partial publication details.

Completion: split and combined glTF imports produce valid independently usable
assets with correct bindings; conflicts and partial failures are attributable.

### Stage 3: Deliver the ordinary asset import interface

Depends on Stage 2.

- [ ] Register glTF/GLB asset import independently of LevelEditor and route ordinary
  glTF import through one complete workflow; offer geometry-only as a setting.
- [ ] Provide format-appropriate coordinates, output selection, split/combine,
  material creation/instance/mapping options, and dependency-aware asset preview.
- [ ] Show warnings, planned updates, conflicts, progress, cancellation state,
  completion results, and links/selections for created assets.
- [ ] Retire the glTF Scene Source user concept while preserving the FBX path
  until it has an explicitly compatible replacement.

Completion: importing a GLB with materials requires no Scene Source selection
and no LevelEditor workspace; preview accurately describes resulting assets.

### Stage 4: Complete reimport and compatible persistence

Depends on Stages 2 and 3 and Stage 0 persistence decisions.

- [ ] Persist source/output associations and import options for all output types.
- [ ] Dispatch glTF-owned reimport to the glTF source path rather than treating
  embedded images as standalone image files or meshes as unrelated geometry.
- [ ] Support source relocation, individual output updates, and whole-source refresh.
- [ ] Handle reordered, renamed, added, removed, and ambiguous source elements with
  explicit matching/conflict feedback and preservation of user edits by policy.
- [ ] Read/migrate legacy receipts and verify existing combined imports remain usable.
- [ ] Document implemented boundaries in the owning Editor/Runtime contracts,
  close acceptance gates with evidence, and update plan lifecycle metadata.

Completion: saved imports remain updateable across reloads and source relocation,
legacy assets remain usable, and every required acceptance gate is verified.

## Acceptance and Validation

Follow [native testing guidance](../Agents/Testing.md),
[build guidance](../Agents/BuildAndRun.md), and
[documentation guidance](../Agents/Documentation.md). Shared API changes require
consumer migration and affected project-target validation; shared Engine API
migrations require an `all` build before handoff.

- [ ] Multi-mesh and multi-primitive sources produce expected mesh assets,
  sections, and material assignments in split and combined modes.
- [ ] Repeated instances, node transforms, mirrored/nonuniform scaling, origins,
  and canonical axes/units remain correct without duplicate resources.
- [ ] GLB embedded images, glTF external dependencies, and data URIs work; missing
  or unsafe dependencies fail with attributable diagnostics.
- [ ] Shared images with different texture usage, samplers, and material mappings
  yield correct outputs and dependency reuse.
- [ ] Selection, unused resources, disabled output categories, existing mappings,
  preflight conflicts, and publication-time races have explicit results.
- [ ] Reimport after reload, relocation, material edits, element reorder/add/remove,
  and legacy-record migration preserves intended asset references and settings.
- [ ] Unsupported features/extensions and lossy material conversions are visible;
  skeletal/animation support is not implied by this static-asset scope.
- [ ] Cancellation and injected build/save failures report all committed outputs
  and clean up unpublished candidates according to existing operation contracts.
- [ ] FBX and standalone import consumers retain validated behavior throughout migration.

## Non-goals

Creating Actors, Levels, or runtime scene hierarchies; complete skeletal/animation
import; expanding every glTF extension; replacing the underlying mesh/texture build
systems; introducing a universal plugin-based node graph; automatic deletion of
obsolete outputs; and all-or-nothing rollback of already committed package saves.

## Required Contract References

- [Module ownership](../Workspace/CodeModules.md)
- [Async asset operations](../Editor/Architecture/AsyncAssetOperations.md)
- [Static mesh building](../Runtime/Assets/StaticMeshBuilding.md)
- [Asset packages](../Runtime/Assets/AssetPackages.md)
- [Asset data lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
