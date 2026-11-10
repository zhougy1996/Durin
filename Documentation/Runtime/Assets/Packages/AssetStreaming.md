# Asset Streaming

Summary: GameThread request ownership, batch results and completion over the shared asset loader.
Modules: Engine

Last reviewed: 2026-11-10

## Managers and requests

Include `Engine/StreamableManager.h`. `GetStreamableManager()` supplies the default
owner; explicitly owned `FStreamableManager` instances scope local features such
as thumbnail sessions. All instances reuse the same live registry, package
loader, coalesced reads, SCC completion and GC graph. A manager is not another
resident-package cache. Scene request handles transferred to Worlds use the
default manager so their lifetime continues past the scene operation.

`RequestAsyncLoad(FStreamableAsyncLoadParams)` accepts `TargetsToStream`,
`OnComplete`, `OnCancel`, `Priority`, `bManageActiveHandle` and `DebugName`.
Durin's `ExpectedClass` extension validates every selected object. Single-object
convenience overloads accept exact soft/top-level paths; synchronous requests
accept a span of soft paths or one exact path. Package-only tools use
`RequestSyncLoadPackage` / `RequestAsyncLoadPackage`, selecting a package without
inventing an asset export. There are no free object request functions or
mandatory expected-result wrappers. Stalled start, download, JIT and update
options are absent until implemented.

Submission, observation, destruction and callbacks belong to GameThread.
Async requests always return a handle. Empty requests succeed with no targets;
invalid targets produce errors in their slots. Ordinary errors and resident
hits complete through the pump, with callback delivery on a subsequent pump.
Guard or closed-runtime admission produces a terminal failed handle without
callbacks. Sync requests finish before returning and have no callback.

Managed handles are strongly owned by their manager until explicit release,
cancellation or manager destruction; failed requests without any successful
retention are retired from managed ownership automatically. Other requests are
tracked weakly. `GetActiveHandles`, `IsManaged`, `GetDebugName` and `GetOwningManager` expose
the request owners without introducing another package table.

## Observation and retention

`HasLoadCompleted` observes finished object loading, including partial failure.
`WasCanceled` is independent: cancellation before loading finishes leaves
completion false; cancellation after loading does not erase that historical
completion. `IsComplete` is a terminal-state convenience including cancellation.
`IsActive` observes this request's retention, independently of whether a weak
result remains resident through another owner. Object completion says nothing
about compilation or render-resource readiness.

`GetRequestedAssets`, `GetLoadedAssets`, `GetTargetErrors` and `GetLoadReports`
preserve input ordering and duplicates. Every object target has a slot, including
invalid/missing targets whose object is null. Successful duplicate paths observe
the same object. `GetLoadedAsset` selects the first slot, which can be null;
`GetLoadedPackage` observes the first retained package, including package-only
requests and combinations of packages. Class validation,
subobject selection and redirects use ordinary loading, with diagnostics retained
per target. `HasError` / `GetError` select the first failed input slot after
completion. Partial success keeps its successful objects active.

Active requests retain selected objects and packages. `ReleaseHandle` drops
only this request's ownership; before completion or while a callback is queued,
release is deferred until delivery. Release never suppresses completion and
never forces global GC. After release, successful result slots use weak identities
and can become null after collection, while their historical reports remain. Configured editor Standalone roots and
unsaved/explicit package roots are independent owners; their lifecycle is defined
by [Asset Residency](AssetResidency.md).

`CancelHandle` suppresses undelivered completion, records cancellation, immediately
drops this consumer's retention and invokes its cancel delegate at most once.
It may be called in the queued-completion window. Moving callbacks out before
invocation permits reentrant submission/cancellation; a recursive pump is ignored.
Dropping caller references alone does not withdraw accepted work: explicitly
cancel when the feature no longer needs its callback.

## Composition and teardown

A combined handle waits for child completion delegates to finish before becoming
terminal, so published child state is visible when the parent completes. Its
optional cancel delegate does not implicitly cancel shared children.

`CreateCombinedHandle` flattens children in child order and target order, retaining
child observations and its own copy of each successfully loaded object's/package's
GC ownership. It completes after every child reaches a terminal state; a canceled
child contributes a cancellation error without canceling successful peers.
Null children fail the combination with InvalidPath. Empty combinations succeed.
Children may already be completed or released; released results remain weak
historical observations and are never silently reloaded.

Canceling or releasing a combined parent never calls cancel/release on shared
children. Another parent or direct caller can keep its request active. Internal
batch children are private consumers and are canceled when their owning batch
is canceled. Parent retention is captured when children complete, before child
deferred release. Once a parent completes it owns its results independently;
later explicit child cancellation does not revoke the parent's committed roots.

Manager destruction invalidates owner links, detaches callbacks and cancels its
tracked requests; surviving handles remain safe observations with no dangling
manager pointer. Runtime shutdown closes admission, retires all managers' requests
and drains accepted reads before package resources retire. Callback cancellation
never stops backend work still needed by unrelated consumers.

## Pump and backend limits

EngineLoop advances `ProcessAsyncLoading`; standalone tools/tests pump explicitly.
Requests for the same root package share active reads across managers. Higher
priority pending requests start first. The backend limits detached reads to two
closures of at most 256 MiB of main-package bytes each. File leases are released
before callback delivery. A changed catalog revision fails with StaleData;
callers may retry. Main-file dependencies outside the captured closure fail
rather than falling back to synchronous reads.

Codec decoding, object construction/restoration, resource registration and
validation/PostLoad still execute atomically on GameThread. The pump budget
(default 2 ms, one completion unit) is checked between units, not inside a
package component or callback. It is not a hard frame-time guarantee.
`CancelAsyncLoading` is the blocking shutdown/tools drain, while per-handle
cancellation is the ordinary nonblocking feature operation. Reentrant async
submission during a drain is rejected without callbacks, and nested drains are
idempotent; admission resumes after a tools-only drain completes.

Package residency, checked unloading and detailed synchronous reports remain
defined by [Asset Packages](AssetPackages.md).
