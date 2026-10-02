# glTF Asset Import Refactor Plan

Summary: Introduce a complete GLTFImporter asset workflow over shared format-neutral source data, with multiple mesh outputs and dependency-aware planning as a one-time import tool.

Last reviewed: 2026-10-02

Status: Completed
Completed: 2026-10-02

## Current Status

Stages 0 through 3 are committed as `624a4018f`, `256a0256a`, `e9171670d`
and `32f2873d4`. Stage 4 completes the user-selected one-time import boundary.
The uncommitted import-group prototype is removed. glTF/GLB and FBX imports reject
occupied outputs, while explicit existing-material mapping remains available.
New and legacy model receipts remain readable; their manager, factory and family
reimport routes are disabled, including replacement-file reimport. OBJ and
independent image reimport retain their existing behavior. Source/output update
matching, material-rebuild controls and existing-output replacement paths are removed.

The ordinary glTF ContentBrowser entry remains independent of LevelEditor. It
supports split/combine, resource/category/scene selection, source-detached replanning,
material policies, complete preview, diagnostics, cancellation and per-output results.
Saved mesh/material/texture outputs reload without their physical source files.

Validation on `macos-xcode-arm64` / `MacOS-arm64-Debug-DurinEditor` passes:
`all` build; `SceneImportTests` (31 cases); `AssetImportTests` (22 cases); and
`TextureTests FSingleAssetImportTests.*` (9 cases). Four new one-time import/reload/
replacement-route cases also pass isolated execution with one test job. Changed
Editor/Runtime contracts and this completed plan pass documentation validation.
Native macOS application smoke and GPU qualification were not run; neither is
required for the selected CPU import scope under the repository's default policy.

## Goal

Import one glTF or GLB source into independently usable StaticMesh, Material,
MaterialInstance, and Texture2D assets, with explicit output selection, accurate
resource relationships and useful diagnostics. Users should
not need to choose a Scene Source workflow to obtain materials and textures.

## Scope and Selected Decisions

- GLTFImporter names the format-specific capability; it does not name the shared
  import framework. Internal identifiers follow existing `Gltf` casing.
- glTF and FBX share normalized resource data, output planning, and execution.
  They retain separate parsing, format settings and material
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

### One-time model import boundary

On 2026-10-02 the user narrowed glTF/GLB to a one-time import tool, then
explicitly applied the same boundary to FBX and legacy assets. Reimport,
source relocation, persistent import-group mappings, element matching across
source revisions and whole-source refresh are outside this plan. The previously
selected import-group design is superseded; its uncommitted prototype is removed.

Generated outputs remain independent editable assets. A new import must use
unoccupied destinations; an occupied output reports a conflict instead of
implicitly updating geometry, textures or materials. Explicit existing-material
mapping remains supported and does not modify the mapped asset. After partial
publication, retry into another destination or explicitly remove unwanted outputs.
Keep reflected legacy receipts readable without automatic migration.

## Stage 0 Contracts and Inventory

### Ownership and affected targets

`AssetForgeBuiltins` owns `ImportedScene.cpp`, `GltfSceneAdapter.cpp`,
`AssimpSceneAdapter.cpp`, `AssimpSceneGeometry.cpp`, source snapshots,
`SceneImport.cpp` planning, `SceneDirectImport.cpp` execution, and the standalone
StaticMesh/Texture factories and reimport handlers. Keep the new document,
format adapters, `FAssetImportPlan`, `FAssetImportSession`, and glTF presentation
in that module. These proposed names have no existing declarations in the
workspace. ContentBrowser already provides scoped Import extensions with host
presentation callbacks; add that dependency rather than a new import module.

`LevelEditor` currently registers `level.import-scene` through its workspace and
owns `SceneImportDialog`. `StaticMeshEditor` registers the geometry-only workflow;
`TextureEditor` registers the ordinary image workflow. `DurinEd` owns factory and
reimport dispatch. StaticMesh reimport requires `DStaticMeshImportData` and cannot
currently update `DSceneImportData`; Texture2D reimport accepts base receipts and
can incorrectly interpret a scene root as an image. No Scene reimport handler
exists. Material import provenance lives in Engine's material interface, outside
`DAssetImportData`.

Searches of every source/test root in `Durin.dworkspace` found normalized-scene
consumers only in Engine: AssetForgeBuiltins and `AssetImportTests`. Session
consumers are LevelEditor, `SceneImportTests`, and `SceneImportVulkanTests`.
Sandbox and RoadWeaver have no direct consumers. Shared Engine receipt changes
must also build both projects through the `all` gate. Validate AssetForgeBuiltins,
LevelEditor, ContentBrowser, DurinEd, StaticMeshEditor and TextureEditor through
that build; run AssetImportDataTests, AssetImportTests, SceneImportTests and the
standalone import cases in TextureTests. Vulkan qualification owns rendered PBR
verification when execution access is available.

### Geometry and adapter contract

The canonical engine basis is forward +X, right +Y, up +Z. For new glTF imports,
use `MakeYUpNegativeZForward`: `(x,y,z) -> (-z,x,y)`. glTF lengths are metres;
retain numeric lengths (one engine unit per metre), with no implicit factor of
100. Existing import settings contain axes but no unit multiplier. FBX retains
Assimp's current numeric/unit interpretation and explicit user-selected axes;
unit metadata must not silently change legacy outputs.

Local resource geometry receives the basis exactly once. Node transforms use
`C * T * inverse(C)`; combined expansion applies those canonical transforms to
canonical geometry. Positions retain source origin, normals use inverse transpose,
tangents use the linear transform followed by orthogonalization, tangent signs and
triangle winding track negative determinants. Retain the established UV flip.
Keep glTF source mesh/primitive correlation in adapter-owned explicit references,
not Assimp traversal/material order. FBX Assimp meshes are resource primitives;
retain its node mesh-reference lists, including nodes owning multiple primitives.

The new boundary is `ImportedDocument.h` / `FImportedDocument`, containing mesh
resources with primitives, transform nodes, source scene root sets, materials,
images, bindings, dependencies, and diagnostics. `ImportedScene.h` remains an
explicit legacy flattened adapter while existing standalone/FBX combined callers
are migrated. It is native, not a reflected serialized type. Do not rename any
reflected receipt as part of this source-data migration.

### Matching and persistence decision (superseded)

The original Stage 0 decision proposed generated group/output IDs and a full
mapping snapshot in each output's authored subobject. The user rejected this
complexity on 2026-10-02 and selected one-time glTF import instead. Do not implement
this group record or infer stable identity across reordered/renamed source elements.
Existing receipts remain readable as provenance, not a glTF update contract.

### Compatibility and entrypoint transition

Keep `DSceneImportData`, `Durin.Scene`, `scene:mesh:combined`, surface recipe v1,
and stored source hint formats readable. Do not migrate stored associations.
New split or combined glTF imports require unoccupied destinations and never
replace existing combined outputs or unrelated destination assets.

Register the new glTF/GLB entrypoint with AssetForgeBuiltins independently of a
LevelEditor workspace. The legacy Scene Source menu then accepts FBX only.
Geometry-only remains an explicit glTF setting and retained standalone assets
keep their factory receipt path. Receipt readability does not imply glTF reimport
support. New and legacy glTF assets reject reimport through the manager, factory
methods and family helpers.
OBJ and standalone image formats retain their existing reimport flows; FBX
reimport is also disabled at the user's subsequent explicit request.

### Fixture and acceptance matrix

| Boundary | Existing or added evidence | Later-stage checks |
| --- | --- | --- |
| Multiple meshes/primitives and order | `PrimitiveProjection.gltf`, `MultiSection.gltf` | Split resources, section slots and exact primitive correlation |
| Instances, hierarchy, origin, mirrored/nonuniform scale, multiple scenes | Added `ResourceInstances.gltf`; `AsymmetricAxes.obj` | Local geometry, canonical transforms, selected roots and combined expansion |
| Shared images, usage, sampler and UV | Added `SharedImageUsage.gltf`; `MaterialContract.gltf` | Color/data product separation and binding reuse |
| Embedded/external/data URI source closure | `EmbeddedImage.glb`, `DataUriImage.gltf`, `MaterialContract.gltf` | Missing/unsafe dependency attribution and snapshot changes |
| One-time persistence | Saved glTF outputs and standalone legacy glTF/FBX receipts | Reload without source files, blocked reimport and occupied-destination preservation |
| FBX interpretation | `PhongMaterial.fbx`, `UnsupportedDccMaterial.fbx` and existing frozen assertions | Preserve geometry, diffuse/opacity mapping and loss diagnostics |
| Publication, cancellation, preserved edits | SceneImportTests failure/cancellation cases | Split-output failure states, reload without sources, explicit glTF reimport rejection and collision preservation |
| Unsupported features | Required/optional extension fixtures and generated skins/animation sources | No implicit skeletal/animation promise |

Selection, disabled categories, mapping conflicts, publication races and saved
output reload require session/persistence tests;
source-only fixtures cannot close these acceptance gates.

## Implementation Stages

### Stage 0: Confirm ownership and migration contracts

- [x] Inventory current glTF/FBX adapters, Scene import, standalone geometry import,
  import records, reimport handlers, and ContentBrowser registrations.
- [x] Search all projects in `Durin.dworkspace` for consumers of shared types and
  APIs; record affected owning and consumer targets.
- [x] Confirm shared type names, file/module ownership, importer boundaries, and
  canonical coordinate/unit conventions from current runtime contracts.
- [x] Decide the source element matching policy and persistent import-group mapping
  (superseded by the user-selected one-time import scope).
- [x] Define old-record migration, combined-output preservation, and the transition
  between legacy Scene Source and new glTF asset entrypoints.
- [x] Select existing fixtures and add the missing fixture matrix required by the
  acceptance gates below; characterize FBX behavior before changing shared data.

Completion: contracts and migration choices are recorded here, affected consumers
are known, and no unresolved identity or geometry semantics block Stage 1.

### Stage 1: Separate source resources and instances

Depends on Stage 0.

- [x] Introduce `FImportedDocument` with mesh resources/primitives distinct from nodes.
- [x] Adapt glTF decoding with exact primitive/material mapping, local geometry,
  reusable resources, and retained source transforms.
- [x] Adapt FBX and all other existing shared-data consumers, preserving their
  externally visible behavior through explicit compatibility paths where needed.
- [x] Verify coordinate conversion, units, winding, normals, tangents, UVs, and
  mirrored/nonuniform transforms without double conversion.

Completion: repeated instances do not duplicate source resources, primitives
retain correct materials, and affected consumers pass their selected checks.

### Stage 2: Resolve and execute multiple asset outputs

Depends on Stage 1.

- [x] Replace fixed `scene:mesh:combined` planning with per-source-mesh outputs and
  explicit combined output plans.
- [x] Resolve selected resource dependencies, material policies, texture reuse,
  destinations, existing output matches, and conflicts before product building.
- [x] Extract shared build/materialize/bind/publish responsibilities from the
  existing Scene execution while preserving asynchronous and save ownership.
- [x] Keep FBX import working and preserve legacy combined-output update semantics.
- [x] Provide a complete result with diagnostics and partial publication details.

Completion: split and combined glTF imports produce valid independently usable
assets with correct bindings; conflicts and partial failures are attributable.

### Stage 3: Deliver the ordinary asset import interface

Depends on Stage 2.

- [x] Register glTF/GLB asset import independently of LevelEditor and route ordinary
  glTF import through one complete workflow; offer geometry-only as a setting.
- [x] Provide format-appropriate coordinates, output selection, split/combine,
  material creation/instance/mapping options, and dependency-aware asset preview.
- [x] Show warnings, planned updates, conflicts, progress, cancellation state,
  completion results, and links/selections for created assets.
- [x] Retire the glTF Scene Source user concept while preserving the FBX path
  until it has an explicitly compatible replacement.

Completion: importing a GLB with materials requires no Scene Source selection
and no LevelEditor workspace; preview accurately describes resulting assets.

### Stage 4: Finalize one-time model import boundaries

Depends on Stages 2 and 3. Supersedes the original reimport/persistence stage
following the user's explicit scope decision on 2026-10-02.

- [x] Remove the uncommitted import-group prototype and avoid new group metadata.
- [x] Reject glTF/GLB/FBX destination reuse before building or publication; retain explicit
  existing-material mapping and per-output partial-save reporting.
- [x] Block glTF/GLB/FBX reimport, including legacy assets and replacement-file
  routes, through loaded-object capabilities and direct APIs.
- [x] Verify saved outputs reload and remain usable/editable without source files.
- [x] Document boundaries in owning contracts, validate retained import consumers,
  close acceptance gates and update plan lifecycle metadata.

Completion: one-time imports produce usable independent assets, repeated imports
cannot silently replace them, and required acceptance gates are verified.

## Acceptance and Validation

Follow [native testing guidance](../Agents/Testing.md),
[build guidance](../Agents/BuildAndRun.md), and
[documentation guidance](../Agents/Documentation.md). Shared API changes require
consumer migration and affected project-target validation; shared Engine API
migrations require an `all` build before handoff.

- [x] Multi-mesh and multi-primitive sources produce expected mesh assets,
  sections, and material assignments in split and combined modes.
- [x] Repeated instances, node transforms, mirrored/nonuniform scaling, origins,
  and canonical axes/units remain correct without duplicate resources.
- [x] GLB embedded images, glTF external dependencies, and data URIs work; missing
  or unsafe dependencies fail with attributable diagnostics.
- [x] Shared images with different texture usage, samplers, and material mappings
  yield correct outputs and dependency reuse.
- [x] Selection, unused resources, disabled output categories, existing mappings,
  preflight conflicts, and publication-time races have explicit results.
- [x] Saved outputs reload without source files; repeat imports reject occupied
  outputs, preserve existing edits, and glTF/GLB/FBX reimport is explicitly unsupported.
- [x] Unsupported features/extensions and lossy material conversions are visible;
  skeletal/animation support is not implied by this static-asset scope.
- [x] Cancellation and injected build/save failures report all committed outputs
  and clean up unpublished candidates according to existing operation contracts.
- [x] FBX first import and standalone import consumers retain validated behavior;
  OBJ and independent image reimport remain supported.

## Non-goals

glTF/GLB/FBX reimport, source relocation, import-group records, whole-source refresh,
automatic source-element matching and legacy-record migration;

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
