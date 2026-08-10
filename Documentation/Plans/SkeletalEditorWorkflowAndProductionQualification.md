# Skeletal Editor Workflow and Production Qualification Plan

Summary: Make imported Skeleton, SkeletalMesh, and AnimationClip graphs discoverable, inspectable, previewable, reimportable, and production-qualified in the editor.

Last reviewed: 2026-08-11

Status: Active
Completed:

## Current Status

This plan executes Skeletal Mesh and Animation roadmap S4 against baseline
`651b3f84`. S1-S3 already provide deterministic glTF/GLB skeletal Scene import,
stable Skeleton/SkeletalMesh/AnimationClip peer assets and import records,
record-level reimport, runtime playback, GPU skinning, conservative bounds, and
Debug Editor/Shipping Game validation.

The editor already exposes the provider-neutral Scene Source dialog and can
publish or reimport the skeletal peer graph. The Content Browser can enumerate
the resulting packages and route record actions, but it does not present a
skeletal-specific workflow. `DSkeleton`, `DSkeletalMesh`, and `DAnimationClip`
have no exact-class asset-editor registrations, dedicated details, hierarchy or
track views, animation preview, playback controls, or skeletal thumbnail
provider. Existing documentation still describes Scene outputs primarily in
terms of static meshes, materials, and textures.

S4 therefore begins at presentation and workflow integration rather than source
decoding. It reuses `AssetImportCore`, `StandardAssetImport`, Content Browser,
`FEditorWorkspaceManager`, `FPreviewScene`, `DSkeletalMeshComponent`, and the
shared thumbnail service. It does not create a second importer, import record,
preview world, renderer, or animation runtime.

## Goal

Let an editor user import or reimport a supported glTF/GLB skeletal scene,
identify every generated peer, inspect its relationships and diagnostics,
preview the mesh and compatible clips with deterministic playback controls,
and carry the same graph through save, reload, cook, and Shipping runtime
without using command-line or test-only seams.

## Scope

- Skeletal output roles, relationships, estimates, warnings, and destinations
  in the existing Scene Source preview and completion workflow.
- Content Browser identity, filters/icons, details, source-record navigation,
  record-level reimport/recreate actions, and skeletal package refresh.
- A `SkeletalMeshEditor` module that registers read-only documents for exact
  `DSkeleton`, `DSkeletalMesh`, and `DAnimationClip` classes.
- Bounded Skeleton hierarchy, SkeletalMesh geometry/material/palette, and
  AnimationClip track/key/interpolation inspection.
- SkeletalMesh reference-pose preview and compatible AnimationClip playback
  using the production component, scene, renderer, and viewport contracts.
- Play, pause, loop, playback-rate, reset, and timeline scrub controls whose
  state belongs only to the open preview document.
- A deterministic reference-pose SkeletalMesh thumbnail provider registered
  through the shared rendered-thumbnail service.
- Import, reimport, asset replacement, move/delete, failure, cancellation,
  module unload, cook, runtime-only load, and editor/game qualification.
- Lasting editor architecture and user workflow documentation.

## Non-Goals

- Adding FBX skeletal import, new glTF extensions, or changing normalized
  source, payload, DDC, cook, or compatibility schemas.
- Single-asset reimport for one peer from a multi-output Scene record; skeletal
  peers continue to reimport transactionally through their owning record.
- Editing skeleton hierarchies, bind poses, mesh topology, influences, material
  assignments, animation keys, or source import settings in an inspector.
- Animation graphs, blending, state machines, retargeting, root motion, events,
  IK, control rigs, keyframe authoring, compression, or streaming.
- Animated Content Browser thumbnails or thumbnails for Skeleton and
  AnimationClip assets.
- A skeletal-only preview renderer, material system, viewport implementation,
  scene mutation path, or duplicate playback evaluator.
- Directional shadow execution, additional skeletal LODs, or compute skinning.

## Design Decisions and Invariants

### Ownership and module boundary

- `AssetImportCore` continues to own planning, records, reconciliation,
  transactional publication, cancellation, and record actions.
- `StandardAssetImport` continues to own glTF/GLB decoding and Scene output
  construction. UI code consumes plan previews and execution results without
  parsing source files or third-party types.
- `LevelEditor` continues to own the Scene Source dialog and Content Browser
  presentation. It does not load assets merely to draw a card or details row.
- A new `SkeletalMeshEditor` module owns the three exact-class asset routes,
  read-only inspector workspace, skeletal preview controllers, and the
  SkeletalMesh thumbnail extension. `DurinEd` retains provider-neutral
  workspaces, preview scenes, thumbnail scheduling, persistence, and budgets.
- Runtime `Engine`/`Renderer` types gain editor-neutral inspection or revision
  accessors only when current immutable state cannot be observed safely. They
  do not depend on editor modules or import records.

### Import and reimport workflow

- The existing Scene Source workflow is the only import entry. A supported
  glTF/GLB plan explicitly presents Skeleton, SkeletalMesh, AnimationClip,
  StaticMesh, Material, and Texture outputs that are actually populated; there
  is no primary skeletal output.
- External sources use the existing mounted-source ingestion transaction.
  Mounted sources retain their virtual path. Preview and execution use the same
  immutable plan and must agree on output identities, dependencies, policies,
  collisions, warnings, byte estimates, missing outputs, and orphans.
- Selecting any managed skeletal peer resolves its `DImportRecord` through the
  current index. Reimport, recreate-missing, source repair, and diagnostics act
  on the complete record graph and never mutate one peer in isolation.
- Successful publication advances the existing mounted-content and registry
  revisions before Content Browser refresh. Navigation may reveal the output
  directory, record, or a selected peer, but it does not invent ownership.

### Document identity and inspection

- One closable read-only workspace hosts documents keyed by exact asset class
  and normalized virtual path. Opening the same identity activates its existing
  document; different classes or paths cannot alias one another.
- Documents retain counted asset references only while open and revalidate
  package, import-record, payload, render-resource, and dependency revisions
  before exposing derived state. Close, project switch, module unload, and
  shutdown release every document and preview lease.
- Skeleton rows preserve canonical parent-before-child index order and expose
  index, name, parent, and reference transform through a clipped/virtualized
  view. The inspector never recursively traverses unbounded child widgets.
- Mesh details expose Skeleton identity, compatibility, LOD 0 summary, bounds,
  palette count, section ranges, and material slots without editing them.
- Clip details expose Skeleton identity, duration, track/key counts, and
  per-track bone/path/interpolation/key ranges through bounded tables.
- Source and record details use package inspection and record-index metadata
  when possible. Drawing ordinary Content Browser details never loads the
  package or scans every asset.

### Preview and playback

- Every active preview owns one `FPreviewScene`, actor, and
  `DSkeletalMeshComponent`. It uses the production render resources, proxy,
  material fallback, animation instance, palette publication, bounds,
  visibility, passes, post-process, and output path.
- A SkeletalMesh document begins in reference pose. A clip may be selected only
  when its structural Skeleton compatibility matches the mesh.
- An AnimationClip document resolves candidate preview meshes only from the
  same import record. Candidate paths are ordered deterministically, inspected
  without global object scans, and loaded lazily after selection. If no
  compatible peer exists, metadata remains available and preview reports one
  explicit unavailable reason.
- Preview-only time, looping, rate, paused state, selected clip/mesh, camera,
  wireframe, and lit state never dirty an authored package. Scrubbing sets one
  exact clip time and evaluates synchronously; play advances through the normal
  component tick contract.
- Only the active/visible document renders. Inactive documents retain small UI
  state but release or hide live preview work according to the shared preview
  lifetime contract.

### Thumbnail contract

- Only `DSkeletalMesh` receives a rendered provider. The fixed visual contract
  is reference pose, LOD 0, default material slots, deterministic elevated
  three-quarter framing, and transparent output.
- The persistent key includes mesh, Skeleton, default material/texture closure,
  provider schema, preview fixture, shader/visual contract, and fixed output
  settings. A thumbnail session loads only after a cache miss and revalidates
  asset, dependency, render-resource, and provider generations before publish.
- Invalid compatibility, payload, bounds, render resources, or dependencies
  retain the ordinary SkeletalMesh icon and one stable asset-qualified
  diagnostic. Failures are not persisted as successful thumbnails.

### Failure and ordering

- Import, reimport, recreation, document binding, preview selection, and
  thumbnail generation are complete-or-unavailable transitions. Failure keeps
  the previous authored graph and never displays a stale incompatible preview.
- Import dialog close, newer preview request, project switch, provider removal,
  editor shutdown, document close, and thumbnail-provider removal cancel and
  drain the exact owned work before releasing callbacks or module code.
- Import diagnostics retain category, severity, source subject, output role,
  and record context. Inspector/preview failures add asset and dependency
  identity without collapsing missing source, incompatibility, payload,
  render-resource, or device failure into one generic message.
- MainFrame unload removes `SkeletalMeshEditor` thumbnail registrations before
  its workspace routes and before shared preview/thumbnail services drain.

## Current Foundations and Gaps

| Area | Existing foundation | S4 gap |
| --- | --- | --- |
| Scene import | Provider-neutral async plan/execute UI and deterministic skeletal peer publication already work for glTF/GLB | Preview/completion presentation and guides do not make skeletal roles, relationships, and output locations explicit |
| Import records | Every skeletal peer is indexed under one multi-output record with reimport/recreate/orphan semantics | Skeletal record status and actions are not presented as one coherent user workflow |
| Content Browser | Generic package enumeration, exact-class filters, asset routes, record actions, revision refresh, and thumbnail facade exist | No skeletal-specific identity/details/routes; only ordinary icons are available |
| Workspace | Exact-class registration, closable resource documents, compatibility policy, and StaticMesh inspector patterns exist | No module or document owner for Skeleton, SkeletalMesh, or AnimationClip |
| Preview | Shared preview scenes and production skeletal playback/rendering are complete | No editor controller connects a skeletal asset graph to preview, playback, camera, or unavailable states |
| Thumbnails | Shared bounded rendered-thumbnail service and StaticMesh provider contract exist | No reference-pose SkeletalMesh provider or dependency/invalidation key |
| Qualification | Import/runtime/render fixtures, Debug/Shipping Vulkan tests, cook, and hidden-window smoke exist | No user-visible import-to-preview-to-reimport workflow or editor lifecycle qualification |

## Implementation Stages

### Stage 0: Freeze the editor workflow and entry baselines

Dependencies: completed S1-S3 and baseline `651b3f84`.

- [ ] Record current Scene dialog output rows, completion behavior, Content
  Browser class/details/actions, record lookup, route registration, preview
  scene, thumbnail extension, and shutdown behavior for the three asset types.
- [ ] Freeze exact user journeys for mounted and external glTF/GLB import,
  record reimport, missing-output recreation, asset opening, clip preview, and
  cook/run qualification.
- [ ] Select the final workspace name, module dependencies, class icons/labels,
  details fields, hierarchy/track table columns, playback controls, preview
  camera, and reference-pose thumbnail visual contract.
- [ ] Prove same-record compatible-mesh discovery can use import/package
  metadata without a whole-project object scan; record a bounded fallback if
  the existing metadata is insufficient.
- [ ] Freeze representative fixture outputs, package/record fingerprints,
  preview pose times, image tolerances, thumbnail key fields, counters, and
  affected focused test targets before source implementation.

#### Acceptance Gate

- The workflow starts from the existing Scene Source entry and names every
  user-visible step through reimport, preview, cook, and runtime launch.
- Module, thread, lifetime, compatibility, failure, ordering, and budget
  boundaries are selected without reopening S1-S3 schemas or renderer design.
- Existing static Scene, StaticMesh inspector, Content Browser, import record,
  and thumbnail baselines are captured for regression comparison.

### Stage 1: Present skeletal Scene import and Content Browser identity

Dependencies: Stage 0 workflow and presentation contract.

- [ ] Extend Scene Source preview/completion presentation to group populated
  Skeleton, SkeletalMesh, AnimationClip, StaticMesh, Material, and Texture
  outputs with relationships, policy, estimates, warnings, collisions, and
  destinations from the immutable plan.
- [ ] Make successful import reveal the published output directory and retain
  stable access to the import record without choosing a false primary asset.
- [ ] Add exact Skeleton, SkeletalMesh, and AnimationClip labels/icons/filters
  and bounded package-inspection details to Content Browser presentation.
- [ ] Expose owning-record status, navigate-to-record/output, reimport, recreate
  missing outputs, repair, orphan, and failure diagnostics consistently from
  every managed skeletal peer using existing record actions.
- [ ] Cover mounted/external glTF, external-buffer dependency closure, GLB,
  empty static/skeletal categories, collisions, cancellation, and refresh.

#### Acceptance Gate

- A user can predict every skeletal output and warning before confirmation and
  locate every published peer afterward.
- All skeletal peer actions address one import record transaction; no peer
  gains an independent source authority or single-asset reimport path.
- Content Browser drawing remains package-inspection based and does not load or
  globally scan skeletal assets.

### Stage 2: Add read-only skeletal asset documents

Dependencies: Stage 1 identity, record, and navigation presentation.

- [ ] Add `SkeletalMeshEditor` with one scoped integration batch for the
  workspace and exact `DSkeleton`, `DSkeletalMesh`, and `DAnimationClip` routes.
- [ ] Implement class+path document identity, open/activate/close, compatibility
  rejection, unavailable state, layout reset, module unload, and shutdown.
- [ ] Implement virtualized Skeleton hierarchy and AnimationClip track tables
  plus bounded SkeletalMesh geometry, palette, section, bound, and material
  details from immutable asset state.
- [ ] Present exact Skeleton references, compatibility identities, cooked/DDC
  storage status, import-record/source status, and asset-qualified diagnostics.
- [ ] Keep all documents read-only: inspection and navigation never dirty or
  save an authored package.

#### Acceptance Gate

- Double-clicking each exact class opens or focuses the correct independent
  document and cannot alias another asset or class.
- Maximum supported hierarchy/track counts remain responsive through clipped
  views and bounded formatting; ordinary drawing performs no recursive widget
  explosion or whole-project scan.
- Missing, incompatible, moved, reimported, or unloaded assets produce a stable
  unavailable state and release every counted reference on close/unload.

### Stage 3: Add production skeletal preview and clip playback

Dependencies: Stage 2 document and lifetime owner.

- [ ] Build a SkeletalMesh preview controller on `FPreviewScene` and
  `DSkeletalMeshComponent` with reference pose, deterministic framing,
  orbit/pan/zoom, frame selection, Lit/Unlit, and Solid/Wireframe controls.
- [ ] Resolve and list compatible same-record clips for a mesh and compatible
  same-record meshes for a clip through metadata-first, path-stable selection.
- [ ] Add play, pause, loop, rate, reset, exact timeline scrub, duration, and
  current-time presentation using the production animation instance.
- [ ] Rebind atomically on mesh, Skeleton, clip, material, payload, render-
  resource, record, and device revisions; reject incompatible combinations
  without retaining stale pose or geometry.
- [ ] Ensure inactive/closed documents stop rendering and release preview
  actors, components, scene membership, targets, and callbacks in owner order.

#### Acceptance Gate

- Reference, key, interpolated, loop, clamp, pause, rate, and scrub states match
  runtime CPU/GPU goldens and visibly deform the representative fixture.
- Mesh and clip selection is deterministic, compatibility-gated, record-local,
  and free of reflected-object reads on the rendering thread.
- Sequential documents and main/auxiliary editor views do not share mutable
  playback, camera, pose, target, or prepared-view state.

### Stage 4: Integrate thumbnails, reimport, and recovery

Dependencies: Stage 3 production preview path.

- [ ] Register one `DSkeletalMesh` rendered-thumbnail extension with immutable
  reference-pose inputs, deterministic dependency closure, and the shared
  scheduler/persistence budgets.
- [ ] Validate cold/warm cache, coalescing, priority, move/save/reimport
  invalidation, corruption recovery, provider replacement, and transparent
  output without changing existing thumbnail visual baselines.
- [ ] Exercise record reimport, missing-output recreation, unchanged reimport,
  output removal/orphans, occupied paths, source repair, failed publication,
  material replacement, and open inspector/thumbnail refresh.
- [ ] Exercise dialog/document close, newer request, project switch, module
  unload, device invalidation, and editor shutdown while import, preview,
  resource creation, render/readback, encoding, or upload work is pending.
- [ ] Conserve requested/loaded/ready/failed previews and thumbnail jobs and
  prove every component, scene entry, RHI resource, lease, and callback is
  released exactly once.

#### Acceptance Gate

- Reimport changes become one coherent package/record/document/preview/
  thumbnail revision without stale publication or partial peer graphs.
- A thumbnail cold miss uses the production reference pose and a warm hit loads
  no authored asset or preview scene; unsupported Skeleton/Clip cards retain
  stable icons and issue no rendered job.
- Cancellation, failure, unload, invalidation, and shutdown leave no import
  request, preview scene entry, thumbnail lease, callback, or RHI resource alive.

### Stage 5: Qualify the production workflow and close S4

Dependencies: Stages 0-4 and their handoffs.

- [ ] Run repository-authored data-URI glTF, external-buffer glTF, and GLB
  through external ingestion and mounted-source import, save/reload, unchanged
  and changed reimport, editor restart, clean DDC rebuild, and clean cook.
- [ ] Compare exact asset/record identities, peer relationships, diagnostics,
  preview poses/bounds/pixels, reference thumbnails, and runtime-only playback
  across source encodings and authored/DDC/cooked ownership.
- [ ] Run focused AssetImport, Content Browser, workspace/editor, skeletal
  asset/playback/rendering, thumbnail, viewport, reload, and lifecycle targets
  using repository guidance, including the plan-gated Shipping Game path.
- [ ] Run document validation, the required full Debug Editor `all` build, and
  hidden-window editor smoke from the same Agent Build Profile.
- [ ] Publish lasting Editor Architecture and Guide documentation, update the
  roadmap with S4 completion evidence, and record commits, fixtures, hashes,
  tolerances, budgets, counters, validation, and verified executables.

#### Acceptance Gate

- A user can complete import -> identify -> inspect -> preview/play -> reimport
  -> save/reload -> cook -> Shipping runtime using documented editor actions.
- Equivalent glTF/GLB and authored/DDC/cooked paths agree on identities,
  relationships, animation, bounds, diagnostics, and qualified pixels.
- Existing static import, StaticMesh inspector/thumbnail, Content Browser,
  viewport, renderer, and runtime paths retain their frozen behavior.
- Documentation, focused tests, full build, editor smoke, Shipping cook/load,
  and runtime playback pass with clean Vulkan validation and a verified editor.

## Validation Matrix

| Contract | Focused validation | Integration outcome |
| --- | --- | --- |
| Import presentation | Populated roles, relationships, estimates, warnings, collisions, destinations, cancellation | Preview and execution agree and every published peer is discoverable |
| Import record | Output lookup, record navigation, unchanged/changed reimport, recreate, repair, orphan, rollback | One transactional graph remains authoritative from every peer |
| Content Browser | Exact classes, filters/icons, package details, refresh, move/delete, no-load drawing | Skeletal peers are identifiable without object scans or false ownership |
| Documents | Exact route, class+path identity, compatibility, close/unload, layout, read-only state | Skeleton, mesh, and clip open independently and never dirty packages |
| Inspection | Maximum bones/tracks/keys, sections/materials/palette, source/storage diagnostics | Large valid assets remain bounded and relationship failures are explicit |
| Preview | Reference/key/interpolated/loop/clamp, pause/rate/scrub, camera/view modes | Production component and renderer reproduce runtime poses and pixels |
| Compatibility | Same-record peer discovery, multiple compatible meshes/clips, absent/incompatible peers | Selection is deterministic and stale or unrelated assets never preview |
| Thumbnail | Key closure, cold/warm, visual baseline, invalidation, corruption, unload | Reference-pose mesh cards remain bounded and stale work cannot publish |
| Lifecycle | Reimport, replacement, close, project switch, reload, device invalidation, shutdown | Requests, objects, scenes, resources, callbacks, and leases release once |
| Qualification | glTF/GLB, authored/DDC/cooked, Debug Editor, Shipping Game, full build/smoke | The documented user workflow reaches an equivalent runtime result |

## Definition of Done

- Existing Scene Source import visibly and transactionally handles complete
  skeletal peer graphs without a second importer or primary-output fiction.
- Skeleton, SkeletalMesh, and AnimationClip have exact, read-only, bounded
  Content Browser and inspector identities.
- SkeletalMesh and AnimationClip documents render deterministic compatible
  playback through the production component and renderer.
- SkeletalMesh thumbnails use the shared service, reference-pose contract, and
  complete dependency/invalidation identity.
- Reimport, failure, reload, cancellation, unload, cook, and runtime-only paths
  preserve complete state and release all owned work.
- Representative editor and Shipping workflows pass focused tests, full build,
  smoke, image/identity baselines, and clean Vulkan validation.
- Lasting behavior is documented outside the plan and S4 completion is recorded
  in the Skeletal Mesh and Animation roadmap.

## Deferred Follow-ups

- FBX skeletal import through a separately activated source-compatibility plan.
- Editable skeleton/mesh/animation authoring, material assignment, sockets,
  retargeting, compression settings, and animation graph tooling.
- Project-wide compatible preview-mesh libraries beyond same-record peers.
- Animated AnimationClip thumbnails and Skeleton visualization thumbnails after
  measured Content Browser value justifies their cache and scheduling cost.
- Advanced animation, additional LODs, morphs, cloth, compute skinning, and
  directional shadows through their owning roadmaps and evidence gates.

## Related Documentation

- [Skeletal Mesh and Animation Roadmap](../Roadmaps/SkeletalMeshAndAnimation.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Content Browser](../Editor/Architecture/ContentBrowser.md)
- [Asset Thumbnails](../Editor/Architecture/AssetThumbnails.md)
- [Mounted Source Workflows](../Editor/Guides/MountedSourceWorkflows.md)
- [StaticMesh Inspector](../Editor/Guides/StaticMeshInspector.md)
- [Skeletal Animation Playback](../Runtime/Animation/SkeletalAnimationPlayback.md)
- [Skeletal Mesh Rendering](../Runtime/Rendering/SkeletalMeshRendering.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Editor/AssetImportCore/`
- `Engine/Source/Editor/StandardAssetImport/Public/SceneImport.h`
- `Engine/Source/Editor/StandardAssetImport/Private/SceneImport.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SceneImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserItemView.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorWorkspace.h`
- `Engine/Source/Editor/DurinEd/Public/Preview/PreviewScene.h`
- `Engine/Source/Editor/DurinEd/Public/Thumbnail/RenderedAssetThumbnailExtension.h`
- `Engine/Source/Editor/StaticMeshEditor/`
- `Engine/Source/Editor/SkeletalMeshEditor/` (planned)
- `Engine/Source/Runtime/Engine/Public/SkeletalMesh/Skeleton.h`
- `Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalMesh.h`
- `Engine/Source/Runtime/Engine/Public/Animation/AnimationClip.h`
- `Engine/Source/Runtime/Engine/Public/Components/SkeletalMeshComponent.h`
- `Engine/Tests/Native/AssetCoreTests/Private/AssetImportTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshEditorTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Editor/ContentBrowserItemViewTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorWorkspaceTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalAssetTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalMeshRenderResourcesVulkanTests.cpp`
