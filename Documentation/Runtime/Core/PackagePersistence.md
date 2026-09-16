# Package Persistence

Summary: Define Engine-free reflected package capture, synchronous and asynchronous persistence, and staged file commit ownership.

Modules: Core, CoreDObject

Last reviewed: 2026-09-16

## Ownership and Entry Points

`DPackage::Save(FPackageSaveOptions)` and `SaveAsync(Admission, Options)` persist
ordinary reflected package files without Engine or an initialized asset registry.
They do not admit assets, prepare texture payloads, validate catalog dependencies,
or publish asset metadata. Asset callers continue through the
[Engine publication coordinator](../Assets/AssetCatalogAndMutation.md#asynchronous-save-staging).
CoreDObject depends only on lower layers for these operations.

`FPackageSaveOptions::Destination` supplies an explicit physical `.dasset` path.
An empty destination invokes the optional `SetPackageDestinationResolver` callback;
without either, admission fails with `InvalidPath`. The resolver is configured and
called on GameThread and resolves once per operation. Logical package identity
still follows the Core mount/path contract; explicit physical destinations do not
require the asset runtime. Private prepared graphs are owned by Engine publication
and are not admitted by these ordinary save members.

`CapturePackageLinker` captures reflected and native archive fields into the
existing canonical DAST v10 linker representation. `FObjectSaveOverrides` owns
non-mutating omissions/replacements. `FPackageCaptureOptions` supplies generic
archive targets, property filters and per-export redirect metadata. Engine adapts
its cook enums and concrete redirector objects at this boundary. Bulk capture
consumes immutable archive buffers and generic descriptors; family compilation,
source-data preparation and asset dependency policy remain in Engine.

Delta follows the existing paired default plan; Complete retains every selected
value. Object identities, reference ordering, format version, bulk placement and
hashing are unchanged. `ObjectPackage::ReadPackage` provides detached read-back;
this API does not add a general object loader.

## Operation Lifetime and Completion

`SaveAsync` returns an owned `FPackageSaveOperation` and a separate admission
result. Successful admission is not successful persistence. Preparation and live
object access run on GameThread. A bounded BlockingIO worker owns no live-object
access: it stages detached bytes and paths only. The operation retains the package
pin and output buffers until its I/O has drained.

`IsStagingReady()` reports worker readiness; `IsCompleted()` reports a cached
terminal result. GameThread `Complete()` returns `Busy` while staging runs, then
validates the package identity, edit revision, destination stamps and staged byte
hashes before committing. `WaitAndComplete()` waits only for I/O and then commits
on its calling GameThread, allowing headless tools to finish without a tick loop.
Workers never queue GameThread work. Repeated terminal completion is idempotent.
Recursive preparation of the same package is rejected.

`Cancel()` drains admitted I/O and deletes temporary files before commit starts.
It cannot interrupt an active or staged commit. Destruction drains I/O, abandons
unpublished files and rolls back a committed-but-unfinalized operation. Owners
must release operations before object-system shutdown; scheduler draining leaves
an observable success or failure. Only the saved revision can have its dirty flag
cleared, and failures before commit keep it dirty. Filtered, cooked or overridden
snapshots preserve authored dirty state. Saving files does not mark an
asset as catalog-published.

## Commit and Recovery Boundaries

`CommitStaged()` retains backups for a coordinator; `FinalizeCommit()` releases
those backups and finalizes dirty state; `RollbackCommit()` restores the previous
closure and leaves dirty state intact. A rolled-back operation terminates with
`Cancelled`. `FPackageSaveResult::CommitState` distinguishes `NotCommitted`,
`Committed`, and `RecoveryRequired`; an I/O error after content commit must not be
interpreted as an uncommitted save. Failed restoration retains backup files and reports their paths in `RecoveryFiles`.
Synchronous saving does not require an initialized task scheduler.

Core's `StageFileVerified`, `FFilePublicationStamp` and `FFileReplacement` own
generic staging verification, optimistic destination checks, file switching,
backup retention and rollback. CoreDObject commits the companion before its main
file and removes obsolete companions transactionally. Engine uses the same file
primitives while retaining multi-package ordering, companion recovery policy,
catalog snapshots and registry-failure rollback. Editor callbacks and notifications
remain attached to final Engine publication.

Two file renames are not crash-atomic. These primitives offer in-process rollback,
not persistent multi-file crash recovery. Cross-process writers require external
coordination; destination stamps provide optimistic conflict detection. Engine
retains its asset-specific recovery and projection-pending dispositions.

## Validation

`PackagePersistenceTests` links only Core and CoreDObject. It covers Delta and
Complete, internal references, external bulk, byte-equivalent synchronous and
asynchronous saves, final completion, resolver admission, stale revisions,
competing destinations, stage corruption, cancellation, abandonment, staged
rollback, I/O failure and scheduler draining. Engine package and editor workflow
tests cover the higher-layer publication contracts.
