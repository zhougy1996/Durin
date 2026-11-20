# Asset Catalog And Mutation

Summary: Define mounted package discovery, rebuildable catalog/reference projections, synchronous asset mutations, and irreversible deletion.

Modules: Core, AssetRegistry, Engine, AssetTools, ContentBrowser, DurinEd, LevelEditor

Last reviewed: 2026-11-17

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

Merged live/on-disk queries publish `OnAssetQueryChanged` events. Package changes
invalidate query metadata; catalog publications provide committed deltas after
unlocking on the publishing thread; listeners defer owner-thread work. Deferred
work coalesces publication, and query reads synchronize before returning. Consumers explicitly invalidate owned caches;
there is no global query revision or query-history replay. Catalog revisions
remain native publication/deletion consistency checks. Committed mount events
invalidate query metadata and coalesce a full-refresh notification; queries keep
no mount-version cursor.

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
redirector repair captures native registry publication snapshots.
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

CoreDObject exposes live package enumeration and metadata-change events over its
existing registration. AssetRegistry caches value metadata with non-owning object
keys; it neither owns residency nor scans the entire object array. Registration,
graph application, rename/relocation, eligibility changes and garbage marking
invalidate metadata. Metadata-neutral authored edits do not publish asset changes.

Creation, modification, rename and removal events carry exact asset identities and
intended package filenames. Saving updates presentation metadata without duplicating
assets; unloading a saved asset restores disk metadata, while discarding an unsaved
asset removes it. Old snapshots remain owned values. The separate native disk catalog
revision/journal protects save, Cook and mutation admission from storage changes.

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
transformations used by authoring tools and Cook. `IAssetTools` owns synchronous rename and referencer fix-up;
`ObjectTools.h` and `AssetDeleteModel.h` own editor deletion. Operation failure seams live in
`AssetTools/MutationTesting.h`; Engine catalog testing remains in `Asset/Testing.h`.
Runtime and offline consumers include these capability headers directly; the
former ambiguous root `AssetTools.h` aggregate no longer exists. The supported
aggregate entry points are defined by
[Asset Packages](AssetPackages.md#public-capability-boundary). There is no
public mutable catalog manager.

AssetRegistry and Engine asset APIs live directly in the `Durin` namespace.
Their domain is expressed by names such as `InspectAssetPackage`, `FindAssetExact`,
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
Engine status-only reads return `std::expected<void, UnifiedError::FError>`;
synchronous saving returns `bool` with an optional `FAssetWriteResult` completion
report; effectful mutation and asynchronous completion retain `FAssetWriteResult`.
Read diagnostics retain domain/code identity, optional text and ordered context.
Request cancellation and pending Registry projection remain states of their owning
request and Registry contracts, with matching diagnostic identity on propagation.
`FAssetWriteResult::Error` is the sole optional failure channel. Read, encoding
and Registry failures retain their original identities; they are not remapped
through a write-error enumeration. Asset-write policy rejections use the
`AssetWrite` domain. Only native write reports carry observed write effects. Private byte-transform
preparation returns a nullable prepared object
with optional diagnostics; object presence is their sole success signal.
Revalidation, companion-ownership queries and file-validation callbacks return
bool with optional diagnostics. Reference-store application also returns bool;
the mutation owner tracks attempted/published effects. A callback rejection
without a diagnostic still rejects the operation and cannot become success.
Browser capability and execution queries return their operation-specific bools;
there is no public Valid/Stale/Rejected deletion protocol. Private byte-transform
ledgers retain affected files and backup roots at their I/O owner; physical save
details stay with the package writer.
AssetRegistry owns its native result and its `ToError` conversion. Physical
writer outcomes cross the Engine boundary once to attach effects and recovery
context. Errors format through `ToString` only at presentation boundaries.
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

Editor creation and duplication, explicit targets, unique naming and Dirty-copy
retention are specified by [Editor Asset Tools](../../../Editor/Architecture/Assets/AssetTools.md#creation-and-duplication).
CoreDObject supplies the graph-copy primitive; Engine exposes no parallel asset
creation or duplication command.

## Editor Orchestration Boundary

AssetRegistry owns persistent package discovery, bounded header projection,
immutable package metadata and dependency snapshots, their revisions, and the
single rebuildable registry cache. CoreDObject owns object/package construction
and graph copying, reflected capture and package persistence. Engine owns asset
publication transactions, package residency, exact
on-demand package inspection, Cook, bounded package-byte transformations, and
catalog reconciliation. AssetTools owns object rename and fix-up preparation, validation,
multifile staging, publication ordering, and retained-backup policy. `Asset/PackageRemoval.h` supplies bounded
batch residency release and catalog removal against expected package metadata
and revision. It owns no selection, warning, companion-ownership, or physical
deletion policy. `IAssetTools` owns asset creation, duplication, import, rename and referencer
fix-up. `ObjectTools` and `FAssetDeleteModel` own deletion policy, analysis and
confirmation over a private native executor. Editor hosts own UI and presentation;
AssetTools additionally owns recursive ordinary-file planning and permanent
physical deletion for mixed selections; ContentBrowser adapts provider selection
and publishes committed effects through that private core. Successful authored operations advance
DurinEd's mounted-content mutation revision exactly once and never enter the
global object-edit Undo/Redo history.

## Editor Operation Results

Creation and duplication return live Dirty objects or nullptr. File-list imports
return successful objects only; managed tasks retain zero or more actual outputs
through completion, including outputs whose optional save failed.
Editor package saving and prompt results belong to the
[save facades](../../../Editor/Architecture/Assets/AssetTools.md#saving-and-feedback),
not IAssetTools. Saving keeps physical commits separate from Registry failure;
lexical observers receive only their own batch's committed paths. Creation and
copying use scoped feedback rather than trailing diagnostic parameters.
Rename returns bool and referencer fix-up returns void, with scoped feedback and
exact identity events as defined by [Editor Asset Tools](../../../Editor/Architecture/Assets/AssetTools.md#rename-and-referencer-fix-up).
Deletion returns object/folder counts through ObjectTools and exposes incremental
analysis through FAssetDeleteModel; see the [deletion contract](../../../Editor/Architecture/Assets/AssetTools.md#deletion).
No common editor result envelope is introduced.

Native `FAssetWriteResult` and private byte-transform records retain disk effects
and recovery paths at the owning I/O boundary. Private deletion attempts retain
their native write result and affected files after rejected repeat execution. A false high-level return does not
prove that no bytes changed: orchestration requiring partial-commit evidence
observes the owning native persistence boundary. Browser commands return booleans and
operation-specific item outputs. Their independent item-update notifier drives refresh from observed content changes;
the Panel does not receive native write effects or an operation-result envelope.
Folder validation returns bool with an optional diagnostic; automatic naming
checks only the existing-name identity before trying another candidate.
The level-document move coordinator forwards bool and display text from editor
feedback without rebuilding a structured write error from that text.

Batch progress and observed write effects remain in the owning completion report.
Diagnostic errors do not duplicate saved-package counts, the failing package or
write-effect state; presentation combines the error with those completion facts.

## Asynchronous Save Staging

`SavePackage(Package, Args)` performs synchronous saving.
For asynchronous persistence use
`SavePackageAsync(Package, Args, IsCancellationRequested)`, which returns
`Tasks::TTask<FAssetWriteResult>`. Synchronous and asynchronous saving share
`FAssetPackageSaveArgs`; the async entry point also accepts cancellation.
Clean resident packages are accepted, with the same revision checks as dirty ones.
The Engine entry point keeps asset readiness,
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

## Rename And Private Byte Transformations

Editor rename/move is object based and owned by
[Editor Asset Tools](../../../Editor/Architecture/Assets/AssetTools.md#rename-and-referencer-fix-up).
Normal rename changes the selected object and its package identity together;
explicit paths support independent object names. A multi-asset package keeps
unselected exports. Hard references retain their live pointers, matching soft
paths preserve subobject suffixes, and writable referencers may be rewritten as
part of rename. Dirty authored values do not require a preliminary save.
Engine's committed live identity publisher emits Registry OnAssetRenamed once,
with the new exact asset record and old soft object path. Disk publication and
projection reconciliation do not repeat this identity notification.

The private path-only byte transform still preserves existing export names.
Its native characterization is distinct from editor rename. Native package codecs
also rewrite exact object mappings for alias repair and Cook. Registry metadata,
owned payload closure and source aliases remain bounded checked participants.
Loaded material functions notify callers through their existing compile policy
after identity publication; manual callers retain their accepted generation.

Owned authored payload closure is metadata-derived. DAST companions follow the
validated Bulk Directory and writer closure; temporary and backup files are not
authored payloads. See [bulk publication ownership](../../ObjectSystem/BulkData.md#publication-and-companion-ownership).

Private file staging retains exact participant fingerprints, pre/post images and
published-file evidence. Source changes, read-only participants and preparation
failures reject before authority changes. Partial file/store publication retains
backups under `.durin-asset-mutation` for inspection. Registry-only lag returns
ContentCommittedProjectionPending and fences affected paths without rolling back
valid content. These mechanisms provide no startup replay or multi-file crash
atomicity. Retried editor identity operations recognize already moved objects;
private byte repair prepares a new operation from current content.

Private byte-transform records retain write effects, affected files and every
backup location. The explicit native characterization surface copies this evidence
into FPackageMutationTestResult; production commands have no dependency on that
test result. These are not public editor rename/fix-up results. Reference stores retain their
owned revalidation, application and verification callbacks; successful rewrites
are verified before alias deletion. Later failures preserve valid aliases and
previous committed work for a subsequent repair or manual recovery.

## Deletion And Fix-Up

ObjectTools and FAssetDeleteModel own editor deletion analysis and confirmation.
The private FAssetDeletionOperation retains whole-package execution facts,
including entries, blockers, detailed warnings and the irreversible callback.
AssetTools determines companion ownership using AssetRegistry's bounded reader
for the current package header and the physical bulk extent. Ownership queries
filter packages by whether their fixed `.dbulk` sibling intersects the selected
physical roots or confirmed companion paths before opening the package. A
candidate with a cached zero bulk extent still needs inspection because an
external edit may add a segment. External reference-store registration stays
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

Deletion never rewrites persistent paths. ObjectTools and the native asset delete
model own exact selected exports, reference analysis and confirmation. The model
allows selected Dirty loss, preserves unselected siblings and permits an unused
redirector to be selected alone. Incoming aliases, external persistent/live
references, loading state, open levels and rooted/native owners retain their
normal protection. See [Editor Asset Tools](../../../Editor/Architecture/Assets/AssetTools.md#deletion)
for the editor-facing model and prompt contract.

AssetTools' private native attempt retains the catalog revision, exact
companion closure, external-store snapshots and confirmed file identities.
File owns its private immutable ordinary-file/mixed-folder scope and physical
effect records. Execution checks current files and policy without rebuilding an
accepted scope. A stale pre-effect rejection may prepare a replacement that needs
fresh confirmation; a destructive attempt is retired before feedback. No recovery
copy, undo record, restore command or reverse callback is retained.

`ValidatePackageRemovalReferences` observes current references without collecting
or modifying graphs. The exact model-owned strong registrations are excluded;
other strong/function owners and external live referencers remain blockers.
`ReleasePackagesForRemoval` repeats native admission after host validation and
checks the complete batch before retiring any resident graph. Selected Dirty
loss is an explicit editor option; the default native primitive rejects Dirty
packages. Garbage package registrations retire before deferred collection, and
`PublishPackageRemoval` removes matching catalog entries only after their files
are absent. Partial persistence failures fence affected Registry projections.

Open workspace documents retain native strong references even in inactive tabs
and read-only inspectors. Closing a document releases its own registration;
other consumers may continue to retain the same object. No type whitelist or
asset-wide reference exemption bypasses this protection.

The physical executor freshly hashes the confirmed selection, supplies complete
participant identities to the private native attempt, and rechecks each physical
effect. The native handoff rechecks callback-sensitive live identity and external
store facts without reopening host-guarded package files or repeating companion
analysis. Cached hashes and matching size/timestamps do not replace fresh byte
validation. Physical failures retain every committed, failed and unattempted
participant, native path/error identity and any uncertain recovery location.
Verified earlier removals yield partial effects; unobservable post-removal state
remains uncertain. The same destructive callback cannot run twice.

Rename can update affected references; explicit Fix Up repairs remaining aliases. It rewrites
tagged hard and soft package fields plus registered external stores, reopens
package-level candidates to verify that no exact incoming occurrence remains,
and may then delete proven aliases. AssetTools retains exact Engine reference
occurrences only for the synchronous operation.
Private byte fix-up rejects Dirty, incompatible, read-only, incomplete or stale
participants before mutation. The editor facade first repairs/saves loaded Dirty
referencers with their current authored values. Later participant failures retain verified rewrites and valid
redirectors; a subsequent call prepares a new operation from current content.

External reference stores register through exact handles. Providers unregister
and finish active capture, mutation and fix-up callbacks before unloading their
code.

## Related Documentation

- [Asset Packages](AssetPackages.md)
- [Asset Data Lifecycle and Storage](AssetDataLifecycle.md)
- [Content Browser](../../../Editor/Architecture/Assets/ContentBrowser.md)
- [Content Version Control](../../../Development/VersionControl/ContentVersionControl.md)
