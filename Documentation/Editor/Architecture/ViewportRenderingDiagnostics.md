# Viewport Rendering Diagnostics

Summary: Define Level Editor render controls, the viewport statistics summary, and sampled Render Graph inspection.

Modules: LevelEditor, Engine, RenderCore, Renderer

Last reviewed: 2026-09-03

## Ownership

The Level Editor presents per-viewport policy and immutable completed results.
Statistics publication, viewport isolation, capture revisions, and failure
availability belong to [Viewport Rendering](../../Runtime/Rendering/ViewportRendering.md#viewport-rendering-statistics).
Graph capture contents belong to [Render Graph](../../Runtime/Rendering/RenderGraph.md#diagnostics-and-budgets).
The UI never changes rendering from telemetry or waits for GPU completion.

## Render Controls

The Level Editor View menu groups controls by feature ownership. Features with
subordinate quality or route policy own one submenu containing their boolean `Enabled`
checkbox and mutually exclusive radio choices. Independent visibility toggles
such as grid and collision remain checkboxes. Instantaneous commands use plain
actions. Checkbox and radio controls do not close their popup hierarchy,
allowing repeated A/B changes without reopening the menu.

`Shadows > Directional Shadows` groups shadow controls. `Filter Quality`
contains the PCF tier, while `Contact Shadows` contains an `Enabled` checkbox
and a `Visibility Route` selector. `Auto` preserves compute-first production fallback,
`Compute Only` suppresses fragment fallback, and `Fragment Only` bypasses
compute. Its mutually exclusive `Debug Views > Contact Shadow Contribution`
mode enables the pass and displays the computed contribution as a red mask;
selecting another diagnostic clears that mode. `Debug Views > Reset Debug
Views` restores normal rendering and clears every shadow diagnostic mode.

`Post Processing > GTAO` contains an `Enabled` checkbox and mutually exclusive
half/full-resolution quality choices.

## Statistics and Inspection

The Level Editor FPS badge is the statistics entry point. Activating the badge
toggles a compact frame summary right-aligned directly below it. The summary is
limited to frame interval, visibility, triangles, and draw calls; its `Details...`
action opens the independently dockable Rendering Diagnostics panel. The full
badge/panel rectangle is excluded from drag/drop, selection, gizmo editing,
camera navigation, wheel input, and embedded-PIE capture before those paths
evaluate the viewport. The summary is suppressed when its minimum readable
size cannot fit inside the viewport, while the FPS badge remains available.
Expansion is an editor session preference under `SceneViewport.ShowStatistics`;
it defaults to collapsed and never dirties level or asset packages.

Rendering Diagnostics separates Overview, Scene, and Render Graph inspection.
Overview separates the smoothed wall-clock frame interval into game-thread work
and the measured end-of-frame render synchronization wait. The latter is a
pacing boundary that may include render-thread, RHI, GPU, Present, or VSync
backlog; it is not presented as pure VSync time. Overview also reports
graph-budget values. The three frame-timing values publish one synchronized
snapshot every half second while their underlying accumulators continue to
sample every frame. Scene owns feature breakdowns, and Render Graph provides
pass filtering, pass/resource inspection, dependency visualization, resource
lifetimes, and transition counts. Pass filtering compacts the graph to matches
plus their direct dependency context. Hovering or selecting a pass focuses its
incoming and outgoing edges by default, with an opt-out for whole-graph
inspection; dependency tooltips identify value, execution, and explicit edges
and their captured resource cause. The panel is optional in the workspace and
is also available from the Level Editor Panels menu.

Opening the panel without a capture requests the next frame once. Later captures
occur only through `Capture next frame`; ordinary panel drawing does not request
a fresh graph every frame. Failed requested renders show unavailable capture
state rather than presenting an older capture as the requested frame.

## Related Documentation

- [Scene viewport navigation](../Guides/SceneViewportNavigation.md)
- [Volumetric cloud authoring](VolumetricCloudAuthoring.md)
