# Asset Catalog And Mutation

Summary: Define mounted package discovery, rebuildable catalog/reference projections, forward-only asset jobs, and irreversible deletion.

Modules: Core, AssetRegistry, Engine, AssetTools, ContentBrowser, DurinEd, LevelEditor

Last reviewed: 2026-09-18

Package identity, serialization, loading, and residency are defined by
[Asset Packages](AssetPackages.md). Authored, derived, and cooked storage
classes are defined by [Asset Data Lifecycle and Storage](AssetDataLifecycle.md).
This document owns only rebuildable discovery/reference projections and
asset mutations over those package identities.

## Catalog Ownership

Mounted `Content` is authoritative. `AssetRegistry/Registry.bin` is a
rebuildable, deterministic discovery snapshot keyed by virtual mount and
normalized package path; it is never a content mount or a source of package
bytes. Startup enumerates every registered auto-scan mount once. A package in a
manual-scan mount remains a valid authored identity but becomes load-visible
only through explicit admission. Exact file-size and stable last-write-time
matches may reuse cached package facts plus the complete top-level asset list:
exact asset path, class, and optional exact redirect destination. New or changed
files read only the declared bounded front matter. V9 metadata is projected by
CoreDObject's construct-free Registry reader. Unsupported
versions fail before metadata publication; ordinary discovery has no legacy
header fallback.

Schema, package-format, serialization, mount-manifest, or snapshot corruption
causes a nonfatal rebuild. Full validation bypasses fingerprint reuse but keeps
the same front-matter boundary; it does not read export/value payloads.
AssetRegistry `AssetsSaved` accepts owned saved metadata and the caller's
`FAssetRegistryPublication` admission snapshot, returning `FAssetRegistryResult`.
It selects Add/Replace from that snapshot and publishes a path-scoped delta against
its revision. Empty batches succeed without mutation; invalid or duplicate paths
fail before mutation. Registry delta validation atomically updates metadata and
reference facts while preserving unrelated reference errors and completeness.
Engine converts results at its boundaries and constructs saved metadata from the
captured bytes plus observed output stamps, without a success-path rescan;
removal uses `PublishAssetRegistryDelta` directly. Internal Engine
`RefreshSavedPackages` retains mount resolution and recovery/removal policy;
relocation and redirector repair capture native registry publication snapshots.
There is no publication coordinator service or injected registry wrapper;
affected paths may be fenced while a failed projection is reconciled. Public callers receive
owned `FAssetCatalogEntry` values and immutable `FAssetCatalogSnapshot` values,
never pointers into mutable storage.

The process-local catalog revision advances only when published metadata
changes. `RefreshAssetRegistry` returns requested mode, completeness,
prior/resulting revisions, counters, warnings, and errors in one value. Explicit
Content Browser refresh reconciles every registered auto-scan mount; its folder
scope affects only presentation.

`FAssetRegistrySnapshot::ResolveAssetPath` and `ResolveAssetObjectPath` use only
the owned catalog and `FAssetPathQueryOptions`. Unloaded serialized class names
are valid metadata. Exact resolution preserves descendant suffixes across aliases;
redirect depth, cycles, missing targets, and corrupt redirect metadata remain
structural results. Later fences or publications cannot change an old query.

The free Registry resolvers inspect current metadata and fence every traversed
package, without consulting loaded types. Runtime operations use Engine's
`Asset/RegistryOperations.h` and its `ResolveAssetPathForOperation` or
`ResolveAssetObjectPathForOperation` to add loaded-class compatibility checks.
`ValidateResolvedAssetForOperation` validates captured aliases and the final
package before checking types. `ValidateAssetRegistryParticipants` compares
participant metadata and fences under the Registry mutex; unrelated publications
are admitted. This is a point-in-time check, not a lease on files or objects.
The revision is a publication concurrency token, never a fence or content identity.

Public headers remain split by responsibility: `AssetRegistry/Catalog.h` owns
discovery values and immutable queries, `AssetRegistry/References.h` owns the
reference projection, `AssetRegistry/Scan.h` owns reconciliation and cache
lifecycle, `Asset/Load.h` owns runtime resolution and residency,
`Asset/Mutation.h` owns forward-only relocation/fix-up jobs and package mutation
mechanisms, `AssetTools/AssetDeletion.h` owns editor deletion operations, and `Asset/Testing.h` owns
Engine's deterministic failure seams.
Runtime and offline consumers include these capability headers directly; the
former ambiguous root `AssetTools.h` aggregate no longer exists. The supported
aggregate entry points are defined by
[Asset Packages](AssetPackages.md#public-capability-boundary). There is no
public mutable catalog manager.

AssetRegistry and Engine asset APIs live directly in the `Durin` namespace.
Their domain is expressed by names such as `FAssetReadResult`, `FindAssetExact`,
`RefreshAssetRegistry`, `LoadObject`, and `SavePackage`; there is no redundant
public `Durin::Asset` namespace. File-private and cross-file implementation
details use `Durin::AssetPrivate` when they require a shared internal scope.

## Result And Diagnostic Boundary

Typed domain errors are the authoritative values used by program logic.

AssetRegistry exports `EAssetRegistryError` and `FAssetRegistryResult` for
discovery, bounded header projection, snapshot, and publication failures. Results
contain `FAssetRegistryErrorContext` rather than prose: precise reason, owned path,
actual/expected bounds or revisions, filesystem code, envelope code and complete
package Reader cause. `FormatAssetRegistryError` is the presentation boundary.
Nonfatal cache-warning contracts remain separate.
Engine loading and inspection return `FAssetReadResult` with `EAssetReadError`;
saving and mutation return `FAssetWriteResult` with `EAssetWriteError`.
Read cancellation and pending Registry projection have explicit classifications.
Write preparation failures retain their full diagnostic text; object/field
validation failures use `InvalidData` at the write boundary. Only write results
carry observed write effects. Mutation ledgers and backup roots are kept in
`FAssetMutationResultDetails`; physical save details stay with the package writer.
AssetRegistry owns its result contract; Engine adapts it explicitly.
Both read and write results expose their presentation text through `Message`.
Cook inputs and contributor callbacks use their Cook-owned result types.

## Reference Projection

AssetRegistry stores package-level hard, soft, and redirect edges only. Every
edge is derived deterministically from the same `FAssetData` candidate as the
catalog, and the catalog, fingerprints, and edge projection publish under one
expected revision. Duplicate `(source, target, category)` edges collapse;
publication rejects unsorted metadata, invalid redirects, fingerprint drift,
or any edge set that does not exactly match catalog metadata.

There is one rebuildable cache, `AssetRegistry/Registry.bin`. It contains the
complete persistent package metadata projection and never contains object ids,
declaring fields, container routes, display paths, or canonical Map-key tokens.
There is no second cache or payload-extraction pass. Incremental scans reuse
an exact identity/size/stable-timestamp match; full validation reparses bounded
front matter. Both produce identical package metadata and edges.

An exact object/property occurrence is transient Engine tooling data. A
relocation, deletion, or redirector operation first uses package edges to select
candidate packages, then explicitly opens only those candidates through
Engine's package-inspection seam. Exact records never enter AssetRegistry state,
snapshots, publications, or caches.

Cook reachability resolves explicit and registered external roots, follows
canonical hard and soft edges, validates final classes and redirects, excludes
alias packages, and terminates cycles through a visited set. Runtime loading and
unload guards continue to use package-header hard dependencies. Cook discovers aliases and final participants from a workflow-stable source tree
in a dedicated process; unrelated Registry revisions do not invalidate reuse. Build dependencies are separately declared and
persisted, and build-only inputs do not extend runtime reachability. See
[Cook and publication rules](Cooking.md)
for the standalone host, ordinary loading, and dependency responsibilities.

## Duplication

`IAssetTools::DuplicateAsset` accepts an exact source `FTopLevelAssetPath`,
selects the deterministic `_Copy`, `_Copy2`, and later destination against
catalog, residency, and physical occupancy, and loads the exact source object.
AssetTools creates the destination package and uses CoreDObject's
`DuplicateObject` graph primitive directly. Internal references are remapped to
the cloned inner objects while cross-package references retain their authored
targets. Redirectors are rejected, every partially constructed graph is
discarded on failure, and the requested dirty-versus-persisted policy owns
failed-save cleanup. Engine exposes no separate asset-duplication operation.

## Editor Orchestration Boundary

AssetRegistry owns persistent package discovery, bounded header projection,
immutable package metadata and dependency snapshots, their revisions, and the
single rebuildable registry cache. CoreDObject owns object/package construction
and graph copying, reflected capture and package persistence. Engine owns asset
publication transactions, package residency, exact
on-demand package inspection, Cook, bounded artifact publication, and
forward-only relocation/fix-up jobs. `Asset/PackageRemoval.h` supplies bounded
batch residency release and catalog removal against expected package metadata
and revision. It owns no selection, warning, companion-provider, or physical
deletion policy. `IAssetTools` owns asset creation, duplication,
import, deletion planning and execution, reusable editor acceptance, typed terminal/persistence results, and one
completion publication. Editor hosts own UI and presentation;
ContentBrowser additionally owns recursive ordinary-file planning and permanent
physical deletion for mixed selections. Successful authored operations advance
DurinEd's mounted-content mutation revision exactly once and never enter the
global object-edit Undo/Redo history.

## Asynchronous Save Staging

`SavePackage(Package, SAVE_None, Mode)` retains synchronous saving.
`SAVE_Async` is a scheduling flag, independent of Delta/Complete mode. It returns
only admission and exposes no task handle. For persistence confirmation use
`Package->SaveAsync(Admission, FAssetPackageSaveContext{Options})`, which returns
`Tasks::TTask<FAssetWriteResult>`. The explicit Engine context keeps asset readiness,
participant validation and Registry publication; the CoreDObject file-only
context cannot substitute for it in editor workflows. Shared lifetime, capacity,
wait and shutdown rules are defined by
[Package persistence](../Core/PackagePersistence.md#operation-lifetime-and-completion).

Protected tasks capture on GameThread, stage detached output on BlockingIO and
complete after GameThread validation, commit, Registry publication and finalization
or rollback. Dropping the task does not cancel publication. The old public
`FAsyncPackageSave::Begin` / `Complete` protocol is removed. Completion options,
including optional Registry-failure rollback, are copied at submission.

Both contracts capture the root and hard-dependency catalog participants. Hard
dependencies must retain catalog presence, physical path, top-level identities,
classes and redirect destinations, and must not be projection-fenced. Dependency
content metadata changes do not invalidate that identity snapshot. Soft references
and unrelated catalog changes are not commit participants. Protected saves reject
changed root revisions, destinations and staged files before commit, then use the
commit-time Registry revision. Ordinary sync and protected saves share their
commit policy; explicit bundles retain coordinated root-last publication and
Registry-failure rollback.

Direct async saves capture validated detached bytes, retire and drain the old
loose resource generation, and exclusively reserve the physical main/bulk closure
before admitting work. Authored bulk values retain validated immutable bytes for
editing and retry; external old resource handles stay retired. New reads and
competing writes reject while the reservation is held. The worker writes final
files, companion first, without temporary files, backups or old-file restoration.
Admission neither clears Dirty/NewlyCreated nor publishes Registry metadata.
After successful I/O, GameThread publishes captured metadata and clears Dirty
only if the captured revision still matches.

A destructive failure reports `PartiallyWritten` and `AffectedFiles`, keeps Dirty
and fences the projection. No backup recovery paths exist. Successful bytes with
failed participant or Registry publication report
`ContentCommittedProjectionPending`. The same resident package can retry with a
fresh capture; successful publication clears the fence. Explicit validated catalog
admission can also reconcile disk content. Other operations remain fenced until
recovery. `SetAsyncPackageSaveSink` installs a callback copied at submission;
it receives success or failure exactly once on GameThread after owned cleanup.
Failures and affected paths are logged independently of the sink. Direct output
has no rollback or persistent multi-file crash-recovery guarantee.

AssetTools' `FAssetSaveOperation` consumes the protected task's final result and
adds once-only editor notification. Its `Complete` method observes persistence;
it does not trigger file or Registry publication. It accepts one loaded dirty
package; existing synchronous batch and canonical-resave behavior is unchanged.
Serialization and protected file switching remain on GameThread; detached disk
transfer is the background work.

Ordinary saves do not require a globally complete reference index. Their path-scoped
Registry deltas preserve unrelated index errors and completeness state. Prepared
graph publication retains the complete-projection admission requirement.

Synchronous AssetTools `LoadedDirtyPackage` batches save each distinct package
independently and continue after failures. `AffectedAssets` contains committed
packages; path-associated `Warnings` report failures and projection-pending saves.
A mixed batch returns a failed terminal state with `PartiallyPersisted`, retains
the successful files, and emits one completion notification for those successes.
Content Browser refreshes after partial persistence even when the aggregate result
is unsuccessful. Explicit `SavePackagesAtomically` callers and canonical resaves
retain their existing coordination contracts.

## Relocation Jobs

Relocation is batched even for one mapping. Preparation captures the catalog
revision, exact participant fingerprints, resident finalizers, destination
artifacts, source redirectors, and owned payload moves behind an opaque job.
`Execute()` publishes destinations and owned payloads before source redirectors.
Each job executes once; success and failure are terminal. Relocation neither opens nor rewrites
unrelated referencer packages: their authored paths continue to target the
source alias until an explicit Fix Up operation canonicalizes those paths.

Moving package `A -> B` retains one redirect record for each moved top-level
asset and preserves every asset name and descendant suffix. Asset rename is a
separate exact operation. Relocation does not
compress unrelated upstream aliases; canonicalization belongs exclusively to
Fix Up. Reclaiming a destination alias requires exact proof that it resolves to
the selected real source.

After committing relocation, loaded material function assets notify their nested
material and instance callers through the existing compile policy. This refreshes
dependency paths and per-owner expression sources without changing shader identity
for a path-only move. Manual callers retain the accepted generation until compiling.
Function references participate in ordinary deletion blockers; deleting a function
does not silently sever call bindings.

Owned authored payload closure is metadata-derived, not suffix-guessed. A DAST
v9 package contributes its validated raw `.dbulk` only when Registry and Bulk
Directory bind a nonempty external segment. Relocation, duplication, and Save publish that companion with the `.dasset`. Atomic
temporaries and transaction backups end in `.tmp` and are never authored
mutation participants. Legacy backup siblings are neither restored nor removed
by readers; see [bulk publication ownership](BulkData.md#publication-and-companion-ownership).

Stale jobs, read-only participants, collisions, and preparation failures leave
authority unchanged. Failures after publication report the affected files and
retain staging backups for manual repair. Mutation staging stores `pre-*.tmp`
and `post-*.tmp` payloads under its owned operation directory; `owner` is the
ownership marker used to guard directory cleanup. Jobs never resume partially completed
work; a new operation must analyze the current content. Registry-only lag returns
`ContentCommittedProjectionPending`, fences affected paths, and never rolls back
valid package bytes.

Asset mutation jobs keep progress in memory and stage before/after images beside
owned content in `.durin-asset-mutation`. Prepared and completed jobs clean up
those owned stages; interrupted publication retains backups for manual repair.
No locator directory or persistent journal is written. Runtime initialization
neither scans `Saved/AssetMutationRecovery` nor replays interrupted operations.
Legacy records are left untouched and do not block startup.
Staging keeps the original file fingerprint for conflict detection and the
prepared output hash for duplicate-participant checks and pre-publication
validation. The fingerprint also supplies the original byte hash; no second
original hash or post-publication fingerprint is stored. Original backup paths
are local to preparation, while before images remain on disk for manual repair.

`FAssetMutationJob` owns execution state: Empty, Prepared, Executing, Completed,
or Failed. Copies share the same single execution. Result details carry errors
and observed effects, without a second job state or resumability flag. Rejected
second executions preserve the original result details.

Fix Up verifies reference rewrites before deleting redirectors. There is no
in-process continuation, startup replay, or multi-file crash-atomicity guarantee;
partial changes require inspection and possibly manual repair or version-control
restoration before preparing a new operation.

`FAssetWriteResult::Effect` describes observed effects independently of its error
code: partial writes, uncertain content, or committed content with Registry
projection lag. It does not indicate retryability. Mutation details expose
`AffectedFiles` and all retained `BackupLocations`, never a replayable record.

## Deletion And Fix-Up

`FAssetDeletionOperation` in AssetTools owns the sole confirmed deletion state,
including entries, blockers, detailed warnings, and the irreversible callback.
Engine exposes no deletion job or deletion-specific extension registry. AssetTools
owns deletion contributors and companion-ownership queries; package and `.dbulk`
inspection remain Engine mechanisms. Standard deletion companion ownership uses
AssetRegistry's current bounded package-header reader and physical bulk extent,
without reading package values or bulk payloads. Only matching custom deletion
contributors request complete Engine package inspection. Ownership queries filter
standard packages by whether their fixed `.dbulk` sibling intersects the selected
physical roots or confirmed companion paths before any package I/O. Custom
contributors remain conservative candidates. Cached zero bulk extent never
excludes a candidate, since external edits may add a segment. External reference-store registration stays
in Engine for shared Cook/fix-up use. `CaptureAssetReferenceStores` returns owned
snapshots under provider gates; AssetTools interprets them as deletion warnings
and revalidates their fingerprints and registration revision before execution.
Capture and registration run on the object owner thread. Capture copies the
registration list before invoking providers and fails with `StaleData` and empty
output if a callback changes that list, before calling another provider. Owners
must keep their store and native code alive until active callbacks return.
Successful snapshots retain owned values and remain usable after unregistration;
the registration revision is an availability check, not a provider lifetime pin.

Deletion never rewrites persistent paths. Preparation evaluates the selected
target, complete direct/upstream alias closure, unified reference projection,
registered external stores, residency, and exact files. Alias-only deletion,
broken aliases, incomplete alias closure, dirty/loading packages, unsafe paths,
and changed fingerprints are blocked. After confirmation, maximal roots are
permanently removed and no Engine recovery copy, Undo record, Restore command,
or reverse callback is retained. Recovery belongs to version control. A partial
I/O failure stops the operation and fences stale Registry paths. The destructive
callback cannot run again on the same operation.
AssetTools calls Engine's `ReleasePackagesForRemoval` only after editor policy
revalidation. Confirmation retains one AssetTools plan and its outside companion
snapshot. Execution checks current participant files, residency, reference-store
fingerprints, and catalog/contributor revisions without rebuilding that plan;
ContentBrowser retires failed destructive confirmations and validates its
physical selection once during execution and prepares a replacement confirmation
only after rejection before destructive work begins. Engine checks the complete batch before retiring any resident
graph, allowing internal hard references while rejecting outside hard
referencers, dirty/loading packages, and stale catalog state. After the physical
callback, `PublishPackageRemoval` removes matching catalog entries only when
their package files are absent. An empty, moved-from, blocked, or previously
executed AssetTools operation cannot invoke the callback.

ContentBrowser hashes each surviving confirmed file once per execution attempt,
after asset policy validation, and shares those freshly verified identities through
`FAssetDeletionCommit::ValidateFiles`. AssetTools requires complete participant
coverage without rehashing the files. A later deletion requires a new plan and
fresh confirmation. Hosts without this callback use AssetTools' own hashing.
Neither size/timestamp matches nor cached hashes replace the final byte check.

Fix Up is the only path-canonicalizing asset-mutation job. It rewrites
tagged hard and soft package fields plus registered external stores, reopens
package-level candidates to verify that no exact incoming occurrence remains,
and may then delete proven aliases. Exact occurrences remain transient Engine
tooling data throughout the job.
Dirty, incompatible, read-only, incomplete, or stale participants block before
mutation. Later participant failures retain verified rewrites and valid
redirectors; the failed job cannot execute again.

Owned-payload relocators, deletion companions, persistent external reference
stores, and committed-only observers register through exact handles and reject
duplicate class providers. Owners unregister and complete active mutation/fix-up
operations, including destruction of retained plans and callback copies, before
unloading provider code.

## Related Documentation

- [Asset Packages](AssetPackages.md)
- [Asset Data Lifecycle and Storage](AssetDataLifecycle.md)
- [Content Browser](../../Editor/Architecture/ContentBrowser.md)
- [Content Version Control](../../Development/VersionControl/ContentVersionControl.md)
