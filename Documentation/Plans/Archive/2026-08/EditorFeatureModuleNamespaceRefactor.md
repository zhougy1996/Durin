# Editor Feature Module Namespace Refactor Plan

Summary: Move MainFrame, LevelEditor, and concrete asset-editor implementation APIs into feature-owned `Durin::Editor` subnamespaces while preserving module, reflection, persistence, UI identity, and runtime behavior.

Last reviewed: 2026-08-12

Status: Archived
Completed: 2026-08-12

## Current Status

Shared editor infrastructure now lives in the flat `Durin::Editor` namespace:
transactions, property editing, workspaces, interaction, asset support, preview
scenes, and thumbnail architecture have completed their public migrations.
The next ownership gap is the concrete editor layer. MainFrame, LevelEditor,
MaterialEditor, TextureEditor, StaticMeshEditor, and SkeletalMeshEditor still
declare almost all module-owned public and private C++ types directly in the
`Durin` root namespace.

The six modules contain approximately 176 repository-owned C++ headers and
sources. LevelEditor accounts for most of that surface and is also touched by
the active Native Graybox Scene Authoring plan. This plan therefore starts with
the smaller host and asset-editor boundaries, keeps every stage independently
buildable, and migrates LevelEditor only after re-inventorying the symbols added
or changed by active feature work.

Stages 0 through 3 are complete. The repository-wide classification selects
the owning feature namespace for every ordinary type and retains the existing
name except for the explicit public map below. Stable runtime strings remain
covered by the existing workspace/bootstrap/provider tests. MainFrame now lives
in `Durin::Editor::MainFrame`; `DEditorEngine` loads `IMainFrameModule`, and
the interface and host-settings files use responsibility-matched names.
`EditorShellTests` passes 34 tests, `ExternalToolTests` passes 5 tests, the
MainFrame target builds, and a hidden Sandbox startup/exit smoke passes.
MaterialEditor and TextureEditor ordinary APIs now live in their selected
feature namespaces; shared Editor references use explicit parent qualification,
while `DTextureCubePreviewComponent` retains its reflected root identity.
`MaterialThumbnailTests` passes 6 tests, `TextureThumbnailTests` passes 7 tests,
`EditorPropertyTests` passes 27 tests, `EditorRenderingTests` passes 39 tests,
and `MaterialTests` passes 78 tests.
StaticMeshEditor and SkeletalMeshEditor ordinary APIs now live in their selected
feature namespaces, including inspectors, preview controllers, thumbnail
contracts/providers. Their explicit module entry classes remain in `Durin`.
`StaticMeshThumbnailTests` passes
8 tests, `StaticMeshTests` passes 52 tests, `SkeletalMeshEditorTests` passes 3
tests, and the post-migration `EditorRenderingTests` run passes 39 tests.
LevelEditor public contracts and private implementation now live in
`Durin::Editor::Level`; the former workspace/helper sibling namespaces
fold into `Workspace` and `Helpers`.
Runtime/reflected types and runtime enums remain rooted in `Durin`. The
LevelEditor and MainFrame targets build, while `EditorAssetWorkflowTests`,
`LevelAuthoringTests`, `ViewportTests`, `EditorShellTests`, `StaticMeshTests`,
`ThumbnailTests`, `AssetReferenceStoreTests`, and `EditorHierarchyTests` pass
their post-migration focused runs.

Stage 6 is complete. The owning workspace, Content Browser, and viewport
architecture documents record the feature namespace topology and reflected-type
exceptions. Stale-name searches find no production consumer of the old module
types or ad-hoc LevelEditor namespaces, the complete native-test suite passes at
default target granularity, the full editor `all` build links successfully, and
the hidden two-tick Sandbox startup/exit smoke exits normally from the same
`windows-msvc-x64` Agent Build Profile.

A post-completion vocabulary review removed the redundant trailing `Editor`
from the four asset-domain namespace segments. Their C++ owners are now
`Editor::Material`, `Editor::Texture`, `Editor::StaticMesh`, and
`Editor::SkeletalMesh`; physical module names, lookup strings, directories,
persisted identities, and reflected types remain unchanged. `MainFrame` retains
its role-bearing name. The four focused editor targets, the complete native-test
suite, full `all` build, hidden Sandbox startup/exit smoke, and all-plan
validator pass after the refinement.

A second post-completion vocabulary review applies the same redundancy rule to
the level-editing domain: its C++ owner is now `Editor::Level`, while the
physical `LevelEditor` module name, directory, lookup string, persisted
identities, and level-editor type names remain unchanged.

Concrete module entry classes are the deliberate root-owned exception to the
feature namespace rule. `FMainFrameModule`, `FLevelEditorModule`,
`FMaterialEditorModule`, `FTextureEditorModule`, `FStaticMeshEditorModule`, and
`FSkeletalMeshEditorModule` remain in `Durin`, aligned with the other engine
module entry points and their physical module identities. Their business APIs
remain under the selected `Durin::Editor` feature owners.

The selected destination is one feature subnamespace per module:
`Durin::Editor::MainFrame`, `Durin::Editor::Level`,
`Durin::Editor::Material`, `Durin::Editor::Texture`,
`Durin::Editor::StaticMesh`, and
`Durin::Editor::SkeletalMesh`. Runtime/reflected object types remain in
`Durin`; shared DurinEd contracts remain directly in `Durin::Editor`.

## Goal

- Make the C++ namespace hierarchy express the existing editor module ownership.
- Remove concrete editor implementation types from the `Durin` root namespace,
  except for explicitly named physical module entry classes.
- Give MainFrame and each feature editor a closed public/private vocabulary
  without mixing feature-owned types into shared `Durin::Editor` infrastructure.
- Shorten only names whose complete feature meaning is supplied by the new
  namespace, while retaining names that distinguish multiple responsibilities.
- Preserve editor startup, workspace registration, document lifecycle,
  authoring, rendering, import, thumbnail, unload, and UI behavior.

## Scope

- MainFrame public module interface, bootstrap values, implementation, project
  browser, compatibility window, profiling service, and host settings.
- MaterialEditor, TextureEditor, StaticMeshEditor, and SkeletalMeshEditor module
  entry points, workspaces, widgets, previews, and concrete thumbnail providers.
- LevelEditor public extension contracts and all module-owned private
  implementation types, including workspace, viewport, panels, documents,
  Content Browser, authoring, import dialogs, settings, and customizations.
- DurinEd editor-engine forward declarations and module loading, MainFrame
  composition, native tests, and non-archived documentation that name migrated
  C++ APIs.
- Responsibility-matched source/header renames only where an old filename
  becomes misleading after an approved public type rename.

## Non-Goals

- Moving `DEditorEngine`, `GEditor`, `DTextureCubePreviewComponent`, or any
  other reflected/runtime object type out of `Durin`; reflection-generated
  identities and runtime class lookup remain unchanged.
- Moving shared DurinEd contracts into feature subnamespaces or introducing a
  deeper namespace under shared transactions, workspaces, previews, assets, or
  thumbnails.
- Renaming DLL/module targets, module lookup strings, workspace type strings,
  document keys, asset routes, provider names, settings files, configuration
  keys, ImGui labels/IDs, command names, log categories, or serialized data.
- Changing feature behavior, dependency direction, registration order,
  startup/shutdown order, thread ownership, persistence formats, or rendering.
- Reorganizing directories solely to mirror namespaces; existing module and
  responsibility directories remain authoritative.
- Adding root-namespace aliases, forwarding facades, deprecated spellings, or
  compatibility headers for migrated APIs.
- Folding active Graybox feature implementation into this namespace plan. New
  Graybox types are migrated as consumers when LevelEditor reaches its stage;
  their behavior and acceptance gates remain owned by the Graybox plan.
- Migrating AssetImportCore or StandardAssetImport. They already express a
  separate import-domain ownership model and require an independent review.

## Design Decisions and Invariants

### Namespace topology

- Shared cross-feature editor infrastructure remains directly in
  `Durin::Editor`.
- Concrete module-owned types move to exactly one of the six
  `Durin::Editor::<Feature>` namespaces selected above.
- A feature subnamespace may refer to its shared parent contracts without
  aliases. Cross-feature use must use the full owning qualification and must
  not be hidden behind `using namespace` directives in public headers.
- Existing ad-hoc namespaces such as `LevelEditorWorkspace`,
  `MaterialEditorWorkspace`, `TextureEditorWorkspace`,
  `StaticMeshEditorWorkspace`, `SkeletalMeshEditorWorkspace`, and
  `LevelEditorHelpers` fold into their owning feature namespace. Internal
  helper grouping may use a nested `Private` or responsibility namespace only
  when it prevents an actual collision; it is not a second public API layer.
- Anonymous namespaces remain the owner of translation-unit-local helpers.

### Naming policy

- Public feature interfaces shorten after qualification; concrete physical
  module entry classes stay explicitly named in `Durin`.
- MainFrame bootstrap names remove the redundant `Editor` stem.
- To keep the broad migration reviewable, all other public and private type
  names remain unchanged in this plan. Further vocabulary shortening requires
  a separate ownership review after namespace migration rather than being
  inferred by mechanical replacement.
- Concrete provider names retain asset-kind distinctions when a feature owns
  more than one provider. Texture2D and TextureCube names therefore remain
  distinct even inside `Editor::Texture`.
- Reflected `D` types keep their existing C++ names and namespace. Feature code
  refers to them through their stable `Durin` ownership.

The complete public rename map is intentionally small:

| Current | Selected |
| --- | --- |
| `Durin::IMainFrameModule` | `Durin::IMainFrameModule` |
| `Durin::FMainFrameModule` | `Durin::FMainFrameModule` |
| `EEditorBootstrapState` | `Editor::MainFrame::EBootstrapState` |
| `EEditorDefaultDocumentState` | `Editor::MainFrame::EDefaultDocumentState` |
| `EEditorBootstrapStepStatus` | `Editor::MainFrame::EBootstrapStepStatus` |
| `FEditorBootstrapProgress` | `Editor::MainFrame::FBootstrapProgress` |
| `GetEditorBootstrapPhaseIndex()` | `Editor::MainFrame::GetBootstrapPhaseIndex()` |
| `GetEditorBootstrapStepStatus()` | `Editor::MainFrame::GetBootstrapStepStatus()` |
| `IsValidEditorBootstrapTransition()` | `Editor::MainFrame::IsValidBootstrapTransition()` |
| `FLevelEditorModule` | `Durin::FLevelEditorModule` |
| `FMaterialEditorModule` | `Durin::FMaterialEditorModule` |
| `FTextureEditorModule` | `Durin::FTextureEditorModule` |
| `FStaticMeshEditorModule` | `Durin::FStaticMeshEditorModule` |
| `FSkeletalMeshEditorModule` | `Durin::FSkeletalMeshEditorModule` |

MainFrame's private `FMainFrameBootstrapContext` and `FEditorHostSettings`
become `FBootstrapContext` and `FHostSettings`. Other ordinary types retain
their spelling and move under their selected owner. The reflected
`DTextureCubePreviewComponent` is the explicit exception and stays in `Durin`.

### Binary, module, and lifecycle invariants

- `IMPLEMENT_MODULE` continues to export the same `InitializeModule` ABI and
  the module manager continues to load the exact strings `MainFrame`,
  `LevelEditor`, `MaterialEditor`, `TextureEditor`, `StaticMeshEditor`, and
  `SkeletalMeshEditor`.
- DLL export coverage remains on the same cross-module functions,
  constructors, and destructors. Namespace movement must not accidentally turn
  a formerly linked symbol into a header-only or unexported contract.
- MainFrame retains its current bootstrap state machine, first-present gate,
  workspace/default-document progression, rollback behavior, and reverse-order
  shutdown.
- Concrete editor modules retain atomic workspace/asset-route/provider
  registration and provider-before-workspace teardown.
- No object, callback, provider, workspace, preview scene, or UI texture may
  outlive its current module-registration handle or shutdown boundary.

### Persistent and visible identity invariants

- Namespace and C++ symbol changes never feed persisted or user-visible identity.
- Workspace type IDs, document keys, asset class routes, singleton policies,
  provider names, thumbnail generator versions, and cache keys remain byte-for-byte stable.
- Window names, dock-space names, panel names, layout versions, ImGui hash
  inputs, settings filenames, YAML keys, recent-project data, and command-line
  command names remain byte-for-byte stable.
- Existing UI text, diagnostics, logs, callback ordering, selection behavior,
  transactions, undo/redo, and save/dirty-state semantics remain unchanged.

### Source migration policy

- Every repository consumer migrates in the same stage as its owner; no root
  alias or forwarding surface may conceal an incomplete migration.
- Each stage starts with a fresh symbol/include inventory because active plans
  may add feature-owned types after this plan was authored.
- Namespace-only edits and approved responsibility renames are separated from
  behavioral edits. Any required behavior correction is documented as a plan
  decision before implementation or handled in its owning plan.
- Module registration macros and exported entry classes remain root-owned;
  persisted strings are reviewed explicitly rather than changed by unbounded
  search-and-replace.

## Current Foundations and Gaps

- DurinEd's reusable services now present a coherent `Durin::Editor` boundary,
  so concrete feature namespaces can depend inward on shared contracts without
  creating a parallel abstraction.
- MainFrame still exposes bootstrap enums, progress values, and its module
  interface in `Durin`; `DEditorEngine` loads that root-owned interface.
- The four asset editor modules expose root-owned module and concrete thumbnail
  provider types even though their shared provider interfaces now live in
  `Durin::Editor`.
- LevelEditor exposes module, selection, viewport editing/picking, transform
  targets, customizations, authoring, details, and workspace types in `Durin`.
  Its private implementation also contains multiple ad-hoc sibling namespaces
  that encode responsibility inconsistently.
- `DTextureCubePreviewComponent` is the only reflected type in the selected six
  module roots. It must remain in `Durin` while ordinary TextureEditor types
  move around it.
- Existing tests cover MainFrame bootstrap transitions, workspace identity,
  Content Browser behavior, viewport editing and picking, level authoring,
  reflected details, feature workspaces, previews, concrete thumbnail
  providers, module registration, unload, and renderer-backed publication.
- The active Native Graybox Scene Authoring plan owns ongoing LevelEditor
  behavior. LevelEditor migration must use its stage-entry source state rather
  than the inventory captured on 2026-08-12.

## Implementation Stages

### Stage 0: Freeze the migration map and stable identities

- [x] Inventory every declaration, definition, forward declaration, module
  loader, export annotation, include, test, and non-archived documentation
  consumer in the six selected modules.
- [x] Classify each symbol as shared Editor infrastructure, feature-owned
  ordinary C++, reflected/runtime-owned, or translation-unit-local.
- [x] Record the complete public old-to-new symbol map and any responsibility
  filename renames; resolve collisions before moving code.
- [x] Capture module strings, workspace/document IDs, provider names, ImGui
  identities, layout/settings versions, configuration keys, and command names
  that must remain stable.
- [x] Pin missing representative identity and bootstrap transition tests before
  changing declarations.
- [x] Reconcile the LevelEditor inventory with the then-current Native Graybox
  Scene Authoring stage and update cross-plan code references without claiming
  its behavioral work.

#### Acceptance Gate

- Every selected C++ symbol has one destination or a documented reason to stay
  in `Durin`.
- All public renames and potential collisions are decided before consumer edits.
- Stable string/ID surfaces have test coverage or a recorded direct comparison.
- No selected destination crosses a module dependency in the wrong direction.

### Stage 1: Move the MainFrame host boundary

- [x] Move the public module interface, bootstrap values, module
  implementation, bootstrap context, and private host services into
  `Durin::Editor::MainFrame` using the approved concise names.
- [x] Update `DEditorEngine` forward declarations, typed module loading, stored
  interface pointer, and bootstrap calls without moving the reflected engine.
- [x] Preserve the `MainFrame` module string, exported initializer ABI,
  first-present gate, progress values, failure messages, settings identity, and
  destruction order.
- [x] Update bootstrap tests and non-archived workspace documentation.

#### Acceptance Gate

- Editor bootstrap state tests pass with unchanged transition and phase outputs.
- Editor engine and MainFrame link across the DLL boundary using the new types.
- Searches find no root-owned MainFrame ordinary C++ API or compatibility alias
  other than the explicit `FMainFrameModule` entry class.
- A hidden editor startup/exit smoke reaches the same bootstrap outcome.

### Stage 2: Move MaterialEditor and TextureEditor

- [x] Keep each concrete module entry point root-owned while moving its
  workspace, widget/model, preview, and concrete thumbnail provider into the
  selected feature namespace.
- [x] Keep `DTextureCubePreviewComponent` in `Durin` and update its ordinary
  TextureEditor consumers without changing reflection or serialization identity.
- [x] Apply the approved concise public names and retain material-instance,
  Texture2D, and TextureCube distinctions where required.
- [x] Update MainFrame composition, tests, exports, and direct includes.
- [x] Preserve asset routes, workspace IDs, layout IDs, transaction behavior,
  preview state, thumbnail keys/providers, registration rollback, and unload.

#### Acceptance Gate

- Material and texture editor/thumbnail focused targets pass.
- Reflected property and rendering integration targets covering these editors pass.
- Module registration, rollback, provider reset, and re-registration tests pass.
- Reflection lookup still resolves `DTextureCubePreviewComponent` under its
  existing identity.
- Searches find no root-owned ordinary MaterialEditor or TextureEditor API
  other than their explicit module entry classes.

### Stage 3: Move StaticMeshEditor and SkeletalMeshEditor

- [x] Keep concrete module entry points root-owned while moving
  workspaces/inspectors, preview controllers, and concrete thumbnail providers
  into their feature namespaces.
- [x] Apply approved concise names without erasing StaticMesh, Skeleton,
  SkeletalMesh, or AnimationClip distinctions needed inside the modules.
- [x] Update MainFrame composition, tests, exports, and direct includes.
- [x] Preserve exact asset routes, read-only policies, preview ownership,
  rendered fixtures, provider identities, registration rollback, and unload.

#### Acceptance Gate

- StaticMesh and SkeletalMesh editor/thumbnail focused targets pass.
- Shared editor rendering integration passes for both feature modules.
- Exact-route, preview teardown, provider reset, and module re-registration
  tests pass.
- Searches find no root-owned ordinary StaticMeshEditor or SkeletalMeshEditor API
  other than their explicit module entry classes.

### Stage 4: Move the LevelEditor public extension boundary

- [x] Keep the `FLevelEditorModule` entry class root-owned while moving
  selection, viewport editing/picking, transform target, customization,
  authoring, details, and public workspace contracts into
  `Durin::Editor::Level`.
- [x] Apply the Stage 0 public rename map, retaining authoring-domain qualifiers
  that remain meaningful inside LevelEditor.
- [x] Update MainFrame, DurinEd, feature modules, tests, active-plan references,
  and external module consumers atomically.
- [x] Preserve workspace/document IDs, customization handles, edit-mode IDs,
  selection semantics, viewport coordinates, pick ordering, transaction
  boundaries, authoring diagnostics, and command names.

#### Acceptance Gate

- Public LevelEditor contract, viewport, selection, customization, and
  authoring focused tests pass.
- Graybox tests that are complete and applicable at stage entry still pass
  without changing their behavioral expectations.
- No public consumer depends on a root-owned LevelEditor ordinary C++ type,
  alias, forwarding header, or accidental transitive include other than the
  explicit `FLevelEditorModule` entry class.

### Stage 5: Move the LevelEditor private implementation

- [x] Move workspace, viewport, panels, documents, Content Browser, authoring,
  assets/import dialogs, settings, customizations, and widgets into the feature
  namespace.
- [x] Fold ad-hoc feature sibling namespaces into the selected boundary and
  keep translation-unit helpers anonymous.
- [x] Remove redundant forward declarations and add the narrow direct includes
  revealed by the migration.
- [x] Preserve Content Browser refresh/thumbnail behavior, asset move/import
  transactions, source workflows, document save/dirty state, panel/window
  identity, viewport presentation, and editor shutdown.

#### Acceptance Gate

- LevelEditor, Content Browser, asset workflow, source-image thumbnail,
  viewport, document, settings, and authoring focused targets pass.
- Workspace identity tests prove every persisted ImGui name and ID remains stable.
- Stale-surface searches find no selected root-owned private type or old ad-hoc
  feature namespace.

### Stage 6: Documentation, stale-surface removal, and full qualification

- [x] Update owning Editor architecture documentation with the feature namespace
  topology and the reflected-type exception.
- [x] Update active plans that name migrated C++ symbols or code paths without
  rewriting archived historical evidence unless a link must be repaired.
- [x] Search source, tests, and non-archived documentation for old qualified
  names, root forward declarations, compatibility aliases, broad `using
  namespace` directives, and stale responsibility filenames.
- [x] Run all focused validation entries after the final source state.
- [x] Run the complete native-test suite at default target granularity because
  the migration crosses module, DLL, test-target, and editor-composition boundaries.
- [x] Complete a full editor `all` build and a hidden Sandbox startup/exit smoke
  from the same Agent Build Profile.
- [x] Validate all plans and record final evidence before marking this plan complete.

#### Acceptance Gate

- Every validation-matrix entry passes.
- Full native tests, full editor linkage, and startup/shutdown smoke pass.
- The final tree contains no selected concrete editor API in `Durin`, no
  compatibility surface, and no undocumented namespace exception.
- Module names, reflected identities, persistent data, settings, UI IDs,
  provider/cache identity, behavior, and lifecycle ordering are unchanged.

## Validation Matrix

| Surface | Validation |
| --- | --- |
| MainFrame bootstrap states and stable progress | MainFrame/editor bootstrap focused tests |
| Shared workspace, transaction, notification, property, and asset picker integration | `EditorShellTests` and `EditorPropertyTests` |
| Level workspace, documents, selection, Content Browser, viewport, customization, and authoring | LevelEditor and editor asset-workflow focused targets |
| Material workspace, preview, properties, thumbnails, and unload | Material editor/material/thumbnail focused targets |
| Texture workspace, reflected cube preview, source workflows, thumbnails, and unload | Texture editor/thumbnail and source-image focused targets |
| StaticMesh workspace, preview, details, thumbnails, and unload | StaticMesh editor/thumbnail focused targets |
| Skeletal workspace, preview, routes, thumbnails, and unload | `SkeletalMeshEditorTests` and related thumbnail coverage |
| Shared rendered provider service and GPU publication | `EditorRenderingTests` and representative renderer-backed fixture targets |
| Namespace and DLL migration across all consumers | Complete native-test suite at default target granularity |
| MainFrame and all feature editor DLL linkage | Full editor `all` build |
| Bootstrap, default workspace/document, and reverse-order shutdown | Hidden Sandbox startup/exit smoke |
| Documentation lifecycle and cross-plan links | All-plan validator |

Exact DurinDevTool target names are confirmed in Stage 0 from the current test
registry rather than duplicated here when ownership spans an aggregate target.

## Definition of Done

- MainFrame and every non-reflected ordinary C++ type owned by the five concrete
  feature-editor modules lives in its selected `Durin::Editor::<Feature>` namespace.
- Shared editor infrastructure remains directly in `Durin::Editor`; reflected
  and runtime-owned types retain their existing `Durin` identities.
- Public names follow the approved qualification-aware naming map and no root
  alias, facade, or forwarding compatibility surface remains.
- Module strings, reflection, persisted data, UI identity, settings, commands,
  rendering, transactions, registration, and shutdown behavior remain unchanged.
- Focused tests, complete native tests, full editor build, hidden startup/exit
  smoke, stale-surface searches, and plan validation pass with recorded evidence.

## Deferred Follow-ups

- Review AssetImportCore and StandardAssetImport namespace/file ownership after
  concrete editor modules stabilize; do not infer that import-domain types
  belong under a UI feature namespace.
- Review whether `DEditorEngine` should expose a smaller private PIE/session
  coordinator. Its reflection ownership and behavioral decomposition are a
  separate architectural change, not part of namespace migration.
- Revisit physical directory topology only if post-migration include metrics or
  ownership searches demonstrate a concrete navigation problem.
- Remove or rename remaining historical test-suite labels only when doing so
  improves test ownership; test display names are not compatibility aliases.

## Related Documentation

- [Workspace Framework](../../../Editor/Architecture/WorkspaceFramework.md)
- [Viewport Editing](../../../Editor/Architecture/ViewportEditing.md)
- [Content Browser](../../../Editor/Architecture/ContentBrowser.md)
- [Asset Thumbnails](../../../Editor/Architecture/AssetThumbnails.md)
- [Reflected Property Editing](../../../Editor/Architecture/ReflectedPropertyEditing.md)
- [Play In Editor Architecture](../../../Editor/Architecture/PlayInEditorArchitecture.md)
- [Skeletal Asset Editor](../../../Editor/Architecture/SkeletalAssetEditor.md)
- [Static Mesh Level Authoring](../../../Editor/Architecture/StaticMeshLevelMutations.md)
- [Native Graybox Scene Authoring Investigation](../../../Investigations/NativeGrayboxSceneAuthoring.md)
- [C++ Coding Standards](../../../Development/Standards/CodingStandards.md)

## Related Code

- `Engine/Source/Editor/MainFrame`
- `Engine/Source/Editor/LevelEditor`
- `Engine/Source/Editor/MaterialEditor`
- `Engine/Source/Editor/TextureEditor`
- `Engine/Source/Editor/StaticMeshEditor`
- `Engine/Source/Editor/SkeletalMeshEditor`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorEngine.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp`
- `Engine/Tests/Native/EngineTests/Private`
