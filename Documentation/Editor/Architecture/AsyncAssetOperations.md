# Async Asset Operations

Summary: Define completion, compensation, and UI ownership for nonblocking editor asset mutations.

Modules: TextureBuild, AssetForgeBuiltins, DurinEd, TextureEditor, StaticMeshEditor, Engine

Last reviewed: 2026-09-22

## Ownership Layers

Asynchronous asset work crosses three independent concerns:

1. Asset-family compilation domains schedule typed work and return typed products.
2. A family editor adapts typed completion to package save and diagnostics.
3. DurinEd's optional compensating utility remains available for a mutation
   that actually needs external rollback and asynchronous repair.

Typed compilation domains retain their own workers, priorities, cancellation,
and metrics. Direct standalone-family import performs synchronous detached
preparation and explicit setter application or delegates only build work to its typed family
domain. Scene uses a family-specific `FSceneImportSession` around private captured
values. It does not introduce a generic import job framework.

Texture2D adapters consume the GameThread terminal result defined by
[Asset Compilation](../../Runtime/Assets/AssetCompilation.md#texture2d-completion).
Request identity, cancellation, and supersession remain compilation concerns.

CPU task construction follows the direct-return
[task contract](../../Runtime/Core/TaskSystem.md): valid ordinary work queues
under scheduler saturation. Family managers retain request-count, compute and
payload budgets before submitting work. These owner limits remain legitimate
domain rejection or pending states.

Every owner must reap failed and canceled terminal tasks even if their body or
optional publication observer never runs. Loading flags, request reservations
and serial records are settled once on the owner thread. Shutdown stops new
requests, cancels or drains accepted tasks, then releases captures and module
code. Thumbnail decoding retains the unique result until this owner-side reap;
current serials alone may publish an upload.

## Scene Import Sessions

LevelEditor owns a `FSceneImportSession` and advances it each frame. Selecting a
source starts detached capture, dependency discovery and scene decoding. The Ready
phase exposes material configuration using the retained source snapshot and parsed
scene; destination and material changes do not decode geometry again. Coordinate
changes create a new session. Before building, the worker rehashes the captured
closure and rejects changed or missing sources. Preparation retains one bounded
source closure per session and builds products serially to avoid unbounded parallel
texture memory growth.

Workers construct only detached mesh and texture products. Material policy, asset
lookup, candidate creation, dependency binding and graph publication remain on
GameThread. The session yields between candidate operations and observes material
completion with `HasPendingMaterialCompilation`, including canceled requests awaiting
owner-side reap. It does not finish pending compilation from normal UI ticks.

Private-package persistence uses `FPreparedAssetSave`: capture on GameThread,
background protected staging, then prepare and commit reference replacement on
GameThread. Graph preparation happens after staging because its membership snapshot
must not span unrelated object creation during disk I/O. Each committed package is
final; cancellation or failure includes earlier successes in `SavedPackages`.
Reference refresh and render fences settle before retiring replaced graphs.

Progress reports the active phase, activity and available work counts. Cancellation
keeps ticking through worker completion and unpublished candidate cleanup. Closing
the active progress view requests cancellation; host destruction cancels and drains
at its teardown safe point before module or compiler teardown. Worker captures never
refer to widgets. The compatibility `ImportSceneAssets` entry point runs the same
pipeline without yielding for synchronous/headless callers.

Source decoding, product construction, compilation waits and staged writes are
nonblocking in the interactive path. Material policy checks, individual package
capture and atomic graph commit remain owner-thread operations; the session does
not claim a hard frame-time bound for a single large package.

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

AssetTools saves consume `DPackage::SaveAsync` with an explicit
`FAssetPackageSaveContext`. The typed task represents final persistence and
Registry disposition. Editor `FAssetSaveOperation::Complete` converts that result
and emits notifications once; it never initiates publication. Normal host ticks
advance the GameThread continuation. Headless hosts explicitly drain saves as
specified by [Package persistence](../../Runtime/Core/PackagePersistence.md#operation-lifetime-and-completion).
Do not replace confirmation-dependent import saves with admission-only
`SAVE_Async`; accepted direct I/O cannot justify an editor Persisted result.

An asset-family adapter supplies preparation, apply start, commit, rollback,
compensation start, cancellation, and final notification callables. The adapter
may use typed weak object references but cannot make the generic coordinator
retain a reflected object implicitly.

`PrepareTexture2DImport` and texture source translation return `std::expected`
with owned detached values and typed domain errors. Failure exposes no prepared
value; decoding and validation remain silent. Texture2D reimport submission
returns `expected<void, FTexture2DSubmissionError>` for admission only. Final
application and save results still arrive through the existing completion
callback. StaticMesh and VolumeTexture rebuild errors retain nested completion
and save causes, including observed write effects; an error does not undo live
state already applied. Transient mesh creation returns the created object pointer
on success and preserves the existing object/garbage-collection ownership.

For direct Texture2D source selection:

- AssetForgeBuiltins captures the selected file without mutating it.
- TextureBuild prepares a detached Texture2D platform-data candidate.
- Engine rechecks request/object/source/settings identity before the first live
  mutation, then applies explicit source/settings/platform setters and calls
  `DTexture::UpdateResource()`.
- TextureEditor owns Dirty and save sequencing after successful application.
- A pre-application failure preserves the live asset; save failure leaves the
  valid applied state Dirty for retry.
- The Widget owns only the active asset identity, phase label, conflicting
  control state, close rejection, and final diagnostic presentation.

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

StaticMesh import/reimport captures and decodes physical input into canonical
values synchronously, then submits Engine compilation. Default object-returning
factories and bool/save APIs explicitly finish the selected object. The import
dialog opts into deferred completion; factory reimport uses it by default.
Neither path finishes unrelated assets or delegates Scene's transaction to the
manager.

Provenance is prepared as a private owned inner. Engine validates it and all
captured mesh facts before mutation, then installs its pointer with the complete
source/render/collision candidate before one registered-consumer refresh.
Cancellation, supersession and failure preserve old provenance and never save.
Successful application marks the package dirty before the adapter saves it;
save failure retains the applied state and reports a retryable save failure.
The dialog's Retry Save performs only persistence, without rebuilding.

The dialog disables conflicting input/import controls and explicit close while
its request is pending. It defers persistence when asset mutation is disallowed.
Its completion captures weak operation state, never a Widget pointer. Destruction
detaches UI ownership, cancels its own request and explicitly drains the selected
completion before callback code can unload. Factory reimport callbacks capture
no factory pointer. This is direct apply-then-save sequencing; it does not use
the compensating utility or roll back a successfully applied mesh.

## Related Documentation

- [Asset Data Lifecycle and Storage](../../Runtime/Assets/AssetDataLifecycle.md)
- [CPU Task System](../../Runtime/Core/TaskSystem.md)
- [Source File Workflows](../Guides/SourceFileWorkflows.md)
