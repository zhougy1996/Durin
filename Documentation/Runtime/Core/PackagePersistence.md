# Package Persistence

Summary: Define Engine-free reflected package capture, synchronous and asynchronous persistence, and staged file commit ownership.

Modules: Core, CoreDObject

Last reviewed: 2026-09-17

## Ownership and Entry Points

`DPackage::Save(FPackageSaveOptions)` and
`SaveAsync(Admission, FSavePackageContext{Options})` persist
ordinary reflected package files without Engine or an initialized asset registry.
They do not admit assets, prepare texture payloads, validate catalog dependencies,
or publish asset metadata. Asset callers continue through the
[Engine save orchestration](../Assets/AssetCatalogAndMutation.md#asynchronous-save-staging).
CoreDObject depends only on lower layers for these operations.

`FPackageSaveOptions::Destination` supplies an explicit physical `.dasset` path.
An empty destination invokes the optional `SetPackageDestinationResolver` callback;
without either, admission fails with `InvalidPath`. The resolver is configured and
called on GameThread and resolves once per operation. Logical package identity
still follows the Core mount/path contract; explicit physical destinations do not
require the asset runtime. Private prepared graphs are owned by Engine publication
and are not admitted by these ordinary save members.

`FSavePackageContext` owns effective `FPackageSaveOptions` and a shared
`IPackageWriter`. Existing save overloads construct a default file-backed context;
`FPackageSaveOperation::Begin` also accepts an explicit context. Its owned copy
keeps settings and writer alive through asynchronous completion. `Capture` maps
the effective mode and generic archive target once; `BeginWrite` transfers detached
file descriptors and buffers without recapturing objects.

`CapturePackageLinker` captures reflected and native archive fields into the
existing canonical DAST v10 linker representation. It and `FSavePackageContext::Capture`
return `FPackageCaptureResult` without diagnostic output parameters. Errors own
object/field identities, routes and numeric bounds, retain path, default-delta,
property-value and snapshot causes, and preserve Archive code/path. Capture
publishes the Linker only on success. Internal container adaptation rejects missing
child descriptors and propagates recursive failures. `FormatPackageCaptureError`
owns prose; pending save admission retains `CaptureCause`, while the pending Engine
asset adapter formats explicitly.

`FObjectSaveOverrides` owns
non-mutating omissions/replacements. Its mutation APIs return `FSaveOverrideResult`
with owned object/property identities, exact layout/type mismatch facts, and
typed property-value or snapshot causes. Failed replacement preparation publishes
no new object entry or property override. `FormatSaveOverrideError` owns prose.
`FPackageCaptureOptions` supplies generic
archive targets, property filters and per-export redirect metadata. Engine adapts
its cook enums and concrete redirector objects at this boundary. Bulk capture
consumes immutable archive buffers and generic descriptors; family compilation,
source-data preparation and asset dependency policy remain in Engine.

Delta follows the existing paired default plan; Complete retains every selected
value. Object identities, reference ordering, format version, bulk placement and
hashing are unchanged. `ObjectPackage::ReadPackage` provides detached read-back;
this API does not add a general object loader.

## Operation Lifetime and Completion

`SaveAsync` requires an explicit context and returns an existing typed Task.
`FSavePackageContext` supplies `Tasks::TTask<FPackageSaveResult>`; Engine's
`FAssetPackageSaveContext` supplies `Tasks::TTask<FAssetWriteResult>` with asset policy
and Registry publication. There is no default context or implicit file-only
fallback. Both reject direct-write flags. Options are copied at submission.
Rejected admission returns an invalid task and a domain error in `Admission`;
accepted admission is not successful persistence.

Capture and live-object access run on GameThread. Internally owned BlockingIO
work stages detached bytes and paths. A registered GameThread continuation
validates identities, revisions, destination and staged stamps, commits, publishes
when applicable, and finalizes or rolls back before completing the returned task.
Normal host ticks advance publication without caller completion calls. Inspect
both task execution state and the typed save result: a successfully executed task
can contain a save-domain failure. Only the saved revision can clear Dirty;
filtered, cooked or overridden snapshots preserve authored Dirty. File-only
saving does not mark an asset as catalog-published.

Dropping the handle does not cancel the save. Cancellation is advisory before
commit and discards stages; a commit already in progress reaches its terminal
publication/finalization or rollback boundary. A late cancellation does not undo
committed bytes, even if the task reports Canceled. Pins and adapter captures are
released on GameThread before terminal result delivery. Recursive preparation
and submission from a save terminal callback are rejected.

The shared owner admits at most 64 operations and 256 MiB of detached output
across direct and protected async saves. Slots and byte accounting remain held
through terminal publication. Admission requires a running scheduler and an
accepting GameThread deferred executor. Disk work is gated until the completion
continuation has been admitted; failure cannot start a destructive write.

`DPackage::HasAsyncFileWrites()` reports disk work only, including protected
staging. `WaitForAsyncFileWrites()` snapshots admitted disk tasks and waits that
cutoff, returning `FTaskWaitResult`; it does not publish, pump GameThread or wait
for later submissions. Domain I/O errors remain in save results or the direct
sink, independently of the worker task state. Ordinary Task waits never pump;
a GameThread wait on an unfinished final save task is unsupported.

Headless hosts initialize the deferred executor and explicitly call GameThread
`DPackage::DrainAsyncSaves()` to finish a submission cutoff through publication.
This drain reports host-drain success, not aggregate save-domain success; inspect
individual results. It rejects reentrant publication callbacks. Shutdown closes
admission, drains saves and diagnostics, then tears down resources, objects,
modules and the task system. Engine shutdown and the CoreDObject module teardown
boundary perform this drain; standalone owners must drain before shutting down
the scheduler. The owner also reaps canceled deferred continuations on host ticks
or drain, so a worker never destroys the last live-object pin.

`FPackageSaveOperation` remains a lower-level staged coordinator for file-only
transactions. Its explicit commit/finalize/rollback methods do not describe the
public task-returning member's completion protocol.

## Commit and Recovery Boundaries

`CommitStaged()` retains backups for a coordinator; `FinalizeCommit()` releases
those backups and finalizes dirty state; `RollbackCommit()` restores the previous
closure and leaves dirty state intact. A rolled-back operation terminates with
`Cancelled`. `FPackageSaveResult::CommitState` distinguishes `NotCommitted`,
`Committed`, and `RecoveryRequired`; an I/O error after content commit must not be
interpreted as an uncommitted save. Failed restoration retains backup files and reports their paths in `RecoveryFiles`.
Synchronous saving does not require an initialized task scheduler.

Core's reusable `IPackageWriter` creates isolated `IPackageWriteOperation`
instances. The file implementation uses `FFileHelper::SaveArrayToNewFile`,
`FFilePublicationStamp` and `FFileReplacement` for all package/bulk staging and
switching. Each input owns its bytes, expected destination stamp and distinct
staging/backup paths; an empty stage denotes removal. `Stage` exclusively creates
each staging file and writes, flushes and closes it without an intermediate
replacement. Failed creation never grants ownership of an occupied path; failed
writes clean up only newly created files. After a successful write, `Stage`
checks its size against the input buffer and records its modification time, then
releases the buffer. `Commit` rechecks destination and staged stamps before switching
files in input order, `Finalize` releases backups, and `Rollback` restores files
in reverse order. Results retain committed/recovery-required state and recovery
paths. Destruction removes only owned stages and rolls back unfinished commits;
failed rollback or finalization retains recovery files. The enclosing operation
must drain staging before calling other methods or destroying writer state.

Staged checks use existence, size and modification time only; saving does not
read back file contents or compute verification hashes. These checks detect
common accidental changes, not tampering that preserves size and time, or changes
between inspection and rename. Internal staging files are trusted. Atomic write
error handling, destination conflict checks and rollback remain required.

CoreDObject commits the companion before its main file and removes obsolete
companions transactionally. Engine uses the same writer while retaining
multi-package ordering, transaction-specific sibling paths, catalog snapshots and
registry-failure rollback. Editor callbacks and notifications remain attached to
final Engine publication.

Two file renames are not crash-atomic. These primitives offer in-process rollback,
not persistent multi-file crash recovery. Cross-process writers require external
coordination; destination stamps provide optimistic conflict detection. Engine
retains its manual-repair and projection-pending dispositions. Asset mutation
jobs may retry in process, but Engine does not persist journals or replay
interrupted jobs at startup.

## Detached Direct Writer Primitives

Core also provides `GetDirectFilePackageWriter` for detached output. Its `Stage`
writes final destinations in input order, without creating staged or backup files.
An empty staging marker requests removal, as in the protected writer; nonempty
markers select writing but are not opened. It validates the complete destination
stamp set before the first destructive operation. `Commit` and `Finalize` observe
the result; `Rollback` cannot restore overwritten bytes, and destruction does not
restore them. Engine uses it for admission-only `SavePackage(..., SAVE_Async)`.

Failure before destructive output is `NotCommitted`. Failure after any attempted
write or removal is conservatively `PartiallyWritten`; `AffectedFiles` names
attempted destinations, while `RecoveryFiles` remains empty. CoreDObject preserves
that distinction in `FPackageSaveResult`. `IPackageWriter::SupportsRollback`
identifies protected writers; `FPackageSaveOperation` rejects direct writers
before capture or I/O so its existing protected contract cannot be weakened.

`FPackageFileAccess` atomically admits a set of physical paths for shared reading
or exclusive writing. Tokens can cross threads and release without thread-affine
mutex ownership. Keys use absolute, weakly canonical paths and Windows case
normalization; hard-linked files are rejected because their aliases cannot safely
be represented by independent path keys. Direct writers acquire at construction
and retain admission through their lifetime. Protected writers acquire before
commit and retain ownership through finalization or rollback; staging remains
optimistic. Conflicts reject without blocking or retaining a partial reservation.
This is process-local cooperation, not an OS lock. Engine load, inspection,
resource registration and loose-resource reads participate in shared admission;
`TryReadPackage` reserves the main and companion together. Direct saves retire
and drain old lazy resources before destructive output. Explicit bundles reserve
their complete destination set before publishing any member.

## Validation

`PackagePersistenceTests` links only Core and CoreDObject. It covers Delta and
Complete, internal references, external bulk, byte-equivalent synchronous and
asynchronous saves, final completion, resolver admission, stale revisions,
competing destinations, stage corruption, cancellation, dropped handles, bounded admission, host completion, staged
rollback, I/O failure and scheduler draining. Engine package and editor workflow
tests cover the higher-layer publication contracts.
