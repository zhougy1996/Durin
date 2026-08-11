# Editor Asset Support Namespace Refactor Plan

Summary: Move shared editor asset retention, open-compatibility, and source-management contracts into `Durin::Editor` without changing asset or transaction behavior.

Last reviewed: 2026-08-11

Status: Completed
Completed: 2026-08-11

## Current Status

All stages are complete. Asset retention now uses `Editor::FRetainedAsset` and
`Editor::FAssetRetentionService` from `AssetRetention.h/.cpp`; workspace open
compatibility, source reference indexing, and mounted-source relocation now
live in the flat `Durin::Editor` namespace. All source and native-test consumers
use the new owner directly, with no aliases or forwarding headers, and no
runtime module consumes the migrated contracts.

Validation passed on the `windows-msvc-x64` / `Win64-Debug-DurinEditor`
profile: `EditorAssetWorkflowTests` (80 passed, 1 skipped), `MaterialTests`
(78/78), `ThumbnailTests` (53/53), `SceneImportVulkanTests` (1/1), the complete
native-test suite at default target granularity, a full `all` build, and a
hidden Sandbox startup that exited normally after two ticks. Source and
non-archived-documentation searches found no stale consumer or compatibility
surface.

Preview scenes and thumbnail contracts remain explicitly deferred because they
add renderer-thread, module-unload, and GPU lifetime validation surfaces that
are not required for this ownership cleanup.

## Goal

- Complete the `Durin::Editor` ownership boundary for shared editor-only asset services.
- Remove namespace-redundant `Editor` stems from asset-retention type and file names.
- Preserve asset loading, retention coalescing, compatibility rejection,
  source-index invalidation, package mutation, rollback, and transaction behavior.
- Migrate every repository consumer without root-namespace aliases or forwarding headers.

## Scope

- `DurinEd` asset retention and workspace asset-open compatibility contracts.
- `DurinEd` source reference indexing and mounted-source relocation contracts.
- MaterialEditor, TextureEditor, StaticMeshEditor, SkeletalMeshEditor,
  LevelEditor, thumbnail-provider, and native-test consumers.
- Direct public include names and non-archived documentation that names the migrated APIs.

## Non-Goals

- Moving `FPreviewScene` or any thumbnail request, provider, cache, service, or pipeline API.
- Moving `AssetImportCore` contracts out of `Durin::AssetImport`.
- Changing reflected asset types, package formats, asset paths, source paths,
  registry schemas, transaction descriptions, or persistent data.
- Introducing `Durin::Editor::Asset` or `Durin::Editor::Source` subnamespaces.
- Adding compatibility aliases, deprecated spellings, or forwarding headers.
- Changing loading, error handling, rollback, or thread-ownership behavior.

## Design Decisions and Invariants

- Shared contracts live directly in `Durin::Editor`, matching transactions,
  property editing, workspaces, interaction services, and compatibility audits.
- `FRetainedEditorAsset` becomes `FRetainedAsset`, preserving the handle's
  retained-value meaning while removing the namespace-redundant stem. Its
  owning service becomes `FAssetRetentionService`.
- `EditorAssetRetention.h/.cpp` becomes `AssetRetention.h/.cpp`. The old files
  are removed after all consumers migrate.
- `FWorkspaceAssetOpenCompatibility`, `FSourceReference`,
  `FSourceReferenceIndex`, `FMountedSourceRelocationRequest`, and
  `RelocateMountedSourceAcrossPackages()` retain their names because their
  remaining qualifiers are responsibility-bearing rather than namespace-redundant.
- Asset retention remains game-thread owned and continues to coalesce handles
  by canonical virtual asset identity. Releasing the final handle releases the
  retained object and removes the expired weak entry.
- Workspace compatibility remains synchronous and request-scoped. It rejects
  incompatible loads before document activation and releases only packages
  introduced by that request.
- Source indexing remains a bounded registry-revision snapshot. Refresh,
  explicit invalidation, inspection limits, warnings, and no-load/no-PostLoad
  behavior remain unchanged.
- Mounted-source relocation remains one recoverable editor transaction across
  the supplied packages. Validation occurs before publication, and any failure
  preserves the original source and package state.
- Existing strings, diagnostics, callback order, limits, path normalization,
  and serialization remain byte-for-byte or semantically unchanged as applicable.

## Current Foundations and Gaps

- `Editor::FAssetCompatibilityAuditModel` already establishes `Durin::Editor`
  ownership for shared editor asset compatibility policy.
- Asset retention is editor-only but currently exposes
  `FRetainedEditorAsset` and `FEditorAssetRetentionService` in `Durin`.
- Workspace open compatibility is consumed by LevelEditor and four concrete
  asset editors but remains in the root namespace.
- Source reference indexing and mounted-source relocation are consumed by
  TextureEditor and `EditorAssetWorkflowTests` but remain in the root namespace.
- Retention behavior is exercised by material, thumbnail, and Vulkan-backed
  scene-import tests; compatibility and source behavior are owned by
  `EditorAssetWorkflowTests`.

## Implementation Stages

### Stage 0: Select ownership, vocabulary, and validation surfaces

- [x] Inventory source, test, and non-archived documentation consumers.
- [x] Select the flat `Durin::Editor` boundary and concise retention names.
- [x] Keep compatibility and source-management responsibility names unchanged.
- [x] Defer PreviewScene and thumbnail contracts to a separate plan.

#### Acceptance Gate

- Every migrated symbol has one selected owner and spelling.
- The scoped validation targets cover asset retention, compatibility, source
  indexing, relocation, and representative renderer-backed consumers.
- No reflected or serialized contract requires a compatibility layer.

### Stage 1: Asset retention and open compatibility

- [x] Rename `EditorAssetRetention.h/.cpp` to `AssetRetention.h/.cpp`.
- [x] Move retention contracts and implementation into `Durin::Editor` and
  shorten the retained handle and service names.
- [x] Move `FWorkspaceAssetOpenCompatibility` into `Durin::Editor` without renaming it.
- [x] Update concrete asset editors, preview/thumbnail providers, LevelEditor,
  and native-test consumers.
- [x] Confirm canonical coalescing, final-handle release, and introduced-package
  release behavior remain covered by existing tests.

#### Acceptance Gate

- `EditorAssetWorkflowTests` passes its compatibility coverage.
- `MaterialTests`, `ThumbnailTests`, and `SceneImportVulkanTests` pass their
  retention and renderer-backed integration coverage.
- Old retention filenames and root-namespace symbols have no source or test consumers.

### Stage 2: Source indexing and mounted-source relocation

- [x] Move `FSourceReference`, `FSourceReferenceIndex`,
  `FMountedSourceRelocationRequest`, and `RelocateMountedSourceAcrossPackages()`
  into `Durin::Editor`.
- [x] Update TextureEditor and native-test consumers with direct qualification.
- [x] Preserve registry-revision caching, explicit invalidation, inspection
  bounds, package ordering, transaction ownership, and rollback semantics.
- [x] Confirm no runtime module consumes the migrated editor-only contracts.

#### Acceptance Gate

- `EditorAssetWorkflowTests` passes its source-index and relocation coverage.
- Searches find no migrated source-management symbol in the `Durin` root namespace.
- No compatibility alias or forwarding header exists.

### Stage 3: Documentation and final validation

- [x] Update lasting editor architecture documentation only where it names migrated contracts.
- [x] Search source, tests, and non-archived documentation for old filenames and symbols.
- [x] Run every focused target in the validation matrix.
- [x] Run the complete native-test suite because the public migration crosses test targets.
- [x] Complete a full editor `all` build because public editor DLL boundaries
  and user-visible editor modules change.
- [x] Run a hidden Sandbox startup/exit smoke using the same Agent Build Profile.
- [x] Validate all plans and record final evidence.

#### Acceptance Gate

- Every validation-matrix entry passes.
- The full editor build and startup smoke pass from the same profile.
- No stale active documentation, old include, compatibility alias, or forwarding header remains.

## Validation Matrix

| Surface | Validation |
| --- | --- |
| Workspace compatibility, source index, relocation | `EditorAssetWorkflowTests` |
| Asset retention and material preview | `MaterialTests` |
| Retention in shared rendered-thumbnail fixtures | `ThumbnailTests` |
| Retention in Vulkan-backed scene import | `SceneImportVulkanTests` |
| Cross-target public API migration | Complete native-test suite at default target granularity |
| Editor DLL ABI and dynamic module consumers | Full editor `all` build |
| MainFrame, default workspace, and startup integration | Hidden Sandbox startup/exit smoke |
| Plan lifecycle and links | All-plan validator |

## Definition of Done

- Asset retention, workspace open compatibility, source reference indexing,
  and mounted-source relocation live in `Durin::Editor`.
- Retention uses `FRetainedAsset`, `FAssetRetentionService`, and
  `AssetRetention.h/.cpp` without old spellings.
- Compatibility and source-management behavior, limits, diagnostics,
  transactions, rollback, and persistence remain unchanged.
- All repository consumers migrate without aliases or forwarding headers.
- Focused tests, complete native tests, full editor build, startup smoke, and
  plan validation pass with recorded evidence.

## Deferred Follow-ups

- Move `FPreviewScene` into the editor ownership boundary with renderer-lifecycle validation.
- Migrate thumbnail contracts into `Durin::Editor` and decompose the monolithic
  public request/provider/scheduler header by responsibility.
- Review module-specific namespaces for concrete editor implementations after
  shared `DurinEd` contracts are complete.

## Related Documentation

- [Editor Common Namespace Refactor](EditorCommonNamespaceRefactor.md)
- [Editor Workspace Namespace Refactor](EditorWorkspaceNamespaceRefactor.md)
- [Editor Interaction Namespace Refactor](EditorInteractionNamespaceRefactor.md)
- [Asset Thumbnails](../Editor/Architecture/AssetThumbnails.md)
- [C++ Coding Standards](../Development/Standards/CodingStandards.md)

## Related Code

- `Engine/Source/Editor/DurinEd/Public/Asset/AssetRetention.h`
- `Engine/Source/Editor/DurinEd/Public/Asset/WorkspaceAssetOpenCompatibility.h`
- `Engine/Source/Editor/DurinEd/Public/Source/SourceReferenceIndex.h`
- `Engine/Source/Editor/DurinEd/Public/Source/MountedSourceRelocation.h`
- `Engine/Source/Editor/LevelEditor`
- `Engine/Source/Editor/MaterialEditor`
- `Engine/Source/Editor/TextureEditor`
- `Engine/Source/Editor/StaticMeshEditor`
- `Engine/Source/Editor/SkeletalMeshEditor`
- `Engine/Tests/Native/EngineTests`
