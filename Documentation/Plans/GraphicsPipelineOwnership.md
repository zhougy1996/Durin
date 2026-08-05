# Graphics Pipeline Ownership and Reload Plan

Summary: Remove name-keyed RHI ownership of graphics PSOs so Renderer resource slots can publish, retain, replace, and release complete pipeline candidates correctly across shader reload.

Last reviewed: 2026-08-06

Status: Active
Completed:

## Current Status

Vulkan currently stores every created graphics PSO in
`FVulkanPipelineStateCacheManager::PSOCache`, keyed only by `FName`. A repeated
creation with the same name compares only `FRHIRenderTargetLayout` and returns
the old object. Shader modules and content, vertex declaration, pipeline layout,
primitive and raster state, blend state, and depth state do not participate in
the lookup. Stable Renderer diagnostic names can therefore turn a shader
refresh into a silent cache hit on the previous native pipeline.

The cache also adds an RHI-owned strong reference for the lifetime of the
device. Renderer resource slots can appear to replace or reset their payloads,
but cannot actually release a PSO held by the backend. This contradicts the
existing transactional resource contract: the slot must be able to keep one
last-known-good payload after a failed refresh, atomically replace it after a
successful refresh, and release it on device invalidation or shutdown.

The creation path has a second publication defect. A non-success result from
`vkCreateGraphicsPipelines` logs an error but returns a non-null
`FVulkanGraphicsPipelineState` whose `vk::Pipeline` is null. The named cache then
retains that incomplete wrapper and converts later attempts with the same name
into permanent false successes.

No production caller uses `FDynamicRHI::RHIGetGraphicsPipelineState`; it exists
only in the public interface, Vulkan implementation, and a test double. The
existing Vulkan reload test proves last-known-good and replacement behavior,
but includes the shader generation in the PSO name, which avoids the production
aliasing defect instead of detecting it.

No implementation work has started.

## Goal

- Make Renderer resource slots and explicit Renderer resource owners the only
  long-lived owners of logical graphics PSOs.
- Make every graphics PSO creation request build a fresh candidate regardless
  of diagnostic-name reuse.
- Publish either a fully usable graphics PSO or null; never publish or retain a
  wrapper with a null native pipeline or partially committed native state.
- Preserve shader-refresh last-known-good behavior: failed refresh keeps the
  old complete payload, while successful refresh with the same diagnostic name
  installs a distinct pipeline built from the new shaders and state.
- Prove that replaced, reset, and shutdown PSOs become eligible for the existing
  RHI and Vulkan deferred-deletion paths once commands holding transient
  references have completed.

## Scope

- The backend-neutral graphics-PSO creation and lookup surface in `FDynamicRHI`.
- Vulkan graphics pipeline creation, pipeline-layout ownership, diagnostic
  naming, and `FVulkanPipelineStateCacheManager` responsibilities.
- All Renderer graphics-PSO creation sites and their
  `TRenderResourceCreationSlot`, `TRendererResourceSlotCache`, or explicit owner.
- Focused RHI, Vulkan RHI, and Renderer reload/lifetime regression tests.
- Lasting shader reload, Renderer resource ownership, and future compute-PSO
  roadmap documentation affected by the selected identity boundary.

## Non-Goals

- Adding an engine-wide PSO cache, PSO precaching, asynchronous compilation,
  serialized PSO databases, prewarming, or eviction policy.
- Removing the Vulkan render-pass, descriptor-set-layout, pipeline-layout
  description, shader artifact, or driver-internal pipeline caches when they do
  not retain logical graphics PSOs.
- Changing shader variant keys, dependency fingerprints, compiler artifact
  publication, or the Renderer shader/manual/device generation model.
- Replacing `TRenderResourceCreationSlot` or moving Renderer logical-resource
  ownership into the RHI.
- Defining the generic recoverable RHI executor boundary or auditing every
  non-pipeline factory. Those remain owned by
  [Recoverable RHI Creation Failures](RecoverableRHICreationFailures.md).
- Making programming errors such as invalid render-target layouts, missing
  required shaders, invalid vertex declarations, or incompatible reflected
  layouts recoverable cache misses.

## Design Decisions and Invariants

### Logical ownership

- `RHICreateGraphicsPipelineState` is a factory, not a lookup. Every successful
  call returns a newly created PSO even when another live PSO has the same
  diagnostic name and an identical initializer.
- Renderer resource slots or explicit Renderer owners retain the strong
  reference that represents a live logical PSO. Recorded RHI commands may keep
  transient strong references until replay completes. The Vulkan device and
  pipeline manager do not retain an owning reference after returning from
  creation.
- `RHIGetGraphicsPipelineState(FName)` and the Vulkan named lookup are removed.
  No backend-neutral API promises that a diagnostic name can recover a PSO.
- Releasing a Renderer payload drops its logical ownership immediately. Native
  destruction still follows the existing `FRHIResource` deletion queue and
  Vulkan frame-safe deferred-deletion rules; removing the cache must not bypass
  either lifetime layer.
- Device-owned caches may continue to own structural dependencies such as
  descriptor-set layouts and compatible render passes. Such caches must not
  retain an `FRHIGraphicsPipelineState` or decide logical PSO identity.

### Identity and diagnostic names

- The `FName` argument is renamed or wrapped as `DebugName` at the API boundary
  and is used only for logs, assertions, capture/debug labels, and telemetry.
  It is excluded from equality, reuse, success, and lifetime decisions.
- Renderer names remain stable and human-readable across reload. Callers must
  not append a generation, pointer, or timestamp to force correctness.
- This plan deliberately selects no engine-level PSO reuse key. If profiling
  later justifies reuse, a separate plan must first define one immutable,
  backend-neutral graphics-pipeline descriptor with value equality and hashing
  over every creation input: shader stage, shader content hash and entry point,
  vertex-declaration contents, render-target and sample layout, primitive
  topology, raster state, blend state, depth/stencil state, dynamic-state
  policy, descriptor binding layouts, and push-constant ranges. Pipeline type
  and device generation form explicit outer cache boundaries. Diagnostic names
  never participate.
- A future cache must define bounded ownership or eviction, failed-entry
  behavior, concurrent creation, device invalidation, and collision-safe full
  equality after hashing. It may not introduce permanent strong ownership or a
  failed/null tombstone merely because a hash was computed.

### Complete-or-null creation

- Vulkan graphics-pipeline construction uses local candidate state. The
  pipeline layout, native pipeline, and any other per-PSO native owner are
  committed to an `FVulkanGraphicsPipelineState` only after every required
  native creation step succeeds.
- All candidate handles are initialized to null and are cleaned up immediately
  on the RHI thread when a later step fails. A non-success
  `vkCreateGraphicsPipelines` result returns null to the Renderer and leaves no
  named entry, incomplete wrapper, leaked pipeline layout, or deferred deletion
  of an invalid handle.
- A published `FVulkanGraphicsPipelineState` always contains a valid native
  pipeline, compatible render pass, descriptor-set layout description, and
  pipeline layout. Bind and push-constant paths may assert this invariant.
- Shader modules and vertex-declaration data are required to remain alive for
  the synchronous native creation call. Vulkan PSOs do not retain unused or
  misleading shader references after successful creation; Renderer payloads
  retain the typed shaders they need for parameter binding and last-known-good
  behavior.
- Contract validation occurs before native creation. Backend creation failure
  returns null through the explicitly fallible synchronous operation owned by
  the recoverable-RHI plan; executor, device-loss, and invariant failures remain
  terminal under that plan's taxonomy.
- Backend diagnostics identify the `DebugName`, native result, and bounded
  descriptor context once. Renderer slots own generation-scoped failure,
  suppression, retained-fallback, and recovery diagnostics.

### Reload and publication ordering

- A shader-generation change makes the demanded Renderer slot construct a
  complete new shader/vertex-declaration/PSO candidate without mutating its live
  payload.
- Failed creation returns null and the slot remains `StaleReady` when a complete
  last-known-good payload exists. The backend never substitutes an older PSO.
- Successful creation atomically replaces the complete slot payload. New draws
  observe the new PSO; already-recorded commands retain the old PSO until their
  owned references are released.
- Device-generation invalidation releases device-dependent payloads instead of
  retaining last-known-good PSOs across devices. Manual and shader invalidation
  keep their existing retry and suppression semantics.
- Multi-PSO aggregates, including static-mesh solid/wireframe and Post Process
  variants, commit only when every required PSO succeeds. Failure of a later
  member releases all uncommitted candidate members and preserves the prior
  aggregate when permitted.

### Cross-plan boundary

- This plan owns graphics-PSO identity, removal of named ownership, transactional
  native pipeline publication, Renderer ownership audit, and reload/lifetime
  evidence.
- [Recoverable RHI Creation Failures](RecoverableRHICreationFailures.md) owns
  the generic fallible synchronous-operation API, executor failure taxonomy,
  and complete-or-null policy shared by all RHI factories. The graphics-pipeline
  implementation in this plan consumes that boundary rather than adding a
  second exception-translation mechanism.
- If named ownership removal lands before the generic fallible boundary,
  explicit Vulkan non-success must already return null and clean its local
  candidate. This plan cannot complete until thrown graphics-pipeline creation
  failures also use the shared fallible boundary and leave the executor live.

## Current Foundations and Gaps

| Area | Foundation | Gap |
| --- | --- | --- |
| Renderer slots | `TRenderResourceCreationSlot` already owns transactional candidate replacement, `StaleReady`, generation-scoped suppression, and device invalidation. | The backend's permanent PSO reference and name alias prevent the slot from controlling identity and lifetime. |
| Renderer payloads | Static Mesh, Sky Box, Post Process, and editor-assistance paths store PSO references in slot-owned payloads or keyed slot entries and generally check null before commit. | Every call site still needs an ownership/null-check audit, especially multi-PSO candidates and shutdown/reset paths. |
| RHI API | `RHICreateGraphicsPipelineState` is synchronous and returns a nullable reference. | The adjacent name-only getter exposes an ownership/cache contract with no production consumer; the create name is semantically ambiguous. |
| Vulkan manager | The manager already centralizes PSO construction and separately caches descriptor-set layout structures. | `PSOCache` owns PSOs forever and aliases all descriptors sharing a name. |
| Vulkan creation | Native state is assembled on the RHI thread and the Vulkan result is available synchronously. | Non-success publishes a non-null invalid wrapper; pipeline-layout cleanup and exception rollback are not transactional. |
| RHI lifetime | `FRHIResource` and recorded command payloads already defer object destruction safely, followed by Vulkan frame-safe native deletion. | The named cache prevents the logical reference count from reaching zero, so these mechanisms cannot run until device teardown. |
| Reload validation | `RendererResourceReloadVulkanTests` covers failed refresh, suppression, recovery, forced reload, and rendered color. | Its generation-suffixed PSO name avoids the production same-name path, and it does not prove replaced PSOs become releasable. |

## Implementation Stages

### Stage 0: Freeze the ownership boundary and make the defect reproducible

- [ ] Inventory every `RHICreateGraphicsPipelineState` caller and record the
  slot, keyed slot cache, explicit Renderer owner, or test fixture that retains
  the returned reference. Reject any call whose logical owner is the RHI name
  cache.
- [ ] Confirm that `RHIGetGraphicsPipelineState` has no production consumer and
  enumerate the interface implementations and test doubles that must be
  migrated when it is removed.
- [ ] Change the Vulkan Renderer reload regression fixture to reuse one stable
  PSO diagnostic name across initial creation, broken refresh, corrected
  refresh, and forced refresh.
- [ ] Add a same-name regression covering changed shader content and at least
  one non-shader initializer difference. On the current implementation the test
  must expose aliasing by pointer, rendered output, or the render-target-only
  assertion boundary.
- [ ] Define focused lifetime evidence that can distinguish Renderer ownership
  from device-lifetime cache ownership without dereferencing a deleted object.
  Prefer test-only creation/destruction counters or an RHI deletion observation
  seam; also cover a recorded command that temporarily keeps the replaced PSO
  alive.
- [ ] Record the implementation-stage dependency on the generic fallible RHI
  operation in the recoverable-RHI plan before changing exception behavior.

#### Acceptance Gate

- Every live graphics PSO has one identified logical owner, the unused named
  lookup migration is bounded, and a stable-name test fails for the current
  aliasing behavior while the lifetime test can detect a permanent backend
  reference.

### Stage 1: Remove named PSO ownership and lookup

- [ ] Remove `FDynamicRHI::RHIGetGraphicsPipelineState`, its Vulkan override,
  pipeline-manager lookup, and affected test-double boilerplate.
- [ ] Remove `FVulkanPipelineStateCacheManager::PSOCache`, cache-hit layout
  comparison, cache clearing, and all strong PSO ownership from the Vulkan
  device/manager.
- [ ] Rename `Name` to `DebugName` in the graphics-pipeline factory boundary and
  carry it only into diagnostics and supported Vulkan debug labels.
- [ ] Make the Vulkan manager create a fresh graphics PSO for every request. If
  the remaining class primarily owns structural layout caches, rename or split
  it so its API does not imply logical PSO caching.
- [ ] Preserve descriptor-set-layout and render-pass sharing only where their
  existing full value keys and device-lifetime ownership remain valid.
- [ ] Update compile-time mocks and focused RHI API tests to reflect a
  creation-only graphics-PSO interface.

#### Acceptance Gate

- Two successful requests with the same diagnostic name return distinct PSOs,
  no RHI or Vulkan container retains either PSO after return, and removing the
  last Renderer/command reference makes each object eligible for normal RHI
  deletion.

### Stage 2: Make Vulkan graphics-pipeline creation transactional

- [ ] Refactor `FVulkanGraphicsPipelineState` construction into a factory or
  private committed-object constructor fed by local candidate native handles.
- [ ] Validate required shaders, vertex declaration, render-target layout,
  reflected binding layouts, push constants, and supported fixed-function state
  before publishing any object.
- [ ] Initialize the native pipeline and pipeline-layout candidates to null;
  destroy the pipeline layout immediately if native pipeline creation returns
  non-success or a later pre-commit step fails.
- [ ] Return null for explicit native pipeline-creation failure and never add a
  failed object to an owning container, RHI pending-delete list, or Vulkan
  deferred-deletion queue.
- [ ] Require destructors and bind/push-constant methods to operate only on
  committed valid handles. Remove unused shader-retention members/helpers, or
  replace them with real owned references only if a documented backend lifetime
  requirement is found.
- [ ] Route thrown native creation failures through the single explicitly
  fallible synchronous operation introduced by the recoverable-RHI plan. Do not
  catch device loss, executor failure, or invariant violations as ordinary PSO
  misses.
- [ ] Add deterministic failure injection for pipeline-layout creation and
  graphics-pipeline creation, including cleanup counts and a successful later
  attempt on the same RHI thread.

#### Acceptance Gate

- Every Vulkan graphics-PSO factory result is either null or a wrapper whose
  native pipeline and pipeline layout are valid; all failed candidates clean up
  exactly once, and an expected injected failure neither poisons a later attempt
  nor creates a persistent failed identity.

### Stage 3: Enforce Renderer-owned reload and aggregate lifetime

- [ ] Audit Static Mesh, Sky Box, Post Process, editor-assistance, Texture
  Editor, tests, and any newly discovered callers so every successful PSO is
  retained by a Renderer slot payload or another explicit Renderer owner.
- [ ] Require every factory to validate each returned PSO before candidate
  commit and return an owned `GraphicsPipeline` failure with shader, device, and
  manual retry dependencies as appropriate.
- [ ] Keep stable descriptive `DebugName` values. Remove any generation suffix
  whose only purpose is to defeat backend caching; retain semantic identity
  text such as feature, output variant, topology, depth mode, or slot index when
  it improves diagnostics.
- [ ] Verify multi-PSO candidates release already-created members when a later
  member fails and keep the previous complete aggregate as `StaleReady` where
  the slot contract permits it.
- [ ] Verify successful shader refresh installs distinct PSO and shader
  payloads atomically, while an already-recorded draw can safely finish using
  the old PSO before deferred deletion.
- [ ] Verify slot reset, device invalidation, Renderer shutdown, and RHI shutdown
  drain logical PSO references and native deferred deletions in the required
  order without pending-delete assertions.

#### Acceptance Gate

- Stable-name shader reload draws with the new shader after successful refresh,
  failed refresh still draws the last-known-good payload, unrelated slots remain
  independent, and replaced/reset PSOs are reclaimed after transient command
  references retire.

### Stage 4: Validate end to end and publish the lasting contract

- [ ] Add focused RHI tests for the creation-only public interface and recorded
  command PSO reference lifetime.
- [ ] Add focused Vulkan tests for same-name creation with identical and changed
  immutable inputs, explicit native failure, cleanup, later retry, and shutdown
  with validation layers enabled.
- [ ] Run Renderer slot/cache tests and the stable-name Vulkan reload test in
  both dedicated-thread and `DURIN_RHI_EXECUTION=inline` modes.
- [ ] Run the affected native test targets and a successful full `all` build
  through the root [build and run](../Development/Build/BuildAndRun.md)
  workflow.
- [ ] Run the verified editor from the same Agent Build Profile; smoke Static
  Mesh, Sky Box, Post Process, editor-assistance variants, shader reload
  changed/all, failed refresh, corrected refresh, resize, and orderly shutdown.
- [ ] Update [Shader Cache](../Runtime/Rendering/ShaderCache.md) and
  [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md) to state that
  Renderer slots own logical PSOs, names are diagnostic-only, and the RHI
  publishes complete-or-null candidates.
- [ ] Update the [Compute Shader Pipeline](../Roadmaps/ComputeShaderPipeline.md)
  roadmap so future compute PSOs inherit the same ownership and diagnostic-name
  boundary rather than introducing a parallel named cache.
- [ ] Update the recoverable-RHI plan's graphics-pipeline evidence and both
  plans' stage handoffs when the shared fallible factory boundary is validated.

#### Acceptance Gate

- Focused RHI/Vulkan/Renderer tests, both executor modes, the full build, Vulkan
  validation, stable-name shader reload, resource release, and editor smoke all
  pass; lasting documentation and the compute roadmap describe one consistent
  PSO ownership model.

## Validation Matrix

| Scenario | Required behavior | Evidence |
| --- | --- | --- |
| Same name, identical initializer | Fresh complete PSO; no RHI ownership alias | Focused Vulkan same-name test |
| Same name, changed shader content | Fresh PSO uses new shader content | Stable-name rendered-color reload test |
| Same name, changed vertex/fixed/layout state | Fresh PSO reflects the complete new initializer without a name collision | Parameterized Vulkan creation tests |
| Broken shader refresh | No candidate PSO is published; last-known-good payload remains `StaleReady` | Renderer reload test |
| Corrected shader refresh | Distinct PSO is committed and new output is visible | Renderer reload rendered-color test |
| `vkCreateGraphicsPipelines` non-success | Null RHI result; candidate pipeline layout cleaned; no failed tombstone | Vulkan failure-injection test |
| Native creation exception | Nullable operation failure; RHI executor remains usable | Shared recoverable-RHI executor/factory test |
| Partial multi-PSO candidate failure | New candidate members release; previous complete aggregate remains | Renderer aggregate failure test |
| Slot replacement during queued draw | Queued draw keeps old PSO alive; later draw uses new PSO; old PSO deletes afterward | Recorded-command lifetime integration test |
| Slot reset or Renderer shutdown | No backend strong reference; RHI and Vulkan deferred deletion drain cleanly | Lifetime counters plus shutdown test |
| Device invalidation | Old-device PSOs are not retained as last-known-good | Renderer device-generation test |
| Same name across graphics and future compute types | Name has no identity role and cannot collide | Compute roadmap contract; future compute test |

## Definition of Done

- The name-keyed graphics PSO ownership cache and public name lookup no longer
  exist.
- `DebugName` is demonstrably diagnostic-only, and correctness does not depend
  on generation-suffixed or globally unique names.
- Renderer slots and explicit Renderer owners control logical PSO publication,
  last-known-good retention, replacement, device invalidation, and release.
- Every Vulkan graphics-pipeline creation publishes a complete valid object or
  null, with deterministic rollback of partial native state.
- Expected graphics-pipeline creation failures use the shared recoverable RHI
  boundary; executor/device/invariant failures retain their terminal semantics.
- Stable-name reload, complete-descriptor variation, aggregate rollback,
  command lifetime, deletion, both executor modes, full build, validation-layer
  runtime, and editor smoke evidence pass.
- Long-lived rendering documentation and the compute roadmap state the final
  ownership and identity rules, and both active plans record consistent stage
  evidence.

## Deferred Follow-ups

- A measured, descriptor-keyed in-memory PSO reuse cache with bounded ownership,
  eviction, concurrent creation, and full collision-safe equality.
- Driver `VkPipelineCache` persistence, compatibility/versioning, corruption
  handling, warmup metrics, and offline or runtime precaching.
- Asynchronous PSO compilation and non-blocking draw fallback policy.
- Shared immutable graphics/compute pipeline descriptors after the compute
  pipeline requirements are concrete.
- Debug tooling that enumerates live Renderer PSO owners, descriptor hashes,
  native creation time, and cache/prewarm misses.

## Related Documentation

- [Recoverable RHI Creation Failures](RecoverableRHICreationFailures.md)
- [Shader Cache](../Runtime/Rendering/ShaderCache.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Shader Parameters](../Runtime/Rendering/ShaderParameters.md)
- [Compute Shader Pipeline](../Roadmaps/ComputeShaderPipeline.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/RHI/Public/DynamicRHI.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/RHI/Private/RHIResources.cpp`
- `Engine/Source/Runtime/VulkanRHI/Public/VulkanDynamicRHI.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp`
- `Engine/Source/Runtime/RenderCore/Public/RenderResourceCreation.h`
- `Engine/Source/Runtime/Renderer/Private/RendererResourceSlotCache.h`
- `Engine/Source/Runtime/Renderer/Private/Resources/RendererResourceCoordinator.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererResourceReloadVulkanTests.cpp`
