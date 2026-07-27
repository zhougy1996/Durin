# Render Resource Lifetime Plan

Summary: Establish one RenderCore lifecycle for cross-thread render handles and render-thread-owned RHI payloads, then migrate Texture2D and TextureCube away from shared ownership of RHI-bearing objects.

Last reviewed: 2026-07-28

Status: Active
Completed:

## Current Status

No implementation stage has started.

Baseline commit `7ea4c0b4` prevents the observed TextureCube destruction failure
by giving `FTextureCubeRenderResource` a class-specific custom deleter that
returns final deletion to the rendering thread. The focused regression and a
full `all` build passed at that baseline. This is a bounded safety fix, not the
target architecture.

`FTexture2DRenderResource` and `FTextureCubeRenderResource` still combine two
different lifetimes:

- a thread-safe, revisioned proxy retained by assets, material snapshots,
  scenes, previews, and queued commands; and
- an RHI texture whose creation, replacement, access, and release belong only
  to the rendering thread.

Because the proxy itself owns the RHI reference, its last `shared_ptr` owner
also decides where the RHI-bearing C++ object is destroyed. TextureCube now
masks that ambiguity with its custom deleter, while Texture2D retains the same
underlying ownership shape without that protection.

RenderCore already contains an `FRenderResource` abstraction and render-command
fences, but its global registration, initialization, release, and update
functions are currently placeholders. Texture resources bypass that foundation
and implement independent build/release command protocols.

## Goal

Make render-resource lifetime deterministic by separating shareable identity
from RHI ownership:

- producer and consumer code may share a stable handle that owns no RHI object;
- only rendering-thread code may create, replace, read, release, or destroy an
  RHI payload;
- release commands explicitly retire payload ownership before the handle can
  become unreachable;
- shutdown drains all accepted retirement work before the rendering thread and
  RHI stop;
- Texture2D and TextureCube use the same RenderCore lifecycle rather than
  resource-specific deletion policies.

## Scope

- Define the ownership, thread-affinity, revision, release, and shutdown
  contract for render resources.
- Complete the minimum `FRenderResource` and rendering-command support required
  by that contract.
- Introduce a shareable texture render handle whose destruction is safe on any
  thread because it owns no live RHI payload at retirement.
- Keep the texture RHI payload exclusively rendering-thread-owned.
- Migrate Texture2D and TextureCube build, replacement, failure, and release
  behavior.
- Migrate material render data, static-mesh scene proxies, skybox scene
  snapshots, texture editors, and thumbnail previews to retain handles rather
  than RHI-bearing resource objects.
- Define deterministic behavior for asset unload, stale commands, failed
  uploads, scene removal, editor preview closure, garbage collection, module
  unload, and engine shutdown.
- Add focused and integration coverage for final-owner ordering and shutdown.
- Remove the TextureCube-specific custom deleter after both texture types use
  the common lifecycle.

## Non-Goals

- Texture streaming, sparse residency, virtual textures, or residency budgets.
- Moving source decoding or platform-data builds to background workers.
- Changing texture package, DDC, or cooked payload formats.
- Replacing low-level RHI reference counting throughout every backend.
- Rewriting the renderer scene architecture or material parameter system.
- Migrating resource types that do not cross the same ownership boundary unless
  the Stage 0 audit proves they violate the selected invariants.
- Requiring a render-command flush for ordinary asset unload or editor preview
  closure.

## Design Decisions and Invariants

### Ownership

- A cross-thread handle and an RHI payload are separate objects with separate
  lifetimes.
- The handle may be retained by producer-thread assets, render snapshots,
  previews, diagnostics, and queued commands. It contains revision and
  thread-safe diagnostic state but no `FRHIResource` reference.
- The payload contains the texture RHI reference and any state that requires
  render-thread destruction. It is created, installed, replaced, released, and
  destroyed only on the rendering thread.
- A scene or material snapshot may extend handle lifetime, but it cannot extend
  payload lifetime independently of the handle's revisioned render commands.
- No custom `shared_ptr` deleter is part of the final texture lifecycle.

### Thread Affinity

- Producer threads may publish immutable platform-data snapshots and monotonic
  requests. They never read or mutate payload state.
- Render-thread accessors resolve a handle to its current payload and return a
  renderer fallback when no matching ready payload exists.
- A handle destructor may run on any thread. It asserts only that no live
  payload remains; it does not require rendering-thread affinity.
- Every path that destroys an initialized payload first executes its RenderCore
  release contract on the rendering thread.

### Revisions and Ordering

- Build and release requests share one monotonic revision sequence per handle.
- A stale build or release cannot install, retire, or report failure over a
  newer request.
- A release request immediately makes newer producer-side observations pending,
  then retires the matching payload on the rendering thread.
- A queued command retains the handle until its work and any required payload
  retirement are complete. Command lifetime is not used to choose the thread on
  which the handle itself must be deleted.
- Failed creation or upload destroys its candidate payload on the rendering
  thread and publishes failure only when its revision remains current.

### Shutdown and Failure

- RenderCore rejects new resource work after shutdown admission closes.
- Engine and module teardown release scenes and producer-owned resources before
  draining accepted render commands.
- The final drain retires all payloads before the rendering thread stops and
  before `RHIExit`.
- A resource that reaches shutdown with a live payload is a lifecycle failure,
  not a tolerated leak or an enqueue into a stopped command pipe.
- Synchronous waiting is reserved for shutdown, tests, and APIs that explicitly
  require completion. Normal replacement and unload remain asynchronous.

### Migration

- The TextureCube custom deleter remains only until the common handle/payload
  path has equivalent regression coverage.
- Texture2D and TextureCube must converge in the same plan. Shipping one on the
  common lifecycle while leaving the other on shared RHI-bearing ownership does
  not satisfy the plan.
- Long-lived lifecycle rules move to Runtime documentation when implemented;
  this plan records stages and evidence rather than becoming a competing
  runtime specification.

## Current Foundations and Gaps

### Foundations

- `FRenderThreadCommandPipe` serializes accepted commands onto the rendering
  thread.
- `FRenderCommandFence` and `FlushRenderingCommands` provide explicit drain
  points.
- `FRenderResource` names render-thread initialization, update, and release
  operations.
- Texture2D and TextureCube already publish immutable platform-data snapshots,
  track monotonic revisions, reject stale commands, and expose thread-safe
  diagnostics.
- Renderer fallbacks already cover missing, pending, and failed texture
  resources.
- Scene release and renderer-resource release occur before rendering-thread
  shutdown in the engine exit path.

### Gaps

- Most `FRenderResource` lifecycle functions are placeholders and texture
  resources do not use them.
- The texture proxy directly owns `FTextureRHIRef`.
- Texture2D and TextureCube duplicate lifecycle code.
- Materials, skybox scene data, and preview proxies retain the RHI-bearing
  proxy through `shared_ptr`.
- TextureCube has a class-specific deletion policy while Texture2D does not.
- Command admission has no explicit closed state for rendering-thread shutdown.
- Existing tests cover stale revisions but not every final-owner ordering,
  payload retirement, or post-admission shutdown case.

## Implementation Stages

### Stage 0: Ownership Audit and Executable Lifetime Contract

- [ ] Record every owner and consumer of Texture2D and TextureCube render
  resources, including assets, material data, scene proxies, skybox snapshots,
  thumbnails, editors, render commands, garbage collection, and shutdown.
- [ ] Classify each reference as producer owner, cross-thread handle owner,
  render-thread payload owner, or non-owning render access.
- [ ] Validate that no unrecorded consumer requires direct ownership of the RHI
  payload.
- [ ] Add deterministic scheduler-controlled tests for both orderings: the
  release command drops its reference before the producer owner, and the
  producer owner drops its reference before the release command.
- [ ] Add equivalent Texture2D coverage so absence of a current failure report
  is not treated as proof of safety.
- [ ] Record the selected handle and payload symbols in this plan before Stage 1
  implementation begins.

#### Acceptance Gate

- The ownership matrix accounts for every repository reference to both texture
  resource types, focused tests reproduce the unsafe pre-migration ordering
  without timing sleeps, and no unresolved consumer requires shared ownership
  of an RHI-bearing object.

### Stage 1: RenderCore Lifecycle Foundation

- [ ] Implement the required `FRenderResource` initialized-state bookkeeping,
  render-thread checks, initialization, update, and release behavior.
- [ ] Add producer-thread helpers for asynchronous initialization and release
  without embedding deletion policy in individual resource classes.
- [ ] Define command-pipe admission states for running, draining, and stopped.
- [ ] Make enqueue-after-close fail synchronously with an actionable diagnostic.
- [ ] Define a shutdown drain that proves the render-resource registry and
  pending retirement set are empty before rendering-thread termination.
- [ ] Add RenderCore unit tests for init, replacement, release, double release,
  stale work, drain ordering, and rejected post-close work.

#### Acceptance Gate

- A test render resource can be initialized, updated, retired, and destroyed
  with all RHI-affine callbacks on the rendering thread; shutdown cannot silently
  lose an accepted command or accept work after admission closes.

### Stage 2: Shared Texture Handle and Render-Thread Payload

- [ ] Introduce the common texture handle with monotonic requested revision,
  applied revision, failure revision, failure reason, and resource state.
- [ ] Introduce render-thread-owned texture payload storage that owns the
  `FTextureRHIRef` and follows the Stage 1 lifecycle.
- [ ] Move common build/release ordering and diagnostic publication out of the
  Texture2D and TextureCube resource classes.
- [ ] Keep type-specific creation descriptors and upload loops behind focused
  Texture2D and TextureCube payload builders.
- [ ] Ensure stale and failed candidate payloads are destroyed on the rendering
  thread without disturbing the current ready payload.
- [ ] Make handle destruction thread-agnostic and assert that payload retirement
  completed.

#### Acceptance Gate

- Common unit tests prove handle lifetime is independent of final-owner thread,
  every payload constructor/destructor and RHI mutation occurs on the rendering
  thread, and stale or failed builds cannot replace or retire a newer payload.

### Stage 3: Texture2D Migration

- [ ] Replace `FTexture2DRenderResource` shared RHI ownership with the common
  handle and Texture2D payload builder.
- [ ] Preserve current fallback, build status, unsupported-format diagnostics,
  mip upload, and revision behavior.
- [ ] Migrate material render data, material dependency updates, Texture Editor
  previews, and material thumbnails to the handle API.
- [ ] Cover asset rebuild, unload, garbage collection, material snapshot
  replacement, preview closure, and upload failure.

#### Acceptance Gate

- Existing Texture2D and material tests remain behaviorally unchanged, new
  lifetime tests pass under both final-owner orderings, and no Texture2D-facing
  consumer retains an RHI-bearing resource object.

### Stage 4: TextureCube Migration

- [ ] Replace `FTextureCubeRenderResource` shared RHI ownership with the common
  handle and TextureCube payload builder.
- [ ] Migrate skybox scene snapshots, cube thumbnails, and preview scene proxies
  to the handle API.
- [ ] Preserve face/mip upload, ready-revision selection, format failure,
  renderer fallback, and Vulkan readback behavior.
- [ ] Remove `FTextureCubeRenderResource::Create` and its custom deleter.
- [ ] Replace the baseline custom-deleter regression with handle/payload
  retirement assertions.

#### Acceptance Gate

- Skybox replacement, component removal, cube editor/thumbnail closure, asset
  unload, and shutdown retire cube payloads on the rendering thread; the
  baseline critical path remains covered without any resource-specific custom
  deleter.

### Stage 5: Shutdown Integration and Resource Audit

- [ ] Integrate resource admission close, scene release, payload drain,
  rendering-thread stop, and `RHIExit` into one asserted exit ordering.
- [ ] Exercise editor shutdown with open texture editors, material previews,
  cube thumbnails, an active skybox, and pending texture rebuild commands.
- [ ] Audit other resource types for cross-thread shared ownership of
  RHI-bearing objects.
- [ ] Migrate only resources that violate the selected invariants; record
  compliant unique-owner or renderer-owned designs without rewriting them.
- [ ] Add diagnostics that identify the resource type, revision, and owner phase
  when shutdown finds a live payload.

#### Acceptance Gate

- Repeated full editor startup/shutdown and forced high-churn texture scenarios
  leave no live payloads, no commands in a stopped pipe, and no RHI release
  after `RHIExit`; the repository-wide audit has no unexplained ownership path.

### Stage 6: Documentation and Final Validation

- [ ] Update the Texture System contract with handle/payload ownership,
  revisions, fallback, and retirement.
- [ ] Update Cube Textures to describe skybox and preview handle retention.
- [ ] Update Runtime Lifecycle with render-resource admission close and shutdown
  drain ordering.
- [ ] Update the Texture Support plan validation gap with final evidence.
- [ ] Run focused unit, integration, hardware-backed Vulkan, full native-test,
  complete `all` build, and editor smoke validation through the documented
  DurinDevTool workflow.

#### Acceptance Gate

- Runtime documentation contains the implemented contract, all validation rows
  have recorded passing evidence or an explicitly accepted environmental
  limitation, and no active documentation describes textures as sharing an
  RHI-bearing proxy across threads.

## Validation Matrix

| Layer | Required coverage |
| --- | --- |
| RenderCore unit | Init/update/release thread affinity, admission close, drain, double release, and stopped-pipe rejection |
| Texture unit | Texture2D and TextureCube build, replacement, stale revision, failure, release, and both final-owner orderings |
| Consumer integration | Material snapshot replacement, static-mesh proxy removal, skybox replacement/removal, editor preview closure, and thumbnail retirement |
| Garbage collection | Asset unload and GC while render commands and consumer handles are outstanding |
| Shutdown | Pending build and release commands, open editor consumers, module unload, final drain, rendering-thread stop, and `RHIExit` ordering |
| Vulkan | Texture2D and TextureCube upload, sampling, readback, replacement, and retirement with validation enabled |
| End to end | Full native tests, full `all` build, hidden-window editor smoke run, and repeated clean editor exit |

## Definition of Done

- Texture2D and TextureCube consumers share only handles that own no RHI
  resource.
- Every live texture RHI payload has exactly one rendering-thread owner.
- Every payload is released and destroyed on the rendering thread before
  handle destruction or RHI shutdown.
- Final handle release is safe on any thread and needs no custom deleter.
- Build, release, failure, and stale-command behavior remains revision-correct.
- Render-command admission and shutdown drain ordering are explicit and tested.
- The TextureCube baseline critical is covered by deterministic regression
  tests, and equivalent Texture2D coverage passes.
- The resource audit contains no unexplained cross-thread ownership of an
  RHI-bearing object.
- Runtime contracts are updated and the complete validation matrix passes.

## Deferred Follow-ups

- Texture streaming and partial residency.
- Background texture decode and platform-data builds.
- A unified renderer resource registry if later resource types require stable
  numeric handles across serialization or process boundaries.
- Static analysis or ownership annotations for render-thread-only payload
  fields.
- General RHI deferred-deletion queues beyond the high-level RenderCore
  lifecycle.

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
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2DRenderResource.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DRenderResource.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCubeRenderResource.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCubeRenderResource.cpp`
- `Engine/Source/Runtime/Engine/Public/Materials/MaterialTypes.h`
- `Engine/Source/Runtime/Engine/Public/Engine/PrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Public/IScene.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Launch/Private/LaunchEngineLoop.cpp`
- `Engine/Tests/Native/EngineTests/Private/TextureCubeTests.cpp`
