# Rendered Asset Thumbnails Plan

Summary: Persistent Content Browser previews for materials, material instances, and cube textures.

Last reviewed: 2026-07-26

## Current Status

Stage 0 is complete. The provider-neutral public request, immutable
generation input, state, view, cancellation, provider registration, output,
visual-contract, budget, cache-key, and dependency-snapshot contracts now live
in `DurinEd`. Focused native tests cover byte-stable key construction, field
invalidation, dependency-order independence, cycle termination, missing and
duplicate registry data, shared cancellation, and bounded defaults.

Versioned test factories now create deterministic Material, MaterialInstance,
parent/texture dependency, invalid-instance, and six-face directional
TextureCube assets at fixed virtual identities. The thumbnail and
[Static Mesh Derived Data and Cooking](StaticMeshDerivedDataAndCooking.md)
plans share one native sphere identity and acquisition contract. Implementing
that native shared-mesh service remains owned by Stage 5 of the static-mesh
plan and is a prerequisite for binding rendered-thumbnail preview scenes; no
thumbnail path may fall back to transient source-model import.

Stage 1 is in progress. `DurinEd` now owns an exact-class provider registry
with monotonic generations and a bounded provider-neutral scheduler that
captures requests, coalesces cache keys, promotes visible work, rejects stale
serials, and cancels replaced or shutdown work. The existing source-image
cache now exposes the public thumbnail state/view contract to Content Browser
without changing its decode, upload, transparency, or cache behavior.
`DurinEd` also owns the reusable versioned object/index store, disk cleanup,
and deterministic CPU/GPU LRU budget selection; the source-image adapter now
uses those shared facilities. Changing Content Browser requests from source
paths to asset identities remains pending.

Texture2D assets and supported source-image files already use an
asynchronous, persistent Content Browser thumbnail cache. Materials have an
isolated live preview scene in the Material Editor, and TextureCube assets have
runtime render resources and a defined sampling-orientation contract. The
Content Browser does not yet request rendered thumbnails for Material,
MaterialInstance, or TextureCube assets, and the existing source-image cache is
not a provider-neutral rendered-thumbnail service.

## Goal

Show deterministic, persistent, dependency-aware previews for Material,
MaterialInstance, and TextureCube assets in the Content Browser without
introducing per-card live viewports or steady-state rendering work.

## Scope

- Generalize Content Browser thumbnail requests around an asset-thumbnail
  identity, provider, state, and cache result while preserving current
  Texture2D/source-image behavior.
- Render Material and MaterialInstance thumbnails on a fixed sphere with a
  fixed camera, neutral background, and stable editor-owned lighting.
- Render TextureCube thumbnails as a fixed-camera reflective sphere using the
  cube asset directly through the documented cube-sampling orientation.
- Persist rendered output beneath the project-local `DerivedDataCache`
  thumbnail domain and reuse it across editor restarts.
- Invalidate a rendered thumbnail when its asset, a transitive package
  dependency, generator schema, preview fixture, shader contract, or output
  settings change.
- Prioritize visible Content Browser items, coalesce duplicate work, bound
  per-frame rendering and memory use, and expose queued, rendering, ready,
  invalid, and failed states.
- Add automated cache, invalidation, ordering, lifetime, and rendered-output
  coverage plus visible Content Browser validation.

## Non-Goals

- StaticMesh, Level, animation, particle, audio, or arbitrary reflected-object
  thumbnails.
- Interactive rotation, zoom, mesh selection, or environment selection inside
  Content Browser cards.
- Replacing the interactive Material Editor preview viewport.
- Baking thumbnails into `.dasset` packages or treating generated output as
  authored content.
- Runtime or cooked-game access to editor thumbnails.
- Continuous rendering of every visible card or automatic background
  regeneration of assets that are not requested.
- A general remote or cross-project derived-data service.
- Introducing image-based lighting into the runtime material model merely to
  display TextureCube thumbnails.

## Design Decisions and Invariants

### Ownership and provider boundary

- `DurinEd` owns the provider-neutral thumbnail request/result contract,
  provider registry, generation scheduler, persistent rendered-output cache,
  and process-local GPU budget.
- Providers are registered by exact supported asset class and return a
  generator identity plus an immutable generation request. The Content Browser
  does not branch into Material Editor or TextureCube rendering internals.
- Material and MaterialInstance share one material-thumbnail provider.
  TextureCube uses a separate provider and preview pass. Unsupported classes
  retain their existing icons.
- Provider registration and removal occur on the game thread. Requests capture
  provider-owned immutable data before crossing to a worker or render thread;
  module shutdown cancels its outstanding work before unregistering providers.
- The existing source-image thumbnail path is adapted to the same public
  request/view state machine and shared budgets. CPU image decode remains a
  source-image provider behavior rather than being forced through a rendered
  preview scene.

### Visual contract

- Rendered thumbnails use one fixed square output size and a versioned output
  color-space/encoding policy. Content Browser card size changes scale the
  cached image and do not create new render keys.
- Material and MaterialInstance thumbnails use the shared
  `/Engine/Editor/MaterialPreview/Sphere` mesh selected by the
  [Static Mesh Derived Data and Cooking](StaticMeshDerivedDataAndCooking.md)
  plan. This plan does not create or import a second sphere fixture.
- The shared editor preview-mesh acquisition boundary accepts the canonical
  virtual identity and returns a retained native asset/render-data handle. The
  service owns load coalescing and lifetime; Material Editor previews and
  rendered-thumbnail scenes retain handles and release them at scene teardown.
  Callers never receive a source-model path and never fall back to
  `CreateTransientFromFile`.
- The material camera, sphere transform, directional key light, fill/ambient
  contribution, exposure, neutral background, and post-process settings are
  fixed generator inputs. A deliberate visual-contract change increments the
  generator schema.
- MaterialInstance rendering uses fully resolved parent and parameter values.
  The thumbnail does not add an instance badge inside the pixels; the existing
  asset type label/icon remains responsible for distinguishing the class.
- TextureCube thumbnails use a dedicated editor preview shader that samples the
  cube with a reflection vector over the same sphere fixture. It must follow
  `CubeTextures.md` face order and orientation and must not alter runtime
  material or sky rendering contracts.
- Alpha is preserved only when the preview contract produces meaningful
  transparency. Otherwise rendered previews are stored opaque against their
  neutral background, avoiding card-theme-dependent output.

### Cache identity and invalidation

- One rendered-thumbnail key contains the normalized virtual asset path, asset
  package fingerprint, exact asset class, provider/generator schema, output
  dimensions, output color/encoding version, preview-fixture identity, and
  shader/visual-contract version.
- Material keys additionally contain a deterministic, sorted, cycle-guarded
  closure of package dependencies reachable through the Asset Registry. Each
  entry contributes virtual path, class, package format, size, and normalized
  last-write fingerprint. This covers parent materials and referenced textures
  whether or not they are loaded.
- TextureCube keys include the cube package fingerprint and provider schema.
  Source-face paths are not reopened merely to look up a thumbnail because the
  saved asset package and its build state are the authoritative preview input.
- Missing registry entries, corrupt dependency cycles, unavailable packages,
  unsupported package versions, invalid material parent chains, and failed
  texture builds never produce a trusted cache hit.
- Save, move, delete, import, dependency edits, generator changes, and cache
  corruption converge through key mismatch or explicit request cancellation.
  Rename may regenerate because virtual asset path is intentionally part of the
  initial key.
- Cache misses and failures do not modify authored assets. Deleting
  `DerivedDataCache` always recovers by loading authored content and rendering
  requested thumbnails again.

### Scheduling, rendering, and persistence

- Only requested items enter the scheduler. Visible cards outrank prefetch
  cards, and duplicate requests for one key coalesce.
- Render generation is a bounded state machine:
  `Queued -> Loading -> WaitingForResources -> Rendering -> Readback ->
  Encoding -> Ready`, with terminal `Invalid` and `Failed` results.
- Asset loading and preview-scene mutation occur on the game thread. RHI
  resource use, offscreen rendering, and readback occur on the rendering
  thread. PNG encoding and atomic disk writes may occur on workers.
- The scheduler renders at most one configurable thumbnail per editor frame by
  default and enforces separate limits for queued jobs, live preview scenes,
  CPU pixels, GPU textures, and persistent objects. Budget pressure evicts
  least-recently-used completed entries, never authored content.
- A job captures an asset identity, cache key, request serial, provider
  generation, and asset/resource revision. Every completion revalidates them
  before publication so stale work cannot replace a newer result.
- A shared offscreen preview scene may be reused serially across jobs after
  state is reset. There is no viewport, scene, world, or rooted mesh per Content
  Browser card.
- Successful output is read back once, encoded, and atomically published into
  the existing `DerivedDataCache/Thumbnails` object/index domain. Warm requests
  decode the persisted result and upload it through the normal UI texture path
  without loading or rendering the asset.
- Closing the Content Browser cancels unstarted work and safely drains or
  rejects in-flight completions. Editor and module shutdown release preview
  scenes and GPU resources on their owning threads.

### Failure and UI behavior

- Queued, loading, waiting, rendering, readback, and uploading states show the
  existing icon plus a nonblocking progress marker.
- Invalid material data, compilation/build failures, missing dependencies, and
  render/readback failures retain the asset icon and expose an asset-qualified
  tooltip diagnostic. They do not retry every frame.
- Retry occurs only after the request key changes, an explicit Content Browser
  refresh, or an explicit retry action. A failed output is never persisted as a
  successful thumbnail.
- Grid view displays thumbnails; Details view remains text-first in the initial
  implementation and may reuse the cache later without changing generation
  semantics.

## Current Foundations and Gaps

### Foundations

- `FSourceImageThumbnailCache` already provides visible-item prioritization,
  asynchronous decode/upload, serial validation, GPU eviction, persistent PNG
  objects, index recovery, and bounded disk maintenance for Texture2D and
  source-image thumbnails.
- `FContentBrowserItem` already stores class identity and source-thumbnail
  fingerprints, and grid rendering already scales ready UI textures while
  displaying progress and failure states.
- `FMaterialPreview` already owns an isolated scene, sphere/box meshes, stable
  camera controls, a directional light, revisioned material updates, and an
  auxiliary offscreen viewport.
- Material packages already record parent and texture dependencies, and the
  Asset Registry exposes package metadata needed to build a deterministic
  unloaded dependency closure.
- `DTextureCube` exposes validated platform data, build state, build revision,
  and a render resource. The RHI supports cube sampling and validation
  readback, and the runtime cube orientation is documented.
- The renderer already supports offscreen scene output and the UI backend can
  display an `FRHITexture`.

### Gaps

- Thumbnail cache identity and requests are physical-source-image-specific
  rather than provider-neutral.
- Content Browser snapshot construction recognizes only Texture2D source
  thumbnails and does not dispatch rendered asset classes.
- The interactive material preview imports and roots preview meshes per
  document; it is not a reusable fixed thumbnail renderer.
- No editor thumbnail provider registry, rendered-job scheduler, shared
  offscreen preview scene, capture/readback pipeline, or rendered-output key
  exists.
- No TextureCube reflective-sphere preview shader exists.
- Material thumbnail invalidation has no deterministic transitive Asset
  Registry dependency-key builder.
- There is no rendered-thumbnail golden fixture or instrumentation for cache
  hits, queue latency, render count, readback count, failure count, and
  eviction.

## Implementation Stages

### Stage 0: Freeze contracts, dependencies, and fixtures

- [x] Define provider registration, request, immutable generation input, state,
  result, cancellation, and shutdown contracts in `DurinEd`.
- [x] Fix output dimensions, color space, encoding, background, camera,
  lighting, sphere transform, and TextureCube reflection-vector convention.
- [x] Define versioned rendered-thumbnail keys and the deterministic,
  cycle-guarded Asset Registry dependency-closure algorithm.
- [x] Define queue, per-frame render, live-scene, CPU, GPU, and disk budgets with
  test overrides.
- [x] Create deterministic Material, MaterialInstance, parent/texture
  dependency, invalid-material, and six-face directional TextureCube fixtures.
- [x] Coordinate the sphere identity and acquisition API with Stage 5 of the
  Static Mesh Derived Data and Cooking plan; do not add a duplicate transient
  import path.

#### Acceptance Gate

- Contract tests prove identical inputs produce identical keys, dependency
  order does not affect keys, cycles terminate deterministically, and every
  visual or encoding input is either represented in the key or explicitly
  fixed by a versioned provider schema.
- The shared sphere fixture has one stable virtual identity and no thumbnail
  request requires per-card source-model import.

### Stage 1: Generalize the thumbnail service without regression

- [x] Introduce the `DurinEd` thumbnail provider registry, scheduler, public
  request/view types, and provider-neutral cache identity.
- [x] Extract or adapt the persistent index/object store and CPU/GPU budget
  logic from `FSourceImageThumbnailCache` for reuse by source and rendered
  providers.
- [ ] Adapt Texture2D and supported source-image requests to the new service
  while preserving source fingerprints, transparency, warm disk hits,
  cancellation, visible-item priority, and error reporting.
- [ ] Change Content Browser grid code to request by item identity and render a
  provider-neutral result instead of branching on source-image paths.
- [ ] Add registration, duplicate-provider, missing-provider, cancellation,
  stale-serial, corruption, and budget tests.

#### Acceptance Gate

- Existing Texture2D/source-file thumbnail tests pass with equivalent pixels,
  invalidation, restart reuse, and bounded GPU/disk behavior.
- Unsupported assets issue no jobs and keep their icons; provider or Content
  Browser shutdown publishes no stale result and leaks no UI texture.

### Stage 2: Add the shared offscreen rendered-thumbnail pipeline

- [ ] Implement the bounded generation state machine and its game-thread,
  render-thread, and worker transitions.
- [ ] Create one resettable offscreen preview-scene pool using the shared sphere
  asset, fixed output target, deterministic view, and editor-only assistance
  disabled.
- [ ] Render requested content, wait for required resource revisions, read back
  the final output once, encode it, and atomically publish its cache object and
  index entry.
- [ ] Revalidate job key, provider generation, request serial, asset identity,
  and resource revision at every asynchronous completion boundary.
- [ ] Add deterministic counters and test hooks for jobs, loads, waits, renders,
  readbacks, disk hits, failures, retries, cancellations, and evictions.

#### Acceptance Gate

- A cold request renders and persists exactly one output; a reconstructed
  service serves the warm request without loading the asset, creating a preview
  scene, rendering, or reading back.
- Multiple cards and repeated frames requesting the same key coalesce to one
  job, the default scheduler starts no more than one render per editor frame,
  and cancellation or revision changes cannot publish stale pixels.

### Stage 3: Add Material and MaterialInstance thumbnails

- [ ] Extract the fixed material preview setup needed by both the interactive
  Material Editor and thumbnail provider without coupling LevelEditor to the
  MaterialEditor module.
- [ ] Register Material and MaterialInstance providers that load the requested
  asset, resolve inherited values, wait for the matching render-resource
  revision, and bind it to the shared sphere.
- [ ] Build keys from the material package plus its sorted transitive package
  dependency closure, including parent materials and referenced textures.
- [ ] Preserve invalid, compiling/waiting, missing-dependency, failed-resource,
  cancellation, garbage-collection, and module-shutdown diagnostics.
- [ ] Add focused key/invalidation tests and rendered-image checks for base
  color, scalar/specular response, texture sampling, instance inheritance, and
  local override differences.

#### Acceptance Gate

- Material and MaterialInstance cards render visually distinguishable,
  deterministic sphere thumbnails and survive editor restart as disk hits.
- Editing or replacing the material, any parent, or any referenced texture
  changes the key and regenerates the dependent thumbnail; unrelated asset
  changes do not.
- Invalid or not-ready materials retain usable Content Browser interaction,
  report one stable diagnostic, and do not create a retry loop.

### Stage 4: Add TextureCube thumbnails

- [ ] Implement and register the TextureCube provider using a dedicated
  editor-only reflective-sphere preview shader and the shared sphere fixture.
- [ ] Bind only a ready render-resource revision; handle unbuilt, rebuilding,
  unsupported, failed, deleted, and fallback-resource states explicitly.
- [ ] Match the documented face order, row orientation, cube sampling, camera,
  reflection vector, sampler, and output color-space contracts.
- [ ] Add directional-face rendered-image tests that detect face swaps, axis
  inversion, horizontal/vertical flips, and accidental use of the black
  fallback cube.
- [ ] Add warm-hit, rebuild-revision, corruption, cancellation, and resource
  lifetime tests.

#### Acceptance Gate

- A directional six-face fixture produces the expected face regions and
  orientation on the sphere, and changing any authored cube content or build
  revision regenerates the thumbnail.
- Warm TextureCube requests perform no asset load, GPU cube build, preview
  render, or readback; failed resources show diagnostics without persisting
  fallback pixels as successful output.

### Stage 5: Content Browser, performance, and architecture handoff

- [ ] Validate grid filtering, searching, scrolling, zooming, selection,
  rename, move, delete, refresh, drag/drop, tooltips, and navigation with mixed
  Texture2D, Material, MaterialInstance, TextureCube, and unsupported assets.
- [ ] Stress a large mixed directory and record queue depth, visible latency,
  renders per frame, warm-hit rate, frame time, CPU/GPU memory, disk size, and
  eviction behavior.
- [ ] Run focused native tests, the applicable editor test suites, a full editor
  build, and the hidden-window editor smoke test through the repository
  BuildTool workflow.
- [ ] Manually verify representative material and cube thumbnails at minimum,
  default, and maximum Content Browser icon sizes and after editor restart.
- [ ] Delete and corrupt the rendered-thumbnail cache and verify safe,
  nonfatal, on-demand recovery.
- [ ] Move lasting provider, invalidation, scheduling, ownership, and recovery
  contracts into the owning Editor and Runtime documentation, update related
  active plans where their tracked gaps have landed, and archive this plan.

#### Acceptance Gate

- No idle frame renders or reloads an unchanged thumbnail; fast scrolling
  prioritizes visible assets and does not exceed configured generation or
  memory budgets.
- Mixed Content Browser workflows remain functional, rendered previews are
  visually stable across runs, deleting `DerivedDataCache` loses no authored
  content, and all required automated, full-build, and smoke validation passes.

## Validation Matrix

| Area | Scenario | Required evidence |
| --- | --- | --- |
| Contract | Provider registration, duplicate class, missing provider, module shutdown | Focused unit tests with deterministic state transitions |
| Key determinism | Same inputs in different dependency orders and cyclic graphs | Byte-identical keys and bounded traversal |
| Source regression | Texture2D and source-image cold/warm/invalidation paths | Existing and adapted thumbnail tests |
| Scheduling | Visible versus prefetch, duplicate requests, scrolling, cancellation | Deterministic queue tests and counters |
| Persistence | Cold render, restart, corrupt index/object, deleted DDC | One cold render, zero warm renders, safe regeneration |
| Material | Base, instance, inheritance, override, parent and texture edits | Key tests plus rendered-pixel/image comparison |
| Material failure | Invalid chain, missing dependency, not-ready or failed resource | Stable diagnostic, no persisted fallback, no retry loop |
| TextureCube | Directional faces, build revision, unsupported/failed resource | Orientation-sensitive rendered-image tests |
| Lifetime | Refresh, move, delete, GC, provider unload, editor shutdown | No stale publication, leaked root, scene, or GPU texture |
| UI | Mixed grid, filters, search, zoom, selection, drag/drop, tooltip | Visible Content Browser interaction matrix |
| Performance | Large mixed directory, cold queue, warm restart, fast scroll | Bounded jobs/frame, memory/disk budgets, latency counters |
| Regression | Applicable native/editor suites and runtime rendering | Full BuildTool build and hidden-window smoke |

## Definition of Done

- Material, MaterialInstance, and TextureCube assets show deterministic rendered
  thumbnails in Content Browser grid view.
- Material thumbnails use resolved values on the shared sphere and regenerate
  for every relevant transitive package dependency change.
- TextureCube thumbnails use the documented cube orientation and never cache a
  fallback resource as valid output.
- Source-image and rendered providers share a provider-neutral request/result
  contract, persistent cache domain, scheduler, and bounded resource policy
  without regressing existing Texture2D thumbnails.
- Cold generation is visible-priority, coalesced, bounded, cancellable, and
  thread-correct; warm reuse performs no asset load or preview render.
- Invalid, compiling/building, failed, moved, deleted, corrupt-cache, and
  shutdown cases remain nonfatal and cannot publish stale pixels.
- Required unit, integration, rendered-output, full-build, and hidden-window
  smoke validation passes.
- Long-lived contracts are documented outside the active plan and the completed
  plan is archived according to `Documentation/Plans/AGENTS.md`.

## Deferred Follow-ups

- StaticMesh, Level, animation, particle, audio, or user-extensible project
  thumbnail providers.
- User-selectable Material preview meshes, environments, turntables, or
  per-asset thumbnail composition settings.
- Live animated material thumbnails.
- Cross-project or remote thumbnail object sharing.
- Content-hash-based rename reuse and cross-path output deduplication.
- Details-view thumbnail columns or a larger interactive TextureCube inspector.
- Background idle-time pre-generation for assets that have not been requested.

## Related Documentation

- [Active Implementation Plans](README.md)
- [Material System](MaterialSystem.md)
- [Texture Support](TextureSupport.md)
- [Static Mesh Derived Data and Cooking](StaticMeshDerivedDataAndCooking.md)
- [Resource Dependency Updates](ResourceDependencyUpdates.md)
- [Material System Runtime Contract](../Runtime/Rendering/MaterialSystem.md)
- [Cube Textures](../Runtime/Rendering/CubeTextures.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Build and Run](../Development/Build/BuildAndRun.md)
- [Native Tests](../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Editor/DurinEd/`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SourceImageThumbnailCache.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SourceImageThumbnailCache.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MaterialPreview.h`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MaterialPreview.cpp`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialInterface.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCubeRenderResource.h`
- `Engine/Source/Programs/Tests/EngineTests/Private/SourceImageThumbnailTests.cpp`
