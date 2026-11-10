# Async Asset Operations

Summary: Define completion, compensation, and UI ownership for nonblocking editor asset mutations.

Modules: TextureCompressor, AssetForgeBuiltins, DurinEd, TextureEditor, StaticMeshEditor, Engine

Last reviewed: 2026-11-10

## Ownership Layers

Asynchronous asset work crosses three independent concerns:

1. Asset-family compilation domains schedule typed work and return typed products.
2. A family editor adapts typed completion to package save and diagnostics.
3. DurinEd's optional compensating utility remains available for a mutation
   that actually needs external rollback and asynchronous repair.

Typed compilation domains retain their own workers, priorities, cancellation,
and metrics. Direct standalone-family import performs synchronous detached
preparation and explicit setter application or delegates only build work to its typed family
domain. Shared asset import uses `FAssetImportSession` around private captured
values. It does not introduce a generic import job framework.

Texture2D imports commit authored Source/settings/import metadata on the
GameThread and then start independent DDC work. Compiler completion follows
[Asset Compilation](../../../Runtime/Assets/Build/AssetCompilation.md#texture2d-completion).
Request identity and cancellation remain compilation concerns. Texture requests
are serialized per object: a mutation cancels and finishes its predecessor before
changing source/settings. That mutation boundary may block; detached preparation
and the newly submitted build remain asynchronous.

CPU task construction follows the direct-return
[task contract](../../../Runtime/Core/TaskSystem.md): valid ordinary work queues
under scheduler saturation. Family managers retain request-count, compute and
payload budgets before submitting work. These owner limits remain legitimate
domain rejection or pending states.

Every owner must reap failed and canceled terminal tasks even if their body or
optional publication observer never runs. Loading flags, request reservations
and serial records are settled once on the owner thread. Shutdown stops new
requests, cancels or drains accepted tasks, then releases captures and module
code. Thumbnail decoding retains the unique result until this owner-side reap;
current serials alone may publish an upload.

## Asset Import Sessions

AssetForgeBuiltins owns the glTF form; LevelEditor owns the FBX form. Both use
`FAssetImportSession` and advance each frame. The FBX form explicitly selects
`MeshMode = EAssetImportMeshMode::Combined`; the shared session defaults to split outputs. Selecting a
source starts detached capture, dependency discovery and scene decoding. The Ready
phase exposes `PreviewOutputs` for all selected outputs, material configuration
and collision diagnostics using the retained source snapshot and parsed scene.
Both forms use this result; destination and material changes do not decode
geometry again. Coordinate changes create a new session. Before building, the
worker rehashes the captured
closure and rejects changed or missing sources. Preparation retains one bounded
source closure per session and builds products serially to avoid unbounded parallel
texture memory growth.

Workers construct only detached mesh and texture products. Material policy, asset
lookup, candidate creation, dependency binding and graph publication remain on
GameThread. The session yields between candidate operations and observes material
completion with `HasPendingMaterialCompilation`, including canceled requests awaiting
owner-side reap. It does not finish pending compilation from normal UI ticks.

After compilation and final validation, reference replacement applies the complete
peer graph on GameThread and marks imported packages Dirty. Optional persistence
then uses ordinary `DPackage::SaveAsync` tasks; graph preparation never spans disk
I/O. Save failure or cancellation preserves all imported live assets. The import
report separates `ImportedPackages` from optional `SaveResult`; ordinary package
saving provides retry. Reference refresh and render fences settle before retirement.

Progress reports the active phase, activity and available work counts. Cancellation
keeps ticking through worker completion and unpublished candidate cleanup. Closing
the active progress view requests cancellation; host destruction cancels and drains
at its teardown safe point before module or compiler teardown. Worker captures never
refer to widgets. The `ImportAssetOutputs` entry point runs the same
pipeline without yielding for synchronous/headless callers.

Source decoding, product construction and staged writes are nonblocking in the
interactive path. Texture mutation may wait for a canceled predecessor to retire. Material policy checks, individual package
capture and atomic graph commit remain owner-thread operations; the session does
not claim a hard frame-time bound for a single large package.

Duplication returns the new live object and performs no saving. The caller saves
its package separately. Saving failures retain it for ordinary retry, without
another duplication or graph discard.

## Compensating Operation Contract

`FCompensatingAsyncOperation` owns this state transition:

```text
prepare -> async apply -> commit -> succeeded
                |
                +-> rollback -> async compensation -> failed
```

Commit failure takes the same compensation path as apply failure. The primary
error is retained; compensation failure is appended instead of replacing it.
Rollback executes at most once.

The operation is completion-driven. It does not poll a family domain, wait on
a worker, or know about assets, packages, source files, or Widgets. Apply and
compensation callbacks execute on the operation's owning editor thread. Inline
completion is valid and produces the same phase transitions as deferred
completion.

Destroying an active operation detaches its callback owner before requesting
cancellation. An applying operation also rolls back prepared external state.
Explicit abort reports a terminal failure to its UI owner; destruction does
not call presentation callbacks.

## Typed Adapters and UI

Editor async consumers call `DPackage::SaveAsync` with an explicit
`FAssetPackageSaveContext` and retain `Tasks::TTask<FAssetWriteResult>` directly.
The Task represents final persistence and Registry disposition. Texture import
polls `IsCompleted`, reads the result and releases observation after processing it.
Consumers read the native write result without an editor result adapter. The
batch owner presents its successful assets once. There is no editor save-operation
object, result cache, separate completion protocol or synchronous batch-test bypass.
Normal host ticks advance the GameThread continuation. Headless hosts explicitly drain saves as
specified by [Package persistence](../../../Runtime/ObjectSystem/PackagePersistence.md#operation-lifetime-and-completion).
Only a completed save Task can confirm persistence; submission
admission and disk-only waits do not confirm publication.

An asset-family adapter supplies preparation, apply start, commit, rollback,
compensation start, cancellation, and final notification callables. The adapter
may use typed weak object references but cannot make the generic coordinator
retain a reflected object implicitly.

`PrepareTexture2DImport` and texture source translation return `std::expected`
with owned detached values and diagnostic strings. Failure exposes no prepared
value; decoding and validation remain silent. Texture2D reimport returns
`FReimportResult` with `EReimportResult` after committing its authored candidate.
`PostEditChange` triggers the independent derived-data build. DDC failure
or cancellation does not undo the import. Factories do not save; the manager
records optional persistence in `FReimportResult::SaveResult` without changing the
reimport status, message or factory cause. Both save success and failure retain
an owned result; skipped saving has no value. Save failures retain authored data
for ordinary retry, with Dirty state and committed effects determined by saving.
VolumeTexture rebuild returns an owned diagnostic on failure; structured settings
and translation errors remain at validation boundaries. StaticMesh import and
build boundaries use owned diagnostic strings. Completion is queried on the mesh
and observed through the compiling manager; no separate ordinary result or
per-request build callback is retained. Save
failures belong to the upper-layer persistence result. Transient mesh creation
returns the authored object before compilation finishes and preserves the existing
object/garbage-collection ownership.

For direct Texture2D source selection:

- AssetForgeBuiltins captures the selected file without mutating it.
- Engine validates Source, settings and import metadata, drains prior compilation,
  then installs the authored candidate together and marks the package Dirty.
- The adapter calls `PostEditChange`; independent TextureCompressor work builds
  platform data. GameThread application
  installs successful products and calls `DTexture::UpdateResource()`.
- TextureEditor or the reimport manager owns optional save sequencing.
- Failure before authored commit preserves the live asset. Later DDC failure keeps
  new authored data; save failure retains that data for ordinary save retry.
- The Widget owns only the active asset identity, phase label, conflicting
  control state, close rejection, and final diagnostic presentation.
  Texture Editor shows the phase in its toolbar, disables Save while a build is
  pending, and offers cancellation. Normal editor ticks apply completion; the
  interactive UI does not block to wait for the build.

Other asset families can reuse the compensating operation only when they have a
real prepare/rollback/compensate transaction. Direct import itself does not use
this utility: each family captures and validates detached state before its
narrow setter/update seam, then reports synchronous rejection or typed build
completion through its owning module.

Package discard is a separate saved-content recovery operation. Editable asset
documents finish active property input, submit package reload, keep close
confirmation unresolved on failure, rebind every open same-package document to the
new top-level objects during the publication window, then retire the old graph and
establish the new save checkpoint. Texture2D, VolumeTexture, Material, and
MaterialInstance editors share this boundary. The former Material window-open
discard snapshot was removed; unsaved packages receive an explicit rejection.

## Standalone StaticMesh Operations

StaticMesh import/reimport captures and decodes physical input synchronously,
validates canonical Source and provenance, then retires prior work for the selected
mesh. Reconciliation reads the current material bindings after cancellation callbacks
have drained. `SetImportedSource` commits Source, slots and the private import-data
inner together while clearing old render/collision projections in one consumer
refresh. `PostEditChange` starts independent render compilation.

Factories report authored import success with `EReimportResult`; compilation failure,
cancellation or stale work cannot undo it. `FReimportManager` and the dialog own
persistence. Save failure retains the Dirty authored object and reports only a save
cause. Retry Save performs persistence without decoding or rebuilding.

The dialog queries `DStaticMesh::IsCompiling`, explicitly finishes selected work
before persistence and saves even when DDC failed. It owns no compile callback or
result cell. Destruction drains selected work and retains the authored import.
Conflicting controls remain disabled while its object is compiling. Scene's
multi-output transaction retains its separate prepared publication contract.

## Related Documentation

- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/Packages/AssetDataLifecycle.md)
- [CPU Task System](../../../Runtime/Core/TaskSystem.md)
- [Source File Workflows](../../Guides/SourceFileWorkflows.md)
