# Volumetric Cloud Authoring Workflow Plan

Summary: Deliver bounded volume preview, specialized cloud editing, viewport quality and debug controls, and observable editor validation workflows.

Last reviewed: 2026-08-23

Status: Archived
Completed: 2026-08-23

## Current Status

P5 completed on 2026-08-23. The runtime already
ships one reflected global-cloud component, imported `DVolumeTexture` inputs,
four Renderer-owned quality tiers, temporal and lighting diagnostics, and
qualified compute/fragment routes. The editor can import a volume atlas and
edit the component through generic reflection, but it cannot inspect a volume
as 3D data, present cloud-specific asset roles, select cloud quality, activate
cloud debug views, or observe the last rendered cloud route and cost.

The initial repository audit selected the existing Texture Editor for exact
`DVolumeTexture` documents, the Level Editor Details/viewport surfaces for
cloud authoring, `FSceneViewSettings` for per-viewport quality and debug policy,
and the existing value-owned `FSceneViewportStatisticsSnapshot` publication
path for editor observation. Stage 0 is complete: exact preview fixtures,
presentation groups, view policy, debug meanings, statistics schema, and
overhead ceilings are frozen below. The existing reflected World Outliner
**Add Actors** route already creates and selects `AVolumetricCloudActor`, so a
second cloud-only creation command would duplicate established transaction and
dirtying behavior. There is no production-asset evidence requiring another
source format or procedural generator, so neither is part of this plan.

The completed implementation adds exact R8/RGBA8 orthogonal volume inspection,
cloud-specific Details presentation and asset navigation, per-view quality and
production-backed debug modes, and a bounded copied statistics snapshot. The
focused Texture, Viewport, Renderer, scene-contract, shader-reflection, and both
Vulkan cloud targets pass. The 4K RTX 3090 qualification gate passed on its
unchanged second run after one transient High/Reference GPU-timing outlier; all
image, memory, work, parity, and recovery assertions passed on both runs.
`fast-all`, the full `all` build, documentation validation, and an eight-second
DurinEditor startup smoke pass. Lasting Runtime and Editor contracts are linked
below, and P6 is now the next eligible roadmap milestone.

## Goal

Make the existing production cloud layer authorable and diagnosable without
introducing a second runtime representation: users can import or select volume
inputs, inspect their slices, create and edit a cloud actor, choose a named
quality tier, view bounded cloud debug outputs, understand eligibility and
render-route failures, and preserve authored results through save/reload and
world reopen.

## Scope

- Extend the Texture Editor with exact `DVolumeTexture` document routing,
  metadata/source inspection, and bounded orthogonal slice preview.
- Add a specialized `DVolumetricCloudComponent` Details layout that groups
  activation, input roles, layer bounds, density mapping, motion, and optical
  properties while preserving reflected property editing and validation.
- Present stable active/ignored selection and existing eligibility reasons in
  actionable read-only rows, with direct navigation to assigned texture assets.
- Add one Level Editor creation/selection path for the existing
  `AVolumetricCloudActor`; do not create a new cloud asset class.
- Promote the four existing cloud quality identities to per-view public policy,
  keep `High` as the default, and expose them in the scene viewport.
- Add per-view cloud debug modes backed by existing production intermediates or
  explicitly bounded visualization passes.
- Publish a copied diagnostic snapshot for the last completed view containing
  tier, route/reason, extents, history state, work, timing availability, and
  active/retained byte counts, then present it without blocking the render
  thread.
- Validate import/select/edit/preview/debug/save/reload/world-reopen, resource
  failure/recovery, both command executors, and editor shutdown.

## Non-Goals

- New density algorithms, cloud lighting equations, shadow representations,
  temporal reconstruction policy, tier sample counts, or runtime budgets.
- Procedural noise/weather generation or additional VDB, DDS, KTX, multi-file,
  or arbitrary atlas adapters without named production-source evidence.
- A new cloud package asset, material domain, preset asset system, editor-only
  scene proxy, or editor-private copy of cloud runtime parameters.
- Multiple cloud layers, local volumes, fog, sky-atmosphere integration,
  weather simulation, animation timelines, or local-light volumetrics.
- Persisting quality tiers or debug modes into the cloud component. They are
  view policy, not physical content, and must not dirty a level.
- Final long-duration, multi-view, cook/package, compatibility, or combined
  reload qualification owned by P6.

## Design Decisions and Invariants

- `DVolumeTexture`, `DVolumetricCloudComponent`, its immutable scene proxy, and
  Renderer-owned resources remain the only runtime authorities. Editor modules
  call public contracts and never retain raw RHI objects from a rendered view.
- The Texture Editor owns exact `DVolumeTexture` document routing alongside its
  existing exact `DTexture2D` route. A preview extracts at most one selected
  mip slice into the existing 2D preview owner; closing/replacing a document
  releases that upload before module shutdown.
- Volume preview exposes `XY`, `XZ`, and `YZ`, a clamped slice index, available
  mip levels, and meaningful source channels. It displays stored voxel values;
  cloud coverage, erosion, extinction, lighting, and weather semantics are not
  baked into the generic texture preview.
- Frozen preview fixtures are a `7 x 5 x 3` R8 ramp and `5 x 3 x 2` RGBA8
  coordinate pattern, each with its deterministic full mip chain. Axis order is
  `XY: X right/Y down at Z`, `XZ: X right/Z down at Y`, and
  `YZ: Y right/Z down at X`. A preview is expanded to RGBA8 and retains at most
  `2048 * 2048 * 4 = 16 MiB` of staging/upload pixels.
- Cloud rendering is previewed in the production Level Editor scene viewport.
  A separate miniature cloud renderer or editor-only preview scene would
  duplicate the depth, lighting, history, shadow, and view-state contract and
  is therefore excluded.
- The specialized Details view is presentation only. It edits the existing
  reflected properties through the normal property transaction, validation,
  undo/redo, dirtying, and save paths; custom rows may navigate or report but
  cannot bypass setters or publish directly to Renderer.
- Quality and debug state live in a new volumetric-cloud group inside
  `FSceneViewSettings`. `Performance`, `High`, `Epic`, and `Reference` preserve
  P3/P4 identities and policies, with `High` as the default. Viewport choices
  are session/view state and do not serialize into cloud content.
- Debug modes are mutually exclusive per view and reset to production `Lit`.
  They may replace final scene presentation for that view but cannot alter
  production history commit, cloud resources, authored settings, or another
  viewport.
- The frozen debug set is `Lit`, premultiplied cloud `Radiance`, scalar
  `Transmittance`, binary `TemporalStatus` (green accepted, amber rejected,
  gray unavailable), and scalar receiver `ShadowVisibility`. Debug presentation
  may reuse the normal cloud composite but adds no retained target and does not
  change history acceptance or commit.
- Renderer reduces private counters into the existing immutable
  `FSceneViewStatistics` value and `FSceneViewport` publishes that result with
  its current revision/availability contract. The editor reads the copied
  snapshot and never waits on a GPU result or rendering command. No separate
  view-keyed cache, stateless fallback, stale clock, or Renderer dependency is
  introduced.
- Normal rendering with the diagnostic panel closed does not add a readback or
  synchronous flush. Stage 0 rejects live GPU timing collection for P5 because
  exact work/byte counters already publish every frame and production timing
  queries would add policy and retained state solely for UI. The panel labels
  GPU time as unavailable and routes users to qualification evidence.
- The statistics extension remains at most 160 bytes, volume preview staging
  remains at most 16 MiB, hidden diagnostics add zero passes/queries/readbacks,
  each debug view adds at most the already-required cloud composite draw, and
  editor controls publish within the next submitted frame without a flush.
- Details groups are `Activation`, `Density Inputs`, `Layer`, `Mapping and
  Motion`, and `Optical Response`. Base density is required shape, detail
  density is required erosion, and weather is optional coverage control. The
  read-only header reports active/ignored selection and the exact shared
  eligibility message; asset actions use existing Content Browser navigation.
- Missing assets, ineligible clouds, unavailable diagnostics, unsupported
  debug sources, and failed Renderer resources retain the established identity
  or last-known-good rendering behavior and produce actionable UI state.
- `TextureEditor` and `LevelEditor` may depend privately on public Engine,
  RenderCore, and narrowly exposed Renderer observation contracts. Runtime
  modules never depend on editor modules.

## Current Foundations and Gaps

| Area | Existing foundation | P5 gap |
| --- | --- | --- |
| Volume ingestion | Content Browser imports and reimports one row-major PNG atlas into package-backed R8/RGBA8 `DVolumeTexture` data with provenance and repair. | The exact asset has no Texture Editor route or slice/mip/channel preview. |
| Cloud authoring | `AVolumetricCloudActor` and `DVolumetricCloudComponent` serialize validated physical intent and already work in generic Details. | Property roles and active/ignored/eligibility state are not presented as one cloud workflow. |
| View policy | `FSceneViewSettings` already owns session-local render, shadow, contact, GTAO, and debug choices. | Cloud quality is hard-coded to `High` in scene rendering and cloud debug policy is not public per view. |
| Runtime diagnostics | P3/P4 produce exact route reasons, work, history, bytes, captures, and qualification timings internally. | Tests can consume seams, but the editor has no bounded value snapshot or stale/unavailable contract. |
| Preview infrastructure | Texture Editor owns document/save/source workflows and a bounded 2D GPU preview; Level Editor renders production scenes and supports specialized Details and viewport menus. | Neither recognizes volume documents or exposes cloud-focused controls. |
| Lifecycle | Workspace registrations, callback gates, preview releases, renderer coordinator generations, and editor smoke patterns are established. | Cloud editor routes need explicit close, reload, failure, recovery, and shutdown coverage. |

## Implementation Stages

### Stage 0: Freeze workflow, debug, and observation contracts

- [x] Record representative R8/RGBA8 volume fixtures, odd dimensions, all three
  slice axes, mip/channel expectations, maximum preview upload bytes, and
  source/platform-data fallback behavior.
- [x] Freeze the cloud Details groups, labels, units, asset-role help, active
  selection/eligibility messages, navigation actions, and creation path against
  one valid, one incomplete, and two-conflicting-cloud fixture.
- [x] Select the exact public cloud quality enum/settings ownership and prove
  `High` default plus one-view isolation without serialized component changes.
- [x] Select the minimum debug set from production evidence: final Lit,
  premultiplied cloud radiance, transmittance, temporal history
  acceptance/rejection, and receiver cloud visibility; record unavailable and
  fallback presentation for each route.
- [x] Freeze the diagnostic snapshot schema, view identity, publication and
  stale policy, bounded entry count, timing-query cadence, disabled overhead,
  and thread/lifecycle ownership.
- [x] Record editor interaction, CPU, GPU-query, transient preview, and retained
  diagnostic-memory ceilings before accepting implementation measurements.

#### Acceptance Gate

- Every UI surface maps to an existing runtime authority; preview formats,
  debug algebra, diagnostic threading/lifetime, fixtures, and numeric overhead
  ceilings are explicit enough to implement without an open ownership choice.

### Stage 1: Expose per-view cloud policy and diagnostics

- [x] Add the public per-view quality/debug settings, canonicalization, default,
  and immutable scene-view propagation; replace both hard-coded `High` call
  sites with the submitted tier.
- [x] Implement the selected debug outputs in compute/fragment, temporal, and
  shadow routes without changing normal composition or history transactions.
- [x] Publish the bounded value-owned last-completed-view snapshot with tier,
  route/reason, input/output extents, sample/work counters, history state,
  shadow work, active/retained bytes, and delayed timing availability.
- [x] Preserve stateless, fitted, offscreen, Present, failed, invalidated, and
  released-view semantics; prune stale identities and release pending queries
  without device-idle waits.
- [x] Add RenderCore/Renderer contract, route parity, isolation, stale-data,
  failure, recovery, and release tests.

#### Acceptance Gate

- Each view independently applies a valid shipped tier and debug mode, `High`
  remains the unchanged default, normal pixels/history are unchanged when
  debugging is off, and observation is bounded, nonblocking, and lifecycle-safe.

### Stage 2: Add exact volume-texture inspection

- [x] Register exact `DVolumeTexture` documents in Texture Editor without
  aliasing `DTexture2D`, and retain one counted asset per open document.
- [x] Present dimensions, format, mip count, voxel/source byte counts, source
  provenance, build readiness, and import/reimport/repair state using existing
  asset and source authorities.
- [x] Implement `XY`/`XZ`/`YZ` slice extraction for R8 and RGBA8 platform data,
  mip and channel selection, clamped indices, zoom/checkerboard behavior, and a
  bounded 2D preview upload.
- [x] Refresh on asset/source/build revision, clear stale images on invalid or
  failed candidates, and release uploads on close, replacement, reload, and
  module shutdown.
- [x] Add slice-orientation/value, odd-extent, mip/channel, document identity,
  reload, failure, and resource-release tests.

#### Acceptance Gate

- Opening any qualified imported volume shows deterministic stored voxels for
  every axis/mip/channel without interpreting cloud semantics; invalid and
  replaced assets never display stale data or leak preview resources.

### Stage 3: Build the cloud authoring workflow

- [x] Register a `DVolumetricCloudComponent` Details customization with the
  frozen groups, units, help, asset-role presentation, and normal reflected
  property transactions.
- [x] Present active/ignored conflicts and the shared Engine eligibility reason
  with direct navigation to assigned base, detail, and weather assets.
- [x] Add the bounded Level Editor command that creates/selects the existing
  volumetric-cloud actor and preserves standard actor naming, undo, dirtying,
  duplication, and deletion behavior.
- [x] Add the viewport quality selector and mutually exclusive debug menu with
  explicit `High (Default)` and `Reset Cloud Debug View` actions.
- [x] Add a nonblocking cloud diagnostics panel/section for the active viewport,
  including stale/unavailable timing, route/fallback reason, history state,
  work, extents, and GPU-byte presentation.
- [x] Add editor tests for customization registration, transactions, conflict
  order, asset navigation, creation, per-viewport isolation, diagnostics, and
  module unload/reload.

#### Acceptance Gate

- A user can create or select the production actor, understand and edit every
  existing cloud input, choose view-only quality/debug state, and diagnose the
  last completed render without dirtying content through observation controls.

### Stage 4: Qualify persistent editor workflows

- [x] Run the frozen end-to-end sequence: import volume, inspect slices, create
  cloud, assign roles, edit valid/invalid values, observe eligibility, render
  every quality/debug mode, save, reload, close/reopen the world, and reimport
  or replace one source.
- [x] Verify authored values and asset references persist while quality/debug
  state remains view-owned, and cooked/runtime content has no editor-only data
  or dependency.
- [x] Measure the Stage 0 editor CPU, diagnostic timing, preview upload, and
  retained-memory ceilings with diagnostics hidden and visible.
- [x] Run focused editor/Engine/Renderer contracts and Vulkan scene coverage on
  inline and threaded executors, then affected aggregate tests, full build,
  documentation validation, and bounded DurinEditor smoke.
- [x] Record any evidence-backed change to debug set, cadence, workflow, or
  budget before closing a failed gate.

#### Acceptance Gate

- The complete authoring loop survives save/reload/reopen and resource
  replacement, every view route remains recoverable, overhead is within frozen
  ceilings, and all required repository validation passes.

### Stage 5: Publish the lasting contract and complete P5

- [x] Publish volume inspection and cloud authoring user guidance plus the
  lasting editor architecture, view policy, diagnostic snapshot, debug,
  fallback, and lifecycle contracts in their owning documentation domains.
- [x] Update the cloud roadmap to mark P5 complete and make P6 eligible only
  after all P5 acceptance gates pass.
- [x] Set this plan to `Completed` only after every prior checklist and
  validation gate is supported by recorded evidence.

#### Acceptance Gate

- Runtime/editor/user documentation is authoritative, P5 evidence is
  reproducible, no required checklist is open, and the roadmap exposes P6 as
  the next eligible child plan.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Volume payload -> preview | Exact R8/RGBA8 values, axes, orientation, slice clamp, mip/channel selection, odd extents, refresh, invalid candidate, and release. |
| Component -> specialized Details | Existing reflected properties only; validation, units, undo/redo, dirtying, duplication, active conflict, eligibility, and asset navigation. |
| View settings -> Renderer | Four tiers, `High` default, invalid canonicalization, main/auxiliary isolation, fitted/offscreen/Present propagation, and no component serialization. |
| Renderer intermediates -> debug view | Compute/fragment parity, radiance/transmittance/history/receiver meaning, production reset, unavailable source, fallback, and unchanged normal history. |
| Render thread -> diagnostics UI | Copied immutable snapshot, view identity, delayed timing, stale/prune policy, bounded entries, failure/recovery, invalidation, and no editor-side flush. |
| Editor -> world/package | Create/select/edit/save/reload/reopen, asset replacement/reimport/deletion, cooked exclusion of editor-only state, and orderly shutdown. |
| Workflow -> cost | Panel hidden/visible CPU overhead, timing-query cadence, preview upload/retained bytes, diagnostic retained bytes, and no unbounded per-frame growth. |

## Definition of Done

- Exact volume assets open in Texture Editor and display deterministic bounded
  slices for supported formats, axes, mips, and channels with safe lifecycle.
- The Level Editor supplies a cohesive cloud Details/creation workflow without
  adding or duplicating runtime content types.
- All four shipped tiers and selected debug modes are controllable per view;
  `High` is the default and observation controls do not dirty authored content.
- Last-render diagnostics are meaningful, copied, bounded, nonblocking, and
  honest about unavailable or stale timing and failed/fallback routes.
- Import/edit/preview/save/reload/world-reopen/reimport and shutdown pass the
  frozen focused, aggregate, Vulkan, build, documentation, and editor-smoke
  gates, and lasting contracts are published.

## Deferred Follow-ups

- P6 owns final cross-feature production qualification, long-duration and
  multi-view soak, combined reload/invalidation sequences, cook/package closure,
  compatibility evidence, and roadmap completion.
- Procedural volume generation and richer source formats require a future named
  production asset and measured workflow gap; this plan records no such need.
- Preview histograms, transfer functions, 3D ray-cast volume inspection, cloud
  animation timelines, preset assets, and capture/export tools remain optional
  future editor work.
- Multiple layers, local volumes, fog, atmosphere, weather simulation, local
  lights, async compute, and Render Graph remain outside this milestone.

## Related Documentation

- [Volumetric Cloud Rendering roadmap](../../../Roadmaps/VolumetricCloudRendering.md)
- [Volumetric cloud scene contract](../../../Runtime/Rendering/VolumetricCloudSceneContract.md)
- [Volumetric cloud temporal reconstruction](../../../Runtime/Rendering/VolumetricCloudTemporalReconstruction.md)
- [Volumetric cloud lighting and shadows](../../../Runtime/Rendering/VolumetricCloudLightingAndShadows.md)
- [Volumetric cloud authoring architecture](../../../Editor/Architecture/VolumetricCloudAuthoring.md)
- [Volumetric cloud authoring guide](../../../Editor/Guides/VolumetricCloudAuthoring.md)
- [Volume texture import and cloud diagnostics](VolumeTextureImportAndCloudDiagnostics.md)
- [Asset Import Framework](../../../Editor/Architecture/AssetImportFramework.md)
- [Implementation plan rules](../../AGENTS.md)
- [Build and run workflow](../../../Agents/BuildAndRun.md)
- [Testing workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/Engine/Public/Client/ViewportClient.h`
- `Engine/Source/Runtime/Engine/Public/Components/VolumetricCloudComponent.h`
- `Engine/Source/Runtime/Engine/Public/Engine/VolumetricCloudSceneProxy.h`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudSpatialRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudShadowRenderer.h`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MTextureEditor.h`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/TexturePreview.h`
- `Engine/Source/Editor/TextureEditor/Private/TextureEditorModule.cpp`
- `Engine/Source/Editor/LevelEditor/Private/LevelEditorModule.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPresentation.cpp`
- `Engine/Source/Editor/LevelEditor/Public/LevelEditorCustomizations.h`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudSceneVulkanTests.cpp`
