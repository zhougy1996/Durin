# Material Graph Operations

Summary: Define the shared MaterialEditor command, presentation, canvas, transaction, clipboard, diagnostic, and document-lifecycle contracts for authored material programs.

Modules: MaterialEditor, Engine, DurinEd

Last reviewed: 2026-09-14

## Ownership

`DMaterial` and `DMaterialFunction` own their respective authored graphs. Material
programs and function graph nodes/pins are bounded values
addressed by stable GUID and input/output indices rather than `DObject`
subobjects or canvas coordinates. Material instances never own or edit a graph.
Base-material documents edit a transient working `DMaterial`; Apply transfers
its authored state to the existing package-owned source material. Graph commands
remain reusable against either owner and never implicitly select a source asset.
Working state includes owned node payloads and function call bindings.
Initialization and Apply commit program owners and call references through
one validated Engine boundary, so a changed callee without changed node IDs is
still an unapplied edit. Invalid call records leave the source unchanged.
Ordinary semantic commands validate the owner's call records alongside its
program, allowing node edits and Undo/Redo in materials that already use functions.

`FMaterialGraphDocument` is the shared owning-thread command boundary for both
owners. It captures program nodes with owned parameters, function interface/calls and
presentation, validates candidate state through the owning Engine asset, and
records a complete undoable change. Function state cannot contain root parameter
declarations or Material Output bindings. Transaction reference collection retains
function and texture dependencies. Material canvas semantic commits use this same
boundary. Interface replacement, call insertion and call input connection commands
use stable GUIDs; insertion rejects a dependency closure that would recurse into
the current function. Adding an interface port creates its typed terminal in the
same transaction; output creation requires a source link. Removing a port rejects
if retained graph links still require its terminal. Interface type edits update
terminal types atomically and reject incompatible retained wiring. Shared node
creation/replacement/removal and positional-input connection work for both graph
kinds. Material Output assignment accepts the full source link, including a
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

`BuildMaterialFunctionPreview` constructs a transient root material for one stable
output GUID. Surface outputs connect directly; scalar values splat to RGB, Float2
maps to RG with zero blue, Float3 maps to RGB, Float4 uses RGB, and Texture2D samples
UV0 before RGB display. Numeric and texture wrappers use unlit emissive output.
Required inputs use explicit neutral preview values and optional inputs keep their
declared defaults. The wrapper uses existing compilation, dependency invalidation
and preview rendering. Closing a function document cancels and retires its wrapper.
An unavailable output displays diagnostics without presenting an earlier wrapper
as the selected result.

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

## Standard function authoring

The shipped library lives in `/Engine/Materials/Functions` and uses the same
function workspace, typed calls and transactions as user assets. ImportedSurface
shows eight texture sample parameters, an explicit normal decoder and an ImportedSurfaceValues call: ten expression nodes plus Material Output.
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

## Editor panels

Each material document owns an isolated ImGui dock space, keyed by its stable
`DocumentKey`, using the shared WorkspaceUI panel and docking helpers. Preview,
Material Graph, Details and Diagnostics are dockable windows. Wide initial
layouts reserve the left 30% for Preview above Details; the graph fills the remaining width and height. Diagnostics shares the
Details dock as an optional tab; small initial layouts use dock tabs. Resizing does not rebuild a
user's arrangement. Hidden document roots keep their dock spaces alive.

Function insertion opens a bounded popup above the graph. Selected-call navigation
lives in Details alongside the shared input editor, so selection does not resize
or displace the canvas. Details owns selected-node authoring and instance
inheritance, rendering properties and parameter overrides. There is no base-material
Parameters manager or docking slot. New materials contain only
Material Output with eight property inputs and retained defaults, without expression
nodes or function calls.

Window controls reopen optional panels and reset the default layout. Material
Info is a collapsed section in Details shown only when no graph node is selected.
Base-material values and metadata are edited through selected parameter owners
and literal inputs in Details. Instances retain
their parameter override list because they do not own a graph. Parameter groups
omit a sole outer container with no direct values.
ImGui persists docking geometry; the
material session settings retain panel visibility and per-asset graph viewports.
Preview visibility follows the actual preview panel, including dock-tab hiding.

## Preview Resources

Material Preview acquires shared `/Engine/Models/Sphere` and
`/Engine/Models/Box` StaticMesh assets through the canonical
editor retention service. Multiple documents coalesce by virtual asset identity;
preview creation performs no transient OBJ import, and retained handles provide
the GC lifetime edge.

Preview framing fits the selected mesh to both viewport axes with a margin and
adapts to panel aspect-ratio changes. Mouse-wheel zoom is relative to that framing;
Fit and mesh changes restore the fitted view.

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

Shared document inspection resolves function call pins from the live signature,
ordered by display order and GUID. Detached pin records retain stable port GUIDs,
types, names, defaults, required flags and missing-port markers. Function terminals
have typed named pins; Surface attribute pins retain their fixed attribute indices.
The material canvas draws each output separately and preserves the full source
link through drag, reconnection, Material Output assignment and node creation.
Callee authored revisions refresh the cached inspection, including interface-only
renames. Surface attribute reconnection uses the same document command boundary.

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

Program schema 7 permits typed retained numeric defaults on ordinary inputs and makes each of the
eight fixed Material Output inputs optionally connected. Disconnecting or
deleting a surface source clears its link and returns to the retained typed
fallback; ordinary inputs similarly restore retained literals. Required inputs without any fallback still reject. The
canvas makes input replacement explicit with Shift and uses the same command
result for invalid-target feedback. Aggregate and per-property sources cannot
coexist in a valid program.

## Compact input and texture authoring

Advanced pins are hidden initially. For inputs, the toggle hides only optional
unconnected inputs with no retained binding. Connected or explicitly bound inputs remain visible; row filtering never
changes stable input indices or function port GUIDs. Material instances retain their
parameter-only editor and expose Open Parent Material for explicit graph navigation.


The shared document commands SetInputDefault, ExtractInputDefault,
InlineInputNode and ExtractUVSettings operate on roots and function documents.
Extraction retains literal fallbacks and adds editable expressions. Inlining accepts
constants and removes a source only after its last consumer is gone. Parameters stay
visible owners: sharing uses explicit links. Each operation commits one validated
Undo/Redo transaction. Function paste rejects all root parameter payloads.

Texture Object Parameter owns a Texture2D resource. Texture Sample Parameter 2D
owns a resource and exposes RGBA/RGB/R/G/B/A/RG slots 0–6 plus Texture2D slot 7.
The resource slot does not execute that node's UV transform or sample. A texture
drop creates one uniquely named owner and sample node atomically. Selected-node
Details edits name, display name, type, group/order, presentation/range hints,
texture usage, resource, sampler/fallback policy and numeric defaults. Type changes
which invalidate links reject with diagnostics. Resource assignment is undoable.

Connected UV replaces local literal settings. Extracting unconnected settings
creates TextureCoordinates with the retained literals; parameterized UV uses
explicit owners connected to its channel/scale/offset/rotation inputs. Function
graphs expose literals and interface ports without owning root parameters.
Texture previews share published RHI allocations and retire registrations after
their last canvas consumer.

## Transactions and gestures

`ReplaceProgram` commits graph owners and references through one Engine validation
boundary and one transaction; the presentation overload includes placement. Undo/Redo
retains the graph, nested texture references and presentation. The shared document
transaction also retains function calls. Storage accounting includes nested arrays
and strings. `CreateParameter`, `RenameParameter` and `DeleteParameter` create, edit
or remove the owning node. Invalid candidates add no undo entry. Names and parameter
GUIDs must be unique; there is no name-based reuse.

Disconnected owners remain editable through selection. Instances retain reachable
controls and inspectable orphan overrides with explicit removal. Constant promotion
preserves the node GUID and links while creating a fresh parameter GUID and carrying
its typed literal into the owned default. Surface-output promotion creates a new
owner and link. Texture-branch creation emits an explicit resource owner, UV channel,
sample and required swizzle/decode operations; resource policy remains on the owner.

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

`FMaterialGraphClipboardPayload` schema 6 contains at most 256 complete nodes with
owned payloads, relative positions, function ports/calls and a weak source-owner
identity. Its strongly retained reference object owns reflected texture/function
slots so package replacement updates dependencies independently of the source asset.
Paste resolves these slots before validating owners. Clearing the payload releases
them. Compiler results and viewport state are excluded.

Copy orders nodes by GUID. Every paste generates fresh node and parameter GUIDs and
unique names, preserving copied default/metadata values. Internal links are remapped
together. Same-root external links can remain; foreign external links reject even
when destination GUIDs happen to match. Duplicate owner identities/names, missing
retained resources and unsupported versions reject atomically. Placement, owners,
links and derived schemas commit and Undo/Redo together.

Material and function documents share copy, paste and cut commands. Paste remaps
ordinary inputs, Surface attribute bindings and function-call inputs while keeping
callee port GUIDs and selected output identities. Copied interface terminals gain
new port GUIDs and unique names in the destination function; root parameters cannot
enter a function and terminals cannot enter a material. Removing terminals removes
their port declarations in the same validated transaction. Function dependency
changes reject recursive closures before mutation, including clipboard insertion.

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

Parameter creation exposes Scalar, Vector2, Vector3, Vector4 and texture owner
entries. Selecting an entry creates and places a fresh uniquely named owner in one
transaction. The catalog has no existing-parameter rebind mode. Sharing connects the
existing owner's output to more consumers. Inspection reads labels from the node's
owned payload. PBR roles and UV controls are explicit template-owned parameters,
not distinct node kinds.

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
node as one command; missing numeric inputs receive inline literal defaults
in the same transaction, while resource inputs without a default reject.
Escape and every document lifecycle cancellation close the palette and discard
reconnection, movement, and inline edit drafts without dirtying or compiling
the material. Every mutation still routes to the stateless
`FMaterialGraphOperations` operation boundary.

`Promote to Parameter` is available on an unconnected Material Output input. It
creates the compatible material-owned Parameter node one column upstream, copies the
fallback into the definition value, connects the input, and records program,
presentation, and value as one Undo/Redo transaction. `Add Texture` explicitly
creates one TextureSampleParameter2D with local UV settings and connects its
RGB or scalar channel output directly. Normal creates a texture object parameter feeding the shipped
SampleNormal function, which owns sampling, decoding, and strength processing. The entire branch and connection form one candidate-validated
Undo/Redo transaction. Extract UV settings only when explicit coordinate
expressions are needed; texture objects remain available for function inputs
and independent sampling. Sampling nodes show RGB, R, G, B, A, and RGBA in that
order by default. The Advanced pins toggle reveals RG and Texture resource outputs;
connected outputs remain visible even when advanced pins are hidden. Display order
and visibility never change serialized output indices.

The toolbar exposes Compile, Apply, and a user-scoped Auto Compile preference,
enabled by default. Opening a base material applies the preference to its working
copy's Engine edit policy. Automatic edits wait for a
400 ms quiet period before compiler-input construction. Each subsequent semantic
edit restarts the deadline. Switching to manual removes scheduled submission;
switching back schedules any unsubmitted edits. Compile immediately submits the
working material through ordinary cache reuse. Cancel Compile cancels its pending
work and pending Apply intent. Engine pumps automatic deadlines even for hidden
documents. Closing the document cancels compilation and retires the working copy.
Reopening or package replacement creates a fresh working copy with the preference.

Apply requests compilation if necessary and publishes after the current preview
has compiled successfully. Editing again while Apply is waiting cancels that
publication intent. A failed or canceled compilation leaves the source unchanged.
Apply refuses to overwrite authored source changes made outside the document;
relocation or sibling-package edits with unchanged source values are allowed.
Publication updates the existing source through material mutation boundaries and
submits its changed root and loaded dependent variants together. Scene references
keep their identity and runtime compilation retains its last-good failure policy.
Apply does not save the package to disk.

Save finishes the selected preview's compilation, applies it, then saves the
source package. It never drains unrelated compiler jobs. Unapplied changes,
preview compilation status, and source disk dirtiness are separate states. The
document unsaved marker includes either unapplied changes or a dirty source
package. Graph validation and dynamic parameter updates remain immediate on the
working copy; they reach the source and its scene dependents only through Apply.

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

MaterialEditor consumes Engine's detached `InspectMaterialParameterDependencies`
projection for reachable instance controls. Base Details edits the selected owner,
including disconnected owners. Numeric inline edits and Details update the same
node-owned default through parameter commands/sessions. Dynamic value changes do
not request shader compilation. Resource-only combined-node outputs contribute the
resource dependency without traversing the unused sampling UV branch.

Instance rows use the resolved root program's same snapshot for override
eligibility and source labels. A local override which becomes unreachable moves
to the collapsed orphan group and remains removable; it is not rendered or
presented as active. Reconnecting the same GUID restores the preserved base
value and makes that override eligible again.

## Document and asset lifecycle

Base-material documents own canvases; instance documents retain the parameter
override workflow and direct users to the root base material for graph editing.
`FMaterialEditingSession` strongly owns the working material and its unique
transient package under the source mount. The transient material is not a
top-level asset export. That package supplies isolated transaction revisions;
it is never saved or used as the document resource identity. Undo/Redo edits the
working copy, including after Apply, and does not implicitly republish to the
source. Apply invalidates the source's saved checkpoint; successful Save marks
both source and working checkpoints. Closing or resetting the working copy
forgets its transaction records and cancels its compile jobs before retirement.

Discard cancels pending Apply. If the source is clean, it simply recreates the
working copy from the source. If the source has unsaved applied changes, the
ordinary package-discard reload restores disk state and rebuilds the working
copy. External package replacement also resets the working copy; relocation
retains it while remapping the existing document. Deletion and module shutdown
release the working copy with its canvas and preview. Material-instance documents
continue editing their own overrides live and see base edits only after Apply.
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
