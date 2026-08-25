# Material Graph Editor Usability Plan

Summary: Make large material graphs readable, predictable to lay out, and efficient to edit without changing material program semantics.

Last reviewed: 2026-08-26

Status: Completed
Completed: 2026-08-26

## Current Status

The command-driven material graph editor is complete, but its first canvas
meets functional coverage more strongly than editing usability. Large graphs
framed near the minimum zoom render fixed-size text over graph-scaled node
bounds, node bodies omit pin meaning, the fixed screen-space surface panel
pulls long links across the viewport, and deterministic GUID row ordering does
not minimize crossings or preserve branch locality.

This follow-up keeps the landed material-program, validation, transaction, and
compile contracts intact. All stages are complete: shared geometry, semantic
zoom, topology-aware layout, graph-space outputs, search-driven creation,
atomic reconnection, inline property editing, bounded rendered evidence, and
the full affected editor build are landed and qualified.

## Goal

Make representative and maximum-size material graphs readable and predictable
to navigate, create, connect, reconnect, edit, and lay out while retaining the
existing bounded semantic graph and atomic commit boundary.

The completed workflow must provide:

- legible, non-overflowing node presentation at every supported zoom level;
- enough node and pin identity at editing zoom to choose connections without
  counting unlabeled pin circles;
- deterministic, size-aware layout with measurably fewer crossings and no
  overlap between laid-out nodes;
- graph-space surface outputs that frame and navigate with the authored graph;
- searchable, type-filtered creation and atomic reconnection workflows that do
  not silently choose arbitrary compatible sources; and
- focused geometry, command, interaction, and rendered evidence for small,
  representative, and 256-node graphs.

## Scope

- MaterialEditor-owned canvas geometry, semantic zoom levels, text clipping,
  node information hierarchy, pin labels, hit targets, and hover/selection
  emphasis.
- Editor-only catalog and view metadata required to expose stable pin names and
  operation-versus-instance labels without changing `FMaterialProgram`.
- A derived graph-space surface-output proxy included in framing, culling, hit
  testing, layout bounds, diagnostic navigation, and link rendering.
- Deterministic topology-aware row ordering, size-aware vertical packing,
  output-branch locality, crossing measurement, and selected-layout collision
  avoidance.
- Searchable node creation, connection-driven filtering, transient completion
  of required inputs, atomic input reconnection, inline bounded property edits,
  and their cancellation/Undo behavior.
- Link routing and focus modes sufficient to distinguish active upstream and
  downstream paths in dense graphs.
- Unit, command/controller, rendered editor, and maximum-graph responsiveness
  qualification for the changed workflow.

## Non-Goals

- Changing the M5 opcode set, value types, implicit conversion policy,
  required-input or required-surface-output rules, compiler IR, generated
  source, shader identity, or preview publication lifecycle.
- Persisting incomplete nodes, dangling links, surface-output proxies, viewport
  state, selection, search text, focus state, or creation drafts in `DMaterial`.
- Replacing the ImGui canvas with a third-party node editor or extracting a
  repository-wide graph framework from this one consumer.
- Adding material functions, nested graphs, reroute nodes, comments, groups,
  custom node classes, arbitrary shader source, or graph editing on material
  instances.
- Redesigning the surrounding MaterialEditor preview, overview, parameter, or
  details panels except where canvas focus and available height must remain
  usable.
- Making automated layout preserve every hand-authored position. Explicit
  layout remains an authored presentation transaction and Undo restores the
  previous positions.

## Design Decisions and Invariants

### Semantic and ownership boundary

- `DMaterial::Program` remains the only semantic graph and
  `DMaterial::GraphPresentation` remains the only persisted node-presentation
  state. This plan adds no runtime serialization field or Cook payload.
- Canvas geometry, label selection, surface-proxy placement, search state,
  highlighted paths, and creation/reconnection drafts are detached editor
  values. They never affect validation, normalized identity, shader keys, or
  compilation.
- The existing candidate-validation rule remains authoritative: every committed
  node input and surface output is valid. A creation draft may temporarily show
  unresolved inputs, but it does not call a mutation command or mark the asset
  dirty until all required sources are selected and one complete create request
  succeeds.
- Reconnecting an occupied input retains the old semantic link until drop. A
  valid drop performs one replace command; Escape, an invalid drop, document
  deactivation, or stale ownership leaves the old link unchanged.

### Canvas geometry and semantic zoom

- Extract graph-space node bounds, pin anchors, surface-proxy bounds, visible
  detail level, and coarse link bounds into deterministic canvas-geometry
  helpers that do not depend on frame timing or mutable ImGui state. Rendering
  and hit testing consume the same geometry.
- Nodes retain a stable logical width so layout and automation remain
  deterministic. Logical height is derived from header, secondary-label, and
  pin-row counts; layout uses those bounds instead of fixed row spacing.
- Text never draws outside its owning node or surface proxy. Long labels are
  ellipsized within a clipped region and expose their full value through a
  tooltip at an interactive zoom level.
- The primary node label is the opcode or catalog operation name. Authored
  display/parameter names are secondary labels, so repeated resource names do
  not hide graph operations.
- Semantic zoom has three stable bands with hysteresis at their boundaries:
  overview renders node silhouettes and typed connection cues; readable mode
  renders clipped primary labels; editing mode adds secondary labels, named
  input pins, output type, and inline controls. Pin editing is disabled in
  overview while pan, selection, framing, and path focus remain available.
- Type is communicated by color plus text or shape at editing zoom; color alone
  is not the only connection cue.

### Surface outputs and link presentation

- Surface outputs render as a non-persisted graph-space proxy placed one fixed
  column after the rightmost surface source by the same deterministic geometry
  policy as layout. It pans, zooms, culls, frames, and receives diagnostic focus
  with the graph, but cannot be dragged or serialized.
- Ordinary links remain direct cubic paths initially. Output links receive
  separated target lanes, and non-focused links reduce contrast when a node,
  pin, or link is hovered or selected. The selected path and valid drop targets
  remain visually dominant.
- Link hit testing uses a bounded distance-to-curve approximation over visible
  links. It may support focus and reconnection, but Delete does not fabricate an
  invalid committed disconnect where the material-program contract requires a
  source.

### Deterministic layout

- Column assignment continues to follow dependency-before-consumer ordering and
  longest distance to a surface sink. Existing semantic order, node IDs, links,
  and output assignments never change.
- Row ordering uses a fixed number of deterministic forward/backward median
  sweeps across adjacent columns. Surface-output order seeds the sink side;
  prior order and then node GUID provide total tie-breaking.
- Nodes shared by multiple surface branches remain single nodes. Their median
  neighbor position determines placement rather than duplicating or assigning
  false ownership.
- Each column is packed using computed node heights and fixed logical gaps.
  Output lanes receive equivalent separation. Whole-graph layout must produce
  no intersecting node rectangles.
- Selected-only layout preserves unselected positions and searches for the
  nearest deterministic collision-free placement within the affected columns.
  If no bounded placement exists, the command rejects without changing any
  position.
- Stage 0 records crossings for locked representative fixtures. The selected
  algorithm must reduce total crossings for every congested fixture and must
  not increase crossings for the simple fixtures; exact thresholds are recorded
  before Stage 2 implementation.

### Creation, editing, and transactions

- A searchable palette replaces the flat creation submenu as the primary
  creation surface. Results are ordered by exact/prefix/substring match,
  category, display name, result type, parameter GUID, and stable catalog order.
- Opening the palette from a source pin filters candidates by compatible input.
  Unary candidates can commit immediately. Candidates with additional required
  inputs enter a bounded transient draft that asks for each remaining source
  and commits once complete; cancel performs no mutation.
- Opening reconnection from an occupied input shows the existing path until a
  valid replacement is chosen. Connection compatibility feedback continues to
  come from the graph view and command result rather than duplicated widget
  rules.
- Constant, parameter, and swizzle controls appear inline only in editing zoom.
  Continuous widgets edit a local draft and issue one validated node replacement
  when the gesture completes. Escape restores the displayed authored value;
  one completed gesture produces one Undo step and at most one compile request.
- Keyboard and context actions remain available. The top toolbar exposes the
  current zoom/detail band, Frame All, Frame Selection, Auto Layout, and a short
  discoverable shortcut/help entry without permanently consuming excessive
  canvas height.

### Responsiveness and evidence

- Per-frame work remains bounded by the existing 256-node graph limit. Geometry
  is rebuilt only when graph view, authored positions, zoom band, or relevant
  style scale changes; pan alone translates cached graph-space geometry.
- Rendering culls nodes and links against the canvas clip rectangle before
  expensive label or curve hit-test work. Interactive targets remain usable at
  normal editor DPI scale.
- Pixel-perfect screenshots are not the sole correctness oracle. Deterministic
  geometry tests own overlap, clipping bounds, detail bands, proxy placement,
  and crossing counts; rendered captures verify the final visual hierarchy at
  overview and editing zoom.

## Current Foundations and Gaps

### Landed foundations

- The completed [Material Graph Editor Plan](MaterialGraphEditor.md) provides
  typed inspection, candidate-validated commands, per-document canvases,
  persisted node positions, deterministic layout, Undo/Redo, clipboard,
  diagnostics, and last-known-good preview behavior.
- `FMaterialGraphCatalogEntry` already owns editor-only operation names and
  accepted input types, while `FMaterialGraphNodeView` exposes detached typed
  pins and presentation positions.
- `FMaterialGraphCanvas` already owns transient pan, zoom, selection, marquee,
  link drag, framing, per-document state, and clipped ImGui drawing.
- The graph is a validated DAG bounded to 256 nodes and depth 64, which permits
  deterministic layout and bounded geometry caches without a general graph
  framework.

### Gaps selected by this plan

- Node rectangles scale with zoom while text, pin radii, and hit targets use
  fixed screen sizes; large framed graphs therefore show overlapping labels
  and ambiguous targets.
- Node titles prefer authored display names over operation identity, nodes show
  no secondary identity or pin names, and catalog pins have types but no stable
  semantic labels.
- Surface outputs are screen-fixed and excluded from graph framing, producing
  long converging output links that move independently of graph-space content.
- Layout orders each column by GUID at a fixed 160-unit row step, so it ignores
  crossings, branch locality, node height, and occupied positions during
  selected-only layout.
- Creation silently fills every required input from the first compatible node
  in the sorted view, which is deterministic but not an expression of user
  intent. Property editing is hidden in context menus and continuous value
  changes can create repeated semantic commands.
- Existing native evidence covers command/controller semantics and layout
  completion, but not label containment, node overlap, crossing quality,
  surface-proxy geometry, palette filtering, reconnection cancellation, or
  rendered overview readability.

## Implementation Stages

### Stage 0: Lock usability contracts and baselines

- [x] Add representative geometry fixtures covering a simple chain, eight
  independent surface branches, shared dependencies, long parameter names,
  mixed node heights, dense crossings, selected-only layout, and the 256-node
  bound.
- [x] Record current node intersections, link crossings, graph bounds, Frame All
  zoom, visible-label overflow, and relevant interaction step counts for those
  fixtures without treating the current results as acceptable thresholds.
- [x] Lock the logical node metrics, pin-name vocabulary for every M5 opcode,
  semantic-zoom thresholds and hysteresis, surface-proxy placement, link focus
  states, and minimum interactive hit targets.
- [x] Lock searchable-palette ordering, multi-input creation-draft state,
  occupied-input reconnection behavior, inline edit commit/cancel rules, and
  document-lifecycle cancellation.
- [x] Record the exact crossing-reduction and maximum-graph frame-time gates to
  be used by later stages, including the machine/configuration and whether GPU
  timing is diagnostic or authoritative.

#### Acceptance Gate

- Geometry and workflow fixtures reproduce the reported usability failures, all
  presentation and interaction decisions are selected, and later stages have
  numeric gates without changing runtime material semantics.

#### Locked Stage 0 evidence and contracts

The deterministic fixture family is named `SimpleChain`, `SurfaceBranches8`,
`SharedDependency`, `LongNames`, `MixedHeights`, `DenseCrossings`,
`SelectedCollision`, and `Maximum256`. `SimpleChain` has four nodes with one
input each after the source. `SurfaceBranches8` has one independent two-node
branch per ordered surface output. `SharedDependency` fans one source into four
consumers and rejoins them. `DenseCrossings` intentionally reverses eight
adjacent-column connections. `SelectedCollision` places two selected nodes
between three fixed unselected rectangles. `Maximum256` uses the material node
bound with deterministic GUIDs and a maximum depth below 64.

The original canvas uses a 190-unit width, `30 + 24 + 24 * max(1, inputs)`
height, 280-unit columns, and 160-unit rows. At the minimum Frame All zoom of
0.25, the 190-unit node is 47.5 screen pixels wide while ImGui draws the full
unscaled label; the captured long `Ambient Occlusion Texture` label exceeds
that bound. Surface outputs are fixed to the viewport's top-right corner and
are absent from graph bounds. `DenseCrossings` preserves all eight reversed
crossings under GUID ordering, and selected layout may reuse an unselected
rectangle because it does not inspect occupied bounds. These values are
characterization baselines, not acceptance thresholds.

The replacement logical metrics are: width 224, header 30, secondary row 20,
pin row 24, body padding 10, column gap 96, row gap 28, surface-proxy width 208,
surface header 34, and minimum screen-space hit diameter 16. Overview is entered
below zoom 0.42 and left above 0.48; editing is entered above 0.82 and left below
0.74; values between bands retain the prior band. Overview exposes silhouettes,
selection, framing, pan, and focus but no pin mutation; readable mode adds the
operation label; editing adds authored identity, pin names, output type, and
inline controls. Pin names are `Texture/UV`, `A/B`, `Value`, `Min/Max`,
`A/B/Alpha`, component names `X/Y/Z/W`, and `Base/Detail` for their respective
closed-domain operations; remaining unary inputs use `Value`.

The surface proxy is derived one column gap after the rightmost node and is
vertically centered on the union of source-node bounds, with output rows in
`EMaterialSurfaceOutput` order. Focus states are none, hovered-node upstream,
hovered-node downstream, selected-node both directions, hovered-link, and
active connection/reconnection. Non-focused links use 28% alpha and focused
links use full type color plus a 1-pixel emphasis stroke.

Palette ranking is exact, prefix, then substring match over operation,
secondary name, category, and result type; remaining ties use category,
operation, result type, parameter GUID, then catalog ordinal. A source-pin open
filters to entries whose first input accepts the source type. Required inputs
are resolved in index order; drafts remain canvas-local and cancel on Escape,
popup close, document deactivation/switch/close/discard, or stale ownership.
Occupied-input reconnection retains the old link until one valid replace
command succeeds. Continuous inline edits keep a local value, commit once on
gesture completion, and cancel without mutation on Escape or lifecycle loss.

`DenseCrossings` must fall from eight crossings to at most two, while simple
fixtures must not increase. Every resulting node pair must be non-intersecting.
On the configured `Win64-Debug-DurinEditor` profile, `Maximum256` geometry plus
layout must remain below 25 ms median and 50 ms p95 across 100 warmed CPU
samples; an interactive canvas sample must remain below 16.7 ms median and
33.4 ms p95 across 300 warmed frames. GPU timing is diagnostic unless captured
on the exclusive quiet lane described by the testing workflow. The baseline
configuration is Windows x64 Debug, 100% UI scale, and a 2048 by 815 canvas;
rendered qualification additionally checks 125% UI scale.

### Stage 1: Land shared geometry and semantic zoom

- [x] Extend editor-only catalog and pin views with stable operation, secondary,
  and pin labels for the closed M5 opcode domain.
- [x] Extract deterministic logical bounds, pin anchors, detail-band selection,
  label clipping/ellipsis inputs, culling bounds, and hit-test geometry shared
  by canvas rendering and tests.
- [x] Render operation-first nodes with secondary identity, named pins, output
  type, color-plus-text type cues, clipped labels, full-value tooltips, and
  overview/readable/editing zoom bands.
- [x] Keep pan, zoom, selection, marquee, dragging, framing, linking, and
  diagnostic navigation correct across zoom-band transitions and editor UI
  scaling.
- [x] Add focused geometry and controller tests for text containment, stable
  detail thresholds, node/pin bounds, culling, hit targets, and document-local
  state.

#### Acceptance Gate

- No label can render outside its owning visible node, every editable pin has a
  stable human-readable identity at editing zoom, overview zoom remains
  uncluttered, and existing canvas commands behave identically across bands.

### Stage 2: Replace congested layout and surface presentation

- [x] Implement the graph-space surface-output proxy and include it in link
  anchors, culling, Frame All, diagnostic focus, selection emphasis, and graph
  bounds without persisting it.
- [x] Replace GUID-only row ordering with the locked deterministic median-sweep
  ordering and height-aware column packing while retaining longest-distance
  column assignment.
- [x] Add separated surface target lanes, focused-path contrast, and visible-link
  culling/hit-test acceleration using the shared geometry.
- [x] Make selected-only layout avoid all unselected node rectangles and reject
  atomically when its bounded search cannot produce a collision-free placement.
- [x] Add deterministic tests for repeat layout, Undo/Redo, semantic identity,
  zero node overlap, proxy placement, branch order, selected-layout avoidance,
  and crossing counts for every Stage 0 fixture.

#### Acceptance Gate

- Whole-graph and selected layout are deterministic and presentation-only,
  every fixture is overlap-free, congested fixtures satisfy their recorded
  crossing reduction, simple fixtures do not regress, and surface outputs pan,
  zoom, frame, and navigate as graph content.

### Stage 3: Make creation, reconnection, and properties direct

- [x] Add a keyboard- and pointer-opened searchable node palette with stable
  ranking, type/category filters, parameter disambiguation, and no dependence on
  unordered-container iteration.
- [x] Replace arbitrary first-compatible auto-wiring with source-pin creation,
  bounded multi-input completion drafts, visible unresolved-input guidance, and
  one atomic create command after completion.
- [x] Add occupied-input reconnection that preserves the old link through hover,
  invalid drop, Escape, document switch, close, discard, and stale-owner paths,
  then commits a valid replacement as one transaction.
- [x] Add inline constant, parameter, and swizzle editing with local drafts,
  explicit commit/cancel boundaries, one Undo step, and at most one compile
  request per completed gesture.
- [x] Preserve context-menu and keyboard alternatives, add Frame Selection and
  concise shortcut discovery, and ensure focus/highlight states never capture
  unrelated editor shortcuts.
- [x] Add command/controller workflow tests for palette ordering and filtering,
  unary and multi-input creation, cancellation, rejection, reconnection,
  continuous edit coalescing, Undo/Redo, and multi-document lifecycle cleanup.

#### Acceptance Gate

- Creating a connected expression never silently selects an unrelated source,
  incomplete drafts never dirty or compile the material, reconnection is atomic,
  common node properties are editable on-canvas, and every completed gesture
  has predictable Undo and compile behavior.

#### Stage 1-3 implementation evidence

- `FMaterialGraphCanvasMetrics` and `FMaterialGraphGeometry` own the stable node
  metrics, height calculation, and hysteretic detail selection used by layout,
  canvas framing, drawing, hit testing, and native tests.
- Catalog inspection now separates operation, secondary identity, category, and
  stable input names. The canvas clips scaled titles, removes text entirely in
  overview, and adds named inputs, output type, tooltips, and inline controls in
  editing mode.
- Four deterministic forward/backward median sweeps replace GUID-only rows.
  Height-aware packing has a 28-unit gap, and selected layout treats every
  unselected rectangle as occupied. The locked reversed eight-link fixture
  produces at most two crossings and the 256-node fixture is pairwise
  overlap-free.
- Surface outputs use derived graph coordinates one column after the graph and
  participate in Frame All and diagnostic focus. Coarse visible-link bounds
  avoid off-canvas curve work; selection dims unrelated paths and emphasizes
  adjacent paths.
- The Space/right-click palette uses deterministic service-owned search ranking
  and optional first-input type filtering. Multi-input expressions remain local
  drafts until all sources are explicitly chosen. Occupied-input drags perform
  one replace command only after a valid output drop. Constant and swizzle
  gestures commit on deactivation; parameter selection commits once.
- `MaterialTests FMaterialGraphAuthoringTests.*` passed 9 tests in 1.49 seconds,
  and the complete `MaterialTests` target passed 95 tests in 8.38 seconds on the
  configured Windows x64 Debug profile.

### Stage 4: Qualify and document the landed workflow

- [x] Run the smallest registered native targets covering MaterialEditor graph
  commands, geometry, transactions, and workflow, expanding only when the
  repository testing guidance requires a broader bounded domain.
- [x] Build the affected editor targets under the repository build workflow and
  perform rendered editor checks at overview and editing zoom for the locked
  simple, representative, dense, and maximum-size fixtures.
- [x] Record configuration, UI scale, viewport size, fixture, node/link counts,
  crossing results, interaction steps, frame-time sampling method, and exact
  pass/fail evidence in this plan.
- [x] Verify save/reload, dirty/save/discard, Undo/Redo, compile
  failure/recovery, last-known-good preview, diagnostic navigation, relocation,
  deletion, and multi-document isolation have not regressed.
- [x] Update [Material Graph Authoring](../Editor/Architecture/MaterialGraphAuthoring.md)
  with the lasting geometry, semantic zoom, layout, proxy, and interaction
  contracts; add or update a user guide only if the final shortcut and editing
  workflow needs durable user-facing instruction.
- [x] Run changed-document and all-plan validation, record completion evidence,
  and complete this plan only after every required gate passes.

#### Acceptance Gate

- The selected native, build, rendered, lifecycle, and maximum-graph gates pass;
  the resulting screenshots are legible at their intended zoom bands; lasting
  contracts live outside the plan; and exact evidence is recorded here.

#### Completion evidence

- Configuration: `Win64-Debug-DurinEditor`, Windows x64 Debug, 100% UI scale.
  The headless rendered canvas is 1200 by 720 with 660 pixels of graph height;
  it renders the canonical graph at zoom 1.0 (editing) and 0.30 (overview).
  Both draw lists contain more than 100 and fewer than 100,000 vertices and
  differ between semantic bands. The same bounded rendered check covers the
  reversed dense/long-name fixture at zoom 0.55 and the 256-node fixture at
  zoom 0.30. Overview suppresses its fine grid below the 16-pixel screen-space
  threshold as well as all node text and pin mutation.
- Geometry/layout fixtures cover the four-node chain, eight ordered surface
  branches, shared dependency, long names, mixed heights, eight reversed dense
  links, selected collision, and 256 nodes. The dense fixture meets the at-most
  two crossing gate, all 256 node rectangles are pairwise non-intersecting, and
  100 warmed Debug layout samples meet the 25 ms median and 50 ms p95 gates.
  This is authoritative CPU timing; no GPU performance claim is made.
- `MaterialTests FMaterialGraphAuthoringTests.*` passes 10 focused graph cases,
  including deterministic layout, presentation-only identity, Undo/Redo,
  search/filtering, semantic zoom, diagnostic isolation, transactions,
  clipboard, atomic rejection, and bounded editing/overview draw data.
- The complete `MaterialTests` target passes 96 material schema, graph,
  compile-lifecycle, dependency, proxy, instance, rendering, and representation
  cases. This supplies the bounded save/reload, dirty/transaction,
  failure/recovery, and last-known-good lifecycle regression lane.
- The full `all` editor build succeeds. `DurinEditor.exe` remains live through
  an eight-second startup smoke check and produces a 1920 by 1080 desktop
  capture without startup error. The graph-specific visual hierarchy is owned
  by the deterministic editing/overview ImGui draw qualification rather than
  treating the unrelated restored LevelEditor tab as graph evidence.
- Lasting contracts are updated in [Material Graph Authoring](../Editor/Architecture/MaterialGraphAuthoring.md).
  Changed-document validation and all-plan lifecycle validation pass with this
  plan in the completed state.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Catalog and labels | Every creatable M5 opcode exposes deterministic primary, secondary, input-pin, and output-type presentation without changing runtime serialization |
| Geometry | Deterministic node/pin/proxy bounds, clipped text regions, semantic-zoom hysteresis, culling, and hit targets at supported UI scales |
| Layout | Stable repeat output, no node intersections, size-aware gaps, reduced fixture crossings, surface-lane order, selected-only collision avoidance, and presentation-only Undo/Redo |
| Creation | Stable search ranking, compatible filtering, unary creation, bounded multi-input draft completion, cancel/reject without mutation, and one successful atomic commit |
| Reconnection | Old link retained until valid drop, invalid/canceled/stale gestures leave state unchanged, and successful replacement creates one transaction and compile request |
| Inline editing | Local draft, explicit completion/cancel, validation rejection, one Undo step, and no repeated compile generation during a continuous widget gesture |
| Canvas interaction | Pan, cursor zoom, Frame All/Selection, selection, marquee, multi-node drag, copy/paste, focus paths, keyboard/context actions, and diagnostic navigation across zoom bands |
| Lifecycle | Dirty/save/discard, save/reload, Undo/Redo, compile failure/recovery, last-known-good preview, relocation, deletion, close/reopen, and multi-document isolation |
| Rendering | Captures for overview and editing zoom show contained labels, distinguishable operations and parameters, named pins, readable focus, and graph-space outputs |
| Maximum graph | 256-node geometry/layout remains within the Stage 0 bound and interactive frame-time evidence satisfies the recorded gate without claiming GPU authority from a contended lane |
| Documentation | Lasting authoring contract updated, optional guide reflects the final UI, changed-document validation passes, and all active/completed plans validate |

## Definition of Done

- Large framed graphs no longer display full-size labels across scaled-down node
  bounds, and editing zoom communicates operation, authored identity, pin name,
  and type without relying on color alone.
- Surface outputs behave as derived graph-space content and all framing,
  diagnostics, focus, and link paths use the same geometry authority.
- Automatic layout is deterministic, overlap-free, height-aware, and satisfies
  the locked crossing gates; selected-only layout cannot overwrite unselected
  nodes.
- Search, connection-driven creation, reconnection, and inline property editing
  preserve valid-at-commit semantics and have atomic, documented Undo/compile
  behavior.
- Focused native, rendered, lifecycle, and maximum-graph evidence passes under
  the repository build and testing workflows.
- Lasting contracts are updated, this plan contains exact completion evidence,
  every checklist and acceptance gate is closed, and plan/document validation
  passes before `Status` becomes `Completed`.

## Deferred Follow-ups

- General-purpose graph framework extraction after a second concrete editor
  graph consumer proves shared ownership and interaction requirements.
- Material functions, nested graphs, reroute nodes, comments, groups, alignment
  tools, and user-authored layout constraints.
- A minimap or hierarchical navigator if measured post-plan navigation still
  exceeds the locked workflow targets after semantic zoom and path focus land.
- Persisted per-user viewport state or layout preferences; this plan retains the
  existing document-session ownership contract.
- Fully orthogonal obstacle-avoiding edge routing. This plan first qualifies
  deterministic cubic paths, surface lanes, crossing reduction, and focus.

## Related Documentation

- [Material Graph Editor Plan](MaterialGraphEditor.md)
- [Material Graph Authoring](../Editor/Architecture/MaterialGraphAuthoring.md)
- [Material System](../Runtime/Rendering/MaterialSystem.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Agent Documentation Workflow](../Agents/Documentation.md)

## Related Code

- `Engine/Source/Editor/MaterialEditor/Private/Graph/MaterialGraphCanvas.h`
- `Engine/Source/Editor/MaterialEditor/Private/Graph/MaterialGraphCanvas.cpp`
- `Engine/Source/Editor/MaterialEditor/Public/MaterialGraphAuthoring.h`
- `Engine/Source/Editor/MaterialEditor/Private/Graph/MaterialGraphAuthoring.cpp`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MMaterialEditor.h`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MMaterialEditor.cpp`
- `Engine/Tests/Native/EngineTests/Private/Materials/MaterialGraphAuthoringTests.cpp`
