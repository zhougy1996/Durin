# Asset Import Architecture

Summary: Define direct built-in asset import, immutable source capture, family-owned builds, and private Scene orchestration.

Modules: AssetForgeBuiltins, DurinEd

Last reviewed: 2026-08-27

Durin imports authored assets through explicit built-in family functions.
Texture2D, TextureCube, VolumeTexture, StaticMesh, TerrainHeightmap, and Scene
each own their accepted formats, settings, decode/build invocation,
publication, diagnostics, and reimport policy. Production editor workflows do
not select translators or builders from an importer registry and do not submit
generic import requests or jobs.

Shared capture, diagnostic, and publication values live in
`AssetForgeBuiltins`. They are implementation helpers, not an extensibility or
orchestration contract. Generic graphs, registries, replay schemas, import
requests/jobs, and mounted-source mutation have been physically removed by the
[AssetForge Framework Removal Plan](../../Plans/AssetForgeFrameworkRemoval.md).

## Ownership And Layering

The intended dependency direction is:

`Core/CoreDObject -> AssetCore/Engine -> family build modules -> AssetForgeBuiltins -> editor hosts`

- Runtime `Engine` owns the editor-only `DAssetImportData` base and lightweight
  `FSourceFile` / `FAssetImportInfo` values. Runtime assets do not know import
  dialogs, decoder selection, or source mutation workflows.
- Family build modules own normalized build inputs, derived-data keys,
  compilation, and disposable-data reconstruction.
- `AssetForgeBuiltins` owns concrete editor import data and direct family
  import/reimport functions. Its implementation may share capture or decode
  helpers where that removes duplication.
- DurinEd and feature editor modules own file selection, destinations,
  diagnostics, and explicit dispatch to the finite built-in family set.
- AssetCore owns package identities, resident publication state, dirty state,
  persistence, atomic package-bundle saves, and cooked data.

Third-party import providers, hot-unloadable importer registration, runtime
import, and user-composed translation/planning stacks are not supported.

## Source Filename Contract

Every selected source is represented by a normalized filename:

- files beneath the active project directory persist project-relative paths;
- other files persist normalized absolute paths;
- resolution does not consult package mounts;
- import and reimport never copy, move, rename, replace, or delete the source.

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

## Direct Family Importers

A family importer follows this boundary:

1. validate source and destination;
2. capture required bytes once;
3. decode into normalized owned values;
4. complete failable build validation;
5. create or update the narrow runtime asset state on the editor thread;
6. publish editor-only source metadata;
7. save the package independently.

Failure before publication leaves an existing live and persisted asset
unchanged. Successful publication and persistence are separate facts: if the
save fails after valid state is published, the package remains Dirty for an
explicit retry.

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

`BuiltinImportDispatch.h` is the finite Content Browser import menu authority.
It contains four families: Texture, TerrainHeightmap, Scene, and standalone
StaticMesh. MainFrame dispatches those values directly to the owning feature
module. Reimport class routing is likewise a closed built-in switch for
Texture2D/TextureCube/VolumeTexture, TerrainHeightmap, and StaticMesh.

Import and Reimport are deliberately not Content Browser extension categories.
The remaining extension registry composes unrelated Create, Details, and
Context Menu UI supplied by editor modules; it does not select importers or
query import capabilities.

Dialogs use ordinary read-only file pickers. Texture and standalone StaticMesh
dialogs are host presentations owned by their feature modules. Scene and
Terrain dialogs are owned by the Level Editor workspace. All call concrete
family APIs directly and report terminal diagnostics through ordinary editor
notifications.

## Persistence, Cooking, And Runtime Closure

Concrete editor-only import data stores common source filenames plus only the
family interpretation settings needed for reimport. It carries no generic
provider, translator, builder, graph, planning-pass, or replay provenance.

Cook strips source filenames, editor settings, diagnostics, and derived-data
identities selected as editor-only. Cooked packages contain validated runtime
payloads and ordinary asset references. Runtime-only loading requires neither
AssetForgeBuiltins, Assimp, offline image/model decoders, authored source files,
nor DDC fallback.

## Compatibility Boundary

The supported authored baseline is the repository-owned asset corpus. Current
standalone family import data is read and written through concrete family
schemas. Scene outputs are ordinary assets and do not persist an import replay
record. Compatibility for retired generic replay and mounted-source schemas is
repository-bounded; the framework-removal milestone audits or regenerates the
remaining corpus instead of adding an indefinite dual-write layer.

## Related Documentation

- [Asset Import Simplification Roadmap](../../Roadmaps/AssetImportSimplification.md)
- [Source File Workflows](../Guides/SourceFileWorkflows.md)
- [Asset Data Lifecycle And Storage](../../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Compilation](../../Runtime/Assets/AssetCompilation.md)
- [Async Asset Operations](AsyncAssetOperations.md)

## Related Code

- [`BuiltinImportDispatch.h`](../../../Engine/Source/Editor/DurinEd/Public/Editor/Import/BuiltinImportDispatch.h)
- [`SceneDirectImport.cpp`](../../../Engine/Source/Editor/AssetForgeBuiltins/Private/SceneDirectImport.cpp)
- [`Texture2DImport.cpp`](../../../Engine/Source/Editor/AssetForgeBuiltins/Private/Texture2DImport.cpp)
- [`StaticMeshImport.cpp`](../../../Engine/Source/Editor/AssetForgeBuiltins/Private/StaticMeshImport.cpp)
- [`SourceFilename.h`](../../../Engine/Source/Runtime/Engine/Public/Asset/SourceFilename.h)
