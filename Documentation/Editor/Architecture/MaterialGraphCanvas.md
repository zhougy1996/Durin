# Material Graph Canvas

Summary: Define material editor panels, canvas geometry, semantic zoom, node creation menus, and diagnostic navigation.

Modules: MaterialEditor, Engine, DurinEd

Last reviewed: 2026-09-15

Commands, ownership, transactions, and clipboard semantics are defined in
[Material Graph Operations](MaterialGraphOperations.md). Preview resources and
Compile/Apply/Save behavior are defined in
[Material Editor Lifecycle](MaterialEditorLifecycle.md).

## Editor panels

Each material document owns an isolated ImGui dock space, keyed by its stable
`DocumentKey`, using the shared WorkspaceUI panel and docking helpers. Preview,
Material Graph, Details, Parameters and Diagnostics are dockable windows. Wide initial
layouts reserve the left 30% for Preview above Details; the graph fills the
remaining width and height. Parameters shares the Details dock as a visible tab,
and Diagnostics as an optional tab. Small initial layouts use dock tabs. Resizing
does not rebuild a user's arrangement. Hidden document roots keep their dock spaces alive.

Function insertion opens a bounded popup above the graph; its picker displays
directly without a collapsible wrapper. Selected-call navigation
lives in Details alongside the shared input editor, so selection does not resize
or displace the canvas. Details owns selected-node authoring and instance
inheritance and rendering properties. Parameters owns shared parameter metadata
and material-instance overrides. New materials contain only
Surface with eight property inputs and retained defaults, without expression
nodes or function calls.

The single nondeletable Surface root displays the material identity, shading model
and blend mode. Selecting it or clearing the graph selection shows the same
material identity and directly expanded properties in Details, without a Surface
Settings header: Lit/Unlit shading, blend mode,
masked cutoff, two-sided rendering and depth-write policy. Settings use reflected
property transactions on the working material, so Apply/Discard and Undo/Redo
retain their ordinary atomic behavior. Inactive cutoff values remain stored.
Root property inputs show only names, pins, and connections at readable and
editing zoom; no scalar fields, color swatches, vector Edit buttons, or inline
default values are displayed. Change input values by connecting expression nodes.
Inactive property labels and pins are dimmed. Existing authored defaults and
connections remain stored; disconnected inputs resume their retained values.
Hovering an active disconnected row shows its retained value. The pin context
menu can promote that value to a connected parameter without changing its effect.
Constant and parameter nodes retain their own value displays and inline editors.
The compiler's explicit final anchor and aggregate/per-property
exclusion rule are unchanged; reusable Surface values do not become extra roots.

Window controls reopen optional panels and reset the default layout. Material
identity and type appear directly in Details when no graph node is selected or
the Surface root is selected. The type includes the fixed Surface domain; the
asset path is available by hovering the name. Selected expression nodes show their
name and properties without a wrapper header.
Surface settings, node values and instance rendering overrides use the shared
MonaImGui property tables with left-aligned labels and a stretching value column.
Material tables ignore saved column widths so older narrow layouts do not persist.
Each input occupies one row: unconnected inputs expose their value, while connected
inputs show a source node/output button that locates the source. The row menu owns
extract, promote, inline and disconnect actions; its When disconnected submenu
exposes the retained value. Sampler settings remain collapsed.
Base-material values and node bindings are edited through selected parameter
owners and literal inputs in Details. Parameter name in Details and Name in Parameters rename the shared definition,
preserving its ID and instance overrides across all references. Renaming to another
parameter's name rejects with guidance to change the binding. The collapsed
Change parameter binding group in Details changes only the selected node:
Bind or create adopts an existing compatible name's ID and value, or creates an
independent parameter for a new name. Existing instance overrides stay with their
original parameter IDs. Both actions support ordinary Undo/Redo. Parameter node
types are fixed at creation; Details has no type conversion control. Texture fallback and filtering/addressing
controls live in collapsed Sampler settings. Parameters lists all base-material
parameters, including disconnected owners, with search by name, display name or
group and grouping by slash-separated paths. Clicking a parameter selects and
frames its owner; shared parameters expose navigation to each reference. Expanding
a parameter exposes shared renaming, display name, group, sort order, compatible
presentation modes and scalar range hints. These edits preserve parameter IDs,
instance overrides and per-node sampling inputs through ordinary transactions.
Instances show their parameter override list in Parameters because they do not
own a graph. The Window menu reopens Parameters and its visibility is persisted.
Instance parameter groups omit a sole outer container with no direct values.
ImGui persists docking geometry; the
material session settings retain panel visibility and per-asset graph viewports.
Preview visibility follows the actual preview panel, including dock-tab hiding.

## Canvas and diagnostics

Creation-menu rendering is isolated from canvas rendering and pointer gestures.
Numeric controls and parameter/literal type conversions are shared by graph
editing paths so each supported vector dimension has one conversion contract.

The MaterialEditor canvas uses the existing ImGui draw/input stack and one
logical geometry authority shared with layout and native tests. Nodes use a
stable 224-unit width and height derived from their named pin rows. Operation
identity is the primary title for operation nodes. Numeric constants instead
show their compact value as the title and constant type below it. Parameters
retain their authored name as the title and show their current numeric value
below it, including in readable zoom. Values use four significant digits, omit
trailing zeros, and retain vector dimensions. Float3/Float4 nodes also show a
small RGB swatch; its channels are clamped for display while numeric values
remain unchanged. Hover tooltips expose labels without width truncation and numeric
values with nine significant digits. Material and function canvases share this
heading renderer, including zoom, clipping, and overview visibility.
Node bodies use approximately 90% opacity (94% when selected), including function
nodes and the material output, so occluded wires remain faintly visible. Headers,
text and pins remain opaque. Selection continues to emphasize adjacent wires.

Numeric Parameter controls are directly draggable in Editing mode without first
selecting the node. Dragging updates the value and preview continuously; release
records one undo operation for the entire gesture. Escape restores the starting
value. Constants and Swizzles expose inline controls when selected. Text is clipped and ellipsized to its owning bounds; editing
zoom adds named inputs and a textual output type so type color is never the only
cue.

Semantic zoom has hysteretic overview, readable, and editing bands. Overview
keeps silhouettes, selection, focus, pan, and framing while disabling pin
mutation. Readable mode adds clipped operation titles. Editing mode adds
secondary identity, named pins, output type, tooltips, and inline constant
controls. Frame All includes the derived surface proxy; Frame Selection uses
only the selection. The derived `Surface` terminal is initially placed
one logical column after the rightmost node and remains stable during manual
node arrangement. Its header displays the material asset name with `Material
Output` as secondary identity. The terminal can be selected and dragged like a
node; its optional integral position is persisted in graph presentation, while
automatic layout derives and persists a fresh position. It pans and zooms with
the graph, participates in bounds and diagnostic framing, and remains absent
from the semantic material program. Per-property mode owns fixed Base Color,
Normal, Metallic, Roughness, Ambient Occlusion, Emissive, Opacity, and Opacity
Mask rows; aggregate mode owns one typed Surface row. Readable mode retains
these input names and read-only fallback values so zooming out does not leave
an unlabeled terminal. Editing mode exposes inline fallback controls for
unconnected rows; each completed gesture is one validated transaction, while
Escape and document lifecycle cancellation discard the draft.

Visible links are coarsely culled before curve drawing. When nodes or a surface
output are selected, unrelated links dim while adjacent paths receive a thicker
typed stroke. Occupied-input reconnection retains its authored link until a
valid source drop succeeds as one replace transaction.

Constant menu creation exposes one entry, initially Float. The node context menu's
Type selector switches between Float, Float2, Float3, and Float4 through the
validated node replacement command. It retains the node GUID, literal components,
and links, rejects incompatible consumers atomically, and records successful
changes in Undo/Redo. Constants remain independent literals unless the graph
explicitly fans out one node's output or promotes it to a named parameter.

Math operations expose one creation row per operation, with no vector-width suffix.
Source-link creation selects the matching numeric shape; empty-canvas creation
starts at Float (Float2 for Normalize). Connecting operands adapts the node and
downstream math widths in the same transaction. Scalar operands broadcast to the
vector width; different non-scalar widths reject atomically. Lerp Alpha remains
scalar and Normalize requires a vector. Pins retain their resolved type display.

Parameter creation exposes Scalar Parameter, Vector Parameter and texture owner
entries. Vector Parameter has a 2/3/4 component selector, defaulting to four;
its declaration type remains fixed after creation. Selecting an entry creates and places a fresh uniquely named owner in one
transaction. The catalog has no existing-parameter rebind mode. Sharing connects the
existing owner's output to more consumers. Inspection reads labels from the node's
owned payload. PBR roles and UV controls are explicit template-owned parameters,
not distinct node kinds.

The node creation context menu opens at the pointer from an empty-canvas right
click, Space, an empty-canvas double click, or an output link dropped on empty
space. It focuses search and supports arrow/Enter/Escape navigation. Compact
node rows are grouped by category; recently used nodes form a leading group
when no search is active, ordered from most recently used. These are additional
shortcuts: each entry also remains in its original category. Active searches show
each match once, and link compatibility filtering applies to both lists. Rows display shortcuts instead of favorite buttons. Descriptions and input
signatures appear in hover tooltips. An active search preserves global match
relevance instead of regrouping by category. Paste and Auto Layout
remain available below the creation list when no source link is active.
Holding a key and left-clicking empty canvas creates a node at the pointer:
`1`/`2`/`3`/`4` create Float through Float4 constants, `A` Add, `M` Multiply,
`L` Lerp, `U` Texture Coordinates, `S` Scalar Parameter, `V` Vector4 Parameter,
and `T` Texture Sample Parameter 2D. Math shortcuts start with scalar nodes that
adapt when connected.
Creation selects the new node and records one Undo/Redo transaction. Shortcuts
require an idle canvas with no text input or Ctrl/Shift/Alt/Super modifier;
node, pin, and material-output clicks retain their existing gestures.
The function canvas shares the creation menu and shortcut dispatch, excluding
parameter nodes that functions cannot own. Both canvases share paste placement:
keyboard paste uses the pointer in graph coordinates, repeated pastes at the same
anchor add a 24-unit offset and select the generated nodes. Function toolbar
paste and Add Node use the viewport center.
Menu requests and editing keyboard commands do not replace an active drag.
Resetting an interaction cancels any remaining move or parameter edit session.
Function inspection and visual topology are retained until the function or its
direct dependencies change revision, its positions change, or interaction cleanup
invalidates a draft. No timing guarantee is implied by this cache.
Right-clicking a node or surface input retains its editing context menu. Search ranks exact,
prefix, and substring matches, then uses stable category, operation, type,
parameter GUID, and catalog order ties. Opening from a source output filters the
first input by compatible type. Selection creates and connects the requested
node as one command; missing numeric inputs receive inline literal defaults
in the same transaction, while resource inputs without a default reject.
Escape and every document lifecycle cancellation close the palette and discard
reconnection, movement, and inline edit drafts without dirtying or compiling
the material. Every mutation still routes to the stateless
`FMaterialGraphOperations` operation boundary.

`Promote to Parameter` is available on an unconnected Surface input. It
creates the compatible material-owned Parameter node one column upstream, copies the
fallback into the definition value, connects the input, and records program,
presentation, and value as one Undo/Redo transaction. `Add Texture` explicitly
creates one TextureSampleParameter2D with default mesh UV0 and connects its
RGB or scalar channel output directly. Normal uses the same combined sample owner
with its decoded tangent-space Normal output and flat RG fallback. Sampling and
RG decoding share one fetch without a flat-normal blend. The reusable SampleNormal
function remains available for explicit strength and RNM composition. The entire
branch and connection form one candidate-validated
Undo/Redo transaction. Connect a TextureCoordinates or other Float2 expression
for custom UVs; texture objects remain available for function inputs and independent
sampling. Sampling nodes show RGB, R, G, B, A, and RGBA in that
order by default. The Advanced pins toggle reveals Texture resource and decoded Normal outputs;
connected outputs remain visible even when advanced pins are hidden. Display order
and visibility never change serialized output indices. RG selection uses an explicit
Swizzle node. Sample output 6 is invalid; there is no legacy RG migration or
compatibility path. Texture and Normal keep indices 7 and 8.
Swizzle titles show the selected channels (for example, `Swizzle RG`), and channel
controls use R/G/B/A selectors and a 1-4 output component selector. Growing the
output repeats the first selected channel; invalid source channels and incompatible
consumers reject the edit atomically. The Channels palette has one Make Vector,
one Splat and one Swizzle entry, with output width selected beside the entry.
Make Vector combines scalar inputs; it does not concatenate vector inputs.
Swizzle covers component masking, repetition and truncation. Dedicated Truncate
expressions and compiler opcodes are removed. Maintained assets use the current
expression classes.

Compile state is observational. Unsubmitted and pending states identify whether
the preview shows last-known-good output; failed states show ErrorMaterial.
Neither blocks canvas input or replaces the M6 publication policy. Display hints
derive from the accepted program and request freshness, rather than stored status
flags. Diagnostic activation uses the retained
`Program`, `Node`, `Input`, or `SurfaceOutput` location. Live node/input targets
select and frame their node; a surface target highlights its fixed output.
Program-wide, invalid, or generation-stale locations remain visible as text and
do not fabricate a target.
