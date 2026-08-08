# Static Mesh Inspector and Modular Thumbnails Plan

Summary: Add a dedicated StaticMesh editor workspace and move asset-specific thumbnail generation behind registrations owned by the corresponding editor modules.

Last reviewed: 2026-08-08

Status: Completed
Completed: 2026-08-08

## Current Status

All five stages are complete. Stage 4 started from baseline
`878cc1e40c8061e3b8cb3f5237b61fcf558303cc`. `DurinEd` now exposes a long-lived
`FRenderedAssetThumbnailService`; caches created before or after extension
registration resolve its one live registry at request time. The rendered cache,
preview pool, scheduler, persistent store, PNG path, readback, GPU upload, and
budgets are provider-neutral. One active generation session owns all concrete
asset loading, readiness, revision validation, preview content, framing, and
diagnostics. `StaticMeshEditor` now owns the read-only **StaticMesh Inspector**
workspace, its exact-class document route, and isolated per-document preview
scenes with bounds framing, orbit, pan, zoom, and solid/wireframe presentation.
The module also owns the exact StaticMesh thumbnail provider, immutable input,
generation session, framing, preview assignment, revisions, and diagnostics.

`MaterialEditor` owns the exact Material and MaterialInstance rendered providers;
`TextureEditor` owns authored Texture2D source selection, TextureCube rendering,
and the cube preview component. Existing rendered provider names, schema versions,
fixture identities, shader versions, output settings, transparent/opaque policy,
and cache keys are unchanged. Content Browser routing now resolves the live
service instead of naming concrete asset classes. Stage 5 added mixed-module
queue/unload qualification, completed the user and ownership documentation, and
passed the focused, full-build, and editor-lifecycle gates.

### Stage 0 Frozen Contracts

#### Module composition

- `StaticMeshEditor` is a shared editor module with private dependencies on
  `Core`, `Engine`, `AssetCore`, `RHI`, `RenderCore`, `Renderer`, `Mona`,
  `MonaImGui`, and `DurinEd`. It exposes only its integration entry point;
  `DurinEd` has no reverse dependency.
- The `DurinEditor` variant lists `StaticMeshEditor` after `TextureEditor`.
  MainFrame creates the long-lived thumbnail service before loading concrete
  editors, then registers Level, Material, Texture, and StaticMesh integrations
  in that order and opens default workspaces only after all registrations pass.
- One module integration call installs that module's complete current
  contribution transactionally. A failed workspace or thumbnail registration
  removes the partial contribution inside the module; MainFrame then rolls back
  previously completed modules in reverse order.
- Shutdown stops Content Browser admission, resets integration handles in
  StaticMesh, Texture, Material, and Level order, drains the shared thumbnail
  service, destroys its caches, and only then permits concrete module unload.

#### StaticMesh Inspector documents

- The workspace type and root key are `StaticMeshEditor`; its display name is
  **StaticMesh Inspector**. The exact qualified `DStaticMesh` class maps to a
  closable `PerResource` route whose normalized virtual asset path is the
  document key. Opening the same path activates the existing document; distinct
  paths may coexist.
- A document captures package ownership before load and applies the shared
  compatibility policy before activation. Load, class, compatibility, or
  activation failure releases only ownership introduced by that request, keeps
  the prior active document unchanged, and reports the standard Asset
  Compatibility Audit guidance.
- The first inspector is permanently read-only: dirty state is always false,
  `CanSaveActiveDocument` and `SaveActiveDocument` are false, close never asks
  for save confirmation, and no asset serialization path is exposed.
- Closing first cancels document requests, detaches preview content, releases
  render resources on their owning threads, and releases the document's package
  ownership. It does not wait for a whole-device idle. Moved, deleted, empty,
  failed, or reimporting assets retain the document identity and show a stable
  unavailable state until a valid revision can be reacquired.

#### Rendered-thumbnail extensions

- The service owns one live exact-class registry. Caches constructed before or
  after a registration resolve that registry at request time; they never copy a
  provider table. Duplicate exact classes fail without mutation. Replacement is
  explicit reset followed by register and receives a later generation.
- Scoped registration accepts unique provider ownership. Reset removes admission
  first, invalidates the generation, cancels all leases, calls idempotent session
  preview reset on the game thread, and destroys the session, immutable input,
  and extension before returning. Remaining core jobs contain only cancelled
  leases and every completion rechecks cancellation and generation.
- Capture owns exact-class matching, deterministic key input, and immutable
  provider input. A persistent hit bypasses session creation. A cold miss creates
  exactly one session, which owns load/type checks, readiness and revision
  polling, preview-world content, view selection, validation diagnostics, and
  detachment. The core alone owns scheduling, scene leasing, capture, readback,
  encoding, upload, persistence, budgets, and final publication.
- Raw source-file image decode remains a provider-neutral Content Browser path:
  LevelEditor owns item presentation and request routing, while `DurinEd` owns
  decode/cache orchestration. `TextureEditor` registers only authored
  Texture2D/TextureCube extensions.

#### Stage 0 Handoff

- Baseline commit: `97a0d1809b28341ec828ae27bfd1044e5836e67e`.
- Working set: `Thumbnail/AssetThumbnail.h`,
  `Thumbnail/RenderedAssetThumbnailExtension.h`,
  `Thumbnail/AssetThumbnail.cpp`, the temporary rendered-cache input access in
  `MaterialAssetThumbnail.cpp`, `AssetThumbnailContractTests.cpp`, and
  `Documentation/Editor/Architecture/AssetThumbnails.md`.
- Key symbols: `FAssetThumbnailProviderRegistrationHandle`,
  `FAssetThumbnailGenerationLease`, `IRenderedAssetThumbnailExtension`, and
  `IRenderedAssetThumbnailGenerationSession`.
- Decisions: scoped registration owns a unique provider; core leases are the
  only provider-owned state carrier after capture; persistent hits never create
  sessions; reset synchronously drains all provider-defined objects.
- Open questions: none at the contract level. Stage 1 may choose private adapter
  structure, but cannot weaken the frozen ownership, generation, or thread
  rules.
- Validation: all 65 `ThumbnailTests` passed on
  `Win64-Debug-DurinEditor-Tests`; changed-document validation and all-plan
  validation passed.

The selected direction is a new `StaticMeshEditor` module. Its first user-visible
surface is an inspector rather than a modeling tool: double-click opens a
per-resource document with an interactive preview and read-only mesh facts. The
same module owns the StaticMesh-specific thumbnail extension. `DurinEd` retains
only provider-neutral thumbnail orchestration and rendering infrastructure.

## Goal

- Double-clicking an exact `DStaticMesh` asset opens a dedicated, closable
  StaticMesh Inspector document without disturbing the Level workspace.
- The inspector provides a useful interactive preview and trustworthy diagnostic
  information without promising topology, vertex, UV, or other modeling edits.
- Asset-specific thumbnail key capture, resource preparation, preview setup,
  validation, and diagnostics are registered by `StaticMeshEditor`,
  `MaterialEditor`, and `TextureEditor`.
- `DurinEd` continues to own one bounded request, scheduling, persistence,
  readback, upload, cancellation, and cache lifecycle shared by all providers.

## Scope

- Add the `StaticMeshEditor` editor module and include it in the DurinEditor
  runtime variant and MainFrame composition root.
- Register one per-resource workspace route for exact `DStaticMesh` assets.
- Add a read-only StaticMesh Inspector with orbit/pan/zoom, frame-to-bounds,
  shaded and wireframe visualization, and essential LOD 0 statistics.
- Apply the existing workspace compatibility and package-ownership policy when
  opening and closing StaticMesh documents.
- Introduce a scoped rendered-thumbnail extension contract that supports
  module-owned registration and safe removal.
- Move StaticMesh thumbnail implementation into `StaticMeshEditor`, then move
  Material/MaterialInstance and Texture2D/TextureCube asset-specific thumbnail
  implementations into their corresponding modules.
- Preserve existing Content Browser behavior, thumbnail keys, persistent cache
  compatibility where visual contracts have not changed, budgets, and failure
  presentation.
- Update editor architecture documentation and focused native coverage.

## Non-Goals

- Vertex, edge, face, topology, UV, skinning, animation, mesh-paint, sculpting,
  or other DCC-style editing.
- Authoring collision, sockets, LODs, Nanite-like data, material slots, import
  settings, or per-asset thumbnail cameras in the first inspector version.
- Changing StaticMesh package format, render-data format, import normalization,
  runtime renderer behavior, or the thumbnail PNG/object-store format.
- Giving each asset module its own scheduler, persistent cache, GPU texture
  cache, preview-scene budget, or Content Browser integration path.
- Moving raw source-file image decoding into `TextureEditor`. Source files are
  not authored texture assets and remain a provider-neutral Content Browser
  preview concern.
- Making `DurinEd` depend on `StaticMeshEditor`, `MaterialEditor`, or
  `TextureEditor`.

## Design Decisions and Invariants

- The new module is named `StaticMeshEditor`, matching `MaterialEditor` and
  `TextureEditor`. The user-visible workspace is initially named
  **StaticMesh Inspector** so the feature does not imply modeling capabilities.
- `StaticMeshEditor` depends privately on `DurinEd` and required runtime/render
  modules. `DurinEd` exposes extension interfaces and never gains a reverse
  dependency on a concrete asset editor.
- MainFrame remains the composition root. It loads the concrete editor modules
  and registers their workspace and thumbnail contributions in deterministic
  order. Failure rolls back all contributions installed by that module.
- Thumbnail registration returns a move-only scoped handle. Destroying the
  handle stops admission for that provider, advances its generation, cancels or
  rejects queued/in-flight work, and releases provider-owned preview state on
  the correct threads before module unload.
- The provider-neutral thumbnail core owns request identity, priority,
  coalescing, cache lookup/publication, budgets, persistent objects, PNG
  encode/decode, UI upload, and stale-completion checks.
- A concrete thumbnail extension owns exact asset-class matching, dependency
  fingerprint capture, immutable generation input, asset loading and type
  validation, readiness/revision polling, preview content setup, visual
  contracts, and asset-qualified diagnostics.
- The generic rendered path dispatches through an interface or type-erased
  session owned by the registered extension. It must not contain a central
  `dynamic_pointer_cast` chain or `ActiveMaterial`/`ActiveTextureCube`/
  `ActiveStaticMesh` branches.
- One cold job receives one provider-owned generation session. The session may
  mutate only its leased shared preview scene while it owns the active capture;
  reset and destruction detach all asset references.
- Existing thumbnail cache keys remain byte-for-byte stable during a pure
  ownership move. A provider, generator, fixture, shader, or output version is
  changed only when the visual or invalidation contract changes.
- The inspector and thumbnail generator may share pure StaticMesh preview math
  and presentation helpers inside `StaticMeshEditor`, but they do not share a
  live world, scene, viewport, camera, or resource controller.
- The first inspector is read-only. It never marks a document dirty, exposes a
  save action, or serializes a StaticMesh. Future authoring capabilities must
  add explicit transactions, dirty-state, validation, and save semantics.
- Exact-class routing remains intentional. A future StaticMesh subclass needs
  its own declared route/provider policy instead of inheriting behavior by
  accident.

## Current Foundations and Gaps

| Area | Existing foundation | Required change |
| --- | --- | --- |
| Workspace framework | `DurinEd` supports registered per-resource workspaces and exact asset routes | Add `StaticMeshEditor` registration and read-only document lifecycle |
| Editor composition | MainFrame loads and registers Level, Material, and Texture workspaces | Add transactional StaticMesh registration and symmetric shutdown |
| StaticMesh runtime | LOD 0 bounds, render-resource readiness/revision, default slots, and rendered-thumbnail coverage exist | Expose only inspector facts actually needed; do not widen mutable runtime access |
| Preview UI | Material and Texture editors already establish per-document preview patterns | Add a StaticMesh-owned preview scene/controller and input behavior |
| Thumbnail registry | Exact-class providers, provider generations, scheduler, cache, and persistent object store exist | Allow concrete modules to install full cold-generation extensions into the cache lifecycle |
| Rendered cache | One shared implementation handles Material, TextureCube, and StaticMesh | Remove concrete asset branches and drive an active type-erased session |
| Provider ownership | Asset providers and preview setup live in `DurinEd` | Move them to matching editor modules without reversing dependencies |
| Source previews | LevelEditor has source-image preview handling | Keep raw-file decoding generic; route authored texture classes through TextureEditor extensions |

## Implementation Stages

### Stage 0: Freeze Module and Extension Contracts

- [x] Define `StaticMeshEditor` module dependencies, MainFrame registration
  order, startup rollback, shutdown order, and DurinEditor variant membership.
- [x] Define the read-only StaticMesh document lifecycle, compatibility failure
  behavior, package ownership, tab identity, and no-dirty/no-save policy.
- [x] Split the thumbnail contract into provider-neutral orchestration and a
  module-owned rendered-generation extension/session boundary.
- [x] Specify registration-handle behavior for duplicate exact classes,
  provider replacement, unload during every asynchronous state, and cache
  instances created before or after registration.
- [x] Record the ownership of raw source-file previews separately from authored
  Texture2D/TextureCube providers.
- [x] Add contract tests using fake extensions before migrating a real provider.

#### Acceptance Gate

- Dependency arrows are one-way from each concrete editor module to `DurinEd`.
- A fake extension can register, capture a deterministic request, run a cold
  generation session, produce a warm hit, unregister, and reject every stale
  completion without concrete asset types appearing in the core path.
- The contract defines behavior for registration failure and module unload with
  no live provider object or asset reference crossing the unload boundary.

### Stage 1: Generalize the Rendered Thumbnail Core

Dependencies: Stage 0.

- [x] Move `FRenderedAssetThumbnailCache` and its statistics to provider-neutral
  headers whose names do not imply Material ownership.
- [x] Let the shared cache consume registrations from the long-lived thumbnail
  service instead of constructing concrete providers internally.
- [x] Replace concrete active-asset pointers, generation-input casts, readiness
  branches, framing branches, and diagnostics with one active extension session.
- [x] Keep the shared preview target, render/readback machinery, PNG pipeline,
  scheduler, object store, GPU upload cache, and budgets in `DurinEd`.
- [x] Ensure registration generation participates in request capture and every
  asynchronous completion check.
- [x] Preserve deterministic keys and warm-cache behavior while running existing
  Material, TextureCube, and StaticMesh providers through temporary adapters.

#### Acceptance Gate

- `DurinEd` rendered-thumbnail orchestration has no include, forward declaration,
  cast, active pointer, or control-flow branch for Material, TextureCube, or
  StaticMesh asset classes.
- Existing thumbnail tests pass without key churn, cache invalidation, budget
  changes, or additional live preview scenes.
- Unregistering an extension while work is queued, loading, waiting, rendering,
  reading back, encoding, or uploading cannot publish a stale result.

#### Stage 1 Handoff

- Baseline commit: `889ecb7dade9c95014b7b813e2702f03017b7e10`.
- Working set: `Thumbnail/RenderedAssetThumbnailCache.h`,
  `Thumbnail/RenderedAssetThumbnailCache.cpp`,
  `Thumbnail/RenderedAssetThumbnailPipeline.h`,
  `Thumbnail/RenderedAssetThumbnailPreviewScene.cpp`, and the temporary
  Material, TextureCube, and StaticMesh adapter headers/implementations.
- Key symbols: `FRenderedAssetThumbnailService`,
  `FRenderedAssetThumbnailCache`, `FRenderedAssetThumbnailPreviewScenePool`,
  and `IRenderedAssetThumbnailGenerationSession`.
- Decisions: cache construction accepts the long-lived service; the compatibility
  constructor resolves a temporary DurinEd-owned service; the preview pool leases
  only a world and provider-neutral view; upload tickets carry cancellation and
  provider generation; transparency is provider-selected presentation metadata
  and does not change persistent identity.
- Open questions: none for the provider-neutral core. Stage 2 can use the frozen
  module/document contract without changing thumbnail ownership.
- Validation: all 66 `ThumbnailTests`, 78 `MaterialTests`, and the Vulkan scene
  import test passed; the full `all` target built; generic rendered orchestration
  contains no concrete Material, TextureCube, or StaticMesh symbols; and
  changed-document/all-plan validation passed.

### Stage 2: Add the StaticMesh Inspector Module

Dependencies: Stage 1.

- [x] Add `StaticMeshEditor.dmodule`, CMake target, module entry point, public
  registration API, workspace descriptor, and DurinEditor/MainFrame wiring.
- [x] Register exact `DStaticMesh` assets as closable per-resource documents and
  reuse the shared compatibility rejection and package release policy.
- [x] Implement a module-private preview controller and scene with orbit, pan,
  zoom, reset/frame selection, deterministic initial framing, resize handling,
  and correct render-resource teardown.
- [x] Present asset path, LOD count, selected LOD, vertex/index/triangle counts,
  section/material-slot counts, and local bounds only where existing public
  runtime contracts can report them reliably.
- [x] Add shaded/wireframe visualization controls and a stable fallback for
  unavailable, empty, failed, incompatible, moved, deleted, or reimported
  assets.
- [x] Add workspace/document, input, lifecycle, and preview rendering tests.

#### Acceptance Gate

- Double-clicking a valid StaticMesh opens or activates exactly one document for
  that resource; multiple different meshes can coexist as documents.
- Opening an incompatible package changes neither the active document nor
  package ownership and reports the standard compatibility guidance.
- Preview navigation remains responsive, statistics match validated render
  data, and close/shutdown releases scene, asset, and render resources without a
  whole-device idle wait.
- The workspace never becomes dirty and global Save is disabled for its active
  document.

#### Stage 2 Handoff

- Baseline commit: `f6508157be9b3445674f627d21aaa1e0c7dd483d`.
- Working set: `Engine/Source/Editor/StaticMeshEditor/`,
  `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`,
  `Engine/Engine.dproject`, and `StaticMeshEditorTests.cpp`.
- Key symbols: `FStaticMeshEditorModule`, `MStaticMeshInspector`,
  `FStaticMeshPreview`, and `FStaticMeshPreviewController`.
- Decisions: the module/workspace type remains `StaticMeshEditor` while the UI
  says **StaticMesh Inspector**; documents are exact-class, closable,
  per-resource, and read-only; preview scenes are created lazily per document;
  revision transitions reassign and deterministically reframe the mesh; scene
  teardown detaches the mesh before viewport/world destruction.
- Open questions: none for the inspector. Stage 3 can move the temporary
  StaticMesh thumbnail adapter without changing this workspace contract.
- Validation: all 48 `StaticMeshTests` passed, including inspector registration,
  duplicate activation, multi-document close/reopen, exact-route rejection,
  no-save behavior, and camera input; the full `all` target built; and the
  editor completed an 8-tick hidden-window startup/shutdown smoke test.

### Stage 3: Move StaticMesh Thumbnail Ownership

Dependencies: Stage 2.

- [x] Move the StaticMesh provider, immutable input, dependency capture,
  readiness/revision handling, bounds framing, preview assignment, visual
  contract, and diagnostics into `StaticMeshEditor`.
- [x] Register the StaticMesh thumbnail extension alongside the workspace and
  roll both back if either contribution cannot be installed.
- [x] Share only pure framing or presentation helpers with the inspector where
  this removes duplication without coupling their live resource lifetimes.
- [x] Move StaticMesh-specific tests to the owning module while retaining
  provider-neutral scheduler/cache tests in `DurinEd`.
- [x] Verify old persistent StaticMesh thumbnails remain warm hits when no
  visual-contract field changed.

#### Acceptance Gate

- Removing `StaticMeshEditor` from the runtime variant removes both its asset
  open route and thumbnail support while the Content Browser safely falls back
  to the StaticMesh icon.
- `DurinEd` contains no StaticMesh thumbnail implementation detail.
- Cold, warm, dependency invalidation, revision race, corruption recovery,
  framing, transparent output, close, and shutdown coverage remains passing.

#### Stage 3 Handoff

- Baseline commit: `2cfe8131de502431e7e36d36d22b55585b0cb557`.
- Working set: `StaticMeshEditor/Thumbnail/StaticMeshAssetThumbnail.*`,
  `StaticMeshEditorModule.*`, `RenderedAssetThumbnailCache.h`,
  `MainFrameModule.cpp`, and `StaticMeshAssetThumbnailTests.cpp`.
- Key symbols: `FStaticMeshAssetThumbnailProvider`,
  `FStaticMeshThumbnailGenerationSession`,
  `FStaticMeshEditorModule::RegisterStaticMeshEditor`, and
  `GetDefaultRenderedAssetThumbnailService`.
- Decisions: MainFrame resolves the long-lived service before loading concrete
  editor modules; StaticMeshEditor installs workspace and thumbnail handles as
  one rollback-safe integration; unload removes thumbnail admission first; the
  provider identity and every visual/key contract field remain unchanged; the
  interactive inspector camera stays separate because its orbit/pan state is a
  different contract from card-size-independent thumbnail framing.
- Open questions: none for StaticMesh ownership. Stage 4 can reuse the same
  integration pattern for Material and Texture without changing the shared core.
- Validation: all 7 `StaticMeshThumbnailTests`, 49 `StaticMeshTests`, 59
  provider-neutral/remaining `ThumbnailTests`, and 78 Vulkan-backed
  `MaterialTests` passed; a preseeded compatible cache object remained a
  no-load/no-render warm hit; `DurinEd` contains no StaticMesh thumbnail
  symbols; the full `all` target built; and the editor completed an 8-tick
  hidden-window lifecycle smoke test.

### Stage 4: Move Material and Texture Thumbnail Ownership

Dependencies: Stage 3.

- [x] Move Material and MaterialInstance key capture, dependency closure,
  resource readiness, preview fixture setup, and diagnostics into
  `MaterialEditor` under two exact-class registrations sharing one implementation.
- [x] Move authored Texture2D and TextureCube asset-specific thumbnail capture,
  readiness, preview setup, orientation, and diagnostics into `TextureEditor`.
- [x] Keep source-file image decode and generic Content Browser presentation out
  of concrete asset editor modules; rename or relocate remaining code if current
  names imply the wrong ownership.
- [x] Bundle each module's workspace and thumbnail registrations behind one
  symmetric editor-integration lifecycle.
- [x] Move concrete tests to their owning modules and keep cross-provider
  scheduling, cache, budget, and persistence tests with `DurinEd`.

#### Acceptance Gate

- Each authored asset class receives thumbnail behavior only while its owning
  editor module is registered, and unsupported classes retain their normal icon.
- Removing any one concrete module neither disables the shared service nor
  changes thumbnails owned by other modules.
- Existing Material, MaterialInstance, Texture2D, TextureCube, and source-file
  cold/warm/failure behavior remains unchanged except for documented intentional
  visual-contract changes.

#### Stage 4 Handoff

- Baseline commit: `878cc1e40c8061e3b8cb3f5237b61fcf558303cc`.
- Working set: `MaterialEditor/Thumbnail/MaterialAssetThumbnail.*`,
  `TextureEditor/Thumbnail/Texture2DAssetThumbnail.*`,
  `TextureEditor/Thumbnail/TextureCubeAssetThumbnail.*`, the Material/Texture
  module entry points, `RenderedAssetThumbnailCache.*`, Content Browser model
  routing, and the three thumbnail test targets.
- Key symbols: `FMaterialEditorModule::RegisterMaterialEditor`,
  `FTextureEditorModule::RegisterTextureEditor`,
  `FTexture2DAssetThumbnailProvider`, and
  `FRenderedAssetThumbnailService::CaptureSourceImage`.
- Decisions: the one service registry admits both rendered extensions and the
  Texture2D source-selection provider; source decode/cache/upload remains generic;
  Content Browser checks live registrations instead of concrete class names;
  module unload removes thumbnail admission before workspace routes; the default
  service owns no concrete providers.
- Open questions: none for ownership migration. Stage 5 should stress mixed
  module unload and in-flight work through the completed composition boundary.
- Validation: all 6 `MaterialThumbnailTests`, 6 `TextureThumbnailTests`, 52
  provider-neutral `ThumbnailTests`, 7 `StaticMeshThumbnailTests`, 78 Vulkan
  `MaterialTests`, and the Content Browser workflow suite passed; full build and
  editor smoke qualification are recorded by Stage 5 rather than duplicated here.

### Stage 5: Integration, Documentation, and Editor Qualification

Dependencies: Stages 2-4.

- [x] Exercise double-click routing, document activation/close, Content Browser
  refresh/move/delete/reimport, module registration rollback, and editor shutdown
  with mixed asset types and in-flight thumbnails.
- [x] Stress visible/prefetch priority, duplicate coalescing, provider unload,
  cache budgets, one-render-per-frame behavior, and warm-cache restart behavior.
- [x] Update `WorkspaceFramework.md`, `AssetThumbnails.md`, Content Browser
  documentation, and a user-facing StaticMesh Inspector guide.
- [x] Run focused native tests, documentation validation, and the
  repository-required full `all` build and editor lifecycle smoke validation
  through the documented DurinDevTool workflow.

#### Acceptance Gate

- All focused and cross-module tests pass, plan/document validation passes, and
  the full editor build and smoke run succeed on the active Agent Build Profile.
- Documentation names the exact owner of every workspace route, asset thumbnail
  extension, provider-neutral service, source-file preview, and shutdown action.
- No concrete asset editor module is required by `DurinEd`, and no concrete
  thumbnail implementation remains centralized there.

#### Stage 5 Handoff

- Baseline commit: `316e045d467beca86496ac3efbe7d7610fa8fc4c`.
- Working set: `StaticMeshAssetThumbnailTests.cpp`, `WorkspaceFramework.md`,
  `AssetThumbnails.md`, `ContentBrowser.md`, and the new user-facing
  `Guides/StaticMeshInspector.md`.
- Key symbols: `FRenderedAssetThumbnailService`,
  `FRenderedAssetThumbnailCache`, `FMaterialEditorModule`,
  `FTextureEditorModule`, and `FStaticMeshEditorModule`.
- Decisions: mixed providers share one service and bounded scheduler; MainFrame
  owns reverse-order integration removal and service draining; each module
  removes thumbnail admission before its workspace; raw source files remain in
  the generic Content Browser path; the public workspace remains named
  **StaticMesh Inspector** while its C++ module/root stays `StaticMeshEditor`.
- Open questions: none. Deferred authoring and plugin-discovery work remains in
  Deferred Follow-ups and is outside this completed plan.
- Validation: 52 `ThumbnailTests`, 6 `MaterialThumbnailTests`, 6
  `TextureThumbnailTests`, 8 `StaticMeshThumbnailTests`, and 49
  `StaticMeshTests` passed. `EditorAssetWorkflowTests` executed 72 tests with 71
  passed and one skipped. Changed-document and all-plan validation passed; the
  full `all` target built on `Win64-Debug-DurinEditor-Tests`; and DurinEditor
  completed the Sandbox hidden-window eight-tick lifecycle smoke run.

## Validation Matrix

| Concern | Unit/contract | Integration | Rendering/end-to-end |
| --- | --- | --- | --- |
| Workspace route | Exact-class registration and duplicate rejection | Open/activate/close multiple mesh documents | Double-click Content Browser card opens inspector |
| Compatibility and ownership | Compatible/incompatible load reports | Failed open preserves active document and package owners | Move/delete/reimport while open fails safely |
| Inspector preview | Framing and navigation math | Resize, resource ready/failure, mode switches | Shaded/wireframe image and lifecycle smoke |
| Statistics | Bounds/count extraction and unavailable states | Reimport updates the displayed snapshot | Representative single/multi-section meshes |
| Extension lifecycle | Register/unregister generations and fake sessions | Unload in each scheduler/pipeline state | No stale texture or provider code after unload |
| Cache compatibility | Stable key vectors before/after migration | Old object is a warm hit | Restart avoids asset load and render |
| Module isolation | Dependency metadata and link closure | Remove one module from runtime variant | Other asset thumbnails continue normally |
| Budgets and cancellation | Priority, coalescing, LRU, serials | Mixed providers share one bounded core | Close/shutdown releases all resources |

## Definition of Done

- Every stage acceptance gate is satisfied and evidence is recorded in Current
  Status and stage handoffs.
- `StaticMeshEditor` owns the StaticMesh Inspector, exact asset-open route, and
  StaticMesh thumbnail extension.
- `MaterialEditor` and `TextureEditor` own thumbnail behavior for their authored
  asset classes.
- `DurinEd` owns only provider-neutral workspace and thumbnail infrastructure and
  has no dependency on a concrete asset editor module.
- Raw source-file previews remain available through the generic Content Browser
  path.
- Lasting ownership and lifecycle contracts are moved into the relevant editor
  architecture documents.
- Focused tests, plan/document validation, the full `all` build, and editor smoke
  validation pass.
- The plan is marked Completed with final handoff evidence; later archive
  maintenance moves it according to the plan lifecycle rules.

## Deferred Follow-ups

- Editable material-slot overrides with transactions, dirty-state, and save.
- Collision, socket, LOD, import-setting, and reimport tools inside the
  StaticMesh workspace.
- User-authored thumbnail camera or presentation overrides.
- Shared preview-toolbar widgets across asset editors after at least two editors
  demonstrate stable identical behavior.
- General plugin discovery for asset editors outside the built-in MainFrame
  composition root.

## Related Documentation

- `Documentation/Editor/Architecture/WorkspaceFramework.md`
- `Documentation/Editor/Architecture/AssetThumbnails.md`
- `Documentation/Editor/Architecture/ContentBrowser.md`
- `Documentation/Runtime/Rendering/StaticMeshRendering.md`
- `Documentation/Development/Build/BuildAndRun.md`

## Related Code

- `Engine/Source/Editor/DurinEd/Public/Editor/EditorWorkspace.h`
- `Engine/Source/Editor/DurinEd/Public/Thumbnail/AssetThumbnail.h`
- `Engine/Source/Editor/DurinEd/Public/Thumbnail/RenderedAssetThumbnailPipeline.h`
- `Engine/Source/Editor/MaterialEditor/Private/Thumbnail/MaterialAssetThumbnail.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Thumbnail/Texture2DAssetThumbnail.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Thumbnail/TextureCubeAssetThumbnail.cpp`
- `Engine/Source/Editor/DurinEd/Private/Thumbnail/RenderedAssetThumbnailPreviewScene.cpp`
- `Engine/Source/Editor/MaterialEditor/`
- `Engine/Source/Editor/TextureEditor/`
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`
- `Engine/Engine.dproject`
