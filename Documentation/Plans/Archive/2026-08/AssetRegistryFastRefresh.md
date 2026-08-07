# Asset Registry Fast Refresh Plan

Summary: Eliminate unchanged-package payload reads from incremental registry and reference-index reconciliation while preserving explicit full validation.

Last reviewed: 2026-08-07

Status: Archived
Completed: 2026-08-07

## Current Status

Completed on 2026-08-07. Incremental reference reconciliation now decides
unchanged-source reuse from package path, file size, and stable last-write-time
ticks before payload I/O. Processed sources share one loaded byte buffer across
fingerprinting, inspection, and extraction; full-validation redirector
inspection is reused by the reference pass. Payload-read attempts and bytes are
published per scan.

Regression coverage verifies cold, unchanged warm, single-change, add/remove,
rename, invalid registry and reference caches, mount-manifest changes, a payload
larger than 4 MiB, and the same-size/restored-timestamp trust boundary. The
focused AssetPackageTests suite passed 74 tests, the complete native-test
aggregate passed, and `build --target all` passed with the
`Win64-Debug-DurinEditor-Tests` Agent Build Profile.

`FAssetRegistry::ScanMountedContent` already persists and reuses registry
metadata using exact file size and stable last-write-time ticks. The registry
header path therefore avoids repeated package-header reads for unchanged
`.dasset` files.

The same incremental scan then walks every discovered asset and calls
`LoadFileToArray` before checking whether the persisted reference-index source
can be reused (`Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`,
the reference loop beginning near line 3612). The cached reference fingerprint
contains a content hash, so the current implementation must read the complete
payload before it can compare the fingerprint. A cache hit therefore reports
`ReusedSources` but still performs payload I/O. A cache miss can read the same
file again through `InspectAssetPackage`.

`References.bin` already persists file size, stable last-write-time ticks,
content hash, and extracted reference occurrences. The missing optimization is
the cheap pre-check, not another copy of the content hash in `Registry.bin`.

Content Browser refresh invokes an incremental scan for every registered
`bAutoScan` mount. The visible item snapshot is limited to the current folder
subtree, but registry and reference-index reconciliation is mount-wide. The
ordinary editor startup path also uses incremental scanning. `FullValidation`
is currently used by tests and the mounted-source relocation rollback path,
not by ordinary Content Browser F5 refresh.

## Goal

Make an unchanged incremental scan avoid all `.dasset` payload reads and full
reference parsing. The scan may still enumerate mounted directories, query
file metadata, load cache files, rebuild deterministic in-memory maps, and
publish cache snapshots.

For new or changed packages, read and parse only the affected payloads. Keep
`FullValidation` as the explicit complete-content verification path, including
content hashing and full package/reference validation.

## Scope

- Add a cheap reference-cache hit path based on current file size and stable
  last-write-time ticks before any payload read.
- Preserve the existing persisted content hash and reference occurrences in
  `References.bin`; do not duplicate the hash in `Registry.bin` unless a later
  cache-ownership decision requires it.
- Reuse one loaded byte buffer for fingerprinting, inspection, and reference
  extraction when a package must be processed.
- Add payload-read diagnostics and regression coverage so a logical cache hit
  cannot hide physical I/O.
- Document the fast-mode trust boundary, `FullValidation` behavior, and the
  distinction between mount-wide registry scanning and current-folder UI
  refresh.
- Record the thread-ownership boundary for a future asynchronous full
  validation without making worker execution a prerequisite for this fix.

## Non-Goals

- Making incremental mode cryptographically complete. A tool that changes
  payload bytes while preserving both size and timestamp may be trusted by
  incremental mode until `FullValidation` runs.
- Restricting registry reconciliation to the currently visible Content Browser
  folder. The registry remains authoritative for all auto-scan mounts.
- Introducing a filesystem watcher, a new package envelope, or a new DAST
  format version.
- Loading `DObject` instances, resolving dependencies, or changing package
  residency during scanning.
- Moving the entire existing `ScanMountedContent` implementation to a worker
  thread without a snapshot and publication protocol.

## Design Decisions and Invariants

### Fast-mode trust contract

- `Incremental` may reuse registry metadata and reference occurrences when the
  normalized path, file size, and stable last-write-time ticks match the
  persisted record.
- The persisted content hash remains the identity of the bytes that produced
  the cached reference occurrences. It is carried forward on a cheap cache hit;
  it is recomputed only after a payload read.
- `FullValidation` never trusts persisted registry or reference occurrences.
  It reparses headers, reads complete package contents, computes a current
  content hash, and extracts references for every discovered package.
- Cache corruption, incompatible schema, missing source records, failed file
  metadata queries, and occurrence-bound violations always fall back to the
  existing error/rebuild behavior.

### Cache ownership

- `Registry.bin` remains the persistent discovery snapshot: class, entry kind,
  redirect destination, format version, dependencies, file size, and stable
  timestamp.
- `References.bin` remains the persistent derived reference index and owns the
  full `FAssetPackageFingerprint` plus occurrences. The two caches are
  validated and published independently but use the same scan generation.
- No cache is authoritative over mounted Content. A cheap fingerprint match is
  an explicit accelerator trust decision only.

### Read and parse ownership

- The scan captures current file size and stable timestamp during enumeration.
- The fast path makes the reference-cache decision from those copied values
  before allocating a payload buffer or calling `LoadFileToArray`.
- A changed/new source loads bytes once, computes its fingerprint from those
  bytes, builds the inspection snapshot from the same bytes, and extracts
  references from that snapshot.
- Full validation uses the same one-buffer preparation path where possible. It
  must not re-open a package merely because fingerprinting and inspection are
  separate helpers.

### Thread and publication ownership

- This plan keeps the public `ScanMountedContent` operation synchronous for the
  first implementation. The asset registry, reference index, revision, scan
  diagnostics, and cache publication retain their owning-thread behavior.
- A future worker implementation must snapshot mount definitions, cache data,
  package paths, and an immutable reflection catalog; workers may only publish
  value-owned inspection results through a generation-checked mailbox.
- The game-thread publication must reject stale results after a save, move,
  delete, mount change, cancellation, or a newer scan.

### Determinism and failure behavior

- Package processing order remains sorted by virtual package path.
- Reused and newly extracted occurrences remain deterministically sorted before
  publication.
- A failed source does not create a trusted cache entry and leaves the
  reference index incomplete using the current error model.
- Successful cache writes remain atomic and non-fatal to asset discovery.

## Current Foundations and Gaps

- `FRegistryCacheEntry` already stores `FileSize` and
  `LastWriteTimeTicks` in `AssetSystem.cpp`.
- `FReferenceCacheSource` already stores a full
  `FAssetPackageFingerprint` and extracted occurrences in `References.bin`.
- The reference-index loop checks its cache only after `LoadFileToArray` and
  `MakePackageFingerprint`, which defeats the intended warm-cache I/O benefit.
- `InspectAssetPackage` currently loads the path internally, allowing a
  changed source to be read twice in one reference-index pass.
- `FAssetReferenceIndexStats` reports logical reuse and extraction counts but
  not payload-read attempts or bytes. Existing tests can therefore pass while
  the cache-hit path still reads every package.
- `FContentBrowserModel::RescanRegistry` always requests `Incremental`; F5
  does not pass a current-folder scope to the registry.
- `ExtractAssetReferences` reads global reflected class/property metadata. Its
  current API is not an immutable-catalog worker contract, so asynchronous
  `FullValidation` requires a separate ownership design.

## Implementation Stages

### Stage 0: Freeze the fast-scan contract and add observability

- [x] Update the owning asset-package documentation with the distinction
  between cheap fingerprint trust and complete validation.
- [x] Define reference-index payload-read diagnostics, including attempts and
  bytes, and reset them for every scan.
- [x] Define the expected stats for cold, warm, changed, corrupt-cache, and
  full-validation scans.
- [x] Add a scan-generation or equivalent guard to the design notes before any
  future asynchronous extension.

#### Acceptance Gate

- The documentation explicitly states that `Incremental` trusts size plus
  timestamp and that `FullValidation` is the complete verification boundary.
- A warm-scan test can distinguish zero payload reads from merely zero header
  reads.

### Stage 1: Reuse unchanged reference sources before payload I/O

- [x] Compare the current `FAssetData::FileSize` and
  `FAssetData::LastWriteTimeTicks` with the cached source fingerprint before
  calling `LoadFileToArray`.
- [x] On an incremental cheap match, copy the cached occurrences and cached
  full fingerprint into the new reference snapshot without reading the
  package.
- [x] Keep cache validation, occurrence bounds, error handling, deterministic
  sorting, and `FullValidation` cache bypass unchanged.
- [x] Ensure missing or changed sources take the existing extraction path.

#### Acceptance Gate

- A cold scan extracts every source and persists the reference snapshot.
- A second unchanged incremental scan reports all eligible sources reused and
  records zero reference payload-read attempts and zero payload bytes.
- Changing one package causes only that package to be read and extracted; all
  unchanged packages remain payload-read free.
- A corrupt or incompatible reference cache rebuilds correctly instead of
  trusting incomplete entries.

### Stage 2: Eliminate redundant reads for processed packages

- [x] Add an internal inspection-from-bytes path, or equivalent shared package
  preparation helper, so fingerprinting and complete inspection consume the
  same byte buffer.
- [x] Route changed/new incremental sources through that one-buffer path.
- [x] Route `FullValidation` through the same preparation path where the
  redirector-body and reference-index checks can share the result without
  retaining every package payload at once.
- [x] Preserve the current complete package parser and reference extractor as
  the validation implementation; this stage changes I/O ownership, not the
  validation rules.

#### Acceptance Gate

- A processed source is not reopened solely because the fingerprint and
  inspection helpers are separate.
- Full validation still rejects truncated, corrupt, unsupported, malformed,
  and reference-schema-incompatible packages.
- Existing package and reference-index behavior remains unchanged apart from
  reduced redundant I/O.

### Stage 3: Regression, performance, and workflow validation

- [x] Add a large-payload fixture whose header is small and whose unchanged
  reference cache must produce zero payload reads on the warm scan.
- [x] Add a same-size, timestamp-restored mutation case: incremental mode may
  reuse the source by contract, while `FullValidation` must detect the changed
  content and rebuild its references.
- [x] Cover added, removed, renamed, changed, duplicate, corrupt, and mount
  manifest cases with reference-read diagnostics.
- [x] Verify Content Browser F5 still refreshes the current visible folder
  snapshot while the registry scan covers all auto-scan mounts.
- [x] Run the focused AssetCore tests, the required editor/native validation,
  and the full build using the repository build and test instructions.

#### Acceptance Gate

- No unchanged warm scan performs a `.dasset` payload read.
- Full validation remains observably complete and is not accidentally routed
  through the fast trust path.
- Startup and Content Browser incremental scans use the optimized path without
  changing the published registry or reference-index semantics.

## Validation Matrix

| Scenario | Registry headers | Reference payloads | Expected result |
| --- | ---: | ---: | --- |
| Cold/missing caches | Read all discovered packages | Read and extract all sources | Fresh deterministic snapshots |
| Warm unchanged incremental scan | Zero rereads | Zero reads; reuse all valid sources | Same revision and cache bytes |
| One changed package | Reparse one header | Read/extract one source | Unchanged sources remain reused |
| Added or removed package | Read added header | Read added source; remove stale source | Complete reconciled map |
| Same size and restored timestamp | Reuse header and reference source | Zero reads in incremental mode | Fast-mode trust is explicit |
| Same mutation under `FullValidation` | Read all headers | Read/hash/parse all sources | Current content determines index |
| Corrupt reference cache | Existing rebuild behavior | Read sources as needed | Warning plus recovered cache |
| Content Browser F5 | All auto-scan mounts | Same scan scope | Current folder subtree is redrawn |

## Definition of Done

- The reference-index cache hit is decided before any payload read.
- Warm incremental scans expose zero `.dasset` payload-read attempts for
  unchanged sources.
- Changed/new packages are fingerprinted, inspected, and indexed without
  redundant file reopening.
- `FullValidation` remains a complete, explicit verification path and is
  documented as the escape hatch from fast-mode timestamp trust.
- Tests assert physical-read diagnostics, not only logical reuse counters.
- Asset-package and task/thread ownership documentation reflects the final
  behavior.
- The focused tests and required full build pass according to the repository
  build instructions.
- The implementation, tests, and required documentation are committed in one
  bounded change after unrelated working-tree changes are preserved.

## Deferred Follow-ups

- Make `FullValidation` user-invocable and asynchronous if profiling shows it is
  used interactively or blocks editor responsiveness on production projects.
- If async validation is adopted, freeze the reflection catalog and publish
  value-only worker results through the existing task/mailbox ownership model.
- Add a folder-scoped discovery API only if a separate workflow needs it; do
  not change the meaning of Content Browser F5 implicitly.
- Consider cryptographic or content-addressed invalidation only for an
  explicitly stronger validation mode, since computing it during every fast
  scan would restore the payload-read cost this plan removes.

## Related Documentation

- `Documentation/Runtime/Assets/AssetPackages.md`
- `Documentation/Runtime/Core/TaskSystem.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/NativeTests.md`
- `Documentation/Plans/Archive/2026-07/AssetRegistryAndThumbnailCache.md`

## Related Code

- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserModel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanelView.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
- `Engine/Source/Runtime/Core/Private/Misc/Paths.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
