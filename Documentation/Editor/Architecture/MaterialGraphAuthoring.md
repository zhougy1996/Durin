# Material Graph Authoring

Summary: Define the shared MaterialEditor command, presentation, canvas, transaction, clipboard, diagnostic, and document-lifecycle contracts for authored material programs.

Modules: MaterialEditor, Engine, DurinEd

Last reviewed: 2026-08-26

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

`MaterialGraphAuthoring.h` is the UI-independent boundary shared by the canvas,
tests, and structured callers. Inspection returns detached deterministic node,
pin, surface-output, presentation, and closed-domain catalog values. Catalog
entries come from the M5 opcode/type rules and the live canonical material
parameter definitions.

Commands use node GUIDs, explicit pin indices, parameter GUIDs, and
`EMaterialSurfaceOutput`. They cover creation, complete node replacement,
removal, connection, disconnection, surface assignment, movement, layout,
copy, cut, paste, and duplication. Each result reports a stable status,
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

The version-1 program representation requires every node input and all eight
surface outputs to remain valid at a committed boundary. A standalone
disconnect or deletion therefore rejects when it would leave an incomplete
node or output. The canvas makes input replacement explicit with Shift and
uses the same command result for invalid-target feedback.

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
orders each column by GUID at fixed row spacing. It handles 256 nodes in linear
graph work without changing program order, node IDs, links, outputs, normalized
IR, or identity. Missing legacy positions invoke this layout when the graph is
first displayed.

## Canvas and diagnostics

The MaterialEditor canvas uses the existing ImGui draw/input stack. It renders a
clipped grid, typed nodes and pins, links, fixed surface outputs, selection,
hover and valid/invalid link targets. It supports pan, cursor-centered zoom,
framing, selection and marquee, multi-node dragging, typed linking, context
creation, constant and parameter editing, delete, copy/cut/paste, duplicate,
automatic layout, and keyboard operations. Every mutation routes to the graph
service.

Compile state is observational. Pending and failed states identify whether the
preview is showing last-known-good output, but never block canvas input or
replace the M6 publication policy. Diagnostic activation uses the retained
`Program`, `Node`, `Input`, or `SurfaceOutput` location. Live node/input targets
select and frame their node; a surface target highlights its fixed output.
Program-wide, invalid, or generation-stale locations remain visible as text and
do not fabricate a target.

## Document and asset lifecycle

Base-material documents own canvases; instance documents retain the parameter
override workflow and direct users to the root base material for graph editing.
Canvas maps use document IDs, so selection and viewport state cannot leak across
materials or instances. Closing a document destroys its controller; reopening
starts fresh transient state while persisted positions return from the package.

MaterialEditor registers the authoritative AssetCore move observer with its
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
