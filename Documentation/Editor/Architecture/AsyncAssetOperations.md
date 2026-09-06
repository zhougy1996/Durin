# Async Asset Operations

Summary: Define completion, compensation, and UI ownership for nonblocking editor asset mutations.

Modules: TextureBuild, AssetForgeBuiltins, DurinEd, TextureEditor, StaticMeshEditor, Engine

Last reviewed: 2026-09-07

## Ownership Layers

Asynchronous asset work crosses three independent concerns:

1. Asset-family compilation domains schedule typed work and return typed products.
2. A family editor adapts typed completion to package save and diagnostics.
3. DurinEd's optional compensating utility remains available for a mutation
   that actually needs external rollback and asynchronous repair.

Typed compilation domains retain their own workers, priorities, cancellation,
and metrics. Direct standalone-family import performs synchronous detached
preparation and explicit setter application or delegates only build work to its typed family
domain. Scene performs direct synchronous orchestration around private captured
values and does not create a generic import job or operation handle.

Texture2D adapters consume the GameThread terminal result defined by
[Asset Compilation](../../Runtime/Assets/AssetCompilation.md#texture2d-completion).
Request identity, cancellation, and supersession remain compilation concerns.

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

An asset-family adapter supplies preparation, apply start, commit, rollback,
compensation start, cancellation, and final notification callables. The adapter
may use typed weak object references but cannot make the generic coordinator
retain a reflected object implicitly.

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
