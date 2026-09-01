# Asset Import Architecture

Summary: Define Factory-backed standalone import, immutable source capture, family-owned builds, and private Scene orchestration.

Modules: AssetTools, AssetForgeBuiltins, DurinEd

Last reviewed: 2026-08-28

Durin creates standalone authored assets through `IAssetTools` and reflected
concrete `DFactory` classes. Texture2D, TextureCube, VolumeTexture, StaticMesh,
and TerrainHeightmap factories own accepted formats, typed invocation settings,
immutable capture, decode/build invocation, diagnostics, and import-data
publication. Independently registered `FReimportHandler` implementations and
`FReimportManager` provide the editor-wide authority for standalone reimport.
Scene remains a private multi-output transaction rather than a single-object
factory.

Shared capture, diagnostic, and publication values live in
`AssetForgeBuiltins`. They are implementation helpers, not an extensibility or
orchestration contract. Generic graphs, registries, replay schemas, import
requests/jobs, and mounted-source mutation have been physically removed by the
[AssetForge Framework Removal Plan](../../Plans/Archive/2026-08/AssetForgeFrameworkRemoval.md).

## Ownership And Layering

The intended dependency direction is:

`Core/CoreDObject -> Engine -> family build modules -> DurinEd -> AssetTools/AssetForgeBuiltins -> editor hosts`

- Runtime `Engine` owns the editor-only `DAssetImportData` base and lightweight
  `FSourceFile` / `FAssetImportInfo` values. Runtime assets do not know import
  dialogs, decoder selection, or source mutation workflows.
- Family build modules own normalized build inputs, derived-data keys,
  compilation, and disposable-data reconstruction.
- `DurinEd` owns the generic `DFactory` descriptor/discovery contract and the
  self-registering `FReimportHandler` / `FReimportManager` capability, routing,
  priority, result, and optional persistence policy.
- `AssetTools` depends on DurinEd and owns the single `IAssetTools` service,
  typed operation terminal/persistence/warning values, package
  creation/adoption, factory invocation, result validation, failed-package
  discard, and reusable editor duplicate/save/mutation orchestration. It has
  no dependency on UI or concrete asset families.
- `AssetForgeBuiltins` owns reflected concrete factories, concrete editor
  import data, family capture/decode/build helpers, and safe candidate/swap
  reimport implementations exposed through `FReimportHandler`.
- Editor hosts and feature modules own file selection, destinations, and
  presentation diagnostics; they query and invoke reimport through the manager.
- Engine owns package identities, resident publication state, dirty state,
  persistence, atomic package-bundle saves, and cooked data.

Third-party import providers, hot-unloadable importer registration, runtime
import, and user-composed translation/planning stacks are not supported.

## Optional Source Hint Contract

Every standalone output may retain a schema-2 `HintBase + Hint` pair solely for
an explicit Reimport action. `HintBase` is never inferred from the text:

- `AssetRelative` resolves from the physical parent of the owning `.dasset`;
- `ProjectRelative` resolves from the active project directory and may not
  escape it;
- `Absolute` is a normalized physical path, normally used outside the project.

Default classification uses `AssetRelative` for a project-local source and
project-local owner, `ProjectRelative` for a project-local source whose owner is
outside the project, and `Absolute` otherwise. Resolution does not consult
package mounts. Move and duplication copy the base and hint bytes unchanged;
therefore an asset-relative hint intentionally rebinds at the destination.
Display, load, inspection, thumbnail, build, DDC recovery, and Cook paths never
resolve or probe a hint. Import and reimport never copy, move, rename, replace,
or delete the source.

One import attempt captures its complete required source closure into immutable
owned bytes. Recognition, hashing, dependency discovery, decoding, and build
preparation consume that capture. A later phase must not reopen the physical
file and accidentally combine bytes from different source revisions.

Relative Scene dependencies resolve from the root source directory. Containment
escapes, missing files, duplicate identities, unsupported encodings, and
resource-limit excess fail before publication. Embedded glTF/GLB data remains
part of the same immutable closure.

See [Source File Workflows](../Guides/SourceFileWorkflows.md) for the editor
interaction and portability rules.

## Standalone Factory Import

A standalone first import follows this boundary:

1. the feature editor selects an asset class and a default or typed configured
   concrete factory;
2. `IAssetTools` validates the destination, creates a Public/Standalone package,
   adopts it into Engine residency, and invokes the factory;
3. the factory creates the formal object directly under that package, captures
   required bytes once, decodes them, builds into the object, and publishes
   concrete import data;
4. `IAssetTools` validates the returned main asset and discards the complete
   unsaved package on failure;
5. the feature editor requests save through the same AssetTools service and
   supplies the host publication callback; AssetTools publishes one structured
   completion only after Engine persistence succeeds.

Factories are discovered from reflected immutable CDO descriptors. A dialog
with non-default settings creates a transient factory instance and configures
typed fields. `SupportedClass` is authoritative; extension lookup only narrows
candidates. Extension-only PNG lookup is intentionally ambiguous, while a
requested Texture2D, TextureCube, VolumeTexture, or TerrainHeightmap class
selects its concrete factory deterministically.

First-import failure discards the disposable package, including a formal object
that was created before decode/build completed. Reimport has a different safety
boundary: failure before live-state commit leaves an existing live and
persisted asset unchanged. Live-state commit and package save are separate
facts in both flows.

`FReimportHandler` registers itself with `FReimportManager` for its lifetime.
The manager evaluates handlers in descending priority order; reflected Factory
CDOs are preferred over transient instances at equal priority. Capability
queries distinguish retained-source Reimport from Reimport From File, including
the one-file panorama and six-file face layouts of TextureCube. Terminal results
distinguish unsupported classes, missing retained sources, source/build failure,
successful live replacement, and persistence failure. Factory handlers publish
only a complete successful candidate and leave the package Dirty; the manager
then saves only when requested, so a save failure does not erase or misreport
the valid live replacement.

Texture and mesh compilation may remain asynchronous under their family build
systems. The built-in import boundary does not provide a second operation state machine,
cancellation history, mailbox, activity projection, or shutdown pump for these
workflows. Missing or corrupt disposable data uses the owning family build or
PostLoad policy rather than import `Recover` semantics.

## Scene Import

Scene import is the one supported multi-output importer. It is creation-only
and owns a private transient dependency model for textures, materials, static
meshes, Skeletons, SkeletalMeshes, and AnimationClips. The private model is not
a public AssetForge graph and is never persisted for replay.

The importer:

- captures the root and bounded external dependencies once;
- decodes FBX or glTF/GLB into normalized scene values;
- derives a stable private topological order;
- preflights every destination collision;
- constructs and validates all peer candidates before publication;
- binds material/texture and skeletal relationships in dependency order;
- saves the complete output package set atomically.

Scene deliberately calls the Engine `CreateAsset` materialization
seam for its private candidate packages. It does not call single-object
`IAssetTools`: doing so would assign independent acceptance semantics before
the complete dependency-ordered peer set is bound, validated, and ready for
one atomic bundle save. Static-mesh and texture preparation still reuse their
family build adapters below that transaction boundary.

Every generated output is an ordinary independent asset. There is no aggregate
Scene asset, primary output, generated-output ownership record, reconciliation
identity, tombstone, repair action, or whole-scene reimport. Importing a revised
scene requires a fresh destination.

FBX remains static-only. The selected glTF 2.0 subset supports contained
external buffers, data URIs, GLB BIN data, static geometry, materials and
textures, skins, and STEP/LINEAR transform animation. Unsupported sparse
accessors, morphs, cubic interpolation, second influence sets, ambiguous skin
ownership, and animated intervening non-joints fail the complete transaction.

glTF values use the deterministic basis conversion `(x, y, z) -> (-z, x, y)`
for geometry, transforms, inverse binds, and animation. The handedness change
reverses triangle winding. Skeleton bones are parent-before-child; multiple
roots receive a stable `$DurinRoot`.

## Editor Dispatch

Content Browser Import workflows are feature-owned scoped extensions.
TextureEditor registers Texture, LevelEditor registers Terrain Heightmap and
Scene, and StaticMeshEditor registers standalone Static Mesh. Stable IDs and
explicit order values preserve the visible menu independently of module load
order. ContentBrowser invokes applicable entries through their owner gate and
MainFrame has no import-family enum, descriptor table, or feature switch.

Reimport has no extension entry, family enum, or host switch: Content Browser asks
`FReimportManager` for loaded-object capabilities and sends Reimport or the
complete selected replacement file set back to the same manager. **Reimport**
resolves a retained complete hint set, while **Reimport From File...** remains
available for a loaded supported asset even when no hint exists. Selecting a
new file can change hints only through a successful complete candidate commit.

Import is an editor presentation category rather than factory discovery:
extensions select a user-facing workflow for a virtual destination directory,
while `DFactory` remains authoritative for the object class and source formats
inside that workflow. Reimport continues to query loaded-object capabilities.
Create and Import invocation share Content Browser's asset-mutation admission
policy.

Dialogs use ordinary read-only file pickers. Texture and standalone StaticMesh
dialogs are host presentations owned by their feature modules. Scene and
Terrain dialogs are owned by the Level Editor workspace and also draw through
their registered host presenters, independently of the active workspace.
Standalone dialogs
configure a concrete factory and call `IAssetTools`; Scene calls its private
multi-output transaction. Terminal diagnostics and post-save Content Browser
refresh/reveal remain host presentation concerns.

## Persistence, Cooking, And Runtime Closure

Concrete editor-only import data stores optional common source hints plus only
the family interpretation settings needed for reimport. Runtime assets own the
bounded, decoder-free canonical imported data required by their builds, using
`FEditorBulkData` when the payload crosses the authored bulk threshold. Import
data carries no generic provider, translator, builder, graph, planning-pass,
or replay provenance.

Cook strips source hints, editor-only import data, diagnostics, and derived-data
identities selected as editor-only. Cooked packages contain validated runtime
payloads and ordinary asset references. Runtime-only loading requires neither
AssetForgeBuiltins, Assimp, offline image/model decoders, authored source files,
nor DDC fallback.

## Compatibility Boundary

The supported authored baseline is the repository-owned asset corpus. Old
standalone first-import result wrappers and direct `CreateAsset` family
entrypoints have no production compatibility route. Current
standalone family import data is schema 2 and is read and written only through
concrete family schemas. Scene outputs are ordinary independently rebuildable
assets and do not persist an import replay record. Retired filename, generic
replay, mounted-source, and source-backed recovery schemas have no production
reader or dual-write route.

## Related Documentation

- [Asset Import Simplification Roadmap](../../Roadmaps/Archive/2026-08/AssetImportSimplification.md)
- [Source File Workflows](../Guides/SourceFileWorkflows.md)
- [Asset Data Lifecycle And Storage](../../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Compilation](../../Runtime/Assets/AssetCompilation.md)
- [Async Asset Operations](AsyncAssetOperations.md)

## Related Code

- [`ContentBrowserContracts.h`](../../../Engine/Source/Editor/ContentBrowser/Public/ContentBrowser/ContentBrowserContracts.h)
- [`IAssetTools.h`](../../../Engine/Source/Editor/AssetTools/Public/AssetTools/IAssetTools.h)
- [`EditorReimportHandler.h`](../../../Engine/Source/Editor/DurinEd/Public/EditorReimportHandler.h)
- [`Factory.h`](../../../Engine/Source/Editor/DurinEd/Public/Factories/Factory.h)
- [`SceneDirectImport.cpp`](../../../Engine/Source/Editor/AssetForgeBuiltins/Private/SceneDirectImport.cpp)
- [`Texture2DImport.cpp`](../../../Engine/Source/Editor/AssetForgeBuiltins/Private/Texture2DImport.cpp)
- [`StaticMeshImport.cpp`](../../../Engine/Source/Editor/AssetForgeBuiltins/Private/StaticMeshImport.cpp)
- [`SourceHint.h`](../../../Engine/Source/Runtime/Engine/Public/Asset/SourceHint.h)
