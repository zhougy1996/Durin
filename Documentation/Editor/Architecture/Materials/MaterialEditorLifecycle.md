# Material Editor Lifecycle

Summary: Define material preview resources, compilation controls, Apply and Save, working-copy and asset lifetime, and instance parameter overrides.

Modules: MaterialEditor, Engine, DurinEd

Last reviewed: 2026-11-17

Expression ownership and command publication are defined in
[Material Graph Operations](MaterialGraphOperations.md#ownership). Panels and
diagnostic navigation are defined in [Material Graph Canvas](MaterialGraphCanvas.md).

`FMaterialEditor` is the private controller for both material and material-function
sessions. Their reflected factories select Material or Function mode; each
instance owns its document model, profiles, errors and preview state. Materials
and material functions edit isolated transient working graphs. Function Apply
validates and publishes the selected graph to the original function; Save applies
before persisting its source package. Function preview materials remain transient
shader-compilation consumers. Family toolkit IDs,
layout versions (material 4, function 5), profile hashes and existing settings
files remain stable across the controller consolidation.

## Preview Resources

Material Preview acquires shared `/Engine/Models/Sphere` and
`/Engine/Models/Box` StaticMesh assets through the canonical
editor retention service. Multiple documents coalesce by virtual asset identity;
preview creation performs no transient OBJ import, and retained handles provide
the GC lifetime edge. The editor session also owns this fixed pair and prepares
their CPU and GPU resources before opening the shell, so closing every material
document does not evict them. Startup preparation and retirement follow the
[shared preview-mesh lifetime](../Assets/AssetThumbnails.md#identity-and-output-size).

Preview framing fits the selected mesh to both viewport axes with a margin and
adapts to panel aspect-ratio changes. Mouse-wheel zoom is relative to that framing;
Fit and mesh changes restore the fitted view.

Preview rendering follows [Material System](../../../Runtime/Rendering/Materials/MaterialSystem.md).
Each preview document owns explicit Low/High quality and ES3_1/SM5/SM6 feature
selectors. Changing either requests the matching ordinary material variant and
does not alter the asset, global game quality, or RHI capability. Unsupported
feature choices are clamped to the active device capability. Preview, Apply, and
Save continue to use the same compiler lifecycle and accepted-program rules;
there is no editor-only selector evaluator.
Preview selection references an immutable Backend/FeatureLevel/Quality resource.
Changing selectors preserves other configuration resources and their proxies;
resource-specific synchronous preparation does not wait for another configuration.
Thumbnail sessions and texture-node images follow the shared
[texture preview contract](../Assets/AssetThumbnails.md#identity-and-output-size). Texture
nodes sample published built allocations for every usage, including normals;
missing or pending allocations show a placeholder instead of source thumbnails.

## Compile, Apply, and Save

The toolbar exposes Compile and Apply. The user-scoped Auto Compile preference
is edited in Settings > Editor > Material Editor, reached directly through the
material Window menu, and is enabled by default. Opening a base material applies the preference to its working
copy's Engine edit policy. Automatic edits wait for a
400 ms quiet period before compiler-input construction. Each subsequent semantic
edit restarts the deadline. Switching to manual removes scheduled submission;
switching back schedules any unsubmitted edits. Withdrawing an edit deadline
does not deregister a FeatureLevel preparation already in progress. Semantic edits immediately detach obsolete default and FeatureLevel compile
associations while retaining installed previews. Compile submits the
working material through ordinary cache reuse. Running indicators derive from
live resource associations rather than a stored execution enum. Cancel Compile cancels its pending
work and pending Apply intent without blocking for worker retirement. Engine pumps automatic deadlines even for hidden
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

Working-copy checkpoints include Domain as well as static properties, expression
children, roots and presentation. Initialization installs the complete configuration
before copying the graph, so PostProcess scene reads compile in their own domain.
A domain-only change is an unapplied edit and participates in external-source
conflict checks. Apply validates domain/model/blend admission, copies the current
graph and complete configuration in a graph-notification batch, and compiles the
resulting source generation. Save then persists that source; reopening reconstructs
the same domain, model, retained roots and selected shader contract. Preview and
source do not share mutable expression children.


Before applying changes, Save finishes the selected preview's pending compilation,
even when the installed program is already current, then applies it and saves the
source package. It never drains unrelated compiler jobs. Unapplied changes,
preview compilation status, and source disk dirtiness are separate states. The
document unsaved marker includes either unapplied changes or a dirty source
package. Graph validation and dynamic parameter updates remain immediate on the
working copy; they reach the source and its scene dependents only through Apply.

## Reachable parameter views

The Diagnostics panel includes a Statistics section derived from Engine snapshots.
It labels the accepted program as current or last-known-good, shows exact IR,
layout, resource, source, shader-code, dependency, and phase-timing facts, reports
loaded authored-family variant counts, and presents process aggregate compilation
and proxy work. Refreshing the panel performs no compilation, package load, render
resource creation, or owner retention.

MaterialEditor consumes Engine's detached `InspectMaterialParameterDependencies`
projection for reachable instance controls in Parameters. Base Parameters lists
all parameter definitions, including disconnected owners, for shared metadata
editing. Base Details edits the selected owner. Numeric inline edits and Details update the same
node-owned default through parameter commands/sessions. Dynamic value changes do
not request shader compilation. Resource-only combined-node outputs contribute the
resource dependency without traversing the unused sampling UV branch.

Parameter panels subscribe to the current material's owning-thread parameter
invalidation event. Engine propagates definition, value, parent-chain and function
dependency edits to loaded descendants; reflected edits and history replay use the
same mutation boundaries. Package replacement invalidates retained row snapshots.
Stable frames do not traverse parent chains or compare package/render versions.
Position edits and compilation alone do not invalidate parameter rows. A notified
refresh compares schema identities and Engine's immutable reachability snapshot
to decide whether to rebuild dependency rows; value edits reuse that structure.

Instance rows use the resolved root program's same snapshot for override
eligibility and source labels. A local override which becomes unreachable moves
to the collapsed orphan group and remains removable; it is not rendered or
presented as active. Reconnecting the same GUID restores the preserved base
value and makes that override eligible again.

Parameter controls use the shared
[property view and submission API](../PropertyEditing/PropertyDetailsViews.md#view-ownership-and-submission).
Edits snapshot the reflected definition/override collection root and resolve the
entry by GUID in detached storage. The parameter GUID is also the logical
transaction identity, preventing two entries in the same root from coalescing
into one continuous edit. Specialized widgets remain host-owned; submission,
sessions, and history use the shared property pipeline.

## Document and asset lifecycle

MaterialEditor also registers the exact `DMaterialParameterCollection` asset type
with Content Browser creation and a dedicated Material Parameter Collection
workspace. Its bounded reflected declaration list uses the shared property-edit
session and transaction stack, so declaration/default edits, Undo/Redo,
Save/Discard, package reload, relocation and deletion follow the same workspace
lifecycle as other assets. The diagnostics view exposes collection identity,
schema/default revisions and deterministic packed layout without compiling or
creating renderer resources.

Material and function graph creation menus enumerate loaded collection
declarations and create a configured collection expression in one graph command.
The command validates the selected asset/declaration and retains stable GUID
identity; compile diagnostics navigate back to the expression when a dependency is
missing or incompatible.

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

Each function document in `FMaterialEditor` owns one transient duplicate of the
source function and its working package, canvas and derived preview. There is no
separate function Session registry or applied-graph snapshot. The source is a weak
reference to the asset retained by the reflected editor/document roots. Graph,
port and layout edits affect only the draft; ordinary source-package saves and
dependent materials cannot observe them. Working-package transaction checkpoints
are the sole draft dirty authority. Source-package disk dirtiness does not set
the function document's unapplied marker or require a second close confirmation.
The derived preview uses a separate transient package so preview compilation and
presentation mutations cannot dirty the editable draft.

Function Apply checks a source function/package revision token for external
conflicts, validates the prospective dependency closure (including recursion
through the original source identity), then publishes expressions and presentation
through the existing mutation APIs. It preserves source identity, notifies
dependents and leaves the source package dirty without writing disk. Apply marks
the working checkpoint only if notification callbacks did not edit the draft
again. Save applies first and then persists the source. Function Apply is
synchronous; shader compilation stays on the derived transient preview material
and consuming materials. Compiler worker snapshots remain independent of this
editor document state.

Following UE's material-function editor, successful Undo/Redo republishes each
function document touched by that transaction, including after Apply. The toolkit
uses existing transaction notifications, so history replay from another toolkit
also updates affected functions without applying unrelated dirty drafts.

Function Discard drops only the draft and recreates it from the current in-memory
source. Applied-but-unsaved source changes remain dirty and are not reverted.
Explicit package reload remains a separate operation: document admission rejects
a publishing or dirty draft, validates the prepared draft revision, and the normal
reload completion notification rebuilds the clean draft against the replacement.
Relocation preserves the draft and remaps its resource identity and source token.

Publication and drawing hold local document leases across synchronous callbacks;
publication also retains its source. A single scoped publication guard rejects
reentrant Apply, Discard, normal close, function-toolkit history commands and reload. Forced toolkit
close removes document ownership, unregisters history notifications and retires
preview presentation immediately. An executing callback can finish safely but
cannot perform a late Save or resume rendering a retired document. Final document
destruction forgets working-package history and retires its transient packages.

Discard cancels pending Apply. If the source is clean, it simply recreates the
working copy from the source. If the source has unsaved applied changes, the
ordinary package-discard reload restores disk state and rebuilds the working
copy. External package replacement also resets the working copy; relocation
retains it while remapping the existing document. Deletion and module shutdown
release the working copy with its canvas and preview. Material-instance documents
continue editing their own overrides live and see base edits only after Apply.
Material callbacks remain synchronous. Runtime discovery is sealed before
notifications, and edits performed by listeners use fresh batches; see
[scoped material queries](../../../Runtime/Rendering/Materials/MaterialQueries.md#external-notifications-and-reentrancy).

Canvas maps use document IDs, so selection cannot leak across materials or
instances. User-scoped editor profiles retain panel preferences and graph pan/zoom
by resource path. Same-profile windows merge resource edits rather than overwriting
each other's cached settings. Relocation updates live and stored viewport keys;
layout-profile identity remains independent of resource names. Window IDs are
runtime-only. Persistence and the intentional removal of old layout namespaces
follow [asset editor layout profiles](../Framework/AssetEditors.md#panels-and-tool-extensions).

Session document controllers observe authoritative Engine relocation and remap
resource IDs, per-resource document keys and labels without changing document
identity. Material toolkit callbacks rekey their loaded objects and editing state.
A module-scoped observer separately updates stored profile viewport records and
unregisters when the module retires. There is no local
asset catalog mirror. Deletion is observed through the existing object handle;
before drawing, an invalid owner closes its document and releases canvas and
preview state. Active graph gestures are canceled before switch, discard,
close, deletion cleanup, module unload, or shutdown.

## Instance Rendering Configuration

MaterialEditor exposes independent blend, shading, mask-threshold, two-sided and
depth-write override flags and values. Clearing a flag resumes inheritance while
preserving its authored inactive value. Effective supplying-owner labels come
from the bounded runtime resolver. Edits use the shared reflected whole-member
transaction path, so Undo/Redo and direct setters trigger the same owner revision
and variant rules. Toolbar cancellation/recompile, diagnostics, last-known-good
status and thumbnail readiness refer to the instance owner. Submitted work stays
queued in the manager until execution; it never requires an editor capacity retry. Graph navigation still belongs
to the root material document.
