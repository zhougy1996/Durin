# Content Browser Import Extensions Plan

Summary: Register feature-owned Content Browser import workflows and retire the fixed built-in asset-family dispatch.

Last reviewed: 2026-08-29

Status: Active
Completed:

## Current Status

The selected design is ready for implementation. Content Browser already owns
a deterministic, scoped extension registry for Create, Details, and Context
Menu contributions, but Import still bypasses it through a fixed descriptor
table and host-side asset-family dispatch.

The current fixed path has four coupled seams:

- `BuiltinImportDispatch.h` declares `EBuiltinImportFamily` and the four menu
  descriptors for Texture, Terrain Heightmap, Scene, and Static Mesh.
- Content Browser renders that table directly and forwards the selected enum
  through `FConstructionServices::OpenImport`.
- MainFrame switches on the enum to call concrete TextureEditor,
  StaticMeshEditor, or LevelEditor module methods and separately enumerates the
  Texture and Static Mesh modal presenters.
- Focused tests assert that the built-in family set has exactly four members.

No implementation work in this plan is complete. The existing behavior remains
authoritative until a stage acceptance gate passes.

## Goal

Make every Content Browser Import entry a feature-owned, deterministically
ordered, unload-safe extension so that ContentBrowser and MainFrame do not name,
enumerate, or dispatch concrete import families.

At completion, adding an import workflow requires a feature module to register
one descriptor; it does not require editing a central enum, descriptor array,
Content Browser menu loop, construction-service callback, or MainFrame switch.

## Scope

- Add Import as a first-class Content Browser extension category.
- Treat Import invocation as an asset mutation subject to the same Play-mode
  admission policy as Create.
- Register the existing Texture, Terrain Heightmap, Scene, and Static Mesh
  workflows from their owning feature modules with stable IDs and explicit
  ordering.
- Route feature-owned import modal presentation through the existing
  `DrawHostPresentation` extension seam.
- Make the Import menu consume a live ordered extension snapshot and invoke
  entries through the registry's owner gate.
- Remove `EBuiltinImportFamily`, `FBuiltinImportDescriptor`,
  `BuiltinImportDescriptors`, and `BuiltinImportDispatch.h`.
- Remove the import-family callbacks and concrete module captures from
  Content Browser construction and MainFrame composition.
- Preserve existing labels, destination-directory behavior, modal forms,
  import/reimport implementations, completion notifications, and shutdown
  admission rules.
- Replace fixed-family tests with extension-category, ordering, mutation-policy,
  lifetime, and removal coverage.
- Publish the resulting lasting boundary in the Content Browser architecture
  document after validation.

## Non-Goals

- Introducing an `AssetRegistry` module or moving Engine's asset catalog,
  reference index, loading, or package metadata ownership.
- Replacing the explicit Import submenu with a generic source-file picker.
- Redesigning `DFactory`, AssetForge, AssetTools, factory discovery, import
  settings, asynchronous build, or reimport orchestration.
- Inferring Texture2D, TextureCube, VolumeTexture, Scene, or Static Mesh intent
  solely from a source extension.
- Adding import workflows, changing supported source formats, or changing the
  visible order and labels of the four existing workflows.
- Removing feature-private state-machine enums such as
  `ETextureImportAssetType` or LevelEditor's internal import-dialog selector.
- Generalizing every editor menu onto the Content Browser registry.
- Changing Content Browser item classification, drag-and-drop import, source
  file presentation, or Asset Catalog refresh semantics.

## Design Decisions and Invariants

### Extension and factory responsibilities remain separate

The Content Browser Import extension answers which user-facing workflows are
available in a virtual destination directory. `DFactory` continues to answer
which object class and source extension a concrete factory supports. A Scene
workflow may produce multiple assets, and multiple workflows may accept the
same `.fbx`, `.gltf`, or `.glb` source, so neither registry substitutes for the
other.

No new AssetRegistry service is introduced. Persistent asset discovery remains
the immutable Engine Asset Catalog contract. Import workflow registration is an
editor presentation and orchestration concern, not persistent asset metadata.

### Import is an explicit mutation category

`EExtensionCategory::Import` is distinct from Create for discovery and menu
placement. Registry invocation must nevertheless classify both Create and
Import as asset mutations. When `FExtensionInvocation::bAllowAssetMutation` is
false, the registry rejects Import invocation before entering the feature
callback. An already-open feature modal still draws through
`DrawHostPresentation(false)` so it can explain that submission is unavailable
during Play.

Import extensions use the existing descriptor contract:

- `Id` is the stable machine identity.
- `Label` is the menu text.
- `Order` and then `Id` define deterministic order independent of module load
  order.
- `IsApplicable` decides whether the workflow applies to the supplied virtual
  directory.
- `Invoke` opens feature-owned state through the queued Content Browser action.
- `DrawHostPresentation` draws only the modal owned by that descriptor.
- `OwnerGate` prevents entry after module retirement has begun.

The initial registrations preserve the current visible order:

| Order | Stable ID | Label | Owner |
| --- | --- | --- | --- |
| 100 | `texture.import-texture` | `Texture...` | TextureEditor |
| 200 | `level.import-terrain-heightmap` | `Terrain Heightmap...` | LevelEditor |
| 300 | `level.import-scene` | `Scene Source (FBX/glTF)...` | LevelEditor |
| 400 | `static-mesh.import-static-mesh` | `Static Mesh (Geometry Only)...` | StaticMeshEditor |

Each initial entry applies only when `VirtualDirectory` is non-empty. Source
extensions are not copied into the UI descriptor because the current menu does
not consume them and the owning dialogs/factories remain authoritative.

### Invocation and presentation stay feature-owned

Content Browser constructs `FExtensionInvocation` with the selected virtual
directory, current mutation policy, reveal/open callbacks, mounted-content
notification, and error reporting. It queues the invocation until recursive
menu drawing has completed, matching the existing action-queue rule.

TextureEditor and StaticMeshEditor retain their dialog objects and expose them
only to module-owned extension callbacks. LevelEditor registers separate Scene
and Terrain Heightmap entries; each presenter draws only its matching modal.
The workspace stops drawing those two import modals directly, preventing double
submission and making presentation independent of which workspace is active.
Document and workspace dialogs unrelated to Content Browser import remain on
their existing path.

### Registration is transactional and unload-safe

Feature integration succeeds only if every required import registration for
that feature succeeds. A duplicate ID, invalid descriptor, or other
registration failure rolls back registrations created by that attempt together
with the surrounding feature integration state.

Registration handles are owned beside the feature's dialog/workspace state.
Unregistration removes registry admission before destroying the corresponding
dialog or workspace and before retiring the module callback gate. Captured
snapshots remain harmless because `InvokeExtension` and
`DrawHostPresentation` must enter the owner gate on every call.

### Host and public boundaries shrink

`FConstructionServices` retains generic host services such as asset opening,
transactions, mutation notification, movement, fix-up, and reimport. It loses
`OpenImport` and `DrawImportDialogs`. Content Browser panel/tool constructors
lose the corresponding callback types and members.

MainFrame continues to compose and shut down feature modules, but it no longer
captures those modules to dispatch or draw import families. Public
`OpenImportDialog` and `DrawImportDialog` methods that have no remaining
cross-module caller become private implementation details or are folded into
the registration callbacks.

### Compatibility is source-level only

The fixed enum and descriptor header are removed without aliases or a
deprecation bridge. They are editor-only source contracts with a finite set of
repository callers, and retaining them would preserve the coupling this plan
exists to eliminate. No asset package, settings, cache, or user-data migration
is required.

## Current Foundations and Gaps

Foundations already present:

- `RegisterExtension` returns a scoped removal handle and rejects duplicate
  stable IDs.
- Extension capture sorts by `(Order, Id)` and returns an owned snapshot.
- Invocation and host presentation enter a module-owned callback gate.
- The descriptor already carries a host presenter suitable for feature-owned
  immediate-mode modal state.
- MainFrame submits host presenters every frame even when the Content Browser
  panel itself is hidden.
- TextureEditor, StaticMeshEditor, and LevelEditor already own their concrete
  import dialogs and receive shared completion callbacks.
- `DFactory` already discovers immutable class-default factories by supported
  class and source extension, including explicit ambiguity results.

Gaps to close:

- The category enum and mutation check do not include Import.
- `DrawImportMenu` reads the fixed `BuiltinImportDescriptors` array rather than
  an extension snapshot.
- Content Browser construction exposes fixed-family open and draw callbacks.
- MainFrame contains the central family switch and a separate finite modal
  presenter list.
- TextureEditor and StaticMeshEditor do not own Content Browser extension
  registration handles for their import dialogs.
- LevelEditor opens imports through a type switch and draws both dialogs from
  its workspace instead of the host-presenter seam.
- Tests encode the four-family closed world instead of proving open
  registration, policy, and lifetime behavior.

## Implementation Stages

### Stage 0: Freeze the extension contract and behavior baseline

- [ ] Record the four current menu labels, order, preferred-directory routing,
  Play-mode behavior, completion notifications, and modal presentation owner.
- [ ] Confirm the stable IDs and orders in this plan do not collide with live
  Create, Details, or Context Menu registrations.
- [ ] Identify every include and call site for `BuiltinImportDispatch.h`,
  `EBuiltinImportFamily`, `OpenImport`, and `DrawImportDialogs`.
- [ ] Characterize shutdown ordering for ContentBrowser, LevelEditor,
  TextureEditor, and StaticMeshEditor before changing registration ownership.
- [ ] Confirm the selected focused test targets through the configured native
  test registry.

#### Acceptance Gate

- The behavioral baseline, exact removal set, owner/lifetime map, stable IDs,
  and focused validation targets are recorded with no unresolved dispatch or
  presentation decision.

### Stage 1: Add Import registry semantics

- [ ] Add `EExtensionCategory::Import` without changing existing category
  values or their capture behavior unnecessarily.
- [ ] Centralize mutation-category classification so Create and Import are both
  rejected when asset mutation is unavailable.
- [ ] Extend registry tests to cover Import registration, ordering, duplicate
  rejection, removal, retired-owner invocation, and Play-mode rejection.
- [ ] Add or extract a testable Content Browser import-action seam only if the
  menu cutover cannot be covered through the public registry contract; do not
  export panel/UI implementation solely for testing.
- [ ] Keep the fixed production menu path active until all feature registrations
  are ready for the atomic cutover.

#### Acceptance Gate

- Synthetic Import extensions are deterministically discoverable and
  unload-gated, Create behavior is unchanged, and both mutating categories are
  rejected by the same admission policy in `ContentBrowserWorkflowTests`.

### Stage 2: Register feature-owned imports and cut over the menu

- [ ] Give TextureEditor one scoped Texture import registration that opens and
  presents its existing dialog.
- [ ] Give StaticMeshEditor one scoped geometry-only import registration that
  opens and presents its existing dialog.
- [ ] Give LevelEditor separate Terrain Heightmap and Scene registrations with
  matching open and presenter callbacks.
- [ ] Make each feature integration attempt transactional and store handles so
  they reset before the dialog/workspace state they protect.
- [ ] Stop drawing Scene and Terrain Heightmap import dialogs from
  `MLevelEditor::DrawWorkspace`; route them exclusively through registered host
  presenters.
- [ ] Change `DrawImportMenu` to capture applicable Import descriptors, preserve
  the selected order and labels, queue invocation, and populate the complete
  extension context and host callbacks.
- [ ] Verify that hiding the Content Browser or changing the active workspace
  does not strand an already-open import modal.

#### Acceptance Gate

- The four existing workflows appear exactly once in the selected order, each
  opens with the chosen virtual directory, its modal draws through an admitted
  owner gate, and feature teardown leaves no invocable or presentable entry.

### Stage 3: Remove fixed dispatch and narrow host boundaries

- [ ] Remove `OpenImport` and `DrawImportDialogs` from
  `FConstructionServices`, Content Browser panel/tool constructors, aliases,
  members, and forwarding code.
- [ ] Remove MainFrame's asset-family switch and concrete import-dialog draw
  captures.
- [ ] Remove obsolete public feature-module import methods or make them private
  when module-owned callbacks are their only remaining caller.
- [ ] Delete `BuiltinImportDispatch.h` and remove it from all includes and build
  metadata.
- [ ] Replace `FBuiltinImportDispatchTests` with open-registry assertions; keep
  dialog-state tests focused on feature-private form behavior.
- [ ] Search production and tests to prove that no fixed import family,
  descriptor table, construction callback, or host switch remains.

#### Acceptance Gate

- ContentBrowser and MainFrame compile without a built-in import-family type or
  concrete import-module dispatch, all four workflows are supplied only by
  scoped feature registrations, and repository search finds no retired symbol.

### Stage 4: Qualify lifecycle and publish the contract

- [ ] Run the focused Content Browser and editor asset workflow targets using
  the repository test workflow.
- [ ] Build the affected editor module closure following the repository build
  workflow, then run the required hidden-window editor smoke if the build guide
  selects it for this integration change.
- [ ] Exercise each import menu entry, preferred destination, cancel/reopen,
  Play-mode disablement, hidden-browser presentation, workspace switching, and
  editor shutdown.
- [ ] Update the lasting Content Browser architecture contract to describe
  Import as a dynamic category and remove the finite dispatch statement.
- [ ] Validate changed documentation and the complete active-plan set.

#### Acceptance Gate

- Focused automated coverage, affected-module build, applicable editor smoke,
  manual workflow checks, shutdown checks, and documentation validation pass
  with evidence recorded in Current Status.

## Validation Matrix

| Concern | Validation | Required evidence |
| --- | --- | --- |
| Registry category and ordering | `ContentBrowserWorkflowTests` focused cases | Import captures by `(Order, Id)` and removal leaves no entry |
| Mutation admission | `ContentBrowserWorkflowTests` focused cases | Create and Import reject invocation when mutation is disabled; presenters still receive the policy |
| Owner lifetime | `ContentBrowserWorkflowTests` focused cases | Retired owners cannot be invoked or drawn from a captured snapshot |
| Dialog state regression | `EditorAssetWorkflowTests` | Texture form reset/type switching and shared destination callbacks remain unchanged |
| Feature integration | Affected module build plus editor workflow smoke | Four entries appear once, in order, and open the correct existing modal |
| Host decoupling | Targeted repository search and module build | No retired enum/table/callback or MainFrame family switch remains |
| Play and workspace behavior | Editor smoke | Menu invocation is blocked during Play; open modals remain visible but cannot submit; workspace switching does not duplicate or lose them |
| Shutdown | Hidden-window editor smoke or the build guide's selected equivalent | Extension admission stops before feature dialog/workspace destruction and shutdown completes cleanly |
| Documentation | Changed-doc and all-plan validators | Active plan and lasting Content Browser contract are valid |

## Definition of Done

- `EExtensionCategory::Import` is a supported, tested mutation category.
- TextureEditor, LevelEditor, and StaticMeshEditor own all current Import
  registrations and their presenter callbacks.
- Content Browser renders Import entirely from an ordered extension snapshot.
- MainFrame does not dispatch or enumerate concrete import workflows.
- `BuiltinImportDispatch.h`, `EBuiltinImportFamily`,
  `FBuiltinImportDescriptor`, `BuiltinImportDescriptors`, `OpenImport`, and
  `DrawImportDialogs` have no remaining production or test reference.
- Each workflow preserves its current label, order, preferred destination,
  modal behavior, completion notification, and supported source formats.
- Create/Import Play-mode policy, owner retirement, registration rollback, and
  host-presenter lifetime are covered by focused tests.
- Required build, test, smoke, search, and documentation gates pass.
- The lasting Content Browser contract describes the implemented dynamic
  Import boundary, and this plan records evidence before completion.

## Deferred Follow-ups

- A generic `Import Files...` action derived from `DFactory` source-extension
  capabilities, including ambiguity UI and import-priority policy.
- Drag-and-drop routing from ordinary source files into registered import
  workflows.
- Richer import descriptor metadata such as icons, grouping, source-format
  hints, or command bindings, introduced only when a concrete consumer exists.
- A separate Asset Definition layer for class-owned open, thumbnail, details,
  and context behavior if those existing registries later need consolidation.
- Extracting Engine's immutable Asset Catalog into an `AssetCatalog` module if
  dependency analysis demonstrates consumers that should not depend on Engine;
  that work must not be coupled to editor Import registration.

## Related Documentation

- [Content Browser](../Editor/Architecture/ContentBrowser.md)
- [Asset Import Architecture](../Editor/Architecture/AssetImportFramework.md)
- [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Editor/ContentBrowser/Public/ContentBrowser/ContentBrowserContracts.h`
- `Engine/Source/Editor/ContentBrowser/Private/ContentBrowserExtensionRegistry.cpp`
- `Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserPanelView.cpp`
- `Engine/Source/Editor/ContentBrowser/Private/Panels/ContentBrowserPanel.h`
- `Engine/Source/Editor/ContentBrowser/Private/ContentBrowserTool.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/Import/BuiltinImportDispatch.h`
- `Engine/Source/Editor/DurinEd/Public/Factories/Factory.h`
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`
- `Engine/Source/Editor/TextureEditor/Private/TextureEditorModule.cpp`
- `Engine/Source/Editor/StaticMeshEditor/Private/StaticMeshEditorModule.cpp`
- `Engine/Source/Editor/LevelEditor/Private/LevelEditorModule.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserExtensionRegistryTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ImportDialogStateTests.cpp`
