# Async Asset Operations

Summary: Define completion, compensation, and UI ownership for nonblocking editor asset mutations.

Modules: AssetBuildCore, TextureBuild, AssetForge, DurinEd, TextureEditor, AssetCore

Last reviewed: 2026-08-23

## Ownership Layers

Asynchronous asset work crosses three independent concerns:

1. Asset-family services schedule typed work and publish typed products.
2. DurinEd coordinates editor mutations that commit after success or compensate
   after failure.
3. Asset editors adapt their family and source policy to those generic
   contracts and present progress.

These layers do not share a scheduler. `AssetBuildCore` supplies the
family-neutral terminal result vocabulary, while TextureBuild, GeometryBuild,
and other producer modules retain their own workers, priorities, cancellation,
metrics, and publication rules.

## Build Completion Contract

`FAsyncBuildResult` identifies one terminal outcome as `Succeeded`, `Failed`,
`Canceled`, or `Superseded` and carries a bounded diagnostic. An accepted build
request invokes its `FAsyncBuildCompletion` exactly once on the contributing
service's completion thread. Rejection before acceptance is returned
synchronously and does not invoke completion.

Supersession is terminal, not silent callback disposal. When a family admits a
new request for an identity that already has active work, it cancels the old
worker and completes the old observer as `Superseded`. A late worker completion
for that generation cannot publish or complete the observer again.

The terminal contract is not a generic request scheduler or observation
handle. A family service still decides whether requests are identified by
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

The operation is completion-driven. It does not poll a family service, wait on
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

For Texture2D shared-source replacement:

- AssetCore stages and owns rollback for replacement source bytes.
- TextureBuild and AssetForge rebuild and publish the Texture2D candidate.
- TextureEditor saves the package before committing the mounted-source
  replacement.
- A build or save failure restores the source bytes, then asynchronously
  rebuilds from the restored source.
- The Widget owns only the active asset identity, phase label, conflicting
  control state, close rejection, and final diagnostic presentation.

Other asset families can reuse the terminal result and compensating operation
without adopting Texture2D source or package policy.

## Related Documentation

- [Asset Data Lifecycle and Storage](../../Runtime/Assets/AssetDataLifecycle.md)
- [CPU Task System](../../Runtime/Core/TaskSystem.md)
- [Mounted Source Workflows](../Guides/MountedSourceWorkflows.md)
