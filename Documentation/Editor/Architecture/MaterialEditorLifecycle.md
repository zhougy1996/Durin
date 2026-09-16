# Material Editor Lifecycle

Summary: Define material preview resources, compilation controls, Apply and Save, working-copy and asset lifetime, and instance parameter overrides.

Modules: MaterialEditor, Engine, DurinEd

Last reviewed: 2026-09-16

Expression ownership and command publication are defined in
[Material Graph Operations](MaterialGraphOperations.md#ownership). Panels and
diagnostic navigation are defined in [Material Graph Canvas](MaterialGraphCanvas.md).

## Preview Resources

Material Preview acquires shared `/Engine/Models/Sphere` and
`/Engine/Models/Box` StaticMesh assets through the canonical
editor retention service. Multiple documents coalesce by virtual asset identity;
preview creation performs no transient OBJ import, and retained handles provide
the GC lifetime edge. The editor session also owns this fixed pair and prepares
their CPU and GPU resources before opening the shell, so closing every material
document does not evict them. Startup preparation and retirement follow the
[shared preview-mesh lifetime](AssetThumbnails.md#identity-and-output-size).

Preview framing fits the selected mesh to both viewport axes with a margin and
adapts to panel aspect-ratio changes. Mouse-wheel zoom is relative to that framing;
Fit and mesh changes restore the fitted view.

Preview rendering follows [Material System](../../Runtime/Rendering/MaterialSystem.md).
Thumbnail sessions follow [Asset Thumbnails](AssetThumbnails.md).

## Compile, Apply, and Save

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

## Reachable parameter views

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
