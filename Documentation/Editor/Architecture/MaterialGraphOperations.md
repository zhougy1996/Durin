# Material Graph Operations

Summary: Define shared MaterialEditor expression ownership, inspection, commands, texture and UV input authoring, material functions, transactions, and clipboard contracts.

Modules: MaterialEditor, Engine, DurinEd

Last reviewed: 2026-09-16

## Task routing

| Task | Read |
| --- | --- |
| Expression ownership, snapshots, or function publication | [Ownership](#ownership) |
| Built-in function recipes or imported parent authoring | [Standard function authoring](#standard-function-authoring) |
| Graph change notifications and cached inspection | [Change observation](#change-observation) |
| Detached inspection, node creation, or connection commands | [Inspection and commands](#inspection-and-commands) |
| Inline defaults, texture parameters, or UV connections | [Compact input and texture authoring](#compact-input-and-texture-authoring) |
| Undo/Redo or movement sessions | [Transactions and gestures](#transactions-and-gestures) |
| Copy/paste, duplication, or deterministic layout | [Clipboard and layout](#clipboard-and-layout) |
| Panels, canvas geometry, menus, or diagnostic navigation | [Material graph canvas](MaterialGraphCanvas.md) |
| Preview meshes, Compile/Apply/Save, asset lifetime, or instance controls | [Material editor lifecycle](MaterialEditorLifecycle.md) |

Read the relevant section and its required dependencies; these documents are not
an ordered reading sequence.

## Ownership

`DMaterial` and `DMaterialFunction` own concrete `DMaterialExpression` children.
Expression connections use stable GUIDs and output indices; presentation stores
GUID-keyed positions and labels separately from expression payloads. Material instances never own or edit a graph.
Each authored material owns exactly one `DMaterialExpressionMaterialOutput`. Its
`Outputs` field owns terminal links and literal defaults; `GetExpressionOutputs()`
is a read-through accessor, not separately serialized state. The strict graph
setter rejects missing or duplicate output nodes. The recipe overload constructs
the terminal before publishing the graph. The terminal is selectable and movable,
but cannot be deleted or duplicated. Copying a mixed selection excludes it.

`DMaterial::Domain` selects the output-pin definition contract; Surface is the
only implemented domain. `EMaterialOutputPin` gives pins stable semantic keys,
independent of display order. The output terminal stores `bUseMaterialAttributes`: false
shows eight property inputs; true shows only Material Attributes. Both connection
sets and property defaults persist across mode changes.
Connections and edits address those keys. Additional domains must define their
pin schema and validation/migration policy in Engine; canvas rendering consumes
ordinary node input descriptors. Domain switching is not implemented.
Base-material documents edit a transient working `DMaterial`; Apply transfers
its authored state to the existing package-owned source material. Graph commands
remain reusable against either owner and never implicitly select a source asset.
Working-copy initialization and Apply duplicate concrete expression children through
`SetMaterialExpressions`, including call bindings and typed terminal defaults.
Source and preview never share mutable expression children. The applied checkpoint
retains independent expression copies and compares their reflected authored fields,
static properties, outputs and presentation. A changed callee with unchanged node
IDs is still an unapplied edit. Invalid candidates leave the source unchanged.
Semantic commands preserve storage identity and parameter-definition consistency;
numeric type mismatches and missing required values remain compiler diagnostics.
Missing expression references, invalid source pins selected by a connection command,
and recursive function dependencies are rejected. Apply and package validation
retain their strict graph-validation boundary.

`FMaterialGraphDocument` is the shared owning-thread command boundary for both
owners. Ordinary commands use `FGraphEditSession` to mutate the owner's expression
objects directly. Before writing a participant, `Modify()` captures its reflected
members using DurinEd's focused transaction snapshots. Type adaptation registers
its own modified participants. Only changed member payloads enter the global
transactor, grouped into one material change so replay restores all fields before
updating derived state and notifying observers. There is no candidate graph and
no expression duplication during connection commands or their Undo/Redo.
Structural commands also retain collection membership and serialized state for
added/removed children. Deleted children are detached and retained by history's
reference collector, then restored with their original object identity. Unchanged
children remain the same objects. History payloads do not alias mutable live values.
`FMaterialExpressionEditing` provides Engine storage admission, ownership reconciliation,
and publication without invoking the bulk duplicating setters. Abandoned/rejected
edit scopes restore their local writes without publishing revisions or notifications;
this is domain compensation, not a change to generic `Modify()` or `Cancel()` semantics.
Presentation-only changes skip semantic publication. Full presentation replacement
includes labels, so replay can restore an empty name. History enumerates texture,
function, and participant references and reports native payload allocation sizes.
The document has no whole-graph snapshot export/import API. Function previews
construct their own new expressions inside a live edit of the preview material.
Clipboard copy duplicates selected expressions only.
Function state cannot contain root parameters or material outputs. Call inputs
retain numeric defaults while disconnected; connecting another callee output
records its stable output binding in the same transaction.
Call insertion validates required inputs and rejects recursive dependencies before
publishing its concrete expression.
Generic input connection and node removal use the same live edit boundary.
Fixed pins follow reflected connection-member declaration order; Surface override
pins are keyed by attribute and call connections by their input bindings. Deletion
clears references to removed nodes without changing retained numeric defaults.
Deleting a Surface override source removes that binding and restores the base
Surface attribute; Undo restores the original objects and recorded fields. Terminals own their complete `FMaterialFunctionPort`: stable port GUID, type,
name, display order, flags and default. `GetFunctionSignature()` derives and caches
a read-only projection of those nodes; no separate signature is serialized. These commands retain their typed snapshots
for Undo/Redo and provide positions for terminals without saved layout.
Port editing, call insertion and call input connection commands
use stable GUIDs; insertion rejects a dependency closure that would recurse into
the current function. Adding an interface port creates its typed terminal in the
same transaction; output creation requires a source link. Removing a port rejects
if retained graph links still require its terminal. Interface type edits update
terminal types atomically; incompatible retained wiring remains editable with compiler diagnostics. Shared node
creation/replacement/removal and positional-input connection work for both graph
kinds. Surface assignment accepts the full source link, including a
function output GUID. Removing a call also removes its call record and restores
disconnected material output fallbacks. Insertion accepts required input bindings
in the same transaction. Connecting a newly declared output adds its typed call
record without changing existing output identities.

The Content Browser creates and opens `DMaterialFunction` assets in a dedicated
workspace with shared graph commands, geometry, links and clipboard state. Function
edits apply directly to the asset and update loaded callers. Save and Discard use
the ordinary editable-asset document model; relocation preserves document identity
and reload rebinds the open resource. Native document references do not pin the
old asset generation against package replacement.

Function Details edits named typed ports, order, advanced/required flags, typed
defaults, constants, swizzles and Surface attributes. Port and node drafts commit
on Apply as one validated transaction. The canvas supports typed link dragging,
Shift replacement, node movement, copy/cut/paste and function navigation. Numeric
node creation supplies typed inline literal defaults; texture and Surface operations
can be created by dragging a compatible output into the node menu. Movement commits
presentation only on release and Escape cancels its transient positions.

Material and function documents share a call picker that collects required input
connections before insertion. Pin tooltips and selected-call controls display
defaults; optional connected inputs can explicitly return to their defaults.
Diagnostics open the originating function and frame its node by retained identity.

`BuildMaterialFunctionPreview` constructs and publishes concrete expressions into
a transient root material for one stable output GUID. It keeps required values,
callee bindings and output conversion in typed expression fields. Surface outputs connect directly; scalar values splat to RGB, Float2
maps to RG with zero blue, Float3 maps to RGB, Float4 uses RGB, and Texture2D samples
UV0 before RGB display. Numeric and texture wrappers use unlit emissive output.
Required inputs use explicit neutral preview values and optional inputs keep their
declared defaults. The wrapper uses existing compilation, dependency invalidation
and preview rendering. Closing a function document cancels and retires its wrapper.
Preview refresh requests coalesce independently of function revision stamps. Root
and transitive dependency semantic notifications invalidate the wrapper; layout
edits and unrelated functions do not. Output selection, Compile Preview, and
package replacement explicitly request refresh. An unsuccessful preview build
waits for a new invalidation or explicit request instead of retrying every frame.
An unavailable output displays diagnostics without presenting an earlier wrapper
as the selected result.

`DMaterial::GraphPresentation` is a separate `EditorOnly` reflected value. It
contains schema version 2, exactly one integral graph-space position for every
live node GUID, including the material output node. New material and
node creation establish those positions before publication. Sanitization retains
the first valid record and removes duplicate, dangling, invalid, and out-of-range
entries. Read-only inspection supplies deterministic fallback positions for
missing records without persisting them. Presentation is copied by ordinary
reflection but is excluded from program validation, normalized IR, compile
snapshots, shader-map identity, derived data, and the cooked DMAT payload.

MaterialEditor owns transient pan, zoom, hover, selection, marquee, link drag,
and per-document controller state. None of those values are serialized.

## Standard function authoring

`MakeStandardMaterialFunctionExpressions` constructs the seven built-in recipes as
strongly retained concrete expressions whose terminal nodes own port definitions.
Publication duplicates these children through `SetFunctionExpressions`; recipe
objects and published functions never share mutable children. Recipe comparison
uses reflected expression fields and stable port identities, excluding presentation.

The shipped library lives in `/Engine/Materials/Functions` and uses the same
function workspace, typed calls and transactions as user assets. New scene imports
use structural parents with only source-required owners and sample branches.
Open Parent Material on an imported instance to inspect its generated graph.
The explicit `asset material-template` command can create a PBRSurfaceMaterial_MR
parent with independent map/value/UV parameters. Both this template and structural
import parents are built as `FMaterialExpressionRecipe` values, retaining concrete
expression children, typed outputs and GUID-keyed presentation. Applying a recipe
validates and duplicates its children; imported-parent matching compares authored
expression fields and outputs while allowing independent layout edits. It is optional and is not selected
by the current structural importer.
Generated parents use the same Surface root as new materials. Final
numerical output policy belongs to Engine's evaluator, not imported Clamp nodes.
Open a call to edit its function; shared semantic edits update loaded callers and
previews through the ordinary dependency lifecycle. Preserve port GUIDs when
renaming or reordering interfaces. Required inputs are collected during insertion;
optional defaults can remain hidden until needed.

`StandardPBR` keeps independent map and UV inputs. Choose `StandardPBR_ORM` only
when occlusion, roughness and metallic deliberately share one texture and sampling
policy. Its nested SampleORM call exposes three outputs from one fetch. Imported
source channels converted into independent derived textures continue to use the
independent form. Numeric parameters remain declarations of the calling material;
functions never create hidden root declarations.

The maintenance command preserves compatible edits to the library implementation
and refuses incompatible standard signatures or modified historical parent graphs.
Its provenance fields identify the authoring recipe, not a runtime opcode or a
special canvas behavior. Source asset migration is explicit; normal Save/Apply
continues to use the document lifecycle above.

## Inspection and commands

`MaterialGraphOperations.h` is the UI-independent boundary shared by the canvas,
tests, and structured callers. Inspection returns detached deterministic node,
pin, surface-output, presentation, and closed-domain catalog values by reading
the typed expression collection directly. A node descriptor retains identity and
type metadata plus one display payload: constant value, parameter identity, local
sampling metadata, or swizzle components. Connections and defaults reside in pin
views; no universal authored node is copied into the view. Catalog
entries contain a concrete expression class, result type and accepted pin types,
with no universal node template. `CreateCatalogNode()` validates the descriptor
against the catalog and creates the matching expression with typed numeric, UV
and Surface defaults; parameter entries receive unique metadata. `MakeSurface` exposes the aggregate Surface output type;
legacy `StandardSurface` and role-bound `TextureCoordinate` are not authorable
catalog entries.

The catalog retains concrete numeric shapes for inspection and structured callers;
palette search selects one shape per math operation. Before publication, editing
commands infer math result widths only in the downstream closure of changed nodes,
resolving affected dependencies in upstream order. Same-class constant value edits
skip inference. Propagation stops when an adaptive node's output width is unchanged. Newly added
or class-replaced nodes and explicitly authored result widths force propagation
to their consumers. Each inference pass builds temporary
node and reverse-edge indexes; storage checks and publication can still scan the
graph. Undo/Redo replays recorded types without rerunning inference. Linked operands and nonuniform disconnected literals constrain
the width; uniform defaults can collapse to an equivalent scalar when the width
changes. A graph without constraining operands retains its current width. Different
vector widths produce compiler diagnostics, while scalars broadcast. Lerp Alpha
stays scalar; Normalize requires at least two components. These compilation rules
do not block editing. Undo/Redo restores recorded fields, including inferred types.
Append Vector participates in upstream width inference, summing both input widths;
totals above four produce compiler diagnostics. Compilation expands it into existing scalar
selection and vector-construction IR, preserving A-then-B component order.
Graph editing commands turn new narrow parameters into a Float4 owner plus a mask;
templates and imported materials use the same representation. Vector parameter
owners and serialized instance overrides store only Float4 values. Separate
Float2/Float3 parameter classes and the serialized override width field are removed.
Narrow convenience setters pad with zero and getters select the leading components.
Expression compilation lowers scalar broadcasting to typed Splat IR operations,
without creating authored graph nodes.
Fixed numeric inputs, material Surface outputs, Surface attribute overrides and
function input/output ports also broadcast connected scalars to their declared
vector width. Vector-to-scalar and different-vector-width links remain invalid;
Normalize still requires a vector source. Retained defaults keep their declared types.

Shared document inspection resolves function call pins from the live signature,
ordered by display order and GUID. Detached pin records retain stable port GUIDs,
types, names, defaults, required flags and missing-port markers. Function terminals
have typed named pins; Surface attribute pins retain their fixed attribute indices.
The material canvas draws each output separately and preserves the full source
link through drag, reconnection, Surface assignment and node creation.
Callee interface notifications refresh the cached inspection, including interface-only
renames. Surface attribute reconnection uses the same document command boundary.

Commands use node GUIDs, explicit pin indices, parameter GUIDs, and
`EMaterialSurfaceOutput`. They cover creation, complete node replacement,
removal, connection, disconnection, surface assignment, movement, layout,
surface-default edit/reset, parameter promotion, explicit texture-branch
creation, copy, cut, paste, and duplication. Each result reports a stable status,
affected/generated GUIDs, bounded validation diagnostics, and a message; an
automation caller never needs to scrape canvas labels. `SetUseMaterialAttributes`
switches the terminal mode as one undoable edit.
Connection, disconnection, promotion and paste never switch modes or erase the
other connection set. A source incompatible with Surface produces compiler
diagnostics. A disconnected aggregate input uses standard surface defaults,
independently of retained individual defaults. Output package version 2 persists
the mode and is required on load; older output versions are rejected.

Semantic commands record participating objects before changing their live fields
and publish through the owner after storage checks. `ReplaceExpression()` copies
fields into the existing object when the class matches; a class change creates
one replacement object with the same node GUID. Constant value/type and swizzle
edits use typed commands. Function node drafts own a concrete expression
and retain unapplied fields while refreshing shared input edits. Function graph
publication validates dependency closure, including recursive replacement targets.
`CreateExpression()` publishes a concrete candidate with a stable GUID and position.
The former CreateNode, ReplaceNode and ReplaceProgram editor APIs are removed.
New material setup, parameter reachability and texture previews read typed owners;
a resource-only use of a combined sample excludes its unevaluated UV branch. Rejection and no-op results
leave program, presentation, dirty state, transaction history, authored
revision, and compile generation unchanged. A successful semantic command
advances authored state once and schedules compilation through the root material's
edit policy. Immediate callers submit one generation; automatic editor edits
coalesce before submission, and manual edits remain unsubmitted. Presentation-only commands sanitize
and commit positions, mark the package dirty, and never compile or invalidate
render data. Engine presentation setters return `Changed`, `NoChange`, or
`Rejected`; callers consume that result directly instead of comparing revisions.
Whole-presentation writes compare the sanitized state; bounded position writes
validate the complete request before applying it. Parameter definitions and graph
presentation have no public revision counters; observers consume graph changes.

Concrete expression inputs retain numeric defaults, and each of the
eight fixed Surface inputs is optionally connected. Disconnecting or
deleting a surface source clears its link and returns to the retained typed
fallback; ordinary inputs similarly restore retained literals. Required inputs without a fallback remain visible as compiler diagnostics. The
canvas makes input replacement explicit with Shift and uses the same command
result for invalid-target feedback. Both output connection sets remain structurally validated, but only the selected
mode contributes output roots. Normalization unpacks aggregate roots and applies
`IsMaterialSurfaceOutputActive` to both modes: normal, metallic, roughness and AO
require Lit; opacity requires Translucent; opacity mask requires Masked. Unused
property branches and their exclusive parameter bindings are pruned. These rules
use the compiling material variant's settings, including instance overrides.

Imported structural parents use the same Surface as a new material: each input
accepts the final property value. Factor/sample composition lives upstream. Reusable
functions can still return an aggregate Surface through the existing aggregate mode.

## Change observation

`DMaterial` and `DMaterialFunction` publish owning-thread graph changes through
`FMaterialGraphChangeSource`. `FMaterialGraphChangeSet` contains one node list
with GUIDs and combinable Added, Removed, Content, Interface, Inputs, and Position
flags, plus a graph-level Reset flag. Material output connections, defaults and
position produce ordinary node changes; there are no output-specific graph flags.
These are invalidation hints for reading the current owner, not replayable edits
or transaction history. Unknown expression-property changes conservatively
invalidate that node's content, interface, and inputs.

Observation uses exact reflected property payloads without retaining expression
or resource objects. Checkpoints exist only while subscribers are attached;
ordinary setters and reflected property edits compare after publication, so
rejected changes and identical observations emit nothing. Position setters compare
presentation only, without recapturing expression properties. Compilation and
render-state changes are not graph notifications.

`FScopedMaterialGraphChange` batches expression and presentation publication for
commands and history replay; observers see the completed owner state once.
Repeated changes to a node merge, deletion supersedes local edits, an unpublished
addition followed by removal cancels, and removal followed by addition invalidates
the replacement's complete view. Reset supersedes local hints. Reentrant
publications are queued until the current notification finishes, and unsubscribe
also disables callbacks already present in a dispatch snapshot.

`FMaterialGraphReadModel` takes an initial complete inspection, subscribes to its
owner and directly called function interfaces, and accumulates notifications until
refresh. Content changes reread selected nodes; position changes update positions
without rebuilding pins; structural changes rebuild inspection. A callee interface
change refreshes call pins: terminal node events trigger comparison of the derived
interface, without polling revisions or maintaining a graph-level Signature flag. Callee body edits, transitive implementation edits, and
callee layout edits do not invalidate the caller's visible graph; compilation
maintains its separate dependency policy. Committed package replacement refreshes
observed owners after external references have been rewritten, allowing dependency
subscriptions to follow replacement functions.

The canvas and Details share this read model and do not poll compile, render,
expression, schema, or function revisions. Canvas visibility filters and detached
interaction drafts remain UI state. Compiler generations, package save checkpoints,
and optimistic editing-session revisions retain their separate existing roles.

## Compact input and texture authoring

Advanced pins are hidden initially. For inputs, the toggle hides only optional
unconnected inputs with no retained binding. Connected or explicitly bound inputs remain visible; row filtering never
changes stable input indices or function port GUIDs. Material instances retain their
parameter-only editor and expose Open Parent Material for explicit graph navigation.


The shared document commands SetInputDefault, ExtractInputDefault,
and InlineInputNode operate on independent concrete-expression
snapshots for roots and function documents. Numeric defaults use their actual
component widths; the coordinate channel default is scalar, and
function calls select inputs by port GUID. Extraction retains literal fallbacks and adds editable expressions. Inlining accepts
constants and removes a source only after its last consumer is gone. Parameters stay
visible owners: sharing uses explicit links. Each operation commits one validated
Undo/Redo transaction. Function paste rejects all root parameter payloads.

Texture Object Parameter owns a Texture2D resource. Texture Sample Parameter 2D
owns a resource and exposes raw RGBA/R/G/B/A, RGB and Texture2D slot 7.
There is no separate Normal pin, including in the advanced view. RGB automatically decodes tangent-space normals
when the texture parameter's usage is Normal; connect RGB directly to the surface
Normal input. Separate Texture Sample nodes inherit this behavior from their
resource parameter, including resources passed through functions. Raw RGBA and
individual channels remain encoded for channel processing. Decode Normal RG is
compiler-only and has no authored expression or catalog entry. Retired Normal
slot 8 is rejected; normal connections use RGB slot 1.
The resource slot does not execute that node's UV transform or sample. A texture
drop creates one uniquely named owner and sample node atomically, inherits the
asset's texture usage, and selects a flat-normal fallback for normal textures. Selected-node
Details edits node parameter bindings, texture usage, resource, sampler/fallback
policy and numeric defaults. The Parameters panel edits shared names, display
names, group/order and presentation/range hints. Neither panel offers node type
conversion. Both read concrete expression fields and record touched objects only
when an edit is submitted; idle frames do not duplicate managed objects. Parameter metadata
updates preserve sampling expressions and their UV connections. Class replacement
preserves the node GUID and publishes after storage checks. Type changes
which invalidate links remain editable with compiler diagnostics. Resource assignment is undoable.

Texture Sample UV inputs accept Float2 or a scalar broadcast to both coordinates,
and show Mesh UV0 when disconnected.
TextureCoordinates selects a mesh channel and outputs Float2. Scale, offset and
rotation use upstream math expressions or material functions, shared through
explicit connections. Function graphs expose literals and interface ports without
owning root parameters.
Normal texture previews use the Content Browser's source-thumbnail renderer in a
canvas-owned pool shared by its nodes. The canvas pumps that pool even when the
Content Browser is closed, displaying blue-purple source pixels rather than the
two-channel BC5 allocation's yellow. Other texture previews share
published RHI allocations and retire registrations after their last canvas consumer.

## Transactions and gestures

Material graph commands edit live expression objects and retain focused member
before/after payloads in one global transaction. Structural changes additionally
retain membership and added/removed object state. Undo/Redo restores these values
in place and publishes once through the Engine edit boundary. `CreateParameter`, `RenameParameter`
and `DeleteParameter` share the live editing boundary with numeric promotion,
Surface commands, and clipboard paste. Rejected edits add no undo entry. New parameter nodes and constant
promotion reuse an existing case-insensitive name when its type matches, adopting
its parameter GUID, default and shared metadata. Type conflicts reject. Nodes keep
independent GUIDs, positions and connections; the derived instance list has one
entry per parameter. Default and metadata edits synchronize all references in one
transaction. Texture references retain their local sampling inputs.

The selected node's Parameter name field rebinds only that node, creating a fresh
parameter GUID if the name is unused. Rename shared parameter changes all references
without changing the parameter GUID or instance overrides; occupied names reject.
Deleting a node keeps remaining references. DeleteParameter removes all references.
Shared definitions must agree during Engine validation, loading and compilation;
inconsistent snapshots are rejected rather than resolved by traversal order.

Disconnected owners remain editable through selection. Instances retain reachable
controls and inspectable orphan overrides with explicit removal. Constant promotion
preserves the node GUID and links; a new name creates a fresh parameter GUID with
its typed literal as the default, while an existing name adopts that parameter. Surface-output promotion creates a new
owner and link. Texture-branch creation emits one combined texture-sample parameter
with default mesh UV0 and selects its attribute channel, including normal
decode. Resource policy remains on that expression.

One user-visible command produces one global editor transaction. Semantic
commands retain changed reflected member values and necessary structural state; presentation-only
commands retain only changed node positions; parameter-only
commands retain only the parameter GUID and before/after values. Every custom
change reports its owned native allocations to the bounded transaction buffer.
Undo and Redo restore semantic state through the ordinary material mutation
boundary, so semantic restores follow the current compile policy while presentation-only and
parameter-only restores do not. Activating an editable asset document establishes
its package's saved revision checkpoint, and a successful save advances that
checkpoint. Undoing back to the open or most recently saved revision therefore
clears the package dirty state and removes the unsaved marker; Redo marks it dirty
again when it moves away from that checkpoint.

Node and Surface movement use `FMaterialGraphMoveSession`. Pointer-down
captures the selection and presentation, every drag sample previews sanitized
positions, and pointer-up records one applied transaction. Escape, document
switch, deactivation, discard, close, destruction, or stale owner cancels the
gesture and restores its original presentation. Each document owns a distinct
canvas and move session.

Creation-menu results have one local invalidation flag, set when their catalog or
recent-entry ordering changes. Search text and source output type remain explicit
query keys. Each canvas owns its results and clears invalidation after rebuilding.

## Clipboard and layout

The shared graph clipboard releases its retained expression objects before editor
module shutdown.

`FMaterialGraphClipboardPayload` schema 8 contains at most 256 independently
cloned concrete expressions (including terminal port definitions), presentation
labels, relative positions and a weak source-owner identity. Strong expression roots keep
reflected texture and callee references alive independently of the source asset;
reference replacement visits the expression properties directly. Paste clones the
payload again before remapping typed connections, so it cannot mutate the payload.
Clearing the payload releases these roots. Compiler results and viewport state
are excluded.

Copy orders nodes by GUID. Every paste generates fresh node GUIDs. Parameter nodes
reuse a matching destination name and type, adopting its current default/metadata;
an unused name creates a fresh parameter GUID with the copied definition. Repeated
references in one payload remain shared. Same-root duplication therefore shares
existing parameters; a renamed or removed source name follows the same name lookup.
Internal links are remapped together. Same-root external links can remain; foreign external links reject even
when destination GUIDs happen to match. Conflicting shared definitions/types, duplicate node identities, missing
retained resources and unsupported versions reject atomically. Placement, owners,
links and derived schemas commit and Undo/Redo together.

Material and function documents share copy, paste and cut commands. Paste remaps
ordinary inputs, Surface attribute bindings and function-call inputs while keeping
callee port GUIDs and selected output identities. Copied interface terminals gain
new port GUIDs and unique names in the destination function; root parameters cannot
enter a function and terminals cannot enter a material. Removing terminals removes
their port declarations in the same validated transaction. Function dependency
changes reject recursive closures before publication, including clipboard insertion.

Retired StandardSurface and role-dependent TextureCoordinate opcode values are
invalid in both authored graphs and clipboard payloads. Custom parameters are
never remapped into fixed role slots.

A selection containing the active aggregate source records that source; paste
reconnects the remapped Surface node atomically without copying the unique material output
terminal. Cut copies before one validated delete. Duplicate uses the same
payload and paste path with a deterministic offset. The canvas stores this
structured payload directly, so canvas and automation semantics agree.

Automatic layout is presentation-only and deterministic. It derives consumer
edges directly from concrete expression connections, including function-call and
Surface override inputs, and calculates each node's longest distance to a
surface sink, places dependencies before consumers in fixed-width columns, and
uses four forward/backward median sweeps over the same node edges. Stable prior order and GUIDs break all ties. Columns use computed node
heights and fixed gaps; selected-only layout treats every unselected node as an
occupied rectangle and searches downward for the nearest collision-free slot.
It rejects atomically if the bounded search fails. Layout never changes program
order, node IDs, links, outputs, normalized IR, identity, or authored node labels. New material
creation persists the initial complete layout before the asset is first
published. Opening, panning, zooming, selection framing, and diagnostic framing
never synthesize or persist missing positions. Explicit Auto Layout and node or
Surface movement persist presentation through ordinary transactions.

## Related documentation

- [Material System](../../Runtime/Rendering/MaterialSystem.md)
- [Reflected Property Editing](ReflectedPropertyEditing.md)
- [Workspace Framework](WorkspaceFramework.md)
- [Asset Catalog and Mutation](../../Runtime/Assets/AssetCatalogAndMutation.md)

## Native validation

`MaterialGraphEditingTests` owns graph commands, inference, and Undo/Redo;
`MaterialEditorInteractionTests` owns canvas, property-panel, and preview tests;
`MaterialEditingPersistenceTests` owns editing sessions, package roundtrips, and
workspace save/reload. The former `MaterialEditingTests` target is retired.
Run `DevTool test "@domain=material-editing"` for all three, or select one target.
Their finer domains are `material-graph-editing`, `material-editor-interaction`,
and `material-editing-persistence`. Test source changes resolve to their registered
target; production module changes can still select all three.
