# Asset Catalog And Mutation

Summary: Define mounted package discovery, immutable catalog and reference projections, and transactional asset relocation, deletion, and path fix-up.

Modules: AssetCore, DurinEd, LevelEditor

Last reviewed: 2026-08-23

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
matches may reuse cached class, entry kind, redirect, format, and dependency
metadata; new or changed files use the bounded package header reader.

Schema, package-format, serialization, mount-manifest, or snapshot corruption
causes a nonfatal rebuild. Full validation bypasses fingerprint reuse and
verifies package and redirector bodies. Successful mutation updates the private
catalog store and publishes one new immutable snapshot. Public callers receive
owned `FAssetCatalogEntry` values and immutable `FAssetCatalogSnapshot` values,
never pointers into mutable storage.

The process-local catalog revision advances only when published metadata
changes. `RefreshAssetCatalog` returns requested mode, completeness,
prior/resulting revisions, counters, warnings, and errors in one value. Explicit
Content Browser refresh reconciles every registered auto-scan mount; its folder
scope affects only presentation.

Public headers remain split by responsibility: `Asset/Catalog.h` owns discovery
values, `Asset/References.h` owns the immutable reference projection,
`Asset/Load.h` owns runtime resolution and residency, `Asset/Mutation.h` owns
asset-mutation transactions, and `Asset/Testing.h` owns deterministic failure seams.
The supported aggregate entry points are defined by
[Asset Packages](AssetPackages.md#public-capability-boundary). There is no
public mutable catalog manager.

## Reference Projection

`AssetRegistry/References.bin` is the single rebuildable hard, soft, and
redirect occurrence projection. A source entry is keyed by package fingerprint,
DAST version, and extractor schema. Each occurrence records source package and
object, declaring type and field, reference kind and expected class, target
path, typed container route, and deterministic display path.

Extraction reads package fields and reflection metadata without constructing
objects, invoking `PostLoad`, resolving targets, or changing residency. It is
bounded to four container levels, 100,000 occurrences per package, 1,000,000
per snapshot, 1 MiB paths and map-key tokens, and 4 KiB display paths. A failed
source extraction publishes no partial entry.

Incremental reconciliation may trust an exact path, size, and stable timestamp
match. `FullValidation` reads and hashes every package and performs complete
package and reference validation. Callers that require destructive safety must
reject incomplete projections rather than interpreting missing occurrences as
absence.

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

Fix Up is the only path-canonicalizing asset-mutation transaction. It rewrites tagged
hard and soft package fields plus registered external stores, verifies that no
incoming persistent occurrence remains, and may then delete proven aliases.
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
