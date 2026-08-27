# Scene And Import Workflow Simplification Plan

Summary: Replace Scene's generic orchestration and mounted-source editor workflows with private ordering and direct built-in dispatch

Last reviewed: 2026-08-27

Status: Completed
Completed: 2026-08-27

## Current Status

Milestones A and B of the Asset Import Simplification roadmap and every stage
of this plan are complete. Scene now captures and translates one immutable source
closure, constructs typed products in a private stable topological order, and
publishes its peer packages directly. The production Scene path no longer
creates a generic request, graph, planning pass, job, provider registration, or
replay record. Physical deletion continues in the
[AssetForge Framework Removal Plan](AssetForgeFrameworkRemoval.md).

The central cutover surface is 3,089 lines across `SceneImportProvider.cpp`,
its schema, `SceneImport.cpp`, the Scene dialog, generic import-dialog support,
and mounted-source relocation. The provider/schema accounted for 745 lines;
all of those production call paths are now removed.

The direct path retains the normalized imported-scene values and typed product
builders, performs collision preflight before materialization, binds textures,
materials, skeletons, skeletal meshes, and animations before one atomic package
save, and publishes concrete Texture2D import data for embedded Scene images.
Scene and Texture dialogs now retain ordinary physical filenames without source
copies or mount destinations. Content Browser Import/Reimport routing comes
from a four-entry compile-time built-in table and direct module calls; generic
import operation presentation, pumping, relocation, and replacement adapters
have no editor caller.
SceneImportTests pass 4/4 and SkeletalSceneLifecycleTests pass 1/1. The Vulkan
qualification reaches successful direct import, unload, and reload; its current
host run then fails the existing missing `VK_KHR_surface` validation condition
before render capture. This host limitation is independent of the import
cutover and is recorded rather than treated as a product regression.

Final qualification passed SceneImportTests 4/4,
SkeletalSceneLifecycleTests 1/1, TextureTests 75/75, MaterialTests 100/100,
EditorAssetWorkflowTests 33/33, ContentBrowserWorkflowTests 60/60,
ThumbnailTests 58/58, AssetPackageTests 125/125, AssetCookTests 13/13, and
AssetImportDataTests 4/4. The default Editor `all` build and hidden 8-tick
startup/normal-exit smoke passed; macOS has no registered Game preset, so its
selected host-profile runtime evidence is Cook coverage plus module and
dependency-boundary searches. Milestone C changed 62 files, deleted 12 files,
and removed a net 2,307 lines before final documentation updates.

## Goal

Make Scene a creation-only built-in importer with private transient dependency
ordering, then make DurinEd dispatch directly to the finite built-in importer
set using regular file selection and common source inspection.

## Scope

- Replace Scene's public source/build graphs, planning passes, provider, and
  generic job with private capture, translation, ordering, and publication.
- Preserve glTF/GLB/FBX outputs, atomic multi-package publication, diagnostics,
  cancellation at safe boundaries, and creation-only semantics.
- Replace mounted-source ingestion in Scene and the remaining editor dialogs
  with normalized project-relative or absolute filenames.
- Replace registry-driven editor dispatch, operation-history assumptions, and
  source capability queries with finite built-in routing.
- Remove the last production callers of generic AssetForge orchestration so the
  framework-removal milestone can physically delete it.

## Non-Goals

- Whole-scene reimport, generated-output ownership records, reconciliation, or
  tombstone cleanup.
- A new public scene graph, importer registry, generic async operation, or
  extensibility protocol.
- Physical deletion of every AssetForge framework type; the next roadmap
  milestone owns final framework removal after this caller cutover.
- Changing normalized mesh, skeletal, animation, material, or texture build
  products and runtime payload formats.

## Design Decisions and Invariants

- Scene owns one private immutable captured source closure and one private
  dependency order; neither is reflected, persisted, or exported by AssetForge.
- Every source is captured once. Recognition, dependency discovery, decoding,
  hashes, and output construction consume the same owned bytes.
- Scene remains creation-only and publishes ordinary peer assets. No aggregate
  Scene asset or replay record is introduced.
- All failable decode/build validation completes before the non-cancelable
  publication boundary. Multi-package save failure preserves valid Dirty state
  or restores the complete pre-publication transaction as currently selected.
- DurinEd uses a compile-time finite descriptor table only for labels,
  extensions, and direct dispatch; it does not recreate registration, leases,
  or hot-unload semantics.
- Source selection never copies, relocates, replaces, or deletes user files.

## Current Foundations and Gaps

| Area | Foundation | Cutover gap |
| --- | --- | --- |
| Scene decode | Qualified glTF/GLB/FBX capture and normalized output builders | Expressed through public graphs/provider schemas |
| Publication | Atomic peer-output construction and dependency binding | Driven by generic job/editor rounds |
| Editor dispatch | Existing dialogs, Content Browser actions, and diagnostics | Registry/capability and operation-handle assumptions |
| Source policy | Qualified common filename normalization for all standalone families | Scene still ingests into mounted content |
| Framework | No standalone production caller remains | Scene prevents physical framework removal |

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Inventory Scene graph/provider/job contracts, editor registry/operation
  callers, mounted-source transactions, tests, and module dependencies.
- [x] Freeze the private Scene capture/order model, direct UI descriptor table,
  cancellation/publication boundary, and retained diagnostics.
- [x] Record baseline files, lines, symbols, and exact deletion/handoff list for
  the framework-removal milestone.

#### Acceptance Gate

- Scene and editor workflow ownership, ordering, failure semantics, caller map,
  deletion boundary, and validation selections have no unresolved decision.

### Stage 1: Privatize Scene orchestration

Dependencies: Stage 0 complete.

- [x] Move source capture, dependency discovery, normalized translation, output
  planning, build preparation, and topological ordering behind private Scene
  values and functions.
- [x] Publish the existing peer output set atomically without `FSourceGraph`,
  `FBuildGraph`, planning passes, component leases, or provider selection.
- [x] Preserve bounded cancellation, diagnostics, collision checks, rollback,
  skeletal relationships, and material/texture dependencies.

#### Acceptance Gate

- Scene creation passes its focused suite and no production Scene path uses a
  generic graph, provider, planning pass, component registry, or replay model.

### Stage 2: Cut over editor dispatch and source workflows

Dependencies: Stage 1 complete.

- [x] Replace mounted Scene ingestion with direct regular-file capture and
  normalized filenames while preserving dependency containment and limits.
- [x] Replace DurinEd registry/capability dispatch with a finite built-in
  descriptor table and direct family calls.
- [x] Remove shared-source repair, replacement, relocation, and generic import
  operation UI that has no remaining product requirement.

#### Acceptance Gate

- Scene, dialogs, Content Browser actions, and source inspection require no
  mounted-source mutation or generic import operation/registry workflow.

### Stage 3: Qualify and hand off framework removal

Dependencies: Stage 2 complete.

- [x] Run focused Scene, skeletal, material, texture, editor-workflow,
  package/Cook, failure, cancellation, and source-policy tests.
- [x] Pass the default Editor build, hidden-window smoke, runtime/Cook closure,
  content compatibility audit, and boundary searches.
- [x] Record reduction evidence, update lasting documentation and the roadmap,
  complete this plan, and create the framework-removal child plan.

#### Acceptance Gate

- Milestone C behavior, deployment, removal searches, documentation validators,
  and the entry gate for physical framework removal all pass.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Scene outputs | Static/skeletal mesh, Skeleton, AnimationClip, materials, textures, embedded and external dependencies |
| Ordering | Stable private dependency order, collisions, cycles, and bounded resource failure |
| Transactions | Cancellation and failure preserve all-or-nothing peer publication and save semantics |
| Source policy | Project-relative/external absolute roots and dependencies; no source mutation |
| Editor | Direct dialog dispatch, Content Browser actions, progress/diagnostics, shutdown |
| Deployment | Cooked/runtime closure has no editor import or offline decoder dependency |
| Removal | No production registry, graph, provider, generic job, replay, or mounted-source caller remains |

## Definition of Done

- Scene uses private transient orchestration and publishes ordinary peer assets
  exactly once without generic AssetForge protocols.
- DurinEd dispatches directly to the finite built-in importer set and all user
  source selection uses normalized physical filenames without mutation.
- Focused and aggregate tests, Editor build/smoke, Cook/runtime closure,
  compatibility audit, boundary searches, and documentation validation pass.
- The roadmap activates the physical AssetForge framework-removal plan.

## Deferred Follow-ups

- Execute the active
  [AssetForge Framework Removal Plan](AssetForgeFrameworkRemoval.md).

## Related Documentation

- [Asset Import Simplification Roadmap](../Roadmaps/AssetImportSimplification.md)
- [Asset Import Architecture](../Editor/Architecture/AssetImportFramework.md)
- [Source File Workflows](../Editor/Guides/SourceFileWorkflows.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Agent Build And Run](../Agents/BuildAndRun.md)
- [Agent Testing](../Agents/Testing.md)

## Related Code

- [`SceneImport.cpp`](../../Engine/Source/Editor/AssetForgeBuiltins/Private/SceneImport.cpp)
- [`SceneDirectImport.cpp`](../../Engine/Source/Editor/AssetForgeBuiltins/Private/SceneDirectImport.cpp)
- [`ImportDialogSupport.cpp`](../../Engine/Source/Editor/DurinEd/Private/Import/ImportDialogSupport.cpp)
- [`SourceFilename.h`](../../Engine/Source/Runtime/Engine/Public/Asset/SourceFilename.h)
