# Async Asset Operations

Summary: Define completion, compensation, and UI ownership for nonblocking editor asset mutations.

Modules: TextureBuild, AssetForgeBuiltins, DurinEd, TextureEditor, AssetCore

Last reviewed: 2026-08-27

## Ownership Layers

Asynchronous asset work crosses three independent concerns:

1. Asset-family compilation domains schedule typed work and publish typed products.
2. A family editor adapts typed completion to package save and diagnostics.
3. DurinEd's optional compensating utility remains available for a mutation
   that actually needs external rollback and asynchronous repair.

Typed compilation domains retain their own workers, priorities, cancellation,
and metrics. Direct standalone-family import performs synchronous detached
preparation and publication or delegates only build work to its typed family
domain. Scene performs direct synchronous orchestration around private captured
values and does not create a generic import job or operation handle.

## Texture2D Compilation Completion Contract

`FTexture2DCompilationResult` identifies one terminal outcome as `Succeeded`, `Failed`,
`Canceled`, or `Superseded` and carries a bounded diagnostic. An accepted
compilation request invokes its `FTexture2DCompilationCompletion` exactly once
on the contributing domain's completion thread. Rejection before acceptance is returned
synchronously and does not invoke completion.

Supersession is terminal, not silent callback disposal. When a family admits a
new request for an identity that already has active work, it cancels the old
worker and completes the old observer as `Superseded`. A late worker completion
for that generation cannot publish or complete the observer again.

The terminal contract is not a generic request scheduler or observation
handle. A family domain still decides whether requests are identified by
asset, generation, opaque serial, or another typed key.

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
- TextureBuild prepares and publishes the Texture2D candidate.
- TextureEditor saves the package after successful publication.
- A pre-publication failure preserves the live asset; save failure leaves the
  valid published state Dirty for retry.
- The Widget owns only the active asset identity, phase label, conflicting
  control state, close rejection, and final diagnostic presentation.

Other asset families can reuse the compensating operation only when they have a
real prepare/rollback/compensate transaction. Direct import itself does not use
this utility: each family captures and validates detached state before its
narrow publication seam, then reports synchronous rejection or typed build
completion through its owning module.

## Related Documentation

- [Asset Data Lifecycle and Storage](../../Runtime/Assets/AssetDataLifecycle.md)
- [CPU Task System](../../Runtime/Core/TaskSystem.md)
- [Source File Workflows](../Guides/SourceFileWorkflows.md)
