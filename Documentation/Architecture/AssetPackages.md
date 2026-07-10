# Asset Packages

Durin object assets are stored as versioned `.dasset` packages. A package has one public main asset and may contain any number of inner `DObject` instances through the ordinary Outer/Inner ownership tree.

## Paths And Mounts

Asset identities use extensionless virtual paths such as `/Engine/Materials/Default` or `/SandBox/Levels/TestLevel`. The first path segment must match a registered content mount. `PathUtilities::InitDefaultMountPoints()` registers `/Engine/` and every project listed in `Engine/Configs/RegisteredProjects.json`.

The physical filename is the resolved virtual path plus `.dasset`. Main assets use the package path as their object path; inner objects append a colon and their relative Outer chain, for example `/SandBox/Objects/Test:Root.Component`.

## Runtime Ownership

`DPackage` is an Outer-less object graph root. The asset manager roots loaded packages for garbage collection, caches one package instance per `FAssetPath`, and destroys the complete inner graph on unload. A package cannot unload while another loaded package declares it as a strong dependency.

Compiled-in reflection metadata uses a separate `Cpp` package kind. Each reflected module owns one rooted `/Cpp/<ModuleName>` package whose inner objects are its `DClass`, `DStruct`, and `DEnum` metadata. Cpp packages have no main asset, are not saved as `.dasset`, and remain alive for the process lifetime. CoreDObject intrinsic types are attached to `/Cpp/CoreDObject` after reflection bootstrap completes.

## File Format

The v1 binary header records the magic, format version, package path, main asset class, dependencies, and object count. The registry reads only this header.

Object records store object id, outer id, qualified class name, object name, and a field table. Fields are identified by declaring qualified class plus property name and include a recursive type signature and payload size. Unknown, removed, or type-incompatible fields are skipped while missing fields retain constructor defaults. Unknown classes, invalid ownership, malformed references, truncation, and unsupported versions fail the complete load.

Internal references use object ids. Cross-package strong references target the other package's main asset by `FAssetPath` and synchronously load that dependency. Circular dependencies work because object skeletons are constructed before dependency fields are applied.

Supported reflected payloads are numeric values, bool, strings, enums, `DStruct` values, `TObjectPtr`, vectors, maps, and nesting of those containers. Struct payloads contain a qualified-name field table, so unknown, missing, and type-incompatible struct fields follow the same compatibility rules as object fields. Raw object pointers and property kinds without runtime helpers fail package saving instead of being silently omitted.

## Subsystem Boundary

- `CoreDObject` owns `DPackage`, `FAssetPath`, object paths, qualified reflected class identities, and type-erased container access.
- `AssetCore` owns `.dasset` I/O, the synchronous asset registry, package caching, dependency loading, and existing source-file importers.
- `DLevel` objects are main assets inside packages; a `DWorld` remains a runtime/editor session container and activates one level at a time.

Deferred work includes soft references, async loading, cooking, reimport, hot reload, redirects, and editor asset browsing.
