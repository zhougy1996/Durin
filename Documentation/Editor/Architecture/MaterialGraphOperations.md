# Material Graph Operations

Summary: Define the shared MaterialEditor command, presentation, canvas, transaction, clipboard, diagnostic, and document-lifecycle contracts for authored material programs.

Modules: MaterialEditor, Engine, DurinEd

Last reviewed: 2026-09-11

## Ownership

`DMaterial` remains the package and semantic authority. Its reflected
`FMaterialProgram` is the only authored graph; nodes and pins are bounded values
addressed by stable GUID and input/output indices rather than `DObject`
subobjects or canvas coordinates. Material instances never own or edit a graph.

`DMaterial::GraphPresentation` is a separate `EditorOnly` reflected value. It
contains schema version 2, exactly one integral graph-space position for every
live node GUID, and one integral Material Output position. New material and
node creation establish those positions before publication. Sanitization retains
the first valid record and removes duplicate, dangling, invalid, and out-of-range
entries; an incomplete presentation violates the editable graph contract rather
than selecting an alternate transient layout. Presentation is copied by ordinary
reflection but is excluded from program validation, normalized IR, compile
snapshots, shader-map identity, derived data, and the cooked DMAT payload.

MaterialEditor owns transient pan, zoom, hover, selection, marquee, link drag,
and per-document controller state. None of those values are serialized.

## Editor panels

Each material document owns an isolated ImGui dock space, keyed by its stable
`DocumentKey`, using the shared WorkspaceUI panel and docking helpers. Preview,
Material Graph, Details, and Diagnostics are dockable windows. Wide initial
layouts place Preview on the left, Details on the right, and Diagnostics below
the graph; small initial layouts use dock tabs. Resizing does not rebuild a
user's arrangement. Hidden document roots keep their dock spaces alive.

Window controls reopen optional panels and reset the default layout. Material
Info is a collapsed section in Details. ImGui persists docking geometry; the
material session settings retain panel visibility and per-asset graph viewports.
Preview visibility follows the actual preview panel, including dock-tab hiding.

## Preview Resources

Material Preview acquires shared `/Engine/Models/Sphere` and
`/Engine/Models/Box` StaticMesh assets through the canonical
editor retention service. Multiple documents coalesce by virtual asset identity;
preview creation performs no transient OBJ import, and retained handles provide
the GC lifetime edge.

Preview rendering follows [Material System](../../Runtime/Rendering/MaterialSystem.md).
Thumbnail sessions follow [Asset Thumbnails](AssetThumbnails.md).

## Inspection and commands

`MaterialGraphOperations.h` is the UI-independent boundary shared by the canvas,
tests, and structured callers. Inspection returns detached deterministic node,
pin, surface-output, presentation, and closed-domain catalog values. Catalog
entries come from the opcode/type rules and the live material-owned parameter
definitions. `MakeSurface` exposes the aggregate Surface output type;
legacy `StandardSurface` and role-bound `TextureCoordinate` are not authorable
catalog entries.

Commands use node GUIDs, explicit pin indices, parameter GUIDs, and
`EMaterialSurfaceOutput`. They cover creation, complete node replacement,
removal, connection, disconnection, surface assignment, movement, layout,
surface-default edit/reset, parameter promotion, explicit texture-branch
creation, copy, cut, paste, and duplication. Each result reports a stable status,
affected/generated GUIDs, bounded validation diagnostics, and a message; an
automation caller never needs to scrape canvas labels. Aggregate assignment is
atomic: it accepts only a Surface node and clears all property links in the same
validated candidate. Disconnect restores the retained property fallbacks.

A semantic command copies the current program, changes the candidate, validates
it against the live parameter definitions and all M5 bounds, and commits through
`DMaterial::SetMaterialProgram()` only on success. Rejection and no-op results
leave program, presentation, dirty state, transaction history, authored
revision, and compile generation unchanged. A successful semantic command
advances authored state once and schedules compilation through the root material's
edit policy. Immediate callers submit one generation; automatic editor edits
coalesce before submission, and manual edits remain unsubmitted. Presentation-only commands sanitize
and commit positions, mark the package dirty, and never compile or invalidate
render data.

Program schema 4 keeps ordinary node inputs mandatory but makes each of the
eight fixed Material Output inputs optionally connected. Disconnecting or
deleting a surface source clears its link and returns to the retained typed
fallback; disconnecting an ordinary required node input still rejects. The
canvas makes input replacement explicit with Shift and uses the same command
result for invalid-target feedback. Aggregate and per-property sources cannot
coexist in a valid program.

## Transactions and gestures

`ReplaceDefinitionsAndProgram` commits material-owned declarations and graph
references through one Engine validation boundary and one transaction. Its
Undo/Redo state retains definitions, graph and presentation; texture defaults
are enumerated to the reference collector, and owned storage is accounted to
the transaction buffer. Float4 declarations use the four-component vector
editor. The overload accepting presentation commits node placement with the
same declaration and graph transaction. `CreateParameter`, `RenameParameter`
and `DeleteParameter` wrap Engine's identity-safe mutation results in this
history boundary. Reuse and rejected edits add no undo entry. Results carry
parameter GUIDs separately from expression node GUIDs.

The root details panel exposes create/reuse, rename and delete, marks declarations
with no graph references as Unused without deleting them, and displays
all declaration defaults, including unreachable declarations. Instances retain
reachable controls and inspectable orphan overrides with explicit removal.
The canvas constant-node menu and `PromoteConstantToParameter` share one
operation: preserve the node GUID and links, create or reuse a named numeric
declaration, and replace the constant with a reference. Float through Float4
are supported; same-name/type reuse preserves the existing default and metadata.
Surface-output promotion creates an ordinary Parameter reference. Texture-branch
creation emits an explicit TextureParameter, constant UV channel, `UVChannel`,
TextureSample, and required swizzle or normal-decode branch; sampler/fallback
policy remains typed data on the Texture2D declaration.

One user-visible command produces one global editor transaction. Semantic
commands retain before/after program and presentation values; presentation-only
commands retain only changed node or Material Output positions; parameter-only
commands retain only the parameter GUID and before/after values. Every custom
change reports its owned native allocations to the bounded transaction buffer.
Undo and Redo restore semantic state through the ordinary material mutation
boundary, so semantic restores follow the current compile policy while presentation-only and
parameter-only restores do not. Activating an editable asset document establishes
its package's saved revision checkpoint, and a successful save advances that
checkpoint. Undoing back to the open or most recently saved revision therefore
clears the package dirty state and removes the unsaved marker; Redo marks it dirty
again when it moves away from that checkpoint.

Node and Material Output movement use `FMaterialGraphMoveSession`. Pointer-down
captures the selection and presentation, every drag sample previews sanitized
positions, and pointer-up records one applied transaction. Escape, document
switch, deactivation, discard, close, destruction, or stale owner cancels the
gesture and restores its original presentation. Each document owns a distinct
canvas and move session.

## Clipboard and layout

`FMaterialGraphClipboardPayload` schema 3 contains at most 256 complete nodes,
relative positions, referenced declaration snapshots and a weak source-root
object-generation identity. Counted collector-visible strong references retain
texture defaults independently of the source material. Replacing or clearing
the payload releases those references. Clipboard state is process-local; it
contains no compiler result or viewport state.

Copy orders nodes by GUID and retains external links for same-root paste.
Paste generates new node GUIDs and remaps internal links. Same-root paste
requires each declaration GUID/type to remain available and uses current labels
and defaults. Foreign paste rejects external links even if a destination node
happens to have the same GUID. It creates local parameter GUIDs, or reuses a
same-name/type declaration only when default and metadata match. Missing,
duplicate, invalid and conflicting declarations reject the entire operation.
Node placement, declaration creation and graph references commit once and
Undo/Redo together. Unknown clipboard versions are rejected.

Retired StandardSurface and role-dependent TextureCoordinate opcode values are
invalid in both authored graphs and clipboard payloads. Custom parameters are
never remapped into fixed role slots.

A selection containing the active aggregate source records that source; paste
reconnects the remapped Surface node atomically without copying the derived
terminal. Cut copies before one validated delete. Duplicate uses the same
payload and paste path with a deterministic offset. The canvas stores this
structured payload directly, so canvas and automation semantics agree.

Automatic layout is presentation-only and deterministic. It derives consumer
edges from the semantic DAG, calculates each node's longest distance to a
surface sink, places dependencies before consumers in fixed-width columns, and
uses four forward/backward median sweeps with surface-output order as the sink
seed. Stable prior order and GUIDs break all ties. Columns use computed node
heights and fixed gaps; selected-only layout treats every unselected node as an
occupied rectangle and searches downward for the nearest collision-free slot.
It rejects atomically if the bounded search fails. Layout never changes program
order, node IDs, links, outputs, normalized IR, or identity. New material
creation persists the initial complete layout before the asset is first
published. Opening, panning, zooming, selection framing, and diagnostic framing
never synthesize or persist missing positions. Explicit Auto Layout and node or
Material Output movement persist presentation through ordinary transactions.

## Canvas and diagnostics

Creation-menu rendering is isolated from canvas rendering and pointer gestures.
Numeric controls and parameter/literal type conversions are shared by graph
editing paths so each supported vector dimension has one conversion contract.

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
only the selection. The derived `Material Output` terminal is initially placed
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

Constant creation exposes one entry, initially Float. The node context menu's
Type selector switches between Float, Float2, Float3, and Float4 through the
validated node replacement command. It retains the node GUID, literal components,
and links, rejects incompatible consumers atomically, and records successful
changes in Undo/Redo. Constants remain independent literals unless the graph
explicitly fans out one node's output or promotes it to a named parameter.

Parameter creation exposes five generic entries: Scalar, Vector2, Vector3,
Vector4, and Texture Parameter. Selecting a type opens a choice inside the same
menu: create a new parameter or filter and reference an existing declaration of
that type. Only creating a new parameter adds a uniquely named declaration; both
paths create and place the reference node in one transaction. Cancelling the
choice does not mutate the graph or declarations. The node catalog is independent
of material declarations: inspection resolves labels from live declarations, and
binding controls enumerate declarations directly. A node's Parameter menu binds any existing declaration of the
same type, allowing multiple nodes to share one value without changing links.
PBR role names and UV controls remain template declarations, not node kinds.

The node creation context menu opens at the pointer from an empty-canvas right
click, Space, an empty-canvas double click, or an output link dropped on empty
space. It focuses search and supports arrow/Enter/Escape navigation. Compact
node rows are grouped by category; favorites and recently used nodes form
separate leading groups when no search is active. Descriptions and input
signatures appear in hover tooltips. Search keeps matching entries grouped
by category and preserves relevance within each group. Paste and Auto Layout
remain available below the creation list when no source link is active.
Right-clicking a node or surface input retains its editing context menu. Search ranks exact,
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
creates the compatible material-owned Parameter node one column upstream, copies the
fallback into the definition value, connects the input, and records program,
presentation, and value as one Undo/Redo transaction. `Add Texture` explicitly
creates the role's TextureParameter, constant channel, UVChannel, TextureSample,
and required channel swizzle or normal decode nodes before replacing the surface
connection in one candidate-validated transaction. Neither workflow creates a
hidden branch.

The toolbar exposes Compile and a user-scoped Auto Compile preference, enabled
by default. Opening a base material applies the preference to its transient Engine
edit policy; loaded instances inherit that policy. Automatic edits wait for a
400 ms quiet period before compiler-input construction. Each subsequent semantic
edit restarts the deadline. Switching to manual removes scheduled submission;
switching back schedules any unsubmitted edits. Compile immediately submits the
root and loaded dependent variants through ordinary cache reuse. Cancel Compile
cancels their pending work. The policy remains on the loaded material after its
document closes; automatic deadlines are pumped by Engine, not by visible UI.
Reopening or package replacement reapplies the current preference.

Save persists authored changes independently of compilation. The unsaved marker
and Needs Compile status have separate meanings. Manual edits retain the accepted
preview until explicitly compiled; Cook still requires a current successful result.
Graph validation and dynamic parameter updates remain immediate in both modes.

Compile state is observational. Unsubmitted and pending states identify whether
the preview shows last-known-good output; failed states show ErrorMaterial.
Neither blocks canvas input or replaces the M6 publication policy. Display hints
derive from the accepted program and request freshness, rather than stored status
flags. Diagnostic activation uses the retained
`Program`, `Node`, `Input`, or `SurfaceOutput` location. Live node/input targets
select and frame their node; a surface target highlights its fixed output.
Program-wide, invalid, or generation-stale locations remain visible as text and
do not fabricate a target.

## Reachable parameter views

MaterialEditor consumes Engine's detached
`InspectMaterialParameterDependencies` snapshot rather than enumerating the
material-owned definition catalog. Base Details contains one row per reachable
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

MaterialEditor registers the authoritative Engine move observer for its
workspace lifetime. The owner unregisters the observer and finishes
active relocation before unloading its code. A relocation moves the loaded-material entry and asks
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

## Instance Rendering Configuration

MaterialEditor exposes independent blend, shading, mask-threshold, two-sided and
depth-write override flags and values. Clearing a flag resumes inheritance while
preserving its authored inactive value. Effective supplying-owner labels come
from the bounded runtime resolver. Edits use the shared reflected whole-member
transaction path, so Undo/Redo and direct setters trigger the same owner revision
and variant rules. Toolbar cancellation/recompile, diagnostics, last-known-good
status and thumbnail readiness refer to the instance owner. A deferred request
is displayed as waiting for compiler capacity. Graph navigation still belongs
to the root material document.
