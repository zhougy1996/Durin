# Asset Catalog And Mutation

Summary: Define mounted package discovery, immutable catalog and reference projections, and transactional asset relocation, deletion, and path fix-up.

Modules: AssetRegistry, Engine, AssetTools, ContentBrowser, DurinEd, LevelEditor

Last reviewed: 2026-08-31

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
matches may reuse cached class, entry kind, redirect, format, object count,
main/bulk extents, hard and soft package dependencies, and searchable names.
New or changed files read only the declared bounded front matter. V8 metadata
is projected by CoreDObject's construct-free Registry reader. Unsupported
versions fail before metadata publication; ordinary discovery has no legacy
header fallback.

Schema, package-format, serialization, mount-manifest, or snapshot corruption
causes a nonfatal rebuild. Full validation bypasses fingerprint reuse but keeps
the same front-matter boundary; it does not read export/value payloads.
Successful mutation prepares a complete
replacement in Engine and publishes it through AssetRegistry with an expected
prior revision. Public callers receive
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
`Asset/Mutation.h` owns asset-mutation transactions, and `Asset/Testing.h` owns
Engine's deterministic failure seams.
Runtime and offline consumers include these capability headers directly; the
former ambiguous root `AssetTools.h` aggregate no longer exists. The supported
aggregate entry points are defined by
[Asset Packages](AssetPackages.md#public-capability-boundary). There is no
public mutable catalog manager.

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

`DuplicateAsset` clones a real asset's complete persistent object graph into a
distinct, newly created resident package. Internal references are remapped to
the cloned inner objects while cross-package references retain their authored
targets. The result remains unsaved so class-owning editor code can replace
clone-specific identity before publishing it through the ordinary package-save
seam. Redirectors and occupied catalog or resident destinations are rejected;
failure discards every partially constructed clone.

Editor callers do not compose that seam directly. AssetTools selects the
deterministic `_Copy`, `_Copy2`, and later destination against catalog,
residency, and physical occupancy, invokes graph duplication, applies the
requested dirty-versus-persisted policy, and discards only the disposable
destination if persistence fails.

## Editor Orchestration Boundary

AssetRegistry owns persistent package discovery, bounded header projection,
immutable package metadata and dependency snapshots, their revisions, and the
single rebuildable registry cache. Engine owns package bytes and writing, exact
on-demand package inspection, object
construction and residency, Cook, graph copying, publication preparation, and
opaque mutation transactions.
`IAssetTools` owns reusable editor acceptance, typed terminal and persistence
results, history retention, cleanup, and one completion publication. Editor
hosts own UI and presentation; ContentBrowser additionally owns recursive
ordinary-file planning and physical stage/restore for mixed deletion.
Successful transaction Execute, Undo, and Redo each advance DurinEd's
mounted-content mutation revision exactly once; rejected or compensated
attempts publish none.

## Relocation Transactions

Relocation is atomic and batched even for one mapping. Preparation captures the
catalog revision, exact participant fingerprints, resident state, generated
redirectors, and owned payload moves behind an opaque transaction. `Commit`,
`Undo`, and `Redo` revalidate and journal the operation; callers cannot publish
individual phases.

Moving `A -> B` retains a direct redirector. A later `B -> C` retargets upstream
aliases directly to `C`; reclaiming an alias path requires exact proof that it
denotes the same real asset. Relocation does not rewrite persistent hard or soft
paths or arbitrary settings/import stores.

Owned authored payload closure is metadata-derived, not suffix-guessed. A DAST
v8 package contributes its validated raw `.dbulk` only when Registry and Bulk
Directory bind a nonempty external segment. Relocation, duplication, deletion,
Undo, Redo, and recovery journal that companion with the `.dasset`. Atomic
temporaries and `.durin-backup` files are recovery state and never mutation
participants.

Stale tokens, read-only participants, collisions, staging failures, and
publication failures either leave authority unchanged or compensate in reverse
order. Failed compensation retains a recovery-required journal beneath affected
content mounts. The journal records staged paths, pre/post fingerprints,
publication order, and completed/compensated state.

## Deletion And Fix-Up

Deletion never rewrites persistent paths. Preparation evaluates the selected
target, complete direct/upstream alias closure, unified reference projection,
registered external stores, residency, and exact files. Alias-only deletion,
broken aliases, and incomplete alias closure are blocked. Soft and external
references warn that authored paths will dangle. Commit, Undo, and Redo retain
enough exact metadata to restore redirectors and catalog state.

Fix Up is the only path-canonicalizing asset-mutation transaction. It rewrites
tagged hard and soft package fields plus registered external stores, reopens
package-level candidates to verify that no exact incoming occurrence remains,
and may then delete proven aliases. Exact occurrences remain transient Engine
tooling data throughout the transaction.
Dirty, incompatible, read-only, incomplete, stale, or failed participants leave
valid redirectors in place and restore published changes.

Owned-payload relocators, deletion companions, persistent external reference
stores, and committed-only observers register through scoped handles that retain
their module lease, gate invocation against retirement, reject duplicate class
providers, and are removed by exact handle.

## Related Documentation

- [Asset Packages](AssetPackages.md)
- [Asset Data Lifecycle and Storage](AssetDataLifecycle.md)
- [Content Browser](../../Editor/Architecture/ContentBrowser.md)
- [Content Version Control](../../Development/VersionControl/ContentVersionControl.md)
