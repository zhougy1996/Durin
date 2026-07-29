# Asset Packages

Durin object assets are stored as versioned `.dasset` packages. A package has one public main asset and may contain any number of `DObject` instances arranged through the ordinary Outer hierarchy. Outer defines structural containment and object paths, not a GC strong reference.

## Paths And Mounts

Asset identities use extensionless `FAssetPath` values such as
`/Engine/Materials/Default` or `/Game/Levels/TestLevel`. The first path segment
must match a registered mount with a `Content` domain. `FSourcePath` uses the
same logical mount identity but resolves only through its `SourceAssets`
domain; it retains the filename extension. Neither virtual path includes the
physical `Content` or `SourceAssets` directory name.

The immutable Core registry publishes `/Engine/` and `/Game/` plus validated
project-declared extension and external-source mounts. A mount may expose
Content only, SourceAssets only, or both. Typed resolution reports invalid
paths, unknown mounts, unsupported or unavailable domains, escapes, missing
files, forbidden dependencies, and read-only source policy distinctly.
Existing paths and not-yet-created destinations are checked against canonical
domain roots, including junction and symbolic-link targets.

The physical filename is the resolved virtual path plus `.dasset`. Main assets use the package path as their object path; inner objects append a colon and their relative Outer chain, for example `/Game/Objects/Test:Root.Component`.

## Runtime Lifetime

`DPackage` is an Outer-less object graph root. The asset manager roots loaded packages for garbage collection and caches one package instance per `FAssetPath`. Unload removes the package from the active cache, calls `MarkObjectHierarchyAsGarbage()` for the package tree, and runs GC so the path can be loaded again only after GC-controlled physical removal. Objects that must survive unload must be reparented out of that package first. `DPackage::Asset` is a `TObjectPtr` that strongly retains the main asset; arbitrary descendants remain alive only through actual GC strong references, not merely because their Outer is the package or asset. A package cannot unload while another loaded package declares it as a strong dependency.

Compiled-in reflection metadata uses a separate `Cpp` package kind. Each reflected module has one rooted `/Cpp/<ModuleName>` package whose structural children are its `DClass`, `DStruct`, and `DEnum` metadata. Those metadata objects are permanent independently of the package's Outer relationship. Cpp packages have no main asset, are not saved as `.dasset`, and remain alive for the process lifetime. CoreDObject intrinsic types are attached to `/Cpp/CoreDObject` after reflection bootstrap completes.

## File Format

Every authored or cooked `.dasset`, regardless of its main asset class, uses the
same DAST object-package envelope. The v2 binary header records the `DAST`
magic, format version, main asset class, dependencies, and object count. The
asset path is derived from the mounted package filename, so moving a package
within a content mount does not rewrite its payload. The registry reads only
this header. The current reader and writer require v2. Package format versions
describe this wire contract only; reflected property evolution does not require
a package-format increment.

Asset-specific magic values belong to external derived or cooked payloads, not
to alternative `.dasset` envelopes. StaticMesh payloads use DMSH and texture
payloads use TXPL. A cooked `.dbulk` uses the DBLK container format and may
contain one of those asset-specific payloads; the cooked `.dasset` that
references it still begins with DAST.

Object records store object id, outer id, qualified class name, object name, and a field table. Fields are identified by declaring qualified class plus property name and include a recursive type signature and payload size. Missing fields retain constructor defaults. Unknown classes, invalid Outer hierarchies, malformed references, truncation, and unsupported versions fail the complete load.

## Structure Compatibility

AssetCore retains every unknown or removed serialized object field
in an `FAssetLoadReport` instead of reducing compatibility to a log message.
Each object-level issue records the package and object identity, declaring
class, original field signatures and payloads, classification, risk, summary,
and optional handler identifier. Related legacy fields may be grouped into one
issue.

AssetCore owns report construction, payload retention, object-reference
resolution helpers, and class-specific upgrader registration. Modules that own
concrete reflected types register their legacy-field rules without introducing
an AssetCore dependency on those types. A registered rule may classify a
complete change as `SafeCleanup` or `Migrated`; either classification marks the
loaded package Dirty so an explicit save can persist it. Unhandled fields are
`UnknownIncompatible` with `UnknownNewerSchema` risk and do not themselves mark
the package Dirty. Rules that cannot preserve the represented data use
`DataLossRisk`.

A package whose load report contains compatibility-risk payloads rejects an
ordinary save. Persisting the in-memory representation requires the caller to
pass explicit data-loss consent; this prevents unknown newer fields from being
silently discarded by normal editor saves. Opening without saving retains the
loaded package and its Dirty state, so a later unload and reload presents the
same compatibility workflow.

Internal references use object ids. Cross-package strong references target the other package's main asset by `FAssetPath` and synchronously load that dependency. Circular dependencies work because object skeletons are constructed before dependency fields are applied.

Supported reflected payloads are numeric values, bool, strings, enums,
`DStruct` values, `TObjectPtr`, vectors, maps, and nesting of those containers.
Struct payloads contain a qualified-name field table. Missing fields retain
constructor defaults and unknown names are skipped, but a serialized field
whose name matches the current struct with a different kind or recursive type
signature fails loading with `TypeMismatch`; it is never reinterpreted. This
deliberately rejects retired string `SourcePath` provenance now that mounted
`FSourcePath` is authoritative. Raw object pointers and property kinds without
runtime helpers fail package saving instead of being silently omitted.

## Subsystem Boundary

- `CoreDObject` owns `DPackage`, `FAssetPath`, object paths, qualified reflected class identities, and type-erased container access.
- `AssetCore` owns `.dasset` I/O, the synchronous asset registry, package caching, dependency loading, structure-compatibility reports and upgrader registration, DDC storage, and cooked container/publication primitives.
- `Engine` owns asset-specific source provenance, import/build policy, derived-data keys and codecs, and cook contributions.
- Editor modules invoke the asset-specific import and reimport workflows.
- `DLevel` objects are main assets inside packages; a `DWorld` remains a runtime/editor session container and activates one level at a time.

Asset-level cooking and deterministic cooked publication are implemented for
StaticMesh, Texture2D, and TextureCube. Complete project discovery, editor or
DurinDevTool packaging commands, and installable-build orchestration are not yet
connected. Other deferred package-system work includes soft references, async
loading, hot reload, redirects, and broader editor asset browsing.

Package format versions are independent of the Durin engine release version.
Adding, removing, or reordering tagged reflected fields does not change the
package format. An engine release rewrites a package only when the package byte
contract changes or a recognized legacy-field upgrader produces canonical
authored state.

## Rebuildable Asset Caches

Durin stores asset discovery metadata and source-image thumbnails beneath `FPaths::DerivedDataCacheDir()`. The active project owns this directory; without a project, the engine directory is the fallback. It is ignored generated data, is never a content mount, and may be deleted in full while the editor is stopped. Mounted `Content` remains authoritative and repopulates both caches.

Compiled shader manifests, SPIR-V, and reflection sidecars also live beneath `DerivedDataCache/Shaders/`. Their virtual shader mount namespace prevents engine, project, and extension shaders from colliding. Authored Slang sources remain authoritative and rebuild the shader cache after deletion.

`StaticMesh/Objects/<key-prefix>/<key>.bin` stores DMSH render payloads built
from exact source bytes, importer identity and version, semantic import
settings, payload/builder versions, target platform, and target profile.
StaticMesh import writes this object for immediate editor reuse. A safe miss can
be rebuilt from source, and a cook can reuse the resulting render data without
making runtime depend on the DDC path.

`AssetRegistry/Registry.bin` is a versioned, deterministic snapshot keyed by virtual mount root and normalized mount-relative package path. Startup still enumerates mounted `.dasset` files once. Exact file-size and stable last-write-time matches reuse cached class, format-version, and dependency metadata without reading package headers; new or changed files use the bounded header reader, and missing files disappear from the freshly published live map. Full validation bypasses fingerprint reuse. Schema, package-format, serialization, or mount-manifest incompatibility and corrupt or missing snapshots cause a non-fatal rebuild. Successful mutations update the live registry and dirty the snapshot, which is atomically replaced after explicit reconciliation and during orderly asset-manager shutdown. The live registry exposes a monotonic process-local revision that advances only when its published asset metadata changes, allowing editor queries to cache derived views safely. Scan diagnostics expose elapsed milliseconds, enumeration/reuse/reparse/removal/failure counts, and package-header read attempts and bytes.

Registry dependencies are package-level strong-reference edges collected from
the package header. They support loading, unload guards, move/delete checks, and
dependency-closure queries, but do not record which reflected property produced
an edge. In particular, they are not material Parent tags and cannot answer
which unloaded material instances are direct children of a material. Loaded
material hierarchy queries instead inspect canonical Parent chains; an unloaded
child query requires a future searchable-property metadata contract.

`Thumbnails/Index.bin` maps stable provider-neutral keys to PNG objects under
`Thumbnails/Objects/`. Source-image keys include normalized source identity,
source size and last-write time, maximum dimensions, generator schema,
color-space policy, and output encoding version. Rendered Material,
MaterialInstance, and TextureCube keys instead include virtual asset identity,
exact class, package fingerprint, provider and visual-contract schemas, fixed
output settings, and preview-fixture identity; material keys also include the
sorted transitive package-dependency closure. In-flight generation separately
revalidates asset and render-resource revisions before publication. A warm hit
decodes the generated PNG and follows the asynchronous, serial-validated RHI
upload path without reopening source input or loading and rendering the authored
asset. Missing or modified inputs, changed dependencies, settings, revisions or
versions, corrupt indexes or PNGs, and missing objects become safe misses.
Object and index writes are atomic; persistence failure does not fail valid
in-memory pixels. Encoded objects use an independent disk LRU budget, while RHI
textures retain their process-local GPU budget. Cleanup resolves and validates
every target beneath the exact thumbnail cache root before deletion. Editor
provider, scheduling, lifetime, and presentation ownership is documented in
[Asset Thumbnails](../../Editor/Architecture/AssetThumbnails.md).

`Textures/Objects/<key-prefix>/<key>.bin` and
`TextureCube/Objects/<key-prefix>/<key>.bin` store Texture2D and TextureCube
platform mip chains. Their 128-bit keys cover exact source-content identities,
every platform build setting, builder/payload/projection versions where
applicable, target platform, and target profile. Texture2D source fingerprints
avoid reopening unchanged source, while either texture class can use a valid
persisted identity and warm DDC object when source files are unavailable. Each
cache object is TXPL schema 1 with bounded mip/slice records and a payload
checksum.
Missing, incompatible, truncated, corrupt, or structurally invalid objects are
safe misses; a successful source build atomically replaces the object, while a
cache write failure does not invalidate usable in-memory platform data.

DDC objects are content-addressed generated files, so `.dasset` packages do not
store cache paths or byte offsets. Cooked packages instead serialize logical
payload descriptors and resolve their required DMSH or TXPL bytes from validated
DBLK companions beneath the configured cook root.

The shared authored/DDC/cooked storage classes, `.bin` versus `.dbulk`
semantics, loose companion naming, logical bulk descriptors, and runtime failure
policy are defined in [Asset Data Lifecycle and Storage](AssetDataLifecycle.md).

Repository storage rules for packages, source assets, and generated data are documented in [Content Version Control](../../Development/VersionControl/ContentVersionControl.md).
