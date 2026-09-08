# Texture Resource Update Refactor Plan

Summary: Replace texture completion revisions with one owned resource update per asset, ordered publication, and explicit fallback and teardown across Texture2D, TextureCube, and VolumeTexture.

Last reviewed: 2026-09-08

Status: Active
Completed:

## Current Status

Planning only; runtime implementation has not started. The user authorized a
plan-first handoff because this change crosses resource ownership, render-thread
publication, editor invalidation, and destruction. Existing code still uses
`FTextureResourceCompletion` and revision comparisons.

Inspected `DTexture`, all three resource initialization paths, RenderCore texture
reference release, and editor revision consumers. Concrete initialization calls
currently create/upload/publish within `InitRHI`; Stage 0 must verify the complete
execution and completion boundary before removing synchronization. The current
`UpdateResource` queues retirement of the previous resource immediately after
candidate initialization is queued. The Texture System contract instead describes
retirement after successful publication. Preserve the documented last-successful
fallback and characterize this discrepancy before migration.

Local reference inspected: UE 5.8 `Texture.cpp`, `Texture2D.cpp`, and
`StreamableRenderAsset.cpp` under the installed engine. UE waits for pending
initialization/streaming before replacement, orders deletion and initialization
on the render thread, and uses `PendingUpdate` for streaming. This plan retains
Durin's asynchronous ordinary replacement rather than importing UE's blocking
wait into every update.

## Goal

Remove the texture completion/revision protocol through real operation ownership.
Each asset has one executing update, at most one coalesced subsequent input, and
a last-successful render resource. An update owns its immutable input, candidate,
terminal result, and cleanup responsibility. A generic Task may execute work but
is not the public resource lifetime or GPU-readiness contract.

## Selected Design

- GameThread admits updates and owns the asset-facing current resource and
  `PendingUpdate`. RenderThread initializes candidates and switches the stable
  texture reference. Neither thread reads the other's mutable fields without an
  explicit synchronized handoff.
- Keep one executing `FTextureResourceUpdate` per texture. Requests arriving
  during execution replace one retained, immutable next-input snapshot. Do not
  queue an unbounded list or retain a live UObject for worker reads. After the
  active operation is consumed, start the latest queued input automatically.
- An already accepted update may publish before its successor; intermediate
  results are allowed. This is a deliberate change from skipping superseded
  revisions. No numeric generation or pointer-token comparison should recreate
  the old latest-request-wins protocol.
- Build a complete candidate before publishing. Success switches the stable RHI
  reference and retires the previous concrete resource; failure retires only the
  candidate and leaves the last-successful resource usable. Availability and
  update progress are distinct: a texture can be usable while updating or after
  a failed replacement.
- Keep `FTextureReference`, RenderCore command ordering, and deferred C++/RHI
  destruction. `ResetToFallbackIfMatches_RenderThread` already protects a newer
  referenced allocation during old-resource release; this allocation ownership
  check is not a request generation and should remain.
- Task completion is not GPU completion. Define the terminal handoff after all
  required publication and CPU ownership actions are ordered; retain upload data
  for the actual RHI consumption lifetime. Continue using existing RHI retirement.
- Teardown stops admission, discards the queued successor, and synchronizes with
  publication so no publication can occur after the close boundary. Accepted
  operations must finish cleanup before the stable reference and owner storage
  disappear. A mere unsynchronized cancellation flag is insufficient.
- Keep existing failure categories where meaningful. Store the latest completed
  error on the asset after consuming an update; resource release must not clear
  or overwrite the result of another operation.
- Do not change source/build/DDC keys, cooked formats, asset compilation serials,
  or generic RenderCore/RHI scheduling. Coordinate with the active
  [RHI Resource Creation Refactor](RHIResourceCreationRefactor.md) before changing
  any shared factory or command-completion assumption.

## Removal and Consumer Migration

Remove `FTextureResourceCompletion`, texture `BuildRevision`, requested/applied/
failed/state revisions, `ReleaseRevision`, `PrepareForRelease`, revision-bearing
constructors, `GetBuildRevision`, and `GetAppliedRenderRevision`. Simplify or remove
`FTextureAssetResource` if only an empty compatibility layer would remain.

Replace the combined `ERenderResourceState` query with explicit resource
availability and update progress/result queries. These queries remain read-only;
calling an editor getter must not be necessary to advance an update. Do not retain
unused compatibility methods or revision counters after the final migration.

Texture Editor preview invalidation, Volume Texture Editor preview invalidation,
Cube thumbnail acceptance, Material thumbnail dependencies, and payload inspection
must migrate in the same work. Use existing asset/content notifications for content
changes and explicit update completion for readiness/retry. Thumbnail operations
retain the input/resource snapshot they actually render; an unchanged stable
texture-reference pointer or CacheKey alone cannot establish snapshot freshness.
Do not replace the removed render revision with a hidden thumbnail generation on
`DTexture`. Stage 0 must select the exact consumer notification/acceptance seam.

## Implementation Stages

### Stage 0: Freeze ownership and completion boundaries

- [ ] Trace command admission, upload data consumption, publication, deferred
  cleanup, and shutdown for all three families, including inline/no-RHI paths.
- [ ] Select and document a concrete GameThread completion dispatch or pump that
  advances operations without rendering, editor polling, or a second update call.
  Audit callback owner lifetime, shutdown drain, and command rejection handling.
- [ ] Write the ownership table for idle, executing, published-but-not-consumed,
  failed, successor-queued, and closing states. Specify the single transfer point
  for each candidate/current resource and the publication/close synchronization.
- [ ] Inventory all production and test revision users and select the exact
  editor/thumbnail invalidation replacement, including delayed thumbnail results.
- [ ] Characterize failed replacement and delayed old-resource release; resolve
  the implementation/documentation discrepancy against last-successful fallback.

Completion: one executable transition design with no unresolved owner, completion
pump, publication cancellation, or editor invalidation decision. Record any change
to the selected design before implementing it.

### Stage 1: Migrate the common owner and all texture resources

Depends on Stage 0.

- [ ] Implement the owned update operation, bounded successor coalescing, explicit
  result handoff, and automatic progress using the selected existing dispatch seam.
- [ ] Migrate `DTexture` and Texture2D/Cube/Volume candidates together. Keep immutable
  family platform snapshots and preserve descriptor support/failure diagnostics.
- [ ] Publish only complete candidates; retain the previous usable resource on
  failure. Release every displaced/cancelled candidate exactly once.
- [ ] Implement close/admission rejection and ordered reference/resource cleanup,
  including destruction before execution and between publication and consumption.
- [ ] Migrate production status and editor/thumbnail consumers in the same buildable
  change; remove completion/revision fields, APIs, constructor parameters, and uses.
- [ ] Replace revision-specific fixtures with deterministic lifecycle behavior tests.

Completion: all three families and production consumers compile with no legacy
texture revision protocol; focused CPU lifecycle tests pass.

### Stage 2: Validate integrations and publish the implemented contract

Depends on Stage 1.

- [ ] Test initial success/failure, failed replacement retaining the old allocation,
  explicit retry, and success after failure for all three families.
- [ ] Test A executing with B/C queued: bounded storage, at most one executing
  operation, only C follows A, and eventual progress without getter side effects.
- [ ] Test source replacement during upload, late old-resource release, close at
  each handoff boundary, stopped command admission, absent RHI, and producer shutdown.
- [ ] Test editor preview refresh and delayed Cube/Material thumbnail acceptance
  without relying on render revisions or stable-reference pointer changes.
- [ ] Run affected native validation and relevant existing texture/thumbnail/Vulkan
  integration selections under the repository test workflow. Validate real reference
  switching and retirement; CPU Task completion alone is not acceptance evidence.
- [ ] Complete the full build required for changed editor consumers, and record
  native test receipts, any unavailable integration lane, and unrelated failures.
- [ ] Update Texture System and relevant lifecycle/recovery contracts, remove stale
  revision descriptions, validate documentation, and close this plan only after
  required gates pass.

Completion: behavior and integration gates pass, no removed texture API remains,
and implemented documentation describes the actual ownership/publication protocol.

## Validation and Handoff

Follow [Agent Build And Run](../Agents/BuildAndRun.md) and
[Agent Testing](../Agents/Testing.md); discover registered targets rather than
inferring them from source directories. Use controlled scheduling/failure injection
instead of sleeps to prove publication and teardown behavior. No performance or
application-hosted qualification is added implicitly.

Prior unrelated validation evidence: the preceding texture key change passed all
93 TextureTests cases; affected validation passed 81/82 targets.
`CoreConcurrencyTests` case
`FTaskAcceptanceTests.CheckedParallelForPartialSubmissionDrainsAcceptedCaptures`
failed both in the aggregate and in isolation on 2026-09-08. Preserve that diagnostic
if it recurs; it is not evidence against or acceptance for this unimplemented plan.

Each implementation commit updates plan status/checklists and uses exact Plan and
Stage trailers required by the repository. This planning commit is not completion
of Stage 0 or validation of runtime changes.

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/Texture.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureRenderResource.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureRenderResource.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DRenderResource.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCubeRenderResource.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/VolumeTextureRenderResource.cpp`
- `Engine/Source/Runtime/RenderCore/Private/RenderResource.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MTextureEditor.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Widgets/MVolumeTextureEditor.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Thumbnail/TextureCubeThumbnailRenderer.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Thumbnail/MaterialThumbnailRenderer.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureFailureTests.cpp`
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Render Resource Lifecycle](../Runtime/Rendering/RenderResourceLifecycle.md)
- [Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md)
