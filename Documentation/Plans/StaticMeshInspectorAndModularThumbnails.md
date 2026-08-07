# Static Mesh Inspector and Modular Thumbnails Plan

Summary: Add a dedicated StaticMesh editor workspace and move asset-specific thumbnail generation behind registrations owned by the corresponding editor modules.

Last reviewed: 2026-08-08

Status: Active
Completed:

## Current Status

Planning is complete and implementation has not started. The editor already has
per-resource Material and Texture workspaces, exact-class asset-open routes, a
shared document host, and a provider registry for Content Browser thumbnails.
StaticMesh rendered thumbnails are implemented and validated, but their provider,
asset lifecycle handling, framing, and preview-scene setup currently live in
`DurinEd` beside the shared scheduler and cache. Material and TextureCube
thumbnail implementations are centralized there in the same way.

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

- [ ] Define `StaticMeshEditor` module dependencies, MainFrame registration
  order, startup rollback, shutdown order, and DurinEditor variant membership.
- [ ] Define the read-only StaticMesh document lifecycle, compatibility failure
  behavior, package ownership, tab identity, and no-dirty/no-save policy.
- [ ] Split the thumbnail contract into provider-neutral orchestration and a
  module-owned rendered-generation extension/session boundary.
- [ ] Specify registration-handle behavior for duplicate exact classes,
  provider replacement, unload during every asynchronous state, and cache
  instances created before or after registration.
- [ ] Record the ownership of raw source-file previews separately from authored
  Texture2D/TextureCube providers.
- [ ] Add contract tests using fake extensions before migrating a real provider.

#### Acceptance Gate

- Dependency arrows are one-way from each concrete editor module to `DurinEd`.
- A fake extension can register, capture a deterministic request, run a cold
  generation session, produce a warm hit, unregister, and reject every stale
  completion without concrete asset types appearing in the core path.
- The contract defines behavior for registration failure and module unload with
  no live provider object or asset reference crossing the unload boundary.

### Stage 1: Generalize the Rendered Thumbnail Core

Dependencies: Stage 0.

- [ ] Move `FRenderedAssetThumbnailCache` and its statistics to provider-neutral
  headers whose names do not imply Material ownership.
- [ ] Let the shared cache consume registrations from the long-lived thumbnail
  service instead of constructing concrete providers internally.
- [ ] Replace concrete active-asset pointers, generation-input casts, readiness
  branches, framing branches, and diagnostics with one active extension session.
- [ ] Keep the shared preview target, render/readback machinery, PNG pipeline,
  scheduler, object store, GPU upload cache, and budgets in `DurinEd`.
- [ ] Ensure registration generation participates in request capture and every
  asynchronous completion check.
- [ ] Preserve deterministic keys and warm-cache behavior while running existing
  Material, TextureCube, and StaticMesh providers through temporary adapters.

#### Acceptance Gate

- `DurinEd` rendered-thumbnail orchestration has no include, forward declaration,
  cast, active pointer, or control-flow branch for Material, TextureCube, or
  StaticMesh asset classes.
- Existing thumbnail tests pass without key churn, cache invalidation, budget
  changes, or additional live preview scenes.
- Unregistering an extension while work is queued, loading, waiting, rendering,
  reading back, encoding, or uploading cannot publish a stale result.

### Stage 2: Add the StaticMesh Inspector Module

Dependencies: Stage 1.

- [ ] Add `StaticMeshEditor.dmodule`, CMake target, module entry point, public
  registration API, workspace descriptor, and DurinEditor/MainFrame wiring.
- [ ] Register exact `DStaticMesh` assets as closable per-resource documents and
  reuse the shared compatibility rejection and package release policy.
- [ ] Implement a module-private preview controller and scene with orbit, pan,
  zoom, reset/frame selection, deterministic initial framing, resize handling,
  and correct render-resource teardown.
- [ ] Present asset path, LOD count, selected LOD, vertex/index/triangle counts,
  section/material-slot counts, and local bounds only where existing public
  runtime contracts can report them reliably.
- [ ] Add shaded/wireframe visualization controls and a stable fallback for
  unavailable, empty, failed, incompatible, moved, deleted, or reimported
  assets.
- [ ] Add workspace/document, input, lifecycle, and preview rendering tests.

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

### Stage 3: Move StaticMesh Thumbnail Ownership

Dependencies: Stage 2.

- [ ] Move the StaticMesh provider, immutable input, dependency capture,
  readiness/revision handling, bounds framing, preview assignment, visual
  contract, and diagnostics into `StaticMeshEditor`.
- [ ] Register the StaticMesh thumbnail extension alongside the workspace and
  roll both back if either contribution cannot be installed.
- [ ] Share only pure framing or presentation helpers with the inspector where
  this removes duplication without coupling their live resource lifetimes.
- [ ] Move StaticMesh-specific tests to the owning module while retaining
  provider-neutral scheduler/cache tests in `DurinEd`.
- [ ] Verify old persistent StaticMesh thumbnails remain warm hits when no
  visual-contract field changed.

#### Acceptance Gate

- Removing `StaticMeshEditor` from the runtime variant removes both its asset
  open route and thumbnail support while the Content Browser safely falls back
  to the StaticMesh icon.
- `DurinEd` contains no StaticMesh thumbnail implementation detail.
- Cold, warm, dependency invalidation, revision race, corruption recovery,
  framing, transparent output, close, and shutdown coverage remains passing.

### Stage 4: Move Material and Texture Thumbnail Ownership

Dependencies: Stage 3.

- [ ] Move Material and MaterialInstance key capture, dependency closure,
  resource readiness, preview fixture setup, and diagnostics into
  `MaterialEditor` under two exact-class registrations sharing one implementation.
- [ ] Move authored Texture2D and TextureCube asset-specific thumbnail capture,
  readiness, preview setup, orientation, and diagnostics into `TextureEditor`.
- [ ] Keep source-file image decode and generic Content Browser presentation out
  of concrete asset editor modules; rename or relocate remaining code if current
  names imply the wrong ownership.
- [ ] Bundle each module's workspace and thumbnail registrations behind one
  symmetric editor-integration lifecycle.
- [ ] Move concrete tests to their owning modules and keep cross-provider
  scheduling, cache, budget, and persistence tests with `DurinEd`.

#### Acceptance Gate

- Each authored asset class receives thumbnail behavior only while its owning
  editor module is registered, and unsupported classes retain their normal icon.
- Removing any one concrete module neither disables the shared service nor
  changes thumbnails owned by other modules.
- Existing Material, MaterialInstance, Texture2D, TextureCube, and source-file
  cold/warm/failure behavior remains unchanged except for documented intentional
  visual-contract changes.

### Stage 5: Integration, Documentation, and Editor Qualification

Dependencies: Stages 2-4.

- [ ] Exercise double-click routing, document activation/close, Content Browser
  refresh/move/delete/reimport, module registration rollback, and editor shutdown
  with mixed asset types and in-flight thumbnails.
- [ ] Stress visible/prefetch priority, duplicate coalescing, provider unload,
  cache budgets, one-render-per-frame behavior, and warm-cache restart behavior.
- [ ] Update `WorkspaceFramework.md`, `AssetThumbnails.md`, Content Browser
  documentation, and a user-facing StaticMesh Inspector guide.
- [ ] Run focused native tests, documentation validation, and the
  repository-required full `all` build and editor lifecycle smoke validation
  through the documented DurinDevTool workflow.

#### Acceptance Gate

- All focused and cross-module tests pass, plan/document validation passes, and
  the full editor build and smoke run succeed on the active Agent Build Profile.
- Documentation names the exact owner of every workspace route, asset thumbnail
  extension, provider-neutral service, source-file preview, and shutdown action.
- No concrete asset editor module is required by `DurinEd`, and no concrete
  thumbnail implementation remains centralized there.

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
- `Engine/Source/Editor/DurinEd/Private/Thumbnail/MaterialAssetThumbnail.cpp`
- `Engine/Source/Editor/DurinEd/Private/Thumbnail/RenderedAssetThumbnailPreviewScene.cpp`
- `Engine/Source/Editor/MaterialEditor/`
- `Engine/Source/Editor/TextureEditor/`
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`
- `Engine/Engine.dproject`
