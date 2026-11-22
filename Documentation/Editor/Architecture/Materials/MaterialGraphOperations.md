# Material Graph Operations

Summary: Define shared MaterialEditor expression ownership, inspection, commands, texture and UV input authoring, material functions, transactions, and clipboard contracts.

Modules: MaterialEditor, Engine, DurinEd

Last reviewed: 2026-11-23

## Task routing

| Task | Read |
| --- | --- |
| Expression ownership, snapshots, or function publication | [Ownership](#ownership) |
| Built-in function recipes or imported parent authoring | [Standard function authoring](#standard-function-authoring) |
| Graph change notifications and cached inspection | [Change observation](#change-observation) |
| Graph inspection, node creation, or connection commands | [Inspection and commands](#inspection-and-commands) |
| Inline defaults, texture parameters, or UV connections | [Compact input and texture authoring](#compact-input-and-texture-authoring) |
| Undo/Redo or movement sessions | [Transactions and gestures](#transactions-and-gestures) |
| Copy/paste, duplication, or deterministic layout | [Clipboard and layout](#clipboard-and-layout) |
| Panels, canvas geometry, menus, or diagnostic navigation | [Material graph canvas](MaterialGraphCanvas.md) |
| Preview meshes, Compile/Apply/Save, asset lifetime, or instance controls | [Material editor lifecycle](MaterialEditorLifecycle.md) |

Read the relevant section and its required dependencies; these documents are not
an ordered reading sequence.

## Ownership

`DMaterialGraph` holds a weak asset owner and the stateless schema selected for its graph kind. `FMaterialGraphSchema` is an abstract interface;
`FMaterialSchema` and `FMaterialFunctionSchema` override graph-specific capabilities
and creation eligibility. The graph exposes this policy through `GetSchema()`. Creation actions call the editor command to instantiate expressions. The graph synchronizes when the asset publishes the edit.
`CanCreateConnection`, `BreakPinLinks` and `GetGraphContextActions` are virtual extension points. Schema owns creation eligibility, function-port and parameter
capabilities, material-output deletion protection, source-address validity,
connection replacement rules, and type compatibility hints. Menus and commands
consume these policies; direct expression creation and paste also check the
graph-kind restrictions. Graph owns connection writeback. The editor passes the working material directly
to its configuration and shared parameter operations.
Type compatibility is advisory during editing: structurally valid links may be
retained for compiler diagnostics. Hidden output pins remain command-addressable;
the visible read model is not the authority for connection admission. Schema
contains no ImGui or workspace navigation services.

`DMaterial` and `DMaterialFunction` own concrete `DMaterialExpression` children.
Expression connections use stable GUIDs and output indices; presentation stores
GUID-keyed positions and labels separately from expression payloads. Material instances never own or edit a graph.
Each base material owns its root input fields, packed connection, mode and root
position. Its expression collection holds the body. `SetMaterialExpressions`
validates the candidate and remaps every body and root connection against the
same source-to-copy map before publishing. `CaptureRootInputs()` returns a
detached value for bulk graph assignment. Property queries provide access to
individual inputs. Rejected assignments preserve the material.

The editor projects one root for each base material, including an empty expression
body. Every root pin uses a material-root address with an `EMaterialProperty`
key, including `MP_MaterialAttributes` for the packed input. Root pin
`SourceIndex` selects a row in the domain's input property list. Commands,
selection and reconnection use the property key across pin reconstruction. Root
input edits record the material before changing its fields. A published root-input
change rebuilds the root pins and their links. A root-position change updates
presentation only. Working-copy Apply compares root bindings by upstream
expression identity and includes the root coordinates in its save checkpoint.

`DMaterial::Domain` selects Surface or PostProcess output definitions. The read-only
`GetExpressionInputDescription` returns the stored connection, numeric input
when applicable, type and name for every supported property in either mode.
`IsRootInputVisible` applies the domain and mode when projecting canvas pins. Surface
admits DefaultLit, Unlit and ClearCoat. PostProcess admits opaque Unlit. Its root
shows Emissive Color in individual mode or Material Attributes in packed mode.
The mode switch is editable in Surface. Both root representations and their
inactive links/defaults persist across domain/model/output-mode changes. Engine's
attribute registry and selected execution contract determine which values compile.
WPO follows the property-address contract and
[vertex evaluation](../../../Runtime/Rendering/Materials/MaterialVertexEvaluation.md).

`MaterialEditing::SetConfiguration` validates the domain and complete
static properties before recording one command change. Undo/Redo restores both
through `DMaterial::SetMaterialConfiguration`, retaining expression and parameter
identities. Invalid PostProcess/model/blend combinations leave owner and history
unchanged; identical writes create no history. The Engine setter publishes the
complete pair before advancing one authored generation and scheduling one request.
Static-property setters delegate to that same admission boundary.

The details panel exposes Surface/PostProcess and DefaultLit/Unlit/ClearCoat.
Selecting PostProcess supplies Unlit/Opaque/disabled-depth-write settings. Other
unsupported combinations produce diagnostics. Instance shading overrides use the
same model names and existing validation. Domain/model changes invalidate the
output node's Content and Interface, including a domain-only Surface/Unlit to
PostProcess switch. Canvas refresh consumes these hints rather than polling.

Base-material documents edit a transient working `DMaterial`; Apply transfers
its authored state to the existing package-owned source material. Graph commands
remain reusable against either owner and never implicitly select a source asset.
Working-copy initialization and Apply duplicate concrete expression children through
`SetMaterialExpressions`, including call bindings and typed terminal defaults.
Source and preview never share mutable expression children. The applied checkpoint
retains independent expression copies and compares their reflected authored fields,
domain, static properties, outputs and presentation. A changed callee with unchanged node
IDs is still an unapplied edit. Invalid candidates leave the source unchanged.
Semantic commands preserve storage identity and parameter-definition consistency;
numeric type mismatches and missing required values remain compiler diagnostics.
Missing expression references, invalid source pins selected by a connection command,
and recursive function dependencies are rejected. Apply and package validation
retain their strict graph-validation boundary.

`DMaterialGraph` owns structural edits for both material and material-function graphs. Commands use `FGraphEditSession` and `FMaterialExpressionEditing` for
live edits, ownership reconciliation, and publication. Transaction capture,
rollback, and replay follow [Transactions and gestures](#transactions-and-gestures).
The graph owns clipboard commands and supplies a lightweight command object with the same weak owner for asset commands. The public command object has no raw connect or disconnect entry points.
New graph nodes allocate their pins with `AllocateDefaultPins()`. Existing nodes use `ReconstructNode()` to reread the authored links, defaults and function port IDs. Both paths dispatch the virtual `CreateInputPins()` and `CreateOutputPins()` hooks. The material root specializes input creation for its domain and output mode. The read model copies these node views for full and partial refresh. Details routes pin default changes through the owning node. A Set Material Attributes node uses schema admission for existing pins and checks the same type and cycle rules before adding a new input. Choosing a source creates the binding and connection in one edit. Choosing the base value removes the binding and its pin. Undo restores the attribute selection and its previous connection. An open Set prototype starts with standard material defaults.
Each graph node owns transient editor pins. `GetInputPin` and `GetOutputPin` take
display ordinals. A pin's signed 32-bit `SourceIndex` identifies the authored
slot, with -1 for a signature port that has no call binding. Function outputs
use the call's binding order, texture outputs use channel indices, and selected
material attributes use their property slots. After allocating the nodes and
pins, the graph resolves each visible input's expression reference and output
index and fills both pins' `LinkedTo` lists. Pin pointers are borrowed until
reconstruction or removal. The canvas receives detached copies of the resulting
endpoints.

The canvas resolves gesture endpoints to live pins when previewing or committing a wire.
Schema checks graph membership, direction, types, occupancy and upstream cycles
through the pins' linked neighbors. Graph writes the selected output's expression
and signed source slot to the input. A newly exposed function port acquires a
binding in the same transaction as the connection. Breaking an output clears all
its consumers together, including material root inputs retained in a hidden
output mode. Low-level asset repair retains address-based entry points.

A partial refresh builds descriptors only for the requested expressions and uses their original asset ordinals for default positions.
Details resolves the selected graph node and follows the input pin's link to label and locate the upstream output.
Function calls reconstruct their pins after signature changes.
`BreakPinLinks` disconnects one input or all authored consumers of an output in one undo step.
Disconnected function inputs keep their inline defaults. Function previews
create their own expressions; clipboard copy duplicates only selected expressions.

Function state cannot contain root parameters or material outputs. Call inputs
retain numeric defaults while disconnected; connecting another callee output
records its stable output binding in the same transaction.
Call insertion publishes a referenced function node before inputs are connected.
Editing-mode dependency validation retains storage, recursion and closure bounds;
compilation-mode validation additionally checks function bodies and required bindings.
Insertion, nested edits and clipboard paste use editing mode, so unfinished
dependencies remain editable. Compilation, preview and package validation stay strict.
`DMaterialGraph::RemoveNodes` resolves the selection to expression objects and
clears each stored input that references a removed object, including hidden root
properties. `DMaterialGraphNode::InlineInputNode` copies a connected constant
into its input default and deletes the constant only after its last consumer is
gone. Both operations record the complete edit in one transaction.
Fixed pins follow reflected connection-member declaration order; Surface override
pins are keyed by attribute and call connections by their input bindings. Deletion
clears references to removed nodes without changing retained numeric defaults.
Deleting a Surface override source removes that binding and restores the base
Surface attribute; Undo restores the original objects and recorded fields. Terminals own their complete `FMaterialFunctionPort`: stable port GUID, type,
name, display order, flags and default. `GetFunctionSignature()` derives and caches
a read-only projection of those nodes; no separate signature is serialized. These commands record the authored objects
for Undo/Redo and provide positions for terminals without saved layout.
Port editing, call insertion and call input connection commands
use stable GUIDs; insertion rejects a dependency closure that would recurse into
the current function. Adding an interface port creates its typed terminal in the
same transaction; outputs may be created before a source is connected. Removing a port rejects
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
defaults, constants, swizzles and Surface attributes. Port drafts commit on Apply
as one validated transaction. Details calls the selected terminal's `OnRenameNode` for a
name change and `SetFunctionPort` for a combined edit. Both preserve the port GUID.
`OnRenameNode` checks the expression's `CanRenameNode` and uses its
`GetEditableName` and `SetEditableName` within the graph edit. Function input and
output expressions opt in and map the editable name to their own `Port.Name`.
The function asset validates the signature and publishes changes, including Undo/Redo.
Caller graphs refresh their GUID-addressed pins from that publication. Ordinary node
properties use the shared Details editor. The canvas supports typed link dragging,
Shift replacement, node movement, copy/cut/paste and function navigation. Numeric
node creation supplies typed inline literal defaults; texture and Surface operations
can be created by dragging a compatible output into the node menu. Movement commits
presentation only on release and Escape cancels its transient positions.

Material and function documents create function calls through the canvas search
menu or an asset drop. Nodes appear at the requested graph position and expose
the live signature for subsequent wiring. Pin tooltips and selected-call controls
display defaults; optional connected inputs can explicitly return to their defaults.
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
and the graph session state. These interaction values are transient.

## Standard function authoring

`AssetForgeBuiltins` owns the PBR parameter roles, UV parameter declarations,
persistent GUIDs, surface-to-parameter mapping, and default parameter recipes in
`PBRMaterialParameters.h`. Runtime material types expose only generic parameters,
expressions, and surface properties; importing a built-in recipe is an explicit
editor dependency. Existing parameter identities remain stable for saved instances.

`GetStandardMaterialFunctionInterface` declares the three shipped function interfaces
without constructing expression objects. Both the authoring recipes and
`LoadStandardMaterialFunctions` use this contract. Loading compares the asset's
signature directly, preserves implementation edits, and fails on missing assets
or incompatible interfaces without synthesizing a reference graph.

`MakeStandardMaterialFunctionExpressions` constructs the three built-in recipes as
strongly retained concrete expressions whose terminal nodes own port definitions.
Publication duplicates these children through `SetFunctionExpressions`; recipe
objects and published functions never share mutable children. Recipe comparison
uses reflected expression fields and stable port identities, excluding presentation.
SampleORM uses the sampler's scalar channel outputs directly; SampleNormal keeps
the flat-normal blend value inline. The three shipped function graphs have
automatic layout applied, and their function port GUIDs remain unchanged.

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
Call Details display inherited numeric defaults without creating overrides. Dynamic
defaults such as UV0 and another input are shown as descriptions until explicitly
overridden. Numeric edits preserve untouched components, and Use function default
removes the local value override.

The shipped library contains `UVTransform`, `SampleNormal`, and `SampleORM`.
SampleORM is useful when occlusion, roughness and metallic share one texture and
sampling policy. Whole-surface functions are authored as needed within a project;
the engine no longer ships StandardPBR or StandardPBR_ORM assets or recipes.
Numeric parameters remain declarations of the calling material; functions never
create hidden root declarations.

The maintenance command preserves compatible library implementation edits and
refuses missing assets or incompatible standard signatures. Source asset migration
is explicit; normal Save/Apply continues to use the document lifecycle above.

## Inspection and commands

The canvas, tests, and structured callers share the graph creation and replacement commands. The read model snapshots nodes, pins, material outputs and presentation from the synchronized editor graph. A node descriptor retains identity and
type metadata plus one display payload: constant value, parameter identity, local
sampling metadata, or swizzle components. Connections and defaults reside in pin
views; no universal authored node is copied into the view. Catalog
entries contain a concrete expression class, result type and accepted pin types,
with no universal node template. `CreateCatalogNode()` validates the descriptor
against the catalog and creates the matching expression with typed numeric, UV
and attribute defaults; parameter entries receive unique metadata.
`MakeMaterialAttributes` exposes the aggregate MaterialAttributes output type;
legacy `StandardSurface` and role-bound `TextureCoordinate` are not authorable
catalog entries.

Engine class-associated descriptions own names, search aliases, input labels and
authored shapes; see [expression descriptions](../../../Runtime/Rendering/Materials/MaterialExpressionBuilding.md#class-associated-authoring-descriptions).
`MaterialGraphExpressionRegistry` binds those descriptions to editor category,
creation kind and palette policy by reflected class. Initialization validates
unique registrations and complete direct-creation shapes once; class and
description pointers are borrowed for process lifetime. Catalog enumeration
visits registered class shapes, and inspection identifies existing expressions
by their actual class even when the caller supplies a filtered catalog. It does
not construct probe expressions. Collection output types come from their live
declarations; function ports/calls and Surface pins retain instance/GUID behavior.

Class plus result type identifies a creation shape. Opcode remains operation
semantics for numeric rules and stale-payload validation. Creation action IDs use
class names and shape widths (adaptive palette shapes share a class action).
Ordinary and context-aware creation use the registered class factory inside graph
commands; typed function/port/collection payloads still supply required context,
and validation precedes transaction publication. Constants have separate concrete
class registrations; narrow parameter shapes still create their typed swizzle
projection. Internal MIR operations have no registration. Adding an ordinary
expression requires its Engine class adapter, editor policy and Build method,
with no opcode switch in catalog construction, inspection or creation.

`TextureCoordinates` is the single authored mesh-UV expression and always returns
Float2. Both "Texture Coordinates" and "UV Channel" palette searches find it;
compiler capture emits the internal `UVChannel` operation. The duplicate
`DMaterialExpressionUVChannel` class is removed. Repository assets contain no
references to that class; packages outside the canonical corpus using it require
rebuilding and have no automatic class migration.

Asset-bound collection reads use their asset-aware creation action; internal MIR
operations are not expression catalog entries. Parameters, Functions, and Vertex
have dedicated creation-menu categories. Palette search applies class registration policies
rather than maintaining a separate opcode list.

The catalog retains concrete numeric shapes for inspection and structured callers;
palette search selects one shape per math operation. Before publication, editing
commands infer math result widths only in the downstream closure of changed nodes,
resolving affected dependencies in upstream order. Same-class constant value edits
skip inference. Propagation stops when an adaptive node's output width is unchanged. Newly added
or class-replaced nodes and explicitly authored result widths force propagation
to their consumers. Each inference pass indexes upstream expression objects and their consumers,
including pending rewires, and resolves output types by output index. Graph
synchronization follows the output pins’ `LinkedTo` lists to refresh consumer
pin types. Storage checks and publication can still scan the
graph. Undo/Redo replays recorded types without rerunning inference. Linked operands and nonuniform disconnected literals constrain
the width; uniform defaults can collapse to an equivalent scalar when the width
changes. A graph without constraining operands retains its current width. Different
vector widths produce compiler diagnostics, while scalars broadcast. Lerp Alpha
stays scalar; Normalize and Reflect require at least two components. Dot,
Distance, Length, and Cross retain their exact geometric width and semantic
constraints rather than using component-wise adaptive coercion. The catalog and
creation API expose the complete pure-math/geometric family as concrete reflected
classes; inspection, clipboard, transactions, automation, and diagnostic
navigation consequently use the same generic paths as existing expressions.
These compilation rules
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

The graph node resolves function call pins from the live signature,
ordered by display order with ties in signature order. Detached pin records retain
stable port GUIDs, types, names, defaults, required flags, and value constraints.
On authored load or a signature change, `UpdateFromFunctionResource` restores
input bindings by port ID and rewrites consumers' output indices from the old
and new output tables. Deleted ports lose their connections. Explicit
`SetMaterialFunction` replacement migrates same-name ports between functions.

Property pins expose registry stage semantics: Vertex for WPO and
Pixel/Tangent/Normal for Normal. Function terminals and call pins expose unconstrained,
non-spatial, or exact stage/kind/space constraints. The canvas formats those fields
in pin tooltips and uses Engine diagnostics for rejected semantic links, so it does
not duplicate the semantic algebra. Function terminals
have typed named pins; attribute selectors and overrides retain stable GUIDs.
The material canvas draws each output separately and preserves the full source
link through drag, reconnection, Surface assignment and node creation.
Callee interface notifications refresh the cached inspection, including interface-only
renames. Surface attribute reconnection uses the canvas schema rules.

Interactive creation invokes `FMaterialGraphCreationAction::PerformAction` on the session graph. The action resolves the live output pin and applies the schema creation rules before the graph adds expressions and their connections.
Its action has a stable ID, display/search metadata, graph applicability, and a
catalog shape, function asset path, or typed terminal payload. `CanCreate` and
`PerformAction` share input compatibility; function insertion selects the first compatible
signature input, including scalar broadcast. Creation and its optional source link
publish once and occupy one undo entry. Function calls use the current asset signature and asset-level dependency validation before publication.

`FMaterialGraphPinAddress` identifies fixed inputs/outputs, function GUID ports,
material root properties and aggregate Surface inputs. The graph resolves these
addresses and shares source validation, replacement policy, default preservation,
and transaction publication. `CanCreateConnection` returns the admission decision and
reason. `TryCreateConnection` returns true when it changes a link. The canvas
and Details request error text for failed edits. Repeating an existing link
leaves the error text empty and undo history untouched. `BreakPinLinks` follows the same
change and error convention. Callers use `Input`, `Output` and `MaterialOutput`
address constructors. Details uses the node commands `SetAttributeInput` and `ClearAttributeInput` for optional attribute bindings.
Graph change notifications drive pin and downstream type refresh after a connection edit. Type compatibility guides palette filtering, automatic connections,
and connection admission. The internal asset writer also supports incomplete graphs for compiler diagnostics. Commands also use parameter GUIDs and `EMaterialProperty`. They cover creation, complete node replacement,
removal, connection, disconnection, surface assignment, movement, layout,
surface-default edit/reset, parameter promotion, explicit texture-branch
creation, copy, cut, paste, and duplication. Graph creation returns the primary graph node.
Callers that select auxiliary
nodes can request them from the creation action. Paste, duplicate, and input
promotion return the ordered list of created graph nodes. Failures carry the
command status, validation diagnostics, and message. `SetUseMaterialAttributes`
switches the material root mode as one undoable edit.
Connection, disconnection, promotion and paste never switch modes or erase the
other connection set. A source incompatible with Surface produces compiler
diagnostics. A disconnected aggregate input uses standard surface defaults,
independently of retained individual defaults. Material output version 6 stores the mode and both connection sets on the material. The editor opens converted v6 packages through the regular loader.

Semantic commands record participating objects before changing their live fields
and publish through the owner after storage checks. `ReplaceExpression()` copies
fields into the existing object when the class matches; a class change creates
one replacement object with the same node GUID. Constant value/type and swizzle
edits use typed commands. Material and function nodes share the selection Details
editor and submit ordinary properties directly through transactions; there is no
separate function-node draft or Apply Node step. Function graph
publication validates dependency closure, including recursive replacement targets.
`CreateExpression()` publishes a concrete candidate with a stable GUID and position.
The former CreateNode, ReplaceNode and ReplaceProgram editor APIs are removed.
Creating a Static Bool assigns its declaration GUID at command time, derives a
unique root-local name, and commits node storage, presentation, dirty state, and
history atomically. Static Switch, Quality Switch, and Feature Level Switch use
the same catalog, search, schema, clipboard, transaction, and canvas paths as
other expressions; their reflected result type and branch payloads are edited in
the shared Details panel. Failed creation or replay publishes none of those
effects. Static declarations are available in material graphs and functions may
consume selector semantics through the ordinary shared compiler boundary without
owning root declarations.
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

The twelve named numeric material roots retain independent constants. Removing an
upstream expression clears the wire and exposes the enabled constant or the
property default. The individual Normal root always falls back to (0, 0, 1).
Extract creates a tangent-space Normal constant node. Inline is unavailable
because disconnecting would discard the node's effective value. A disconnected Make pin uses the property default. An open Set
assignment inherits its prototype value. The canvas uses Shift for explicit
replacement and reports incompatible links through the graph command result.
Asset validation checks stored links in both root modes. Both domains compile
the stored mode. The
effective variant properties, including instance overrides, determine the active
roots. Normalization removes unused property branches and parameter bindings.

Imported structural parents use the same Surface as a new material: each input
accepts the final property value. Factor/sample composition lives upstream. Reusable
functions can still return an aggregate Surface through the existing aggregate mode.

## Change observation

`DMaterial` and `DMaterialFunction` publish owning-thread graph changes through
`FMaterialGraphChangeSource`. `FMaterialGraphChangeSet` contains one node list
with GUIDs and combinable Added, Removed, Content, Interface, Inputs, and Position
flags. Graph flags identify Reset, RootInputs and RootPosition. Material output
connections, defaults and mode changes invalidate RootInputs. Root movement
invalidates RootPosition. The editor resolves these flags to the projected node
in each observing graph.
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

`DMaterialGraph` observes its asset and directly called functions. On publication
it updates positions in place, reconstructs changed nodes and consumers of changed
output interfaces, and reconciles membership. A reset synchronizes the whole graph.
The graph is queryable as soon as a published edit or history replay returns.
Function signature changes refresh the corresponding call sites. The graph rebinds
dependency observers after membership and interface changes, including package replacement.
Each `FMaterialGraphReadModel` coalesces graph notifications and copies synchronized
views on demand. Reading a snapshot never reconstructs a graph node.
Canvas and Details edit constant values and Swizzle channels on the live graph node.
A same-width constant edit records its value. A width change replaces the concrete
constant expression under the same GUID and records inferred downstream types in
the same transaction. Swizzle edits record the channel sequence and dependent widths together.
Root defaults use the node's `PinDefaultValueChanged`; resetting a root input uses
`SetPinUseConstant(false)` to disable its retained literal.

The canvas and Details share this read model and do not poll compile, render,
expression, schema, or function revisions. Canvas visibility filters and detached
interaction drafts remain UI state. Compiler generations, package save checkpoints,
and optimistic editing-session revisions retain their separate existing roles.

## Compact input and texture authoring

Command results expose one authoritative `Status`: `Succeeded`, `NoChange`,
`Rejected` or `StaleOwner`. `operator bool`, `HasError()` and `GetStatus()` use
that status alone. The graph change notification identifies nodes that require refresh.
Editor commands create an owned `Message` at the rejection boundary, formatting
underlying Engine, object, asset or transaction errors there. Callers branch on
status, never on text. Command results do not retain implementation-specific error
enums, nested cause objects or copies of rejected requests.

`Diagnostics` preserves material validation evidence, including node/port and
function-call locations used for navigation. Non-finite constants, dependency
validation and binding failures retain those diagnostics. Diagnostic evidence does
not independently change command status. `FormatMaterialGraphCommandResult`
combines the command message and the first material diagnostic. Successful
results format to empty text. `ReportCommand` uses
this display boundary.

Constant, swizzle, connection, catalog, function-interface and inline-input
rejections preserve authored state and history. Function loading reports the
underlying path/load explanation, including when creation has a connected source.
`CanCreate` remains an availability predicate rather than the execution validator.
Failure messages own their text and survive request mutation, rollback and retry.

Function previews reject missing outputs before editing and bound input types
before indexing preview caches. Dependency failures preserve material diagnostics.
Static-property application retains its typed Engine result, formatted by the
preview command after graph commit. Expired graph owners return `StaleOwner`.

Advanced pins are hidden initially. For inputs, the toggle hides only optional
unconnected inputs with no retained binding. Connected or explicitly bound inputs remain visible; row filtering never
changes stable input indices or function port GUIDs. Material instances retain their
parameter-only editor and expose Open Parent Material for explicit graph navigation.


Nodes accept pin edits through `PinDefaultValueChanged` and `SetPinUseConstant`. The node commands `ExtractInputDefault` and `InlineInputNode` group changes to expression membership, values, links and presentation. Numeric defaults use their actual
component widths; the coordinate channel default is scalar, and
function calls select inputs by port GUID. Extraction retains literal fallbacks and adds editable expressions. Inlining accepts
constants and removes a source only after its last consumer is gone. Parameters stay
visible owners: sharing uses explicit links. Each operation commits one validated
Undo/Redo transaction. Function paste rejects all root parameter payloads.

Numeric pin access traverses `FMaterialNumericInput.Connection` explicitly.
Definition defaults are queried rather than copied into every newly created pin.
Editing a literal enables `UseConstant`, including when it equals the inherited
value. Reset clears that flag without changing the connection or retained
constant; re-enabling restores the retained value. Details distinguishes inherited
and explicit values. Connecting, disconnecting and deleting an upstream node
preserve the flag and constant. Transactions and clipboard copies carry the
complete reflected input state. Paste checks the clipboard schema version.

Texture Object Parameter owns a Texture2D resource. Texture Sample Parameter 2D
owns a resource and exposes raw RGBA/R/G/B/A, RGB and Texture2D slot 7.
Engine's `EMaterialSampleOutput` and `GetMaterialSampleOutputs()` define stable
sampling output identities, types, component selection and display order.
Commands, type inference, inspection and compilation share this definition through
`FindMaterialSampleOutput()`. Numeric outputs retain slots 0–5; retired slot 6 is
invalid, and resource slot 7 exists only on Texture Sample Parameter 2D. Connection
commands reject unavailable selectors before mutating the graph.
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

Texture Sample UV inputs accept Float2 or a scalar broadcast to both coordinates.
Disconnected UVs inherit Mesh UV0 unless an explicit constant is enabled.
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

Material and graph editing commands select ETransactionMode::Record by
default and resolve the active editor before editing. UI callers do not pass
transactors. Missing or busy editor services reject the command without mutation.
Explicit ETransactionMode::NotTransactable permits standalone edits without
creating history, but rejects an existing GUndo. Both modes validate the proposed
edit before publishing dirty state and compilation revisions.
Read-only inspection, copying and layout calculation need no transaction service.
Move gestures retain their original service and use a private position submission
path; their Commit never redirects to a replacement editor. Working-copy checkpoint
ownership and explicit low-level gesture bindings are separate from command modes.

Same-class constant edits, ordinary pin defaults and constant toggles, swizzle masks, function port rename/reorder, and ordinary single or bulk link edits record the authored expressions
with `DObject::Modify(false)` in one `FScopedTransaction`. Link and numeric default edits determine affected inputs, downstream widths and retained defaults from the proposed values before recording. Creating or removing a function binding keeps the compound edit path.
No-op and invalid requests return before marking the package dirty. Successful replay publishes the settled collection through the owner's graph change source.

A structural edit records the material or function before changing its ordered
`ExpressionCollection` and presentation. It calls `Modify(false)` on changed
expressions and on each expression before marking it as garbage. Compound edits
may validate provisional fields and restore them before recording the baseline.
These temporary values end with the command. The shared transaction buffer owns
the undo data, including the liveness transition and hard references across GC.
Native completion repairs expression ownership before graph observers read it.
An external bulk replacement retires the old children. History requiring a
retired child fails preflight even when its replacement carries the same GUID.

The Details panel binds scalar and vector constant expressions to the shared
`IDetailsView`. Their reflected values use whole-object recording and publish
through the expression's property notification. Graph commands coordinate type
adaptation, expression membership and shared parameter references within one
validated publication and compensation boundary.
Sparse GUID-keyed presentation gestures preserve unrelated positions/labels;
parameter gestures replay through the owner's expression-backed parameter setter.

Material-instance color overrides and graph color defaults share the color picker
and its popup lifetime. Both preview while the popup is open and commit one undo
step on normal dismissal. Escape or losing the edited row restores the original
value without history. Instance colors use the reflected override array and
linear LDR controls. Graph defaults use the parameter setter and linear HDR
controls, including numeric RGB values above one.

Graph scalar, vector and color controls collect input during drawing and use
the [shared frame settlement](../PropertyEditing/ReflectedPropertyEditing.md#edit-session-lifecycle)
after all rows are drawn. The current parameter's final value and terminal action
are processed before applying a new parameter's proposal, regardless of draw
order. The graph's parameter session still owns preview, rollback and history
through the domain command path.
These retained custom responsibilities and participants are listed in the
[transactor consumer audit](../Transactions/Transactors.md#structural-history-and-barriers).

Abandoned or rejected edit scopes restore local writes without revisions or
notifications. This domain compensation does not change generic `Modify()` or
`Cancel()` semantics. Presentation-only changes skip semantic publication;
presentation replacement includes labels so replay can restore an empty name.

Structural and expression changes share the global object transaction. `CreateParameter`, `RenameParameter`
and `DeleteParameter` share the live editing boundary with numeric promotion,
Surface commands, and clipboard paste. Rejected edits add no undo entry. New parameter nodes and constant
promotion reuse an existing case-insensitive name when its type matches, adopting
its parameter GUID, default and shared metadata. Type conflicts reject. Nodes keep
independent GUIDs, positions and connections; the derived instance list has one
entry per parameter. Default and metadata edits synchronize all references in one
transaction. Texture references retain their local sampling inputs.

Parameter resolution and same-class expression assignment return the ordinary
command result directly. Parameter creation returns an owned expression or a
failure message; a failed creation publishes no expression. Definition and value
validation remain typed in Engine and are formatted at the editor boundary.

Failed graph history admission restores the original graph without publishing
revisions. A subsequent valid commit can be undone. Assignment retains scope
rollback, including after a valid retry following a class mismatch. Snapshot,
property-copy and history admission failures include their underlying formatted
explanation. After admission and a successful model write, scope destruction
publishes history. A publication failure leaves the authored graph edit applied
and is reported by the transactor.

A failed parameter gesture commit keeps its preview and session active, allowing
cancellation or retry, without marking the package dirty. A failed move commit
keeps its draft without publishing positions. Rejected move previews preserve the
previous draft. A semantic revision change cancels the move. A released drag
keeps its final positions while the transactor is busy and commits when it
can record again. The move and parameter sessions return error text on
failure. Their Apply methods also report whether the preview changed.

Direct parameter commands open their recording before publishing a changed value.
If a synchronous observer cancels that recording, the command reports the
cancellation and attempts to restore the original value. Its result includes
any restoration failure. A committed change produces one undo entry. Observers
can see preview and restoration writes while package revisions are suppressed.

A failed parameter Apply leaves the session available for retry. A failed restoration
keeps the parameter session active for retry. These results own their
messages independently of subsequent retries. Engine parameter mutation and
transaction replay continue to expose their own typed error contracts.

The selected node's Parameter name field rebinds only that node, creating a fresh
parameter GUID if the name is unused. Rename shared parameter changes all references
without changing the parameter GUID or instance overrides; occupied names reject.
Deleting a node keeps remaining references. DeleteParameter removes all references.
Shared definitions must agree during Engine validation, loading and compilation;
inconsistent snapshots are rejected rather than resolved by traversal order.

Disconnected owners remain editable through selection. Instances retain reachable
controls and inspectable orphan overrides with explicit removal. Constant promotion
preserves the node GUID and links; a new name creates a fresh parameter GUID with
its typed literal as the default, while an existing name adopts that parameter.

The root property context menu offers two editor-only shortcuts. **Promote to Parameter**
creates a parameter from an unconnected property's default and inserts a channel mask
when the widths differ. **Add Texture** creates a texture sample parameter using mesh
UV0 and connects the property-specific channel, replacing any existing link. For
Normal it selects normal-map sampling and a flat-normal fallback. Normal requires
typed tangent-space input, so ordinary numeric parameter promotion is unavailable.
World Position Offset accepts vector parameter promotion. Its vertex input cannot
use the shortcut's implicit-derivative texture sample. Specular, Clear Coat and
Clear Coat Roughness accept scalar parameters and use R for the texture shortcut. Each shortcut
commits node placement, expressions and the root connection in one transaction.

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

Material and function node movement use `FMaterialGraphMoveSession`. Pointer-down
captures the selection and semantic revision; drag samples validate detached position
drafts displayed by the canvas. Pointer-up merges only drafted coordinates into the
current presentation through `DMaterialGraph::MoveNodesInternal` and records one
position-delta transaction. On the first move of a node without an authored position,
the transaction captures its displayed coordinates before writing. Undo removes the
new position entry, or restores those coordinates if a later label needs the entry. Escape, document
switch, deactivation, discard, close, destruction, or stale owner discards the draft
without changing the asset or unrelated presentation edits. Each document owns a
distinct canvas and move session. Structural and label edits retain full presentation
state through the graph edit session.

Creation-menu results have one local invalidation flag, set when their catalog or
recent-entry ordering changes. Search text and source output type remain explicit
query keys. Each canvas owns its results and clears invalidation after rebuilding.

## Clipboard and layout

`DMaterialGraph` owns copy, cut, paste and duplicate for material and function
drafts. Cut composes copy and removal. Duplicate copies the selection and pastes
it at the requested offset in one history step. Catalog lookup and search belong
to private Material Editor code. `MoveNodes` accepts the projected material root alongside expressions. A single coordinate change record restores all moved positions. It writes root coordinates on the material and expression coordinates in the presentation table.
`DMaterialGraph::CalculateLayout` reads node sizes from the current graph and follows
stored expression inputs, including hidden root properties. It returns a detached presentation. `Layout` applies the
calculated positions in one transaction. The canvas captures stable IDs from the returned nodes for its selection.
A position command with no changes leaves undo history untouched.

Move, layout and selection copy return success as a boolean. Callers that display
errors can supply an optional error string, which is empty on success. Layout
calculation and copy return their data through the existing output parameters.
Copy clears the payload on entry and retains any nodes duplicated before a failure. Rejected
paste restores the unfinished edit scope and adds no history entry. Failed moves
do not publish positions. Failed layout calculations retain the current authored
presentation after valid-owner resolution; successful calculation remains detached
from live authored positions.

The shared graph clipboard releases its retained expression objects before editor
module shutdown.

`FMaterialGraphClipboardPayload` schema 14 contains at most 256 independently
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
Copy maps selected expressions to clipboard objects. Paste maps clipboard objects
to destination expressions. Both passes preserve the stored int32 output index,
including the material root's aggregate Surface connection. An external input
survives a same-root paste only while its exact source object is still a member.
A foreign external input rejects the paste even if a node GUID matches. Conflicting shared definitions/types, duplicate node identities, missing
retained resources and unsupported versions reject atomically. Placement, owners,
links and derived schemas commit and Undo/Redo together.

Paste remaps
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

Material and function documents share automatic layout and position-delta history.
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

- [Material System](../../../Runtime/Rendering/Materials/MaterialSystem.md)
- [Reflected Property Editing](../PropertyEditing/ReflectedPropertyEditing.md)
- [Workspace Framework](../Framework/WorkspaceFramework.md)
- [Asset Catalog and Mutation](../../../Runtime/Assets/Packages/AssetCatalogAndMutation.md)

## Native validation

`MaterialGraphEditingTests` owns graph commands, inference, and Undo/Redo;
`MaterialEditorInteractionTests` owns canvas, property-panel, and preview tests;
`MaterialEditingPersistenceTests` owns editing sessions, package roundtrips, and
workspace save/reload. The former `MaterialEditingTests` target is retired.
Run `DevTool test "@domain=material-editing"` for all three, or select one target.
Their finer domains are `material-graph-editing`, `material-editor-interaction`,
and `material-editing-persistence`. Test source changes resolve to their registered
target; production module changes can still select all three.
