# Material Graph Operations

Summary: Define the shared MaterialEditor command, presentation, canvas, transaction, clipboard, diagnostic, and document-lifecycle contracts for authored material programs.

Modules: MaterialEditor, Engine, DurinEd

Last reviewed: 2026-08-27

## Ownership

`DMaterial` remains the package and semantic authority. Its reflected
`FMaterialProgram` is the only authored graph; nodes and pins are bounded values
addressed by stable GUID and input/output indices rather than `DObject`
subobjects or canvas coordinates. Material instances never own or edit a graph.

`DMaterial::GraphPresentation` is a separate `EditorOnly` reflected value. It
contains schema version 1 and at most one integral graph-space position for each
of the 256 live node GUIDs. Sanitization retains the first valid record, removes
duplicate, dangling, invalid, and out-of-range entries, and never makes a valid
program fail to load. Presentation is copied by ordinary reflection but is
excluded from program validation, normalized IR, compile snapshots, shader-map
identity, derived data, and the cooked DMAT payload.

MaterialEditor owns transient pan, zoom, hover, selection, marquee, link drag,
and per-document controller state. None of those values are serialized.

## Inspection and commands

`MaterialGraphOperations.h` is the UI-independent boundary shared by the canvas,
tests, and structured callers. Inspection returns detached deterministic node,
pin, surface-output, presentation, and closed-domain catalog values. Catalog
entries come from the M5 opcode/type rules and the live canonical material
parameter definitions.

Commands use node GUIDs, explicit pin indices, parameter GUIDs, and
`EMaterialSurfaceOutput`. They cover creation, complete node replacement,
removal, connection, disconnection, surface assignment, movement, layout,
surface-default edit/reset, parameter promotion, explicit texture-branch
creation, copy, cut, paste, and duplication. Each result reports a stable status,
affected/generated GUIDs, bounded validation diagnostics, and a message; an
automation caller never needs to scrape canvas labels.

A semantic command copies the current program, changes the candidate, validates
it against the live parameter definitions and all M5 bounds, and commits through
`DMaterial::SetMaterialProgram()` only on success. Rejection and no-op results
leave program, presentation, dirty state, transaction history, authored
revision, and compile generation unchanged. A successful semantic command
submits exactly one M6 compile generation. Presentation-only commands sanitize
and commit positions, mark the package dirty, and never compile or invalidate
render data.

Program schema 2 keeps ordinary node inputs mandatory but makes each of the
eight fixed Material Output inputs optionally connected. Disconnecting or
deleting a surface source clears its link and returns to the retained typed
fallback; disconnecting an ordinary required node input still rejects. The
canvas makes input replacement explicit with Shift and uses the same command
result for invalid-target feedback.

## Transactions and gestures

One user-visible command produces one global editor transaction containing the
before/after program and presentation values. Undo and Redo restore semantic
state through the ordinary material mutation boundary, so semantic restores
compile while presentation-only restores do not.

Node movement uses `FMaterialGraphMoveSession`. Pointer-down captures the
selection and presentation, every drag sample previews sanitized positions,
and pointer-up records one applied transaction. Escape, document switch,
deactivation, discard, close, destruction, or stale owner cancels the gesture
and restores its original presentation. Each document owns a distinct canvas
and move session.

## Clipboard and layout

`FMaterialGraphClipboardPayload` schema 1 contains at most 256 complete nodes,
internal links, parameter GUIDs, and integral offsets from the selection's
minimum corner. It contains no object pointer, package path, surface assignment,
compile result, or viewport state. Copy orders nodes by GUID and drops links to
unselected nodes. Paste rejects unknown versions, malformed or duplicate IDs,
invalid bounds, overflow, and any candidate that ordinary program validation
does not accept.

Paste generates a fresh GUID for every node in payload order, remaps internal
links, preserves parameter GUIDs and relative placement, and commits the full
candidate once. Cut is a read-only copy followed by one validated delete
transaction. Duplicate uses the same payload and paste path with a deterministic
offset. The canvas stores this structured payload directly, so canvas and
automation semantics cannot diverge.

Automatic layout is presentation-only and deterministic. It derives consumer
edges from the semantic DAG, calculates each node's longest distance to a
surface sink, places dependencies before consumers in fixed-width columns, and
uses four forward/backward median sweeps with surface-output order as the sink
seed. Stable prior order and GUIDs break all ties. Columns use computed node
heights and fixed gaps; selected-only layout treats every unselected node as an
occupied rectangle and searches downward for the nearest collision-free slot.
It rejects atomically if the bounded search fails. Layout never changes program
order, node IDs, links, outputs, normalized IR, or identity. Missing legacy
positions invoke this layout when the graph is first displayed.

## Canvas and diagnostics

The MaterialEditor canvas uses the existing ImGui draw/input stack and one
logical geometry authority shared with layout and native tests. Nodes use a
stable 224-unit width and height derived from their named pin rows. Operation
identity is the primary title and an authored parameter/resource name is the
secondary title. Text is clipped and ellipsized to its owning bounds; editing
zoom adds named inputs and a textual output type so type color is never the only
cue.

Semantic zoom has hysteretic overview, readable, and editing bands. Overview
keeps silhouettes, selection, focus, pan, and framing while disabling pin
mutation. Readable mode adds clipped operation titles. Editing mode adds
secondary identity, named pins, output type, tooltips, and inline constant
controls. Frame All includes the derived surface proxy; Frame Selection uses
only the selection. The derived `Material Output` terminal lives one logical
column after the rightmost node, pans and zooms with the graph, participates in
bounds and diagnostic framing, and is never persisted. It owns fixed Base
Color, Normal, Metallic, Roughness, Ambient Occlusion, Emissive, Opacity, and
Opacity Mask input rows. Editing mode exposes inline fallback controls for
unconnected rows; each completed gesture is one validated transaction, while
Escape and document lifecycle cancellation discard the draft.

Visible links are coarsely culled before curve drawing. When nodes or a surface
output are selected, unrelated links dim while adjacent paths receive a thicker
typed stroke. Occupied-input reconnection retains its authored link until a
valid source drop succeeds as one replace transaction.

The centered searchable creation palette opens from Space, an empty-canvas
double click, or an output link dropped on empty space. It focuses search,
supports complete arrow/Enter/Escape navigation, surfaces category and pin type
signatures, and places favorites and recent choices first. Search ranks exact,
prefix, and substring matches, then uses stable category, operation, type,
parameter GUID, and catalog order ties. Opening from a source output filters the
first input by compatible type. Selection creates and connects the requested
node as one command; missing numeric inputs receive visible default Constant
nodes in the same transaction, while resource inputs without a default reject.
Escape and every document lifecycle cancellation close the palette and discard
reconnection, movement, and inline edit drafts without dirtying or compiling
the material. Every mutation still routes to the stateless
`FMaterialGraphOperations` operation boundary.

`Promote to Parameter` is available on an unconnected Material Output input. It
creates the compatible canonical Parameter node one column upstream, copies the
fallback into the definition value, connects the input, and records program,
presentation, and value as one Undo/Redo transaction. `Add Texture` explicitly
creates the role's TextureParameter, TextureCoordinate, TextureSample, and
required channel swizzle or normal decode nodes before replacing the surface
connection in one candidate-validated transaction. Neither workflow creates a
hidden branch.

Compile state is observational. Pending and failed states identify whether the
preview is showing last-known-good output, but never block canvas input or
replace the M6 publication policy. Diagnostic activation uses the retained
`Program`, `Node`, `Input`, or `SurfaceOutput` location. Live node/input targets
select and frame their node; a surface target highlights its fixed output.
Program-wide, invalid, or generation-stale locations remain visible as text and
do not fabricate a target.

## Reachable parameter views

MaterialEditor consumes Engine's detached
`InspectMaterialParameterDependencies` snapshot rather than enumerating the
canonical definition catalog. Base Details contains one row per reachable
declaration in deterministic first-use order. Parameter-node inline controls
and Details submit the same nested definition-value edit, so continuous edits
coalesce through the shared property transaction path and produce dynamic render
updates without compilation.

Instance rows use the resolved root program's same snapshot for override
eligibility and source labels. A local override which becomes unreachable moves
to the collapsed orphan group and remains removable; it is not rendered or
presented as active. Reconnecting the same GUID restores the preserved base
value and makes that override eligible again.

## Document and asset lifecycle

Base-material documents own canvases; instance documents retain the parameter
override workflow and direct users to the root base material for graph editing.
Canvas maps use document IDs, so selection cannot leak across materials or
instances. User-scoped material-editor session settings retain the left, right,
and diagnostic pane proportions and the pan/zoom viewport for each material
resource. Closing a document destroys its controller; reopening restores only
that material's viewport while authored positions continue to come from the
package.

MaterialEditor registers the authoritative Engine move observer with its
module callback gate. A relocation moves the loaded-material entry and asks
`FWorkspaceManager` to remap the existing document resource ID, per-resource
document key, and label without changing document identity. There is no local
asset catalog mirror. Deletion is observed through the existing object handle;
before drawing, an invalid owner closes its document and releases canvas and
preview state. Active graph gestures are canceled before switch, discard,
close, deletion cleanup, module unload, or shutdown.

## Related documentation

- [Material System](../../Runtime/Rendering/MaterialSystem.md)
- [Reflected Property Editing](ReflectedPropertyEditing.md)
- [Workspace Framework](WorkspaceFramework.md)
- [Asset Catalog and Mutation](../../Runtime/Assets/AssetCatalogAndMutation.md)
