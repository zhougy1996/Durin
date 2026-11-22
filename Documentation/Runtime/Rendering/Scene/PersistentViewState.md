# Persistent View State

Summary: Define optional renderer-owned identity, previous-view metadata, discontinuity, and feature-owned history lifetime for logical view streams.

Modules: RenderCore, Renderer, Engine

Last reviewed: 2026-11-22

## Public Boundary and Ownership

`FSceneViewStateId` is an opaque, process-unique identity. Its default value is
invalid and means that the submission is stateless. IDs are allocated
monotonically and are not reused or inferred from scenes, cameras, textures,
matrices, dimensions, or frame counters.

`IRendererModule::CreateViewState()` returns a move-only
`FSceneViewStateOwner`. The owner exposes only its ID and a renderer-provided
release policy. Destruction queues state removal; callers must retain the owner
until every submission carrying its ID has executed. Missing, invalid,
released, or foreign IDs never create state implicitly and render through the
unchanged stateless path.

One owner represents one logical stream. Main and auxiliary
`FSceneViewport`s each own a separate token. Thumbnails, picking helpers,
preview renderers, qualification fixtures, and other direct `RenderView`
callers remain stateless unless they explicitly create and retain an owner.

Engine shutdown releases viewport owners before the Renderer module is
destroyed. Renderer shutdown flushes queued removals, reports leaked live
states, and destroys any remainder on the rendering thread.

## Render-Thread State

The concrete registry and `FSceneViewState` live in Renderer. Lookup, mutation,
reset, feature history access, and destruction occur only on the rendering
thread. RenderCore contains no previous matrices and no feature resource cache.
Future features extend `FSceneViewState` with strongly typed private members;
they do not add a string-keyed, type-erased public cache.

History age uses `GRenderFrameCounterRenderThread`, not the number of view
submissions. Other viewports, captures, and retries in the same render frame do
not age a view's history. Cloud history rejects a gap above 120 render frames or a backwards counter.

After `FitViewToOutput`, Renderer begins a cloud-local candidate with fitted
projection and view-projection matrices, viewport/output extents, depth
convention, and scene identity. The history object compares this compatibility
metadata against its last publication. Reconstruction receives only the previous
view-projection matrix and current output size, plus validity, render-frame age,
discontinuities, and the cloud's successful reconstruction sequence.
Resource preparation only reads that sequence; it does not begin a candidate.

## Feature Publication and Reset Rules

`FVolumetricCloudViewHistory` owns its texture and matching camera metadata.
`Begin` does not change published state. Cloud reconstruction publishes both
after successfully recording its commands. Later cloud composition, translucency,
post-process, or presentation failure does not revoke that publication.
The outer scope calls `Abort` only to recycle unfinished candidates. A candidate
with no texture or explicit clear cannot advance camera metadata alone.

Cloud history rejects first use, explicit camera cuts, scene/output/viewport/
projection/depth changes, inactivity, device/manual invalidation, and interleaved
use. Projection and extent checks remain conservative because the current cloud
algorithm does not define a history-resampling policy. These are cloud
compatibility rules, not a universal validity requirement for future features.

Manual/device invalidation releases cloud GPU history while preserving the
public ID. Explicit cuts and hard-reset signals persist until the feature
publishes a replacement or clear. Disabled consumers publish no candidate; age
and compatibility are rechecked when they next run. Future temporal features
own their own paired resources, metadata, compatibility, and publication point.

## Main-view occlusion history

Occlusion history is enabled by default for eligible persistent main views. Set
`FSceneViewRenderOptions::bEnableOcclusionHistory` to false for frustum-only
admission. The persistent view's `FHZBOcclusionTester` assigns each frustum candidate a test index by
primitive component ID. A changed batch projects its bounds conservatively and
uploads one immutable GPU buffer.
After opaque GBuffer depth is available, the GPU reduces 16×16 tiles and tests
each expanded bound against all covered tiles. Each candidate gets one 32-bit
result. Before mesh collection, the view maps the newest ready result from the
previous two render frames that survived view and scene invalidation. Traversal
order may change. A matching index is culled only for a confirmed occluded value. Shadow caster selection remains independent.

Each view has three feedback slots. A slot supports up to 4096×4096 pixels and
4096 candidates (16 KiB of readback). Going over either limit leaves frustum
admission in effect for the whole view. A full ring skips submission. Missing
or incompatible history leaves each candidate visible. Reuse requires matching
projection, viewport, depth convention, scene, candidate identities, resource
generation and the scene's occlusion revision. A view ignores previous results after more
than eight seconds idle, a time reset, or a camera step exceeding 45 degrees on
any view axis or 10,000 world units of translation. Smaller camera steps submit
fresh projected bounds. Consumption applies the same thresholds against each
ready result's producing view, including when newer feedback is still pending.
Newly exposed objects can still be briefly hidden within those thresholds.
An origin-only rebase also resets history.
An explicit cut takes effect when the view is submitted, even if output validation
rejects the draw.
Switching device generations drops the entire feedback ring.

The scene maintains each primitive's occlusion eligibility and world-space
occlusion bounds. Eligibility comes from the proxy's `CanBeOccluded()` and
is refreshed before visibility when scene publications make it dirty. The
ordinary static mesh proxy checks opaque depth eligibility only for slots
referenced by published LOD sections. An unsupported candidate keeps the
whole view on frustum admission because unsupported geometry can contribute
depth that changes independently. The occlusion tester consumes the scene's
primitive identity, eligibility and bounds. It does not capture mesh batches,
geometry records, materials or instance publications.

Scene membership, visibility, transforms and instance updates advance the
occlusion revision when depth coverage may change. Referenced material proxies
and static mesh render data notify attached scenes through the same occlusion
invalidation boundary. This includes in-place program or parameter changes,
mesh resource initialization and release, and published LOD readiness.
Occlusion invalidation is independent of HitProxy revisions. Explicit static
mesh instances use the entire primitive's bounds. A partially exposed group
continues to per-instance processing.
Direct draws, CPU merging, GPU compaction and its fallback paths all consume
the same surviving main-view candidates.

## Engine Integration

`FSceneViewport` creates its owner through the active Renderer module, attaches
the ID to Engine-built views, and is retained by the queued render command.
Initialization starts with a cut. World replacement and explicit
`RequestHistoryReset()` calls request another cut; camera possession, teleport,
focus, and cinematic lifecycle owners call that method when they reject
continuity. The request is consumed by the next buildable submission and then
persists inside the feature until a successful history publication.

Owner creation failure leaves the viewport stateless. Resize, output fitting,
and scene changes are also detected from the metadata paired with the last published feature result,
so failed reconstruction cannot advance its camera independently.

## Related Documentation

- [Viewport rendering](../Pipeline/ViewportRendering.md)
- [Render resource lifecycle](../Pipeline/RenderResourceLifecycle.md)
- [Renderer resource recovery](../Pipeline/RendererResourceRecovery.md)
- [Runtime lifecycle](../../Application/RuntimeLifecycle.md)
