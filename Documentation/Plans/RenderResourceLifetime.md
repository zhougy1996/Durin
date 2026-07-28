# Texture Render Resource Ownership Refactor Plan

Summary: Refactor Texture2D and TextureCube onto a UE-style asset-owned texture reference and explicitly managed RenderCore resource lifecycle.

Last reviewed: 2026-07-28

Status: Active
Completed:

## Current Status

No implementation stage has started.

The selected architecture changed on 2026-07-28. The prior plan proposed a
cross-thread shared handle with no RHI state plus a separately managed
render-thread payload. That design would preserve the current assumption that
assets, material snapshots, scenes, previews, and queued commands may all
extend the lifetime of a texture render-resource identity.

The refactor now adopts the ownership model proven by Unreal Engine instead:

- `DTexture2D` and `DTextureCube` are the sole high-level owners of their stable
  texture reference and current concrete render resource;
- consumers do not share ownership of `FTextureResource`;
- a stable reference-counted RHI texture reference provides indirection across
  concrete resource replacement;
- `FRenderResource` initialization, release, deferred cleanup, and fences make
  concrete resource lifetime explicit.

This change is intentionally larger than the previously proposed migration. It
removes the current `shared_ptr<FTexture2DRenderResource>` and
`shared_ptr<FTextureCubeRenderResource>` ownership model rather than wrapping it
in another shared object.

Baseline commit `7ea4c0b4` prevents the observed TextureCube destruction failure
by giving `FTextureCubeRenderResource` a class-specific custom deleter that
returns final deletion to the rendering thread. The focused regression and a
full `all` build passed at that baseline. This remains a temporary safety fix
until the asset-owned lifecycle replaces it.

Current code still has the following shape:

- both texture asset types own their resource through `shared_ptr`;
- material render data, static-mesh scene proxies, skybox snapshots, editors,
  thumbnails, tests, and queued commands copy that `shared_ptr`;
- each resource directly owns `FTextureRHIRef` and implements its own revisioned
  build and release protocol;
- TextureCube alone has a custom final-deletion policy;
- most `FRenderResource` lifecycle functions remain placeholders;
- RenderCore has no complete asynchronous init/release, deferred resource
  cleanup, texture-reference update, or shutdown admission contract.

## Goal

Give every texture a single, explicit high-level owner and make concrete render
resource replacement independent of consumer binding:

- each `DTexture2D` and `DTextureCube` owns one stable texture reference for its
  lifetime;
- each texture asset owns at most one current concrete `FTextureResource`;
- no material, scene, editor, thumbnail, command, or test fixture shares
  ownership of a concrete texture render resource;
- render consumers retain a reference-counted RHI texture reference or use a
  render-thread-only non-owning view with a proven enclosing lifetime;
- replacing a concrete resource updates the stable reference on the rendering
  thread without rebinding every consumer;
- initialization, replacement, release, deletion, RHI deferred deletion, and
  shutdown occur in an asserted deterministic order;
- Texture2D and TextureCube use the same RenderCore lifecycle and no
  class-specific `shared_ptr` deleter.

## Scope

- Define the ownership and thread contract for texture assets, stable texture
  references, concrete texture resources, consumer bindings, queued commands,
  and RHI references.
- Complete the required `FRenderResource` registry, initialized-state,
  initialization, update, release, and global RHI reset behavior.
- Add the RenderCore producer-thread entry points corresponding to asynchronous
  begin-init, begin-update, and begin-release operations.
- Introduce a stable `FTextureReference`/`FRHITextureReference` indirection whose
  target is updated only through the rendering thread and whose RHI reference
  may be safely copied by render consumers.
- Introduce a common `FTexture` render-resource base where useful, with
  Texture2D and TextureCube concrete resources owning their actual
  `FTextureRHIRef`.
- Replace shared ownership of concrete resources with asset ownership,
  render-thread pointer mirroring, and explicit deferred cleanup.
- Migrate material render data, static-mesh scene proxies, skybox snapshots,
  texture editors, thumbnail previews, renderer resolution, and tests.
- Preserve revision-correct asynchronous build, replacement, failure, fallback,
  and diagnostic behavior.
- Define deterministic asset unload, garbage collection, scene removal, preview
  closure, module unload, command admission close, and engine shutdown.
- Audit the existing RHI deferred-delete path and close any gap required for
  reference-counted texture references to outlive their originating asset.

## Non-Goals

- Texture streaming, sparse residency, virtual textures, or residency budgets.
- Moving source decoding or platform-data builds to background workers.
- Changing texture package, DDC, or cooked payload formats.
- Replacing intrusive RHI reference counting or the RHI deferred-delete queue.
- Rewriting renderer scene architecture or material parameter evaluation.
- Making `DTexture2D` or `DTextureCube` accessible from the rendering thread.
- Letting consumers own `FTextureResource` through another smart-pointer type.
- Requiring a full render-command flush for ordinary texture rebuild, asset
  unload, scene removal, or editor preview closure.
- Reproducing Unreal Engine class names or implementation details when a smaller
  Durin API satisfies the selected ownership and ordering contract.

## Design Decisions and Invariants

### High-Level Ownership

- `DTexture2D` and `DTextureCube` are the sole high-level owners of their stable
  texture reference and current concrete render resource.
- The concrete resource is not reference-counted. Producer-thread storage uses
  an owning representation whose deletion is gated by the RenderCore release
  protocol; render-thread mirrors and commands use non-owning pointers only
  while command ordering proves validity.
- `FTextureResource` cannot keep its originating `DTexture` alive and cannot
  dereference the asset from the rendering thread.
- A queued command may own immutable platform-data input, but it never acquires
  shared ownership of the concrete resource or the texture asset.
- Asset destruction starts resource/reference release and transfers any object
  awaiting render-thread completion to explicit deferred cleanup. It never
  relies on a smart-pointer destructor choosing the correct thread.

### Stable Texture Reference

- Each texture asset owns one stable `FTextureReference` for its lifetime.
- `FTextureReference` follows `FRenderResource`; it is not itself a shared
  high-level object and is never wrapped in `shared_ptr`.
- `FTextureReference` owns a reference-counted RHI texture-reference object.
  That RHI object is the stable consumer-facing indirection and may outlive the
  asset when an accepted render command or render-thread snapshot still holds a
  counted reference.
- Creating, retargeting, clearing, and releasing the texture reference occur
  only through rendering-thread operations.
- Before a texture asset releases its reference, the reference is retargeted to
  the correct renderer fallback or cleared according to the RHI contract.
- Copying an RHI texture-reference does not extend the lifetime of
  `FTextureResource` or its concrete `FTextureRHIRef`.

### Concrete Resource

- A Texture2D or TextureCube concrete resource derives from the common
  `FRenderResource` lifecycle and owns the actual `FTextureRHIRef`.
- `InitRHI` creates and uploads a candidate texture on the rendering thread.
  Publication updates the stable texture reference only after the complete
  candidate succeeds and its revision remains current.
- `ReleaseRHI` first detaches the stable reference when it still targets the
  resource, then resets the concrete `FTextureRHIRef` on the rendering thread.
- Replacing a resource initializes the new resource, atomically retargets the
  stable reference on the rendering thread, releases the old resource, and
  retires the old C++ object through deferred cleanup.
- Failed or stale candidates are never published. They are released and retired
  through the same RenderCore lifecycle as a previously current resource.

### Consumers

- Producer-side material and component data retain reflected texture assets
  only where ordinary dependency and GC ownership requires them.
- Cross-thread material and scene snapshots contain no `DTexture` pointer and no
  owning `FTextureResource` pointer.
- Long-lived render snapshots retain the counted RHI texture reference. A
  shorter render-thread-only call may use a non-owning reference pointer only
  when its enclosing snapshot or command owns the counted reference.
- Renderer fallback selection resolves through the stable texture reference;
  missing, pending, failed, or released concrete resources never expose a stale
  texture.
- Editor previews and thumbnail jobs either retain the asset through their
  producer-side job lifetime or capture a counted RHI texture reference for
  accepted render work. They do not retain the concrete resource.

### Revisions, Diagnostics, and Ordering

- Build and release requests share one monotonic revision sequence per texture
  asset.
- The asset owns producer-visible requested revision and diagnostic state.
  Render-thread completion publishes a revision-tagged result through a
  thread-safe completion channel that does not require sharing the concrete
  resource.
- A stale build, failure, release, or completion cannot replace or report over
  a newer request.
- Command ordering, not shared ownership, keeps non-owning resource pointers
  valid: object retirement is enqueued after every accepted command that may
  dereference the object.
- Resource replacement does not require a synchronous wait. Synchronous fences
  are reserved for shutdown, tests, module boundaries, and APIs that explicitly
  require completion.

### RHI Reference Counting and Deletion

- `TRefCountPtr` may drop its final reference from any producer or render
  context supported by the engine.
- `FRHIResource::Release` must enqueue zero-reference resources into the
  thread-safe deferred-delete queue rather than directly deleting backend
  objects on the releasing thread.
- Deferred RHI deletion is drained on an RHI-valid thread before `RHIExit`.
- `FRHITextureReference` owns only indirection state. Retargeting it controls
  the counted reference to the current concrete texture without transferring
  ownership of `FTextureResource`.

### Shutdown and Failure

- RenderCore rejects new resource init, update, release, and reference-retarget
  work after admission closes.
- Engine and module teardown stop texture producers, remove scene consumers,
  close editor consumers, and begin release of asset-owned resources before the
  final drain.
- The final drain completes accepted RenderCore commands, releases every
  initialized `FRenderResource`, retires deferred C++ resource objects, drains
  deferred RHI deletion, then permits rendering-thread termination and
  `RHIExit`.
- Enqueue-after-close, destruction of an initialized resource, a live concrete
  resource at asset-finalization completion, or a pending RHI delete at
  `RHIExit` is a lifecycle failure with an actionable diagnostic.
- The TextureCube custom deleter remains only until both texture types satisfy
  the common asset-owned lifecycle with equivalent regression coverage.

## Current Foundations and Gaps

### Foundations

- `FRenderThreadCommandPipe` serializes accepted commands on the rendering
  thread.
- `FRenderCommandFence` and `FlushRenderingCommands` provide explicit drain
  points.
- `FRenderResource` already names render-thread initialization, update, and
  release operations.
- `TRefCountPtr` and `FRHIResource` already provide atomic reference counting
  and a mutex-protected deferred-delete queue.
- Texture2D and TextureCube already publish immutable platform-data snapshots,
  use monotonic revisions, reject stale commands, and expose thread-safe
  diagnostics.
- Renderer fallbacks already cover missing, pending, and failed texture
  resources.
- Scene and renderer resources are released before rendering-thread shutdown in
  the current engine exit path.

### Gaps

- Durin has no stable `FTextureReference`/`FRHITextureReference` path equivalent
  to the selected ownership model.
- Most `FRenderResource` lifecycle functions and registry behavior are
  placeholders.
- Texture assets and every major consumer currently share concrete resources
  through `shared_ptr`.
- Texture2D and TextureCube duplicate build, release, revision, and diagnostic
  publication code.
- There is no explicit deferred C++ cleanup protocol for an asset-owned
  `FRenderResource` awaiting rendering-thread release.
- Command admission has no explicit running, draining, and stopped state.
- Existing tests prove stale revision behavior but do not prove that non-owning
  resource commands remain valid across asset unload, replacement, GC, preview
  closure, or shutdown.
- Existing shutdown validation does not assert that the render-resource
  registry, deferred resource cleanup, command pipe, and deferred RHI delete
  queue are all empty before `RHIExit`.

## Implementation Stages

### Stage 0: Ownership Audit and Executable Refactor Contract

- [ ] Record every owner and consumer of Texture2D and TextureCube resources,
  including assets, material dependencies and render data, static-mesh proxies,
  skybox snapshots, editors, thumbnails, render commands, tests, garbage
  collection, module unload, and shutdown.
- [ ] Classify each use as asset owner, producer-side asset dependency,
  stable-reference owner, counted RHI-reference consumer, render-thread-only
  non-owning resource access, immutable command input, or deferred cleanup.
- [ ] Identify every command that currently captures a resource `shared_ptr` and
  specify the enqueue/retirement ordering that will make its replacement
  non-owning pointer safe.
- [ ] Record the selected concrete symbols for the stable texture reference,
  RHI texture reference, common texture resource, Texture2D resource,
  TextureCube resource, and deferred cleanup mechanism before Stage 1.
- [ ] Add scheduler-controlled lifetime tests for resource replacement and asset
  unload with commands paused before init, publication, release, and retirement.
- [ ] Add equivalent Texture2D and TextureCube coverage; absence of a current
  Texture2D crash is not proof of safety.
- [ ] Verify the RHI zero-reference path defers backend destruction regardless
  of the thread dropping the final `TRefCountPtr`.

#### Acceptance Gate

- The ownership matrix accounts for every repository reference to both concrete
  texture resource types, no consumer requires shared ownership of either
  resource, command-order tests reproduce the unsafe pre-refactor cases without
  timing sleeps, and all public symbols required by Stage 1 are recorded.

### Stage 1: RenderCore Resource Lifecycle

- [ ] Implement `FRenderResource` initialized-state registration, stable list
  removal, render-thread checks, init phase, initialization, update, release,
  and global RHI release/reinit behavior.
- [ ] Add producer-thread begin-init, begin-update, and begin-release helpers
  that enqueue non-owning resource pointers with explicit ordering contracts.
- [ ] Add deferred C++ cleanup for resources whose release has been accepted but
  whose owning asset may proceed with finalization.
- [ ] Define idempotent behavior and diagnostics for double init, double
  release, release-before-init, destruction while initialized, and failed init.
- [ ] Add focused RenderCore tests proving every lifecycle callback and concrete
  resource destruction thread.

#### Acceptance Gate

- A test asset can uniquely own, asynchronously initialize, update, release,
  and retire a test render resource; no consumer shares ownership, every
  RHI-affine callback runs on the rendering thread, and the registry is empty
  after completion.

### Stage 2: Stable Texture Reference

- [ ] Introduce the RHI texture-reference resource and its counted reference
  type, including fallback initialization, render-thread retarget, clear, and
  deferred deletion.
- [ ] Introduce `FTextureReference` as an asset-owned `FRenderResource` with
  producer-thread begin-init/begin-release entry points.
- [ ] Add the common concrete texture resource base required to expose the
  current `FTextureRHIRef` only on the rendering thread.
- [ ] Implement render-thread publication that retargets one stable reference
  from an old concrete texture to a fully initialized replacement.
- [ ] Prove that copied RHI texture references remain valid across concrete
  resource replacement and asset release, resolving to the replacement or
  fallback as specified.
- [ ] Prove that a copied RHI texture reference never keeps the old concrete
  `FTextureResource` C++ object alive.

#### Acceptance Gate

- One asset-owned stable reference can survive repeated concrete resource
  replacement without consumer rebinding; old resources release and retire in
  order, and the final counted RHI-reference release uses deferred RHI deletion
  on every tested thread.

### Stage 3: Texture2D Asset-Owned Resource Migration

- [ ] Replace `shared_ptr<FTexture2DRenderResource>` with one asset-owned stable
  texture reference and one explicitly managed Texture2D concrete resource.
- [ ] Move texture creation and upload into the common `FRenderResource`
  lifecycle while preserving format checks, complete mip upload, fallback,
  build status, failure reason, and revision behavior.
- [ ] Move producer-visible diagnostics from the shared resource proxy to the
  asset/completion channel.
- [ ] Implement asynchronous replacement: initialize candidate, publish through
  the stable reference, release old resource, then retire old C++ storage.
- [ ] Cover rebuild, failed candidate, stale candidate, unload, GC, undo/redo,
  cooked load, and final asset destruction.

#### Acceptance Gate

- `DTexture2D` is the only high-level owner of its reference and concrete
  resource, Texture2D exposes no owning resource smart pointer, existing
  behavioral tests pass, and replacement/unload require no ordinary flush.

### Stage 4: TextureCube Asset-Owned Resource Migration

- [ ] Replace `shared_ptr<FTextureCubeRenderResource>` with the same
  asset-owned reference and concrete-resource lifecycle used by Texture2D.
- [ ] Preserve six-face/mip upload, ready-revision selection, format failure,
  fallback, panorama-derived data, and Vulkan readback behavior.
- [ ] Remove `FTextureCubeRenderResource::Create`, `enable_shared_from_this`,
  and the class-specific custom deleter.
- [ ] Replace the baseline custom-deleter regression with init, publication,
  release, retirement, and final RHI deferred-delete assertions.
- [ ] Cover rebuild, failed/stale candidate, unload, GC, and final asset
  destruction with active cube render consumers.

#### Acceptance Gate

- TextureCube uses no owning resource smart pointer or resource-specific
  deletion policy, all concrete resource and RHI mutation occurs through the
  common RenderCore lifecycle, and the baseline critical path remains covered
  deterministically.

### Stage 5: Consumer Ownership Migration

- [ ] Replace material render-data and static-mesh proxy
  `shared_ptr<FTexture2DRenderResource>` fields with counted stable RHI texture
  references and explicit fallback metadata.
- [ ] Replace skybox scene snapshot and preview-proxy
  `shared_ptr<FTextureCubeRenderResource>` fields with the corresponding stable
  RHI texture reference.
- [ ] Migrate renderer texture resolution to consume stable reference
  indirection without accessing texture assets or owning concrete resources.
- [ ] Migrate Texture Editor, material thumbnail, cube thumbnail, preview jobs,
  and readback fixtures to producer-side asset lifetime or counted render
  references as appropriate.
- [ ] Remove `GetRenderResource()` from texture-facing consumer APIs; expose
  only producer diagnostics, stable reference acquisition, and narrowly scoped
  test hooks.
- [ ] Add integration coverage for material snapshot replacement, static-mesh
  removal, skybox replacement/removal, preview closure, thumbnail cancellation,
  asset unload, and GC while accepted render work is outstanding.

#### Acceptance Gate

- No non-asset owner stores or captures a concrete Texture2D or TextureCube
  resource, render-thread snapshots contain no reflected texture pointer, and
  every consumer continues to observe replacement through the stable reference.

### Stage 6: Command Admission, Shutdown, and Resource Audit

- [ ] Add explicit RenderCore command admission states for running, draining,
  and stopped, with synchronous actionable rejection after close.
- [ ] Integrate producer stop, scene removal, editor-consumer closure, asset
  resource/reference release, accepted-command drain, deferred C++ cleanup,
  render-resource registry verification, deferred RHI deletion, rendering-thread
  stop, and `RHIExit` into one asserted ordering.
- [ ] Exercise shutdown with open texture editors, material previews, cube
  thumbnails, an active skybox, and pending init/replacement/release commands.
- [ ] Audit other `FRenderResource` types for shared ownership, missing explicit
  release, and RHI references that can escape their documented owner.
- [ ] Migrate only resources that violate the selected lifecycle; record
  compliant unique-owner and renderer-owned designs without rewriting them.
- [ ] Add diagnostics naming resource type, owner asset, revision, lifecycle
  phase, and pending queue when shutdown finds live work.

#### Acceptance Gate

- Repeated startup/shutdown and high-churn texture replacement leave the command
  pipe, render-resource registry, deferred C++ cleanup, and deferred RHI delete
  queue empty before `RHIExit`; the audit contains no unexplained owner.

### Stage 7: Runtime Documentation and Final Validation

- [ ] Update Texture System with asset ownership, stable texture-reference
  indirection, concrete-resource replacement, diagnostics, and release ordering.
- [ ] Update Cube Textures with skybox and preview stable-reference retention.
- [ ] Update Runtime Lifecycle with RenderCore admission close, resource release,
  deferred cleanup, RHI deletion drain, and shutdown ordering.
- [ ] Update the Texture Support plan validation gap with final evidence.
- [ ] Run focused unit and integration coverage, hardware-backed Vulkan
  validation, the full native-test suite, complete `all` build, editor smoke
  validation, and repeated clean editor exit through the documented
  DurinDevTool workflow.

#### Acceptance Gate

- Runtime documentation contains the implemented contract, all validation rows
  have passing evidence or an explicitly accepted environmental limitation, and
  no active documentation or code describes texture consumers as sharing
  ownership of a concrete render resource.

## Validation Matrix

| Layer | Required coverage |
| --- | --- |
| RenderCore unit | Registry, init/update/release affinity, deferred C++ cleanup, double operations, and destruction ordering |
| RHI reference unit | Stable indirection creation, retarget, fallback, counted copies, cross-thread final release, and deferred deletion |
| Texture unit | Texture2D and TextureCube build, replacement, stale revision, failed candidate, release, asset unload, and GC |
| Consumer integration | Material snapshot replacement, static-mesh removal, skybox replacement/removal, editor preview closure, and thumbnail cancellation |
| Ownership | No consumer smart pointer to a concrete texture resource; no render snapshot owns or dereferences a texture asset |
| Command ordering | Paused init/publication/release/retirement with asset replacement and destruction in every relevant ordering |
| Shutdown | Admission close, pending texture work, open editor consumers, module unload, registry drain, deferred cleanup, RHI delete drain, and `RHIExit` |
| Vulkan | Texture2D and TextureCube upload, sampling, readback, reference retarget, replacement, retirement, and validation-layer clean shutdown |
| End to end | Full native tests, full `all` build, hidden-window editor smoke run, and repeated clean editor exit |

## Definition of Done

- `DTexture2D` and `DTextureCube` are the only high-level owners of their stable
  texture reference and current concrete resource.
- No consumer stores, captures, or returns an owning smart pointer to a concrete
  texture resource.
- Materials, scenes, previews, and thumbnails observe replacement through a
  stable counted RHI texture reference.
- Every concrete resource is initialized, updated, released, and retired through
  `FRenderResource` in deterministic command order.
- Concrete resource replacement and ordinary unload are asynchronous and do not
  require a full render-command flush.
- TextureCube has no custom deleter and Texture2D/Cube share one lifecycle.
- Build, release, failure, and stale-command behavior remains revision-correct.
- Producer-visible diagnostics do not require shared access to a concrete
  resource.
- RenderCore rejects work after admission closes and drains accepted work before
  rendering-thread termination.
- The render-resource registry, deferred C++ cleanup, and deferred RHI delete
  queue are empty before `RHIExit`.
- The repository resource audit contains no unexplained shared ownership or
  thread-affinity violation.
- Runtime contracts are updated and the complete validation matrix passes.

## Deferred Follow-ups

- Texture streaming and partial residency.
- Background texture decode and platform-data builds.
- Bindless descriptor indexing or stable numeric texture IDs.
- A general renderer resource registry beyond `FRenderResource` if later
  consumers require serialization-stable handles.
- Static analysis or ownership annotations for render-thread-only pointers.
- General GPU-fence-based deferred deletion beyond the existing high-level
  RenderCore and RHI deletion lifecycle.

## Related Documentation

- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/NativeTests.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Runtime/Rendering/TextureSystem.md`
- `Documentation/Runtime/Rendering/CubeTextures.md`
- `Documentation/Plans/TextureSupport.md`

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RenderResource.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderResource.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderingThread.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderingThread.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Private/RHIResources.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2DRenderResource.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DRenderResource.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCubeRenderResource.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCubeRenderResource.cpp`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialTypes.h`
- `Engine/Source/Runtime/Engine/Public/Engine/PrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Public/IScene.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Tests/Native/EngineTests/Private/TextureCubeTests.cpp`
