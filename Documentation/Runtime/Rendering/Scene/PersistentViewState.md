# Persistent View State

Summary: Define optional renderer-owned identity, previous-view metadata, discontinuity, and feature-owned history lifetime for logical view streams.

Modules: RenderCore, Renderer, Engine

Last reviewed: 2026-10-29

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
not age a view's history. A gap above 120 render frames, or a backwards counter,
rejects reuse.

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
