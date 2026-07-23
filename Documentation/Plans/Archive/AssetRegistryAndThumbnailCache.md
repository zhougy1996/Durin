# Asset Registry and Thumbnail Cache Plan

Last reviewed: 2026-07-24

## Current Status

Complete. Registry discovery now validates and reconciles a deterministic project-local snapshot, reuses exact fingerprint matches, supports explicit full validation, atomically publishes a fresh live map, and reports scan timing and I/O diagnostics. Successful save, load-time discovery, move, delete, and import-style save operations mark the snapshot dirty; explicit reconciliation and orderly asset-manager shutdown flush coalesced mutations without making cache failures fatal. Source-image thumbnails use a versioned project-local index and key-addressed PNG objects, serve warm requests without reopening the source image, preserve the existing asynchronous upload and GPU-eviction lifecycle, and safely regenerate after fingerprint, settings, version, index, or object invalidation. Final validation on 2026-07-24 passed all 29 `AssetCoreTests`, all 171 `EngineTests`, the complete `Win64-Debug-DurinEditor-Tests` `all` build, and two eight-second hidden-window DurinEditor launches covering cold and restart startup. The focused registry scenario measured two header reads/118 bytes cold, zero reads warm, one read/59 bytes after one asset changed, and two reads/118 bytes under full validation. Long-lived ownership, invalidation, recovery, and budget rules are recorded in `Documentation/Architecture/AssetPackages.md`.

## Goal

Make editor and game startup reuse previously discovered asset metadata while still detecting added, removed, changed, corrupt, or incompatible `.dasset` packages, and make Content Browser texture thumbnails survive editor restarts without treating either cache as authored content. Deleting `DerivedDataCache` must always recover by rebuilding from mounted Content directories.

## Scope

- Persist the metadata currently held by `FAssetRegistry` for every registered mount point.
- Reconcile the persisted snapshot against mounted `.dasset` files using a cheap filesystem fingerprint before trusting cached entries.
- Read only the package header for new or changed assets instead of loading the complete package into memory.
- Update the persistent snapshot after successful asset save, move, delete, import, explicit refresh, and startup reconciliation.
- Persist resized Texture2D source-image thumbnails and feed them through the existing asynchronous CPU decode and GPU upload lifecycle.
- Version, validate, atomically replace, budget, and safely rebuild both cache domains.
- Add automated coverage for cache hits, invalidation, corruption, mount changes, and thumbnail expiry.

## Non-Goals

- Eliminating startup directory enumeration or relying on a filesystem watcher as the only source of truth.
- Treating the registry cache as authoritative when Content disagrees with it.
- Computing a full content hash for every `.dasset` on every ordinary startup.
- Caching loaded `DObject` instances, serialized object graphs, cooked payloads, or runtime GPU resources.
- Adding preview renderers for materials, static meshes, levels, or other asset classes. Their future generators may use the thumbnail cache contract after they exist.
- Sharing one cache between projects, machines, users, or engine installations.
- Placing generated data in `Content`, committing it to source control, or changing the existing `Intermediate` build-output responsibility.
- Adding filesystem change notifications in the first implementation.

## Design Decisions and Invariants

### Cache ownership and layout

- With a project loaded, the cache root is `<Project>/DerivedDataCache/`. It may contain metadata for both project and engine mounts; duplicating the small engine registry per project is preferred to writing into a possibly read-only engine installation.
- Without a project, the fallback root is `<Engine>/DerivedDataCache/`.
- The initial layout is:

  ```text
  DerivedDataCache/
    AssetRegistry/
      Registry.bin
    Thumbnails/
      Index.bin
      Objects/
        <key-prefix>/
          <key>.png
  ```

- Neither `DerivedDataCache` nor any child may be registered as a mount point.
- Both caches are optional accelerators. Missing, corrupt, partially written, or incompatible cache data results in a rebuild, not a startup failure.

### Registry identity and reconciliation

- A registry entry is identified by its mount virtual root plus its normalized path relative to that mount. Absolute paths are retained only as current-session resolved data so relocating a checkout does not invalidate every entry.
- The cheap file fingerprint is exact file size plus normalized last-write-time ticks. A matching path and fingerprint permits reuse of cached class, format-version, and dependency metadata.
- Persisted timestamps are signed nanoseconds in the platform filesystem-clock domain with fixed little-endian encoding. Finer filesystem precision is truncated; the serialization marker rejects incompatible cache ABIs.
- A path absent from the cache is new; a cached path absent from disk is removed; a fingerprint mismatch is changed. New and changed files have their package header reparsed before entering the live registry.
- `size + mtime` is an intentional fast-path contract, not cryptographic identity. An explicit full-validation mode reparses every header for tests, diagnostics, CI, and recovery from tools that preserve timestamps. Ordinary editor refresh uses incremental reconciliation.
- Cache-wide compatibility includes a registry schema version, the supported `.dasset` package format version, endianness/serialization marker, and a deterministic manifest of normalized mount virtual roots. A mismatch discards the snapshot before reconciliation.
- The mount manifest does not include absolute physical roots. Moving a checkout remains a cache hit when the same cache directory moves with the project; changing virtual roots invalidates the snapshot.
- Duplicate virtual asset paths, invalid packages, and unsupported package versions remain scan errors and never produce trusted cached entries.

### Registry persistence and mutation

- Startup first attempts to load the snapshot, then enumerates all mounted `.dasset` paths exactly once and reconciles them into a fresh live map. It never exposes a partially reconciled map.
- The cache writer serializes deterministic entry order to a temporary sibling file, flushes and closes it, then replaces `Registry.bin`. Failure leaves the previous snapshot usable or forces a later rebuild.
- Cache persistence failure is reported as a warning and must not make asset discovery, save, move, delete, or load fail.
- Engine-owned asset mutations update the live registry immediately. The persistent snapshot may be marked dirty and coalesced, but it must be flushed during orderly asset-manager shutdown and after operations that already require an explicit Content Browser refresh.
- External filesystem modifications are detected on the next reconciliation. A later filesystem-watcher implementation may request reconciliation but may not mutate registry truth directly.

### Package header I/O

- Header-only scanning uses a bounded file reader and stops after the class name, dependency list, and object-count validation required by the registry.
- Header parsing and full package parsing share one validation implementation so cached discovery cannot accept a file that normal loading would reject at the same structural boundary.
- Header-controlled string lengths and dependency counts remain bounded before allocation. A truncated file or declared header larger than the file produces a scan error without reading the object payload.
- The package layout need not gain a fixed-size header in the first stage; the reader must nevertheless avoid `LoadFileToArray` for header-only discovery.

### Thumbnail cache identity and lifecycle

- The first implementation persists only thumbnails already supported by `FSourceImageThumbnailCache`: Texture2D source images and directly browsed source-image files.
- A thumbnail key includes a normalized source identity relative to its mount or project, source file size, source last-write-time, thumbnail generator schema version, requested maximum dimensions, color-space policy, and output encoding version.
- Path remains part of the initial key, so rename may cause regeneration. Cross-path deduplication by source content hash is deferred because computing that hash merely to look up a thumbnail would erase much of the startup I/O benefit.
- Persistent PNG contains only resized CPU image data. RHI textures remain process-local, are uploaded through the current queue, and continue to obey the existing GPU-memory eviction budget.
- A disk hit still validates the index entry, encoded file existence, decoded dimensions, and configured size limit before upload. Invalid data is deleted or ignored and regenerated from the source.
- Source fingerprint changes, generator schema changes, output-setting changes, or missing source files invalidate the lookup without requiring eager deletion of the old object.
- `Index.bin` uses the same versioned, atomic replacement policy as the registry. Orphan objects are reclaimed by a bounded maintenance pass using least-recently-used metadata and a configurable byte budget; cache cleanup never walks or deletes outside `DerivedDataCache/Thumbnails`.
- Generation is asynchronous. Visible items have priority, duplicate requests for the same key coalesce, stale serials cannot upload, and shutdown/cancellation cannot publish a result after its source entry was invalidated.

### Thread boundaries

- Mount enumeration, live registry replacement, cache index publication, and RHI texture registration/unregistration occur on their existing owning threads.
- File header reads and thumbnail file decode/encode may run on worker threads after their immutable request data has been captured.
- Worker completion is validated against a request generation/serial before it can update live state.

## Current Foundations and Gaps

- `FAssetRegistry::ScanMountedContent` already enumerates every registered mount and builds a fresh map, but it reads every complete `.dasset` through `LoadFileToArray` even when requesting header-only parsing.
- `FAssetData` already contains package path, physical path, asset class, package format version, dependencies, and last-write time. It lacks file size and a portable persisted fingerprint representation.
- Asset save and load already call `AddOrUpdate`; move and delete paths already have centralized registry-aware operations that can mark persistence dirty.
- Default mounts already distinguish `/Engine/` from the active project virtual root.
- `FSourceImageThumbnailCache` already provides asynchronous decode/upload, visible-item prioritization, request serial validation, source size/mtime invalidation, and GPU eviction. It has no disk index, resized encoded output, or restart persistence.
- `DerivedDataCache/` is already documented as ignored and rebuildable in `Documentation/Git/ContentVersionControl.md` and excluded by the repository `.gitignore`.
- `FPaths` exposes project and engine roots but does not yet expose a canonical derived-data-cache root.

## Implementation Stages

### Stage 0: Cache contracts and paths

- [x] Add a canonical `FPaths` query for the active derived-data-cache root using the project-first and engine-fallback ownership rule.
- [x] Define versioned registry-cache and thumbnail-index headers, bounds, deterministic ordering, and failure reporting.
- [x] Define a platform-stable persisted timestamp representation and document its conversion/precision behavior.
- [x] Extend `FAssetData` or an internal scan record with file size and the normalized fingerprint used by reconciliation.
- [x] Add test-only cache-root override/injection so tests never write to a developer project cache.

#### Acceptance Gate

- Path tests prove project and no-project roots resolve only beneath the intended `DerivedDataCache` directory.
- Serialization round-trip tests prove deterministic bytes, bounds checking, version rejection, and no dependency on absolute checkout paths.

### Stage 1: Bounded package-header reader

- [x] Refactor package parsing so header-only discovery reads from a bounded file stream/range instead of loading the complete file.
- [x] Preserve one validation path for magic, package version, class name, dependencies, object count, truncation, and allocation limits.
- [x] Keep full package loading behavior and serialization compatibility unchanged.
- [x] Instrument tests with a package containing a large object payload and prove header discovery does not read that payload.

#### Acceptance Gate

- Existing AssetCore package tests pass unchanged.
- New tests cover valid, truncated, corrupt, unsupported-version, and oversized-declaration headers.
- A header-only test demonstrates read volume is bounded by header data rather than total `.dasset` size.

### Stage 2: Persistent registry reconciliation

- [x] Load and validate `AssetRegistry/Registry.bin` before mounted-content discovery.
- [x] Enumerate each mounted directory once and classify unchanged, added, modified, removed, duplicate, and erroneous entries.
- [x] Reuse metadata only for exact cheap-fingerprint matches; reparse new and modified headers.
- [x] Build a fresh map and publish it atomically after reconciliation completes.
- [x] Implement explicit full validation that reparses all discovered headers while retaining the same error reporting and final-map semantics.
- [x] Serialize the reconciled snapshot deterministically through temporary-file replacement.
- [x] Expose diagnostic counters for enumerated, reused, reparsed, removed, and failed entries so startup behavior is testable and observable.

#### Acceptance Gate

- A second unchanged scan reparses zero package headers and returns the same registry contents as a cold scan.
- Add, modify, delete, rename, duplicate-path, corrupt-cache, incompatible-version, and changed-mount-manifest tests all converge to the same live map as a cold full scan.
- Moving the project checkout while retaining relative layout does not invalidate otherwise unchanged entries.
- Cache read/write failures leave asset discovery functional and produce a diagnostic without exposing a partial registry.

### Stage 3: Mutation integration and lifecycle

- [x] Mark the registry snapshot dirty from successful save, create, move, delete, import, and load-time discovery updates.
- [x] Coalesce writes without allowing shutdown to lose successful in-process registry mutations.
- [x] Route editor startup, game startup, Content Browser refresh, and existing post-import rescans through the reconciliation API with explicit incremental/full intent.
- [x] Ensure shutdown flush ordering occurs before asset-manager state is destroyed.
- [x] Report cache statistics and recoverable persistence warnings without turning them into asset-operation failures.

#### Acceptance Gate

- Integration tests exercise each mutation followed by process-style registry reconstruction from the persisted snapshot.
- Editor and game startup observe identical registry contents for the same mounts.
- Forced persistence failure does not change the success result or on-disk authored output of a valid asset mutation.

### Stage 4: Persistent source-image thumbnails

- [x] Introduce a versioned thumbnail index and content-addressed object path confined to `DerivedDataCache/Thumbnails`.
- [x] Extend thumbnail requests with generator parameters and derive stable keys from source identity, fingerprint, and generator/output versions.
- [x] On a disk hit, asynchronously decode the cached resized PNG and use the existing validated upload path.
- [x] On a miss, decode the source once, resize to the cache dimensions, atomically encode/store the PNG, update the index, and upload the same generated pixels.
- [x] Coalesce concurrent requests and preserve visible-item priority, cancellation, serial invalidation, and GPU eviction behavior.
- [x] Add bounded LRU maintenance for encoded objects and stale index entries with resolved-path containment checks before deletion.

#### Acceptance Gate

- The first request generates one disk object; a new cache instance serves the unchanged source without decoding the original image.
- Source changes, generator-version changes, settings changes, corrupt PNGs, missing objects, and cancelled requests regenerate safely and never upload stale pixels.
- Disk and GPU budgets are independent and remain within configured limits under a many-thumbnail stress test.
- All cleanup targets resolve beneath the exact thumbnail-cache root.

### Stage 5: End-to-end validation and architecture handoff

- [x] Add focused timing and I/O counters comparing cold startup, warm startup, one changed asset, and full validation.
- [x] Run native AssetCore and editor thumbnail tests through the repository BuildTool workflow.
- [x] Complete a full editor build and hidden-window DurinEditor smoke test using the same preset.
- [x] Manually verify Content Browser refresh, picker filtering, texture thumbnail restart hits, and recovery after deleting `DerivedDataCache`.
- [x] Record long-lived cache ownership, invalidation, and recovery rules in Architecture documentation after the implementation stabilizes.

#### Acceptance Gate

- Warm startup enumerates Content but performs no package-header reads for unchanged assets and returns the same registry as full validation.
- Changing one `.dasset` reparses exactly that package; adding and removing files updates the registry correctly.
- Texture thumbnails remain visually correct across restart and regenerate after source modification.
- Deleting or corrupting either cache produces a clean rebuild with no authored-content loss and no fatal startup error.
- The required full build, native tests, and hidden-window editor smoke test pass.

## Validation Matrix

| Area | Scenario | Required evidence |
| --- | --- | --- |
| Registry cold path | No cache exists | All valid headers are parsed, errors are retained, and an atomic snapshot is created |
| Registry warm path | Files and mounts unchanged | All entries are reused, zero headers are reparsed, and results equal a full scan |
| Registry delta | Add, modify, delete, and rename | Only required headers are parsed and stale entries disappear |
| Fingerprint limitation | Same size and preserved mtime | Explicit full validation detects changed header metadata and rewrites the snapshot |
| Compatibility | Registry schema, package version, or virtual mount roots change | Snapshot is rejected and rebuilt without fatal failure |
| Corruption | Truncated cache, invalid counts, duplicate entries, or interrupted replacement | Cache is ignored; Content remains authoritative |
| Relocation | Project absolute path changes | Relative identities retain valid warm-cache entries |
| Mutation | Save, move, delete, import, and orderly shutdown | Reconstructed process observes the final registry state |
| Thumbnail cold path | No thumbnail object exists | Source is decoded, resized output is stored, and pixels upload successfully |
| Thumbnail warm path | Source fingerprint and settings unchanged | Cached resized object is decoded without reopening the source image |
| Thumbnail invalidation | Source, generator, dimensions, or encoding changes | Old object is not selected and a new key is generated |
| Thumbnail resilience | Index/object corruption or cancellation | Safe miss/regeneration occurs and stale completion cannot upload |
| Cleanup safety | Disk budget exceeded | Old objects are reclaimed only inside the configured thumbnail root |
| Recovery | Entire `DerivedDataCache` deleted | Registry and thumbnails rebuild from authored Content |

## Definition of Done

- Warm mounted-content discovery still enumerates disk but does not read unchanged `.dasset` headers or payloads.
- Live registry results after every tested delta and failure match a cold source-of-truth scan.
- Cache format, package format, and mount incompatibilities are detected deterministically.
- Registry writes and thumbnail index/object writes are atomic and recoverable.
- Existing asset save/load/move/delete behavior and package compatibility remain intact.
- Existing Texture2D Content Browser thumbnails persist across restart, invalidate correctly, and remain asynchronously uploaded and budgeted.
- Both cache domains live exclusively under the documented ignored `DerivedDataCache` root and are safe to delete.
- Required unit, integration, full-build, and hidden-window smoke validation passes.
- Long-lived design rules are transferred to Architecture documentation and this plan status is updated.

## Deferred Follow-ups

- Filesystem watchers that trigger targeted reconciliation during an editor session.
- Operating-system change journals for startup acceleration without full directory enumeration.
- Shared local or remote derived-data caches across projects and machines.
- Cross-path thumbnail deduplication using source content hashes produced by a broader import/derived-data pipeline.
- Rendered thumbnail generators for materials, static meshes, levels, and other asset classes.
- Persisted dependency graphs or reverse-reference indices beyond the metadata already stored by `FAssetRegistry`.
- Cryptographic verification of every asset on ordinary startup.

## Related Documentation

- [Content Version Control](../../Git/ContentVersionControl.md)
- [Level System](../../Architecture/LevelSystem.md)
- [Build and Run](../../Setup/BuildAndRun.md)
- [Native Tests](../../Setup/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/AssetSystem.h`
- `Engine/Source/Runtime/AssetCore/Private/AssetSystem.cpp`
- `Engine/Source/Runtime/Core/Public/Misc/Paths.h`
- `Engine/Source/Runtime/Core/Private/Misc/Paths.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SourceImageThumbnailCache.h`
- `Engine/Source/Editor/LevelEditor/Private/Assets/SourceImageThumbnailCache.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Programs/Tests/AssetCoreTests/Private/PackageTests.cpp`
