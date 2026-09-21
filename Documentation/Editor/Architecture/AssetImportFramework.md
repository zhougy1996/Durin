# Asset Import Architecture

Summary: Define Factory-backed standalone import, immutable source capture, family-owned builds, and private Scene orchestration.

Modules: CoreDObject, AssetTools, AssetForgeBuiltins, DurinEd

Last reviewed: 2026-09-22

Durin creates standalone authored assets through `IAssetTools` and reflected
concrete `DFactory` classes. Texture2D, TextureCube, VolumeTexture, and StaticMesh
factories own accepted formats, typed invocation settings,
immutable capture, decode/build invocation, diagnostics, and import-data
publication. Independently registered `FReimportHandler` implementations and
`FReimportManager` provide the editor-wide authority for standalone reimport.
Scene remains a private multi-output transaction rather than a single-object
factory.

Shared capture, diagnostic, and publication values live in
`AssetForgeBuiltins`. They support the concrete importers and expose no generic
graph, replay schema, or import-job framework.

## Ownership And Layering

The intended dependency direction is:

`Core/CoreDObject -> Engine -> family build modules -> DurinEd -> AssetTools/AssetForgeBuiltins -> editor hosts`

- Runtime `Engine` owns the editor-only `DAssetImportData` base and lightweight
  `FSourceFile` / `FAssetImportInfo` values. Runtime assets do not know import
  dialogs, decoder selection, or source mutation workflows.
- Family build modules own detached recipes and producer identity. Engine owns
  derived-data keys, cache policy, typed application, and compilation; see
  [Asset Data Lifecycle](../../Runtime/Assets/AssetDataLifecycle.md#serialization-and-production-ownership)
  and [Asset Compilation](../../Runtime/Assets/AssetCompilation.md).
- `DurinEd` owns the generic `DFactory` descriptor/discovery contract and the
  self-registering `FReimportHandler` / `FReimportManager` capability, routing,
  priority, result, and optional persistence policy.
- `AssetTools` depends on DurinEd and owns the single `IAssetTools` service,
  typed operation terminal/persistence/warning values, package
  creation/adoption, factory invocation, result validation, failed-package
  discard, and reusable editor duplicate/save/mutation orchestration. It has
  no dependency on UI or concrete asset families.
- `AssetForgeBuiltins` owns reflected concrete factories, concrete editor
  import data, family capture/decode/build helpers, and reimport implementations
  that publish complete detached build products in place through
  `FReimportHandler`.
- Editor hosts and feature modules own file selection, destinations, and
  presentation diagnostics; they query and invoke reimport through the manager.
- Engine owns package identities, resident publication state, dirty state,
  persistence, per-package batch saves, and cooked data.

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
The shared encoded-source capture returns a typed result with owned logical and
physical file identity, size/limit context and file-system causes. Size inspection,
limit rejection, timestamp inspection, read failure and capture-time mutation are
distinct cases. Capture returns an expected owned snapshot; failure has no value,
while success owns one byte buffer and its matching hash. Pending family string contracts explicitly format
this result until their surrounding rebuild contracts migrate.

Relative Scene dependencies resolve from the root source directory. Containment
escapes, missing files, duplicate identities, unsupported encodings, and
resource-limit excess fail before publication. Embedded glTF/GLB data remains
part of the same immutable closure.

See [Source File Workflows](../Guides/SourceFileWorkflows.md) for the editor
interaction and portability rules.

`MakeSourceHint` returns expected `FSourceHint` containing the inseparable base
and hint; `ResolveSourceHint` returns expected `FFilePath`. Both retain
`FSourceHintError`, including owned diagnostic paths. Import-data validators
return expected void with `FAssetImportDataError`; `InspectAssetImportInfo`
returns an expected owned `FAssetImportInfo`. Optional source hints remain
optional inputs. Expected success never inspects a sentinel error code, and
callers access `error()` only after failure. Buffers and prepared values move
into their owners; expected itself adds no ownership or rollback policy.

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
requested Texture2D, TextureCube, VolumeTexture, or StaticMesh class
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

## Standalone Batch Admission

`FAssetImportValidation` derives success from `EAssetImportError` without stored
messages. It owns request identity, filename/class context, factory ambiguity or
batch collision counts, filesystem errors and a nested destination validation
cause. Factory compatibility validation returns an error code directly. Pending
asset-operation adapters use `FormatAssetImportValidation` explicitly and retain
the original result in `FAssetOperationResult::ImportCause`. Single-item admission,
batch preflight and factory compatibility rejection preserve this cause chain.
Creation admission and invalid factory-product rejection retain `CreationCause`,
including requested path/class/source and the actual product path/class captured
before package cleanup. Wrong type, outer, name and top-level registration are
distinct failures. A null factory product reports `FactoryRejected` and retains
the bounded per-invocation `FFactoryDiagnostics` collection in `FactoryCause`
before cleanup; an empty diagnostic collection still has a typed failure.
The collection preserves report order and shares one entry-count limit across
transitional text, typed `FFactoryError` entries and module-owned
`IFactoryErrorDetail` payloads. The latter retain concrete typed causes behind an
owning pointer and format only when the collection is presented; DurinEd does not
depend on factory implementation modules. The four built-in file
factories report exact-class, asset-package-parent and object-creation failures
with owned expected/requested class and source context. Source admission also
classifies missing files, unsupported formats/layouts, missing cube-face roles,
and prepared-source mismatches; filename, role, layout and prepared filename
remain available without parsing presentation text. `ToString` formats these
entries for presentation. StaticMesh settings rejection and Texture2D source-hint
or compilation rejection retain their Engine-owned typed causes, including nested
settings, build, import and save context. Remaining internal string producers still require
migration; the text entry is an explicit transitional contract.
Texture2D source translation returns `expected<FTextureSource,
FTexture2DTranslationError>`, retaining Core decode causes and dimension/channel
context on failure. Preparation returns an expected owned
`FPreparedTexture2DImport` with typed path/format/capture/translation failures.
Neither failure branch exposes an output value. String presentation contracts
format the owned filename and nested causes at their adapters. The factory retains
`FTexture2DFactoryError`; asynchronous file import transports the typed preparation
cause to the game-thread presentation boundary before formatting it. Worker
exception diagnostics remain a separate pending contract. Texture2D source-file
submission returns expected void for admission and retains typed package/mount/source-hint,
capture, translation and compilation causes. Factory reimport results retain
submission or terminal compilation details through the manager callback. Admission
failure still invokes no compilation completion; accepted requests retain the
existing asynchronous publication and completion contract.
Texture2D property-setting helpers return `FTexture2DCompilationOperationResult`
directly. Invalid usage, quality, alpha mode or threshold retains the requested
settings in the Engine input cause; unchanged values remain successful no-ops,
and rebuild rejection preserves its original compilation error.

Volume import settings expose expected-void `Validate()` with an
AssetForgeBuiltins-owned error code and a copy of the rejected settings. Import format, dimensions, tile-cell
capacity and atlas/volume budgets are distinct cases. The remaining string-based
adapters format only at pending contracts. The VolumeTexture factory retains its
settings or rebuild error as `FVolumeTextureFactoryError`, so AssetTools consumers
can inspect the nested cause after package cleanup.
`TranslateVolumeTextureAtlasSource` returns expected owned
`FVolumeTextureSourceData` with nested settings and Core image-decode causes, owned filename, and expected/actual atlas dimensions.
Signature, decode, dimensions, bulk publication and normalized-layout failures
are distinct. Failure has no source value; the rebuild error retains the
translation cause directly. Volume rebuild and public reimport return expected
void and retain object/source identity, mount classification, source-hint,
capture, translation, import-data and save causes. Build failures retain the
complete current build outcome; its diagnostic contract and the reimport
framework string adapters remain pending migrations. `FReimportResult` retains
module-owned factory details in `FactoryCause`; persistence failures retain the
complete asset result in `SaveCause` without replacing other retained context.
VolumeTexture reimport propagates its concrete rebuild error to the final callback.
StaticMesh reimport retains the complete compilation diagnostic through
`FStaticMeshFactoryError`, including request identity, terminal disposition, phase
and nested completion causes. Cancellation does not publish new provenance.
StaticMesh rebuild and public reimport return the expected-void alias
`FStaticMeshRebuildResult`, preserving settings, mount, source-hint, capture,
source initialization, import validation, submission and terminal completion
causes. Synchronous saves retain their asset result within the completion cause.
Factory import and reimport admission retain this same error through
`FStaticMeshFactoryError`. Reimport entry checks distinguish object type, missing
import data/source and source count, retaining object identity and actual count.
Reimport path resolution and file inspection retain system errors instead of throwing
past the result boundary. Geometry decoding retains the existing scene diagnostic
collection; that diagnostic contract remains pending migration. Failed decoding
leaves resident data and provenance unchanged. Transient creation returns an
expected `DStaticMesh*` with the same rebuild error; the pointer is available
only after successful rebuilding. Rejection retains the requested object name and
source filename, nested rebuild cause and any path/file system error; the failed
private object is marked as garbage before returning.
The outer status/message contract remains transitional. Existing publication order
and save disposition are preserved; save causes retain the full asset result.
Atlas inspection returns expected `FVolumeTextureAtlasInspection` or a Core
image-decode error retaining the file/system cause. A valid atlas with no
confident layout remains a successful inspection. Dimensions, channel suggestions
and layout confidence remain independent inspection facts; `FormatVolumeTextureAtlasInspection` renders
failure or advisory text without storing prose or a separate success flag.

`FAssetImportRequest` carries an explicit top-level destination, source filename,
asset class, optional configured factory, context, and object flags.
`IAssetTools::InspectImports` returns one validation per input without creating
packages or decoding sources. It checks constructibility, writable mounted
destinations, resident/catalog/file occupancy, and factory selection. Every item
sharing a destination package is rejected, even with different top-level names.
Source capture and format validation remain factory responsibilities; preflight
does not guarantee decoding or compilation success.

`ImportAssets` snapshots the request and dispatches standalone factories in input
order on the game thread. Execution revalidates each destination using the same
admission policy as single-file `ImportAsset`. The returned object must have the
requested class, package, and exact name. Configured factories and context objects
are strongly retained throughout the call, including failed-package collection.

Results preserve input order and distinguish `Accepted`, `Rejected`,
`NotAttempted` after stop-on-failure, and `Canceled` before dispatch. Failure
continues by default; stop-on-failure preserves earlier successful peers.
Cancellation is checked between items and cannot interrupt a running factory.
Empty batches return no results. A rejected item retains its operation diagnostic.

Acceptance means the factory returned a valid live asset. It does not imply that
deferred family compilation or persistence has completed. Successful packages
remain dirty; callers coordinate family completion before `SaveAssets` and own
save retry and presentation. The batch performs no automatic save, publication,
peer rollback, naming policy, or Scene orchestration. Existing dialogs continue
to use their single-asset workflows.

## Scene Import

Scene import is the one supported multi-output importer. It owns a private transient dependency model for textures, materials, and static
meshes. Sources with skins or animation channels fail before staging outputs. The private model is not
a public AssetForge graph and is never persisted for replay.

The importer:

- captures the root and bounded external dependencies once;
- decodes FBX or glTF/GLB into normalized scene values;
- derives a stable private topological order;
- preflights every destination collision;
- constructs and validates all peer candidates before publication;
- binds material and texture relationships in dependency order;
- commits packages in dependency order, preserving successful packages when a later package fails.

Scene material policy is explicit. `FSceneMaterialImportOptions` defaults to
CreateMaterials: the importer publishes editable `DMaterial` graphs under the
selected destination's `Materials/M_<source name>`. CreateInstances publishes
`Materials/MI_<source name>` against an explicitly selected existing `DMaterial`.
An import-wide selection can be overridden by stable source-material identity.
No new shared `Materials/ImportedParents` assets are generated.

`PreviewSceneMaterials` captures and decodes the source without publishing
assets, lists resolved paths/modes/parents, and reports compatibility per material.
The mapping contract accepts the built-in full PBR Metallic/Roughness template
and compatible imported PBR graphs with stable parameter IDs. The full template
receives all factors, textures and UV parameters, resetting absent maps to their
canonical fallbacks. Its additive emissive input receives zero when source
emissive factors were already baked into the image. Its opacity input reads the
original base-color alpha; compact imported graphs use the derived red mask. It validates parameter types, the Surface domain, and actual
sampling, channel and UV graph structure, allowing different parameter defaults
and presentation metadata. Arbitrary custom graphs are rejected rather than
silently losing source properties. Selected parents are not edited or saved.
Instances override source blend mode, cutoff and two-sided properties; shading
and depth policy remain inherited. Every parameter application is checked.
Prepared material candidates must finish compilation before publication.

Matching source/output receipts identify reimport outputs independently of
filenames. Ordinary reimport updates geometry and textures but retains complete
existing materials, including graph edits, parent references, values and asset
types. Mesh bindings are preserved by unique source slot name (unnamed or duplicate
slots fall back to matching name and source index). Stored materials are not included in `SavedPackages`.
`bRebuildExistingMaterials` explicitly replaces material edits and mesh bindings
from the current source and selections. Asset-type changes are rejected and
require another destination. Legacy imported instances and their existing
parents remain supported. This conservative policy does not attempt a three-way
parameter merge without a source/edit baseline.

Base-material parameter schemas contain native texture references in addition
to reflected graph defaults. Scene publication and package reload include
`MakeMaterialReferenceReplacementParticipant` to prepare, validate, and commit
those references together. Failed publication leaves the old schema intact;
successful publication refreshes material render bindings before retirement.

Scene constructs private candidate packages from CoreDObject package/object
primitives. It does not call single-object `IAssetTools`: doing so would assign
independent acceptance semantics before the complete dependency-ordered peer
set is bound, validated, and ready for per-package publication. Static-mesh and
texture preparation still reuse their family build adapters below that
publication boundary. Engine exposes no generic `CreateAsset` materialization
seam.

`ImportSceneAssets` returns a complete `FSceneImportResult` report by value.
Its boolean conversion checks `bSucceeded`; cancellation and rejection still
return diagnostics and any output summaries already produced. Output summaries
are planned identities, not proof that their packages were saved.
A persistence failure stops publication and discards only unpublished candidates.
`FSceneImportResult::SavedPackages` identifies committed outputs even on failure;
`bPersisted` is true only when the requested scene import finishes successfully.
A retry preserves existing materials by default and replaces matching geometry
and texture outputs.

Every generated output is an ordinary independent asset. There is no aggregate
Scene asset, primary output, generated-output ownership record, reconciliation
tombstone, or repair action. Matching source/output identities allow reimport
and retry after partial persistence; unrelated destination collisions still fail.

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

Content-directory validation derives success from `EContentDirectoryError` and
retains the requested virtual/physical path, typed path cause or mount error.
Resolution and writability facts remain available independently. The result stores
no message; scene import formats failures through `FormatContentDirectoryValidation`
at its presentation boundary. Asset destination validation likewise derives success
from `EAssetDestinationError`, retaining request/path/mount context and separating
registry assets, redirectors, unsaved packages and other resident packages.
Occupancy facts and redirect targets remain independently inspectable.
`FormatAssetDestinationValidation` is used by creation/import presentation adapters.

Content Browser Import workflows are feature-owned scoped extensions.
TextureEditor registers From File, LevelEditor registers Scene, and StaticMeshEditor
registers standalone Static Mesh. Stable IDs and
explicit order values preserve the visible menu independently of module load
order. ContentBrowser invokes applicable entries directly. Owners unregister extensions
and finish dispatch before unloading their code; MainFrame has no import-family
enum, descriptor table, or feature switch.

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

TextureEditor uses a native multi-select file picker for Texture2D. Its private
`FTextureFileImport` service owns a bounded batch queue, unique naming, factory
invocation, save, and failed-save retry independently from UI. One detached source
is captured, decoded, and classified on a worker at a time. A prepared-source
factory opt-in submits asynchronous texture compilation; the accepted object is
pinned until completion, and saving waits for successful build and provenance
publication. Ordinary factory calls remain synchronous, and reimport preserves
its configured settings.

The import queue retains Engine asset publication through `FAssetSaveOperation`;
CoreDObject package-only saves do not publish catalog metadata or editor notifications.
After compilation: preparation and
final publication run on the GameThread, while package/bulk staging and byte
verification run on the scheduler's BlockingIO pool. The presenter reports
`Saving` and polls without waiting. Mutation pauses defer final publication;
teardown drains and abandons the staged save before discarding the candidate.
Snapshot conflicts retain the dirty asset for retry. Explicit Retry Texture
Saves and ordinary synchronous callers continue to use the existing save path.
See [asynchronous save staging](../../Runtime/Assets/AssetCatalogAndMutation.md#asynchronous-save-staging)
for ownership and transaction boundaries.

Each texture compresses independent block rows through the CPU task scheduler,
with at most eight chunks per mip and a 4096-block batching threshold. Small
mips and nested parallel loops run serially. Encoder settings and output bytes
are unchanged. Cancellation predicates are serialized and all chunks drain
before the result or its storage is released.

Build requests and the uncompressed mip chain retain shared immutable image
buffers, including source mip slices. Building does not duplicate source pixels.
Only generated mips allocate writable pixels; after filtering and optional alpha
coverage adjustment, ownership moves into immutable images without copying.
Compression borrows read-only views until its tasks drain. Intermediate-memory
metrics count generated mip storage, excluding shared decoded input and output.

Completed compilation attempts include preparation, compilation elapsed time,
mip generation, compression, cache write, cache origin, and save timing in the
notification history details. Compilation elapsed time includes admission,
queueing, host ticks and pauses; it is not a pure CPU measurement. Preparation
includes source capture, decoding and classification. Save elapsed time includes
staging, host polling and mutation pauses. These diagnostics do not
set timing acceptance thresholds.

The host presenter advances the queue even when the browser is hidden, subject
to mutation admission. Cancellation skips queued files after the current item;
failures continue to the next item. Batch completion coalesces browser presentation
and reports counts and diagnostics through notifications. Teardown joins decoding,
cancels/drains active compilation, and discards the unfinished package before
releasing feature code. Failed saves retain completed resident assets for retry.

Standalone StaticMesh dialogs are host presentations owned by their feature
module. The Scene dialog
is owned by the Level Editor workspace and also draws through its registered host
presenter, independently of the active workspace.
Standalone dialogs
configure a concrete factory and call `IAssetTools`; Scene calls its private
multi-output transaction. Terminal diagnostics and post-save Content Browser
refresh/reveal remain host presentation concerns.

`DurinEd` supplies shared path text state and input-row presentation through
`FImportDialogPathModel`. Asset and directory models retain their separate
validation and file/folder browsing policies. Mesh coordinate controls used by
StaticMesh and Scene imports live in `Import/MeshCoordinateImportModel.h`, so
generic import-dialog helpers do not include static-mesh settings.

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

Standalone family import data uses concrete schema 2 with no legacy reader or
dual-write route. Package baseline and future migration policy are defined by
[Versioning](../../Runtime/Assets/Versioning.md#authored-package-policy).

## Related Documentation

- [Source File Workflows](../Guides/SourceFileWorkflows.md)
- [Asset Data Lifecycle And Storage](../../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Compilation](../../Runtime/Assets/AssetCompilation.md)
- [Async Asset Operations](AsyncAssetOperations.md)

## Related Code

- [`ContentBrowserContracts.h`](../../../Engine/Source/Editor/ContentBrowser/Public/ContentBrowser/ContentBrowserContracts.h)
- [`IAssetTools.h`](../../../Engine/Source/Editor/AssetTools/Public/AssetTools/IAssetTools.h)
- [`EditorReimportHandler.h`](../../../Engine/Source/Editor/DurinEd/Public/Import/EditorReimportHandler.h)
- [`Factory.h`](../../../Engine/Source/Editor/DurinEd/Public/Factories/Factory.h)
- [`SceneDirectImport.cpp`](../../../Engine/Source/Editor/AssetForgeBuiltins/Private/SceneDirectImport.cpp)
- [`Texture2DImport.cpp`](../../../Engine/Source/Editor/AssetForgeBuiltins/Private/Texture2DImport.cpp)
- [`StaticMeshImport.cpp`](../../../Engine/Source/Editor/AssetForgeBuiltins/Private/StaticMeshImport.cpp)
- [`SourceHint.h`](../../../Engine/Source/Runtime/Engine/Public/Asset/SourceHint.h)
