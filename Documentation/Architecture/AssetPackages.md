# Asset Packages

Durin object assets are stored as versioned `.dasset` packages. A package has one public main asset and may contain any number of `DObject` instances arranged through the ordinary Outer hierarchy. Outer defines structural containment and object paths, not a GC strong reference.

## Paths And Mounts

Asset identities use extensionless virtual paths such as `/Engine/Materials/Default` or `/Game/Levels/TestLevel`. The first path segment must match a registered content mount. `PathUtilities::InitDefaultMountPoints()` registers `/Engine/` and mounts the `Content` directory of the project selected with `--project=<path-to-project.dproject>` at `/Game/`, independent of its `ProjectName`.

Code that already owns non-empty, absolute, lexically normalized physical paths uses `PathUtilities::TryMakeLexicalRelativePath()` and `IsLexicalDescendantPath()` for mount containment. These helpers compare complete path components, reject `..`, ignore trailing separators, require matching roots, and treat Windows components as case-insensitive. Equality is valid containment for physical-to-virtual mount conversion but is not a descendant relationship. Filesystem-querying canonicalization remains limited to boundaries that actually inspect filesystem state.

The physical filename is the resolved virtual path plus `.dasset`. Main assets use the package path as their object path; inner objects append a colon and their relative Outer chain, for example `/Game/Objects/Test:Root.Component`.

## Runtime Lifetime

`DPackage` is an Outer-less object graph root. The asset manager roots loaded packages for garbage collection and caches one package instance per `FAssetPath`. Unload removes the package from the active cache, calls `MarkObjectHierarchyAsGarbage()` for the package tree, and runs GC so the path can be loaded again only after GC-controlled physical removal. Objects that must survive unload must be reparented out of that package first. `DPackage::Asset` is a `TObjectPtr` that strongly retains the main asset; arbitrary descendants remain alive only through actual GC strong references, not merely because their Outer is the package or asset. A package cannot unload while another loaded package declares it as a strong dependency.

Compiled-in reflection metadata uses a separate `Cpp` package kind. Each reflected module has one rooted `/Cpp/<ModuleName>` package whose structural children are its `DClass`, `DStruct`, and `DEnum` metadata. Those metadata objects are permanent independently of the package's Outer relationship. Cpp packages have no main asset, are not saved as `.dasset`, and remain alive for the process lifetime. CoreDObject intrinsic types are attached to `/Cpp/CoreDObject` after reflection bootstrap completes.

## File Format

The v2 binary header records the magic, format version, main asset class, dependencies, and object count. The asset path is derived from the mounted package filename, so moving a package within a content mount does not rewrite its payload. The registry reads only this header.

Object records store object id, outer id, qualified class name, object name, and a field table. Fields are identified by declaring qualified class plus property name and include a recursive type signature and payload size. Unknown, removed, or type-incompatible fields are skipped while missing fields retain constructor defaults. Unknown classes, invalid Outer hierarchies, malformed references, truncation, and unsupported versions fail the complete load.

Internal references use object ids. Cross-package strong references target the other package's main asset by `FAssetPath` and synchronously load that dependency. Circular dependencies work because object skeletons are constructed before dependency fields are applied.

Supported reflected payloads are numeric values, bool, strings, enums, `DStruct` values, `TObjectPtr`, vectors, maps, and nesting of those containers. Struct payloads contain a qualified-name field table, so unknown, missing, and type-incompatible struct fields follow the same compatibility rules as object fields. Raw object pointers and property kinds without runtime helpers fail package saving instead of being silently omitted.

## Subsystem Boundary

- `CoreDObject` owns `DPackage`, `FAssetPath`, object paths, qualified reflected class identities, and type-erased container access.
- `AssetCore` owns `.dasset` I/O, the synchronous asset registry, package caching, dependency loading, and existing source-file importers.
- `DLevel` objects are main assets inside packages; a `DWorld` remains a runtime/editor session container and activates one level at a time.

Deferred work includes soft references, async loading, cooking, reimport, hot reload, redirects, and editor asset browsing.

Package format versions are independent of the Durin engine release version. An engine release does not rewrite packages unless their own format or serialized schema requires a migration.

## Rebuildable Asset Caches

Durin stores asset discovery metadata and source-image thumbnails beneath `FPaths::DerivedDataCacheDir()`. The active project owns this directory; without a project, the engine directory is the fallback. It is ignored generated data, is never a content mount, and may be deleted in full while the editor is stopped. Mounted `Content` remains authoritative and repopulates both caches.

`AssetRegistry/Registry.bin` is a versioned, deterministic snapshot keyed by virtual mount root and normalized mount-relative package path. Startup still enumerates mounted `.dasset` files once. Exact file-size and stable last-write-time matches reuse cached class, format-version, and dependency metadata without reading package headers; new or changed files use the bounded header reader, and missing files disappear from the freshly published live map. Full validation bypasses fingerprint reuse. Schema, package-format, serialization, or mount-manifest incompatibility and corrupt or missing snapshots cause a non-fatal rebuild. Successful mutations update the live registry and dirty the snapshot, which is atomically replaced after explicit reconciliation and during orderly asset-manager shutdown. The live registry exposes a monotonic process-local revision that advances only when its published asset metadata changes, allowing editor queries to cache derived views safely. Scan diagnostics expose elapsed milliseconds, enumeration/reuse/reparse/removal/failure counts, and package-header read attempts and bytes.

`Thumbnails/Index.bin` maps stable source and generator keys to resized PNG objects under `Thumbnails/Objects/`. Keys include normalized source identity, source size and last-write time, maximum dimensions, generator schema, color-space policy, and output encoding version. A warm hit decodes the generated PNG without reopening the source image, then follows the existing asynchronous, serial-validated RHI upload path. Missing or modified sources, changed settings or versions, corrupt indexes or PNGs, and missing objects become safe misses. Object and index writes are atomic; persistence failure does not fail a valid in-memory thumbnail. Encoded objects use an independent disk LRU budget, while RHI textures retain their process-local GPU budget. Cleanup resolves and validates every target beneath the exact thumbnail cache root before deletion.

`Textures/Objects/<key-prefix>/<key>.bin` stores Texture2D platform mip chains.
The 128-bit key covers the imported source-content hash, every platform build
setting, the target platform, and the texture-builder version. The package keeps
the source hash plus file size and stable last-write time, allowing an unchanged
source to reach the cache without reopening or decoding the image. Each cache
object has a versioned header, bounded mip records, and a payload checksum.
Missing, incompatible, truncated, corrupt, or structurally invalid objects are
safe misses; a successful source build atomically replaces the object, while a
cache write failure does not invalidate usable in-memory platform data.

Texture DDC objects are content-addressed generated files, so `.dasset` packages
do not store cache paths or byte offsets. External cooked bulk payloads and their
descriptors remain a separate future package-format concern.

Repository storage rules for packages, source assets, and generated data are documented in [Content Version Control](../Git/ContentVersionControl.md).
