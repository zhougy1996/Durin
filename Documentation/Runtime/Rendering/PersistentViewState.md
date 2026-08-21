# Persistent View State

Summary: Define optional renderer-owned identity, previous-view metadata, discontinuity, and transactional history lifetime for logical view streams.

Modules: RenderCore, Renderer, Engine

Last reviewed: 2026-08-21

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

Every `RenderView_RenderThread` attempt advances a renderer-owned saturating
`uint64` submission serial. The serial does not depend on game ticks,
`GFrameCounter`, wall time, swapchain images, or output identity. A backwards or
wrapped serial is treated as an expired gap rather than accepting ambiguous
history. The inactive threshold is 120 renderer submissions.

After `FitViewToOutput`, Renderer prepares one immutable temporal context with:

- current and previous final view, projection, and view-projection matrices;
- camera location, fitted viewport rectangle, and output extent;
- depth convention and scene identity;
- current and previous submission serials, submission gap, and the count of
  successful commits; and
- a discontinuity bitmask plus explicit previous/history validity.

Feature renderers consume the prepared context. They do not query the registry.
Internal shadow and feature views never begin or commit the outer view state.

## Transaction and Reset Rules

`Begin` prepares a pending candidate without changing committed metadata.
Only a complete outer `ERenderViewResult::Success`, including post-process and
presentation recording, calls `Commit`. Every early return or failed pass calls
`Abort`, discards pending feature candidates, and retains last-known-good
metadata and feature history.

The context reports first use, explicit camera cut, scene change, output extent
change, fitted viewport change, projection change, depth-convention change,
inactive-gap expiry, device invalidation, manual invalidation, missing state,
and duplicate/interleaved use. Any reported discontinuity makes history invalid
for the submission. Projection and extent changes are distinct so a later
feature may document a resampling policy; the foundation defaults both to
invalid history.

Manual invalidation, device invalidation, and explicit cuts persist across
failed attempts and clear only after a successful replacement commit. Device
invalidation releases typed GPU history while preserving the public ID.
Disabled consumers publish no candidate and retain their last-known-good
feature history until a discontinuity, device reset, owner release, or shutdown.

## Engine Integration

`FSceneViewport` creates its owner through the active Renderer module, attaches
the ID to Engine-built views, and is retained by the queued render command.
Initialization starts with a cut. World replacement and explicit
`RequestHistoryReset()` calls request another cut; camera possession, teleport,
focus, and cinematic lifecycle owners call that method when they reject
continuity. The request is consumed by the next buildable submission and then
persists inside Renderer until a successful commit.

Owner creation failure leaves the viewport stateless. Resize, output fitting,
and scene changes are also detected from the last successful fitted metadata,
so no failed frame advances or silently erases continuity.

## Related Documentation

- [Viewport rendering](ViewportRendering.md)
- [Render resource lifecycle](RenderResourceLifecycle.md)
- [Renderer resource recovery](RendererResourceRecovery.md)
- [Runtime lifecycle](../Core/RuntimeLifecycle.md)
