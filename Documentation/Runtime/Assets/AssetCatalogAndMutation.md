# Asset Catalog And Mutation

Summary: Define mounted package discovery, rebuildable catalog/reference projections, forward-only asset jobs, and irreversible deletion.

Modules: Core, AssetRegistry, Engine, AssetTools, ContentBrowser, DurinEd, LevelEditor

Last reviewed: 2026-09-03

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
Engine publishes path-scoped `FAssetRegistryDelta` Add, Replace, Remove, and
reference-invalidation sets against an expected revision. Final metadata and
reference facts are derived at this boundary from committed package artifacts;
affected paths may be fenced while a failed projection is reconciled. Public callers receive
owned `FAssetCatalogEntry` values and immutable `FAssetCatalogSnapshot` values,
never pointers into mutable storage.

The process-local catalog revision advances only when published metadata
changes. `RefreshAssetRegistry` returns requested mode, completeness,
prior/resulting revisions, counters, warnings, and errors in one value. Explicit
Content Browser refresh reconciles every registered auto-scan mount; its folder
scope affects only presentation.

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
Their domain is expressed by names such as `FAssetResult`, `FindAssetExact`,
`RefreshAssetRegistry`, `LoadObject`, and `SavePackage`; there is no redundant
public `Durin::Asset` namespace. File-private and cross-file implementation
details use `Durin::AssetPrivate` when they require a shared internal scope.

## Result And Diagnostic Boundary

Core's `FDiagnostic` is the domain-neutral owning value for logs, tools, and UI.
It carries domain, code, severity, message, and optional context, but never owns
AssetRegistry or Engine control-flow semantics. Typed domain errors remain the
authoritative values used by program logic.

AssetRegistry exports `EAssetRegistryError` and `FAssetRegistryResult` for
discovery, bounded header projection, cache, snapshot, and publication failures.
Engine exports `EAssetError` and `FAssetResult` for loading, storage, Cook, and
mutation operations. Engine translates Registry results explicitly at its
publication and package-header boundaries; AssetRegistry never includes or
returns the Engine result contract. Both result values can produce an
`FDiagnostic` for common presentation without erasing their typed error.

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
unload guards continue to use package-header hard dependencies.

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
and graph copying. Engine owns package residency, bytes and writing, exact
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

## Relocation Jobs

Relocation is batched even for one mapping. Preparation captures the catalog
revision, exact participant fingerprints, resident finalizers, destination
artifacts, source redirectors, and owned payload moves behind an opaque job.
`ResumeForward` is the only execution direction. It publishes destinations and
owned payloads before source redirectors, persists progress at each boundary,
and is idempotent across ordinary retry. Relocation neither opens nor rewrites
unrelated referencer packages: their authored paths continue to target the
source alias until an explicit Fix Up operation canonicalizes those paths.

Moving package `A -> B` retains one redirect record for each moved top-level
asset and preserves every asset name and descendant suffix. Asset rename is a
separate exact operation. Relocation does not
compress unrelated upstream aliases; canonicalization belongs exclusively to
Fix Up. Reclaiming a destination alias requires exact proof that it resolves to
the selected real source.

Owned authored payload closure is metadata-derived, not suffix-guessed. A DAST
v9 package contributes its validated raw `.dbulk` only when Registry and Bulk
Directory bind a nonempty external segment. Relocation, duplication, Save, and
forward recovery publish that companion with the `.dasset`. Atomic
temporaries and `.durin-backup` files are recovery state and never mutation
participants.

Stale jobs, read-only participants, collisions, and preparation failures leave
authority unchanged. After the first authoritative publication, failures retain
durable forward progress; `RecoveryRequired` is reserved for uncertain authored
artifacts or independent stores. Registry-only lag returns
`ContentCommittedProjectionPending`, fences affected paths, and never rolls back
valid package bytes.

Authored runtime initialization scans `Saved/AssetMutationRecovery` before it
accepts requests. Each owned locator resolves replicated versioned journals
beside the affected content mounts. Recovery verifies every completed artifact,
recognizes publication that became visible before its progress write, and
continues remaining package and payload participants in their recorded forward
order. Fix Up journals also persist provider ids, stable rewrite ids, and source
and destination paths; restart reacquires the owner-gated provider, recognizes
already-applied rewrites, and replays only pending occurrences before deleting
redirectors. Projection reconciliation and journal cleanup happen only after all
authoritative participants converge. Missing providers remain forward-pending;
unsafe paths, divergent replicas, or bytes matching neither recorded image
require explicit recovery instead of automatic publication.

`FAssetResult` carries mutation disposition separately from its diagnostic error
code. Forward-pending, projection-pending, and recovery-required results are
therefore consumed structurally rather than inferred from message text. Durable
jobs also return their operation id, desired direction, and recovery location
when those values exist; diagnostic messages remain presentation data.

## Deletion And Fix-Up

`FAssetDeletionOperation` in AssetTools owns the sole confirmed deletion state,
including entries, blockers, detailed warnings, and the irreversible callback.
Engine exposes no deletion job or deletion-specific extension registry. AssetTools
owns deletion contributors and companion-ownership queries; package and `.dbulk`
inspection remain Engine mechanisms. External reference-store registration stays
in Engine for shared Cook/fix-up use. `CaptureAssetReferenceStores` returns owned
snapshots under provider gates; AssetTools interprets them as deletion warnings
and revalidates their fingerprints and registration revision before execution.

Deletion never rewrites persistent paths. Preparation evaluates the selected
target, complete direct/upstream alias closure, unified reference projection,
registered external stores, residency, and exact files. Alias-only deletion,
broken aliases, incomplete alias closure, dirty/loading packages, unsafe paths,
and changed fingerprints are blocked. After confirmation, maximal roots are
permanently removed and no Engine recovery copy, Undo record, Restore command,
or reverse callback is retained. Recovery belongs to version control. A partial
I/O failure remains forward-only and fences stale Registry paths for retry.
AssetTools calls Engine's `ReleasePackagesForRemoval` only after editor policy
revalidation. Engine checks the complete batch before retiring any resident
graph, allowing internal hard references while rejecting outside hard
referencers, dirty/loading packages, and stale catalog state. After the physical
callback, `PublishPackageRemoval` removes matching catalog entries only when
their package files are absent. An empty, moved-from, blocked, or completed
AssetTools operation cannot invoke the callback.

Fix Up is the only path-canonicalizing asset-mutation job. It rewrites
tagged hard and soft package fields plus registered external stores, reopens
package-level candidates to verify that no exact incoming occurrence remains,
and may then delete proven aliases. Exact occurrences remain transient Engine
tooling data throughout the job.
Dirty, incompatible, read-only, incomplete, or stale participants block before
mutation. Later participant failures retain verified rewrites and valid
redirectors; a later invocation resumes remaining forward work.

Owned-payload relocators, deletion companions, persistent external reference
stores, and committed-only observers register through scoped handles that retain
their module lease, gate invocation against retirement, reject duplicate class
providers, and are removed by exact handle.

## Related Documentation

- [Asset Packages](AssetPackages.md)
- [Asset Data Lifecycle and Storage](AssetDataLifecycle.md)
- [Content Browser](../../Editor/Architecture/ContentBrowser.md)
- [Content Version Control](../../Development/VersionControl/ContentVersionControl.md)
