# Asset Catalog And Mutation

Summary: Define mounted package discovery, rebuildable catalog/reference projections, synchronous asset mutations, and irreversible deletion.

Modules: Core, AssetRegistry, Engine, AssetTools, ContentBrowser, DurinEd, LevelEditor

Last reviewed: 2026-11-10

Package identity, serialization, loading, and residency are defined by
[Asset Packages](AssetPackages.md). Authored, derived, and cooked storage
classes are defined by [Asset Data Lifecycle and Storage](AssetDataLifecycle.md).
This document owns only rebuildable discovery/reference projections and
asset mutations over those package identities.

## Catalog Ownership

`FPackageData` describes one persistent package: its physical file, format,
dependencies, bulk segment, and complete `TopLevelAssets` list. Each list entry
is an `FAssetData` with an exact asset path, class, optional exact object
redirect destination, primary ID and named bundle metadata. Management data is
separate from reference edges; see [Primary Asset Management](PrimaryAssetManagement.md). Packages have no representative class, entry kind, or
redirect destination, including when they contain exactly one asset.
`FindPackageExact(FPackagePath)` returns `FPackageCatalogEntry`;
`FindAssetExact(FTopLevelAssetPath)` returns `FAssetCatalogEntry` containing the
exact asset and its owning package from the same revision. Package-wide policies
inspect all asset records; class-specific operations select an exact asset.

For authored execution, mounted `Content` is authoritative. `AssetRegistry/Registry.bin` is a
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

Cooked execution instead admits the existing CookManifest.bin inventory and
bounded headers of its cooked packages at runtime bootstrap, before reflected
default objects. It publishes the complete catalog and reference index together;
any bad listed header retains the previous revision. The Game Engine path does
not refresh authored mounts. Primary ID/bundle discovery reads this same catalog
and needs no Editor services or live-object scan. See
[Cooking](../Build/Cooking.md#primary-asset-policy-and-cooked-discovery) for
inclusion policy, current-format admission and the loose-fixture boundary.

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
owned `FPackageCatalogEntry`/`FAssetCatalogEntry` values and immutable `FAssetCatalogSnapshot` values,
never pointers into mutable storage.

### Asset Discovery And Lifecycle

`CaptureAssetQuerySnapshot` and `FindAssetExact` discover assets from both live
memory and disk by default. `EAssetQuerySource::OnDisk` selects only persistent
metadata and remains safe for detached readers; `InMemory` selects live assets.
All/InMemory queries run on GameThread. A live package's current export set
supersedes its disk export set, including renamed or removed exports. Private
preparation graphs, templates, transient exports, PIE packages, garbage and live
packages outside registered mounts are excluded.

`FAssetQuerySnapshot::Assets` contains independently addressable `FAssetData`.
Its `Packages` contains genuine disk `FPackageData` only. `FAssetCatalogEntry`
succeeds when its asset exists; `Package` is absent for unsaved assets. Querying
never manufactures a zero-filled persistent package record. Existing
`FAssetCatalogSnapshot`, `FindPackageExact`, dependency closures and publication
admission continue to describe disk packages exclusively.

CoreDObject exposes live package enumeration and a package-metadata revision over
its existing registration. AssetRegistry caches value metadata with non-owning
object keys; it neither owns residency nor scans the entire object array. The
revision observes registration, graph application, rename/relocation, eligibility
changes and garbage marking. Discovery synchronizes at owner-thread query
boundaries. Metadata-neutral authored edits do not invalidate discovery caches.

`GetAssetQueryRevision` and `CaptureAssetQueryChanges` expose the unified lifecycle
journal. Readers have independent cursors; changes coalesce between discovery
queries. Creation, modification, rename and removal include exact asset identities
and intended package filenames without requiring filesystem writes. Disk
publication changes join the same journal. Saving replaces memory-only presentation
metadata without duplicating assets; unloading a saved asset restores disk metadata,
while discarding an unsaved asset removes it. Old snapshots remain owned values.
The disk catalog revision/journal remain separate so an unsaved edit cannot stale a
save, Cook or mutation admission snapshot.

The process-local catalog revision advances only when published metadata
changes. `RefreshAssetRegistry` returns requested mode, completeness,
prior/resulting revisions, counters, warnings, and errors in one value. Explicit
Content Browser refresh reconciles every registered auto-scan mount; its folder
scope affects only presentation.

`FAssetRegistrySnapshot::ResolvePackagePath` and `ResolveAssetObjectPath` use only
the owned catalog and `FAssetPathQueryOptions`. Unloaded serialized class names
are valid metadata. Exact resolution preserves descendant suffixes across aliases;
redirect depth, cycles, missing targets, and corrupt redirect metadata remain
structural results. Later fences or publications cannot change an old query.

The free Registry resolvers inspect current metadata and fence every traversed
package, without consulting loaded types. Runtime operations use Engine's
`Asset/RegistryOperations.h` and its `ResolvePackagePathForOperation` or
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
`Asset/Mutation.h` aggregates Engine package persistence mechanisms and shared
reference-provider contracts. `Asset/PackageEditing.h` exposes bounded package-byte
transformations used by authoring tools and Cook. `AssetTools/Relocation.h` and
`AssetTools/RedirectorFixup.h` own the synchronous editor operations;
`AssetTools/AssetDeletion.h` owns editor deletion. Operation failure seams live in
`AssetTools/MutationTesting.h`; Engine catalog testing remains in `Asset/Testing.h`.
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
edge is derived deterministically from the same `FPackageData` candidate as the
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
[Cook and publication rules](../Build/Cooking.md)
for the standalone host, ordinary loading, and dependency responsibilities.

## Duplication

`IAssetTools::DuplicateAsset` accepts an exact source `FTopLevelAssetPath`,
resolves destinations through the asset mount system and selects the deterministic
`_Copy`, `_Copy2`, and later destination against
catalog, residency, and physical occupancy, and loads the exact source object.
AssetTools creates the destination package and uses CoreDObject's
`DuplicateObject` graph primitive directly. Internal references are remapped to
the cloned inner objects while cross-package references retain their authored
targets. Redirectors are rejected and partially constructed graphs are discarded
when duplication fails. Successful duplication creates a Dirty, NewlyCreated
resident package and returns its `DObject*`. Duplication performs no saving.
The caller saves the returned package separately. Save failure retains the live
copy for ordinary save retry; it never triggers discard or another duplication.
Package identity and physical destinations are queried through the package and
mount APIs. Engine exposes no separate asset-duplication operation.

## Editor Orchestration Boundary

AssetRegistry owns persistent package discovery, bounded header projection,
immutable package metadata and dependency snapshots, their revisions, and the
single rebuildable registry cache. CoreDObject owns object/package construction
and graph copying, reflected capture and package persistence. Engine owns asset
publication transactions, package residency, exact
on-demand package inspection, Cook, bounded package-byte transformations, and
catalog reconciliation. AssetTools owns relocation/fix-up preparation, validation,
multifile staging, publication ordering, and retained-backup policy. `Asset/PackageRemoval.h` supplies bounded
batch residency release and catalog removal against expected package metadata
and revision. It owns no selection, warning, companion-provider, or physical
deletion policy. `IAssetTools` owns asset creation, duplication,
import, deletion planning and execution, and reusable editor acceptance. Editor hosts own UI and presentation;
ContentBrowser additionally owns recursive ordinary-file planning and permanent
physical deletion for mixed selections. Successful authored operations advance
DurinEd's mounted-content mutation revision exactly once and never enter the
global object-edit Undo/Redo history.

## Editor Operation Results

`IAssetTools` creation, standalone import and duplication return `DObject*`, with
`nullptr` on failure. Successful objects belong to live Dirty packages. Saving,
relocation, redirector fixup and deletion preparation/execution return `bool`.
Optional `FFeedbackContext*` parameters route detailed diagnostics synchronously;
omitting the context uses the shared context and logging. High-level operations
carry no common result envelope, terminal/persistence enums, or nested causes.

`SaveAssets` continues after individual failures and invokes `OnSaved` for each
committed package file, including a save whose Registry publication is pending.
The boolean remains false for publication failure and feedback carries its error.
The callback reports actual file persistence, not command admission. Duplication has no save option or completion event.

Native `FAssetWriteResult` and `FAssetMutationResultDetails` retain disk effects
and recovery paths at the owning I/O boundary. Deletion operations expose their
native write result and affected files, retaining destructive evidence after
rejected repeat execution. A false high-level return does not
prove that no bytes changed: orchestration requiring partial-commit evidence
consumes the native operation directly. Browser commands return booleans and
operation-specific item outputs. Their independent item-update notifier drives refresh from observed content changes;
the Panel does not receive native write effects or an operation-result envelope.

## Asynchronous Save Staging

`SavePackage(Package, Args)` performs synchronous saving.
For asynchronous persistence use
`Package->SaveAsync(Admission, FAssetPackageSaveContext{.Args = Args})`, which returns
`Tasks::TTask<FAssetWriteResult>`. Synchronous and asynchronous saving share
`FAssetPackageSaveArgs`; the context adds cancellation.
Clean resident packages are accepted, with the same revision checks as dirty ones.
The explicit Engine context keeps asset readiness,
participant validation and Registry publication; the CoreDObject file-only
context cannot substitute for it in editor workflows. Shared lifetime, capacity,
wait and shutdown rules are defined by
[Package persistence](../../ObjectSystem/PackagePersistence.md#operation-lifetime-and-completion).

Protected tasks capture on GameThread, stage detached output on BlockingIO and
complete after GameThread validation, commit, Registry publication and finalization
or rollback. Dropping the task does not cancel publication. Content arguments,
cancellation and test-fault callbacks are copied at submission.
Ordinary Registry failure keeps valid committed content and reports a fenced
projection; rollback policy is not selectable through ordinary save arguments.
Publication and exceptional completion use separate callbacks under the shared
save owner. Protected operations abandon uncompleted writer state on GameThread;
they never retry publication to perform cleanup. The real staging task releases
its write closure, while publication and abandonment retain object ownership
until GameThread release before terminal notification. Writers keep their existing
staging, commit, rollback and destruction protocol.

Scene import applies its validated peer graph to live memory and marks the new
packages Dirty before optional persistence. It submits ordinary protected tasks
for asynchronous saves, or ordinary synchronous saves. Save cancellation or
failure leaves imported objects resident; the import report carries the independent
`FAssetBatchSaveResult`. No persistence callback participates in graph application.

Synchronous and asynchronous saves capture the root and hard-dependency catalog participants. Hard
dependencies must retain catalog presence, physical path, top-level identities,
classes and redirect destinations, and must not be projection-fenced. Dependency
content metadata changes do not invalidate that identity snapshot. Soft references
and unrelated catalog changes are not commit participants. Protected saves reject
changed root revisions, destinations and staged files before commit, then use the
commit-time Registry revision. Ordinary sync and protected saves share their
commit policy. `SavePackages` commits each package independently in input order,
with an optional root moved last. It stops at the first failure; earlier packages
remain committed and clean. `FAssetBatchSaveResult` reports `SavedPackages`,
`FailedPackage`, and the current failure in `Result`. `Result.Effect` describes
only the failed package, including projection-pending content; it does not undo
or reclassify earlier successes. Failure injection uses the zero-based commit
index for every phase, including Registry publication.

Editor consumers directly retain and observe the typed save Task and consume its
native `FAssetWriteResult`; no editor result adapter is involved. The feature
owner presents successful saves once at its own completion boundary.
Dropping UI observation does not cancel admitted Engine work or retain callbacks
into the destroyed UI. Synchronous batch and canonical-resave requests remain
separate editor policies. Serialization and protected file switching remain on
GameThread; detached disk transfer is the background work.

Ordinary saves do not require a globally complete reference index. Their path-scoped
Registry deltas preserve unrelated index errors and completeness state. Prepared
graph publication retains the complete-projection admission requirement.

Synchronous AssetTools `LoadedDirtyPackage` batches save each distinct package
independently and continue after failures. `AffectedAssets` contains committed
packages; path-associated `Warnings` report failures and projection-pending saves.
A mixed batch returns a failed terminal state with `PartiallyPersisted`, retains
the successful files, and emits one completion notification for those successes.
Content Browser refreshes after partial persistence even when the aggregate result
is unsuccessful. `SavePackages` stops at the first failure instead of continuing,
but also preserves earlier successes. Canonical resaves retain per-package
verification and recovery; they do not roll back earlier successful packages.

## Synchronous Relocation

Relocation and redirector Fix Up are AssetTools operations and are unavailable
in runtime-only builds. Engine retains redirector resolution and package codecs.
Relocation is batched even for one mapping. Preparation captures the catalog
revision, exact participant fingerprints, resident finalizers, destination
artifacts, source redirectors, and owned payload moves in private operation state.
`RelocateAssets()` prepares, revalidates, and commits synchronously on the owner
thread, publishing destinations and owned payloads before source redirectors.
Relocation neither opens nor rewrites
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
by readers; see [bulk publication ownership](../../ObjectSystem/BulkData.md#publication-and-companion-ownership).

Stale participants, read-only participants, collisions, and preparation failures leave
authority unchanged. Failures after publication report the affected files and
retain staging backups for manual repair. Mutation staging stores `pre-*.tmp`
and `post-*.tmp` payloads under its owned operation directory; `owner` is the
ownership marker used to guard directory cleanup. Operations never resume partially completed
work; a new operation must analyze the current content. Registry-only lag returns
`ContentCommittedProjectionPending`, fences affected paths, and never rolls back
valid package bytes.

Asset mutations keep progress in memory and stage before/after images beside
owned content in `.durin-asset-mutation`. Operations clean up their owned stages when returning before publication or
after successful completion; interrupted publication retains backups for manual repair.
No locator directory or persistent journal is written. Runtime initialization
neither scans `Saved/AssetMutationRecovery` nor replays interrupted operations.
Legacy records are left untouched and do not block startup.
Staging keeps the original file fingerprint for conflict detection and the
prepared output hash for duplicate-participant checks and pre-publication
validation. The fingerprint also supplies the original byte hash; no second
original hash or post-publication fingerprint is stored. Original backup paths
are local to preparation, while before images remain on disk for manual repair.

`RelocateAssets()` and `FixUpRedirectors()` in AssetTools return `FAssetMutationResultDetails`
directly, including the write result, affected files, and retained backup locations.
Callers receive the complete result
when the synchronous call returns; no public job, execution state machine, or
separate preparation summary is exposed. Preparation failures also return the
write error without publishing content.

Reference-store rewrite contributions provide revalidation, application, and
verification callbacks; no restore callback or rollback-only snapshot is retained.
Fix Up verifies reference rewrites before deleting redirectors. There is no
in-process continuation, startup replay, or multi-file crash-atomicity guarantee;
partial changes require inspection and possibly manual repair or version-control
restoration before preparing a new operation.

`FAssetWriteResult::Effect` describes observed effects independently of its error
code: partial writes, uncertain content, or committed content with Registry
projection lag. It does not indicate retryability. Mutation details expose
`AffectedFiles` and all retained `BackupLocations`, never a replayable record. Editor
operation results preserve the complete backup list and report `ContentUncertain`
when the remaining content state cannot be established. Their terminal `State`
and `Persistence` are separate: creation can succeed with dirty, unsaved content,
and a batch save can fail after persisting some packages. Notification delivery
is owned by the publish callback and is not duplicated as a result flag.

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
in Engine for shared Cook/fix-up use. Fix Up reads a value copy of provider
registrations through `CaptureAssetReferenceStoreRegistrations` and checks its
revision after preparation callbacks and before commit; it never receives the
mutable Engine registry. `CaptureAssetReferenceStores` returns owned
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
broken aliases, incomplete alias closure, dirty/loading packages, open levels,
rooted/native-owned objects, external live GC references, unsafe paths,
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
only after a `StaleData` rejection before destructive work begins. Transient
I/O or in-use failures retain the confirmation and report their original cause;
an explicit retry repeats validation. Engine checks the complete batch before retiring any resident
graph, allowing internal hard references while rejecting outside hard
referencers, dirty/loading packages, and stale catalog state. After the physical
callback, `PublishPackageRemoval` removes matching catalog entries only when
their package files are absent. An empty, moved-from, blocked, or previously
executed AssetTools operation cannot invoke the callback.

`ValidatePackageRemovalReferences` inspects current object references without
collecting or modifying graphs. AssetTools uses it during preparation and
confirmation validation; Engine repeats it after host file-validation callbacks,
before marking any selected graph as garbage. Opening a level or acquiring a
strong reference after confirmation therefore rejects the whole batch before
physical deletion. Close the level/editor or release its owner before retrying;
confirmation never authorizes forced destruction of an active graph.

Workspace asset documents hold native strong references for their entire open
lifetime, including inactive tabs and read-only inspectors. This protects
materials, material instances/functions/parameter collections, textures, volume
textures, and static meshes through the same reference check without a deletion
type whitelist. Closing or unregistering a document releases its own reference;
other consumers may continue to retain the asset. Discard participates in atomic
package reload to migrate document roots before retiring the old generation.

ContentBrowser hashes each surviving confirmed file once per execution attempt,
after asset policy validation, and shares those freshly verified identities through
`FAssetDeletionCommit::ValidateFiles`. AssetTools requires complete participant
coverage without rehashing the files. A later deletion requires a new plan and
fresh confirmation. Hosts without this callback use AssetTools' own hashing.
Neither size/timestamp matches nor cached hashes replace the final byte check.

Fix Up is the only path-canonicalizing asset-mutation operation. It rewrites
tagged hard and soft package fields plus registered external stores, reopens
package-level candidates to verify that no exact incoming occurrence remains,
and may then delete proven aliases. AssetTools retains exact Engine reference
occurrences only for the synchronous operation.
Dirty, incompatible, read-only, incomplete, or stale participants block before
mutation. Later participant failures retain verified rewrites and valid
redirectors; a subsequent call prepares a new operation from current content.

Owned-payload relocators, deletion companions, persistent external reference
stores, and committed-only observers register through exact handles and reject
duplicate class providers. Owners unregister and complete active mutation/fix-up
operations, including destruction of retained plans and callback copies, before
unloading provider code.

## Related Documentation

- [Asset Packages](AssetPackages.md)
- [Asset Data Lifecycle and Storage](AssetDataLifecycle.md)
- [Content Browser](../../../Editor/Architecture/Assets/ContentBrowser.md)
- [Content Version Control](../../../Development/VersionControl/ContentVersionControl.md)
