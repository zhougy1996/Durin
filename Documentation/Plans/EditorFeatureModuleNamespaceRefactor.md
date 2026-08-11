# Editor Feature Module Namespace Refactor Plan

Summary: Move MainFrame, LevelEditor, and concrete asset-editor implementation APIs into feature-owned `Durin::Editor` subnamespaces while preserving module, reflection, persistence, UI identity, and runtime behavior.

Last reviewed: 2026-08-12

Status: Active
Completed:

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

The selected destination is one feature subnamespace per module:
`Durin::Editor::MainFrame`, `Durin::Editor::LevelEditor`,
`Durin::Editor::MaterialEditor`, `Durin::Editor::TextureEditor`,
`Durin::Editor::StaticMeshEditor`, and
`Durin::Editor::SkeletalMeshEditor`. Runtime/reflected object types remain in
`Durin`; shared DurinEd contracts remain directly in `Durin::Editor`.

## Goal

- Make the C++ namespace hierarchy express the existing editor module ownership.
- Remove concrete editor implementation types from the `Durin` root namespace.
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

- Public module interfaces and implementations shorten after qualification:
  `IMainFrameModule` becomes `Editor::MainFrame::IModule`, and each
  `F<Feature>EditorModule` becomes `Editor::<Feature>::FModule`.
- MainFrame bootstrap names remove the redundant `Editor` stem, for example
  `EEditorBootstrapState` becomes `Editor::MainFrame::EBootstrapState` and
  `FEditorBootstrapProgress` becomes `Editor::MainFrame::FBootstrapProgress`.
- A feature-owned type removes its feature prefix only when the remaining name
  is specific inside that namespace. For example, a LevelEditor context may
  become `Editor::LevelEditor::FContext`; a type such as
  `FStaticMeshLevelMutationPlan` retains the `StaticMesh` qualifier because the
  Level Editor can own multiple authoring domains.
- Concrete provider names retain asset-kind distinctions when a feature owns
  more than one provider. Texture2D and TextureCube names therefore remain
  distinct even inside `Editor::TextureEditor`.
- Reflected `D` types keep their existing C++ names and namespace. Feature code
  refers to them through their stable `Durin` ownership.
- Stage 0 records the complete public rename map before implementation. Private
  names follow the same rule but are not renamed merely for stylistic novelty.

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
- Module registration macros, exported entry points, and persisted strings are
  reviewed explicitly rather than changed by unbounded search-and-replace.

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

- [ ] Inventory every declaration, definition, forward declaration, module
  loader, export annotation, include, test, and non-archived documentation
  consumer in the six selected modules.
- [ ] Classify each symbol as shared Editor infrastructure, feature-owned
  ordinary C++, reflected/runtime-owned, or translation-unit-local.
- [ ] Record the complete public old-to-new symbol map and any responsibility
  filename renames; resolve collisions before moving code.
- [ ] Capture module strings, workspace/document IDs, provider names, ImGui
  identities, layout/settings versions, configuration keys, and command names
  that must remain stable.
- [ ] Pin missing representative identity and bootstrap transition tests before
  changing declarations.
- [ ] Reconcile the LevelEditor inventory with the then-current Native Graybox
  Scene Authoring stage and update cross-plan code references without claiming
  its behavioral work.

#### Acceptance Gate

- Every selected C++ symbol has one destination or a documented reason to stay
  in `Durin`.
- All public renames and potential collisions are decided before consumer edits.
- Stable string/ID surfaces have test coverage or a recorded direct comparison.
- No selected destination crosses a module dependency in the wrong direction.

### Stage 1: Move the MainFrame host boundary

- [ ] Move the public module interface, bootstrap values, module
  implementation, bootstrap context, and private host services into
  `Durin::Editor::MainFrame` using the approved concise names.
- [ ] Update `DEditorEngine` forward declarations, typed module loading, stored
  interface pointer, and bootstrap calls without moving the reflected engine.
- [ ] Preserve the `MainFrame` module string, exported initializer ABI,
  first-present gate, progress values, failure messages, settings identity, and
  destruction order.
- [ ] Update bootstrap tests and non-archived workspace/PIE documentation.

#### Acceptance Gate

- Editor bootstrap state tests pass with unchanged transition and phase outputs.
- Editor engine and MainFrame link across the DLL boundary using the new types.
- Searches find no root-owned MainFrame ordinary C++ API or compatibility alias.
- A hidden editor startup/exit smoke reaches the same bootstrap outcome.

### Stage 2: Move MaterialEditor and TextureEditor

- [ ] Move each module entry point, workspace, widget/model, preview, and
  concrete thumbnail provider into its feature namespace.
- [ ] Keep `DTextureCubePreviewComponent` in `Durin` and update its ordinary
  TextureEditor consumers without changing reflection or serialization identity.
- [ ] Apply the approved concise public names and retain material-instance,
  Texture2D, and TextureCube distinctions where required.
- [ ] Update MainFrame composition, tests, exports, and direct includes.
- [ ] Preserve asset routes, workspace IDs, layout IDs, transaction behavior,
  preview state, thumbnail keys/providers, registration rollback, and unload.

#### Acceptance Gate

- Material and texture editor/thumbnail focused targets pass.
- Reflected property and rendering integration targets covering these editors pass.
- Module registration, rollback, provider reset, and re-registration tests pass.
- Reflection lookup still resolves `DTextureCubePreviewComponent` under its
  existing identity.
- Searches find no root-owned ordinary MaterialEditor or TextureEditor API.

### Stage 3: Move StaticMeshEditor and SkeletalMeshEditor

- [ ] Move module entry points, workspaces/inspectors, preview controllers, and
  concrete thumbnail providers into their feature namespaces.
- [ ] Apply approved concise names without erasing StaticMesh, Skeleton,
  SkeletalMesh, or AnimationClip distinctions needed inside the modules.
- [ ] Update MainFrame composition, tests, exports, and direct includes.
- [ ] Preserve exact asset routes, read-only policies, preview ownership,
  rendered fixtures, provider identities, registration rollback, and unload.

#### Acceptance Gate

- StaticMesh and SkeletalMesh editor/thumbnail focused targets pass.
- Shared editor rendering integration passes for both feature modules.
- Exact-route, preview teardown, provider reset, and module re-registration
  tests pass.
- Searches find no root-owned ordinary StaticMeshEditor or SkeletalMeshEditor API.

### Stage 4: Move the LevelEditor public extension boundary

- [ ] Move LevelEditor module, selection, viewport editing/picking, transform
  target, customization, authoring, details, and public workspace contracts
  into `Durin::Editor::LevelEditor`.
- [ ] Apply the Stage 0 public rename map, retaining authoring-domain qualifiers
  that remain meaningful inside LevelEditor.
- [ ] Update MainFrame, DurinEd, feature modules, tests, active-plan references,
  and external module consumers atomically.
- [ ] Preserve workspace/document IDs, customization handles, edit-mode IDs,
  selection semantics, viewport coordinates, pick ordering, transaction
  boundaries, authoring diagnostics, and command names.

#### Acceptance Gate

- Public LevelEditor contract, viewport, selection, customization, and
  authoring focused tests pass.
- Graybox tests that are complete and applicable at stage entry still pass
  without changing their behavioral expectations.
- No public consumer depends on a root-owned LevelEditor ordinary C++ type,
  alias, forwarding header, or accidental transitive include.

### Stage 5: Move the LevelEditor private implementation

- [ ] Move workspace, viewport, panels, documents, Content Browser, authoring,
  assets/import dialogs, settings, customizations, and widgets into the feature
  namespace.
- [ ] Fold ad-hoc feature sibling namespaces into the selected boundary and
  keep translation-unit helpers anonymous.
- [ ] Remove redundant forward declarations and add the narrow direct includes
  revealed by the migration.
- [ ] Preserve Content Browser refresh/thumbnail behavior, asset move/import
  transactions, source workflows, document save/dirty state, panel/window
  identity, viewport presentation, and editor shutdown.

#### Acceptance Gate

- LevelEditor, Content Browser, asset workflow, source-image thumbnail,
  viewport, document, settings, and authoring focused targets pass.
- Workspace identity tests prove every persisted ImGui name and ID remains stable.
- Stale-surface searches find no selected root-owned private type or old ad-hoc
  feature namespace.

### Stage 6: Documentation, stale-surface removal, and full qualification

- [ ] Update owning Editor architecture documentation with the feature namespace
  topology and the reflected-type exception.
- [ ] Update active plans that name migrated C++ symbols or code paths without
  rewriting archived historical evidence unless a link must be repaired.
- [ ] Search source, tests, and non-archived documentation for old qualified
  names, root forward declarations, compatibility aliases, broad `using
  namespace` directives, and stale responsibility filenames.
- [ ] Run all focused validation entries after the final source state.
- [ ] Run the complete native-test suite at default target granularity because
  the migration crosses module, DLL, test-target, and editor-composition boundaries.
- [ ] Complete a full editor `all` build and a hidden Sandbox startup/exit smoke
  from the same Agent Build Profile.
- [ ] Validate all plans and record final evidence before marking this plan complete.

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

- [Workspace Framework](../Editor/Architecture/WorkspaceFramework.md)
- [Viewport Editing](../Editor/Architecture/ViewportEditing.md)
- [Content Browser](../Editor/Architecture/ContentBrowser.md)
- [Asset Thumbnails](../Editor/Architecture/AssetThumbnails.md)
- [Reflected Property Editing](../Editor/Architecture/ReflectedPropertyEditing.md)
- [Play In Editor Architecture](../Editor/Architecture/PlayInEditorArchitecture.md)
- [Skeletal Asset Editor](../Editor/Architecture/SkeletalAssetEditor.md)
- [Static Mesh Level Authoring](../Editor/Architecture/StaticMeshLevelAuthoring.md)
- [Native Graybox Scene Authoring](NativeGrayboxSceneAuthoring.md)
- [C++ Coding Standards](../Development/Standards/CodingStandards.md)

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
