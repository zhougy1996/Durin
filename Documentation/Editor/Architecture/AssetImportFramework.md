# Asset Import Framework

Summary: Define graph-based asset-import admission, translation, planning, typed construction, publication, and extension ownership.

Modules: AssetForge, AssetForgeBuiltins, AssetCore

Last reviewed: 2026-08-26

Durin editor imports use one AssetForge framework for source capture,
translation, ordered planning-pass execution, typed detached construction, preview,
candidate validation, publication, diagnostics, cancellation, reimport, record
actions, and editor recovery. `AssetForge` owns immutable source and build
graphs plus `FImportJob`; `AssetForgeBuiltins` supplies built-in source
translators, planning passes, and asset builders for StaticMesh, Texture2D,
TextureCube, VolumeTexture, Terrain Heightmap, and heterogeneous Scene output. Runtime
targets depend on neither editor module.

Concrete translation terminates at normalized owned values. Built-in implementations
do not pass encoded PNG, HDR, glTF, FBX, or Assimp input into
`TextureBuild`, `StaticMeshBuild`, `SkeletalBuild`, or `TerrainBuild`. Build requests contain no decoder type or mutable
`DObject`; detached products publish on the main thread through narrow Engine
state exchanges and AssetCore transactions.

## Ownership and layering

The dependency direction is `Core/CoreDObject -> AssetCore -> AssetForge
-> AssetForgeBuiltins -> editor hosts`.

- `AssetCore` owns packages, the private catalog store, resident publication
  state, disposable DDC objects, and failure-atomic package-bundle publication.
  Import candidates are `NewlyCreated`, Dirty resident packages. Import code
  consumes catalog values and uses `UnloadPackage(..., DiscardUnsaved)` for
  failed candidate rollback; successful bundle publication promotes the same
  resident entries to `Published`. AssetCore has no knowledge of import extensions or
  concrete imported asset classes.
- `AssetForge` owns one `FImportService`, source snapshots, diagnostics,
  source/build graph contracts, candidates, import records, record
  indexing, component registries, and scheduled/inline orchestration.
- Extension modules register source translators for source semantics, ordered planning
  passes for authoring policy, and typed asset builders for output construction. None owns
  a complete import job.
- Editor hosts build or reconstruct `FImportRequest` values and
  observe framework operation snapshots and terminal outcomes. They do not
  advance domain work from Widget drawing.

`AssetForgeBuiltins` is the default aggregate for built-in implementations and owns
their Assimp and concrete source-translation policy. Asset-independent image
decoding lives in Core; AssetForgeBuiltins alone decides which technically
decodable inputs are admitted for Texture2D, TextureCube, Scene, and Terrain.
Runtime Engine assets contain only runtime state and lightweight single-asset provenance.
Its public Texture2D translation/submission contract intentionally exposes
`TextureBuild` settings and completion values, so `TextureBuild` remains a
public module dependency; geometry recipes are implementation-only and the
`StaticMeshBuild` and `SkeletalBuild` dependencies are private. Terrain recipes and Cook production are likewise
implementation-only through the private `TerrainBuild` dependency.

Public framework contracts supplied by `AssetForge` live in
`Durin::AssetForge`. Built-in registration, direct authoring APIs,
format admission, and typed translators supplied by AssetForgeBuiltins live in
`Durin::AssetForge::Builtins`. Implementation-only build composition and
uncooked policies remain private to the physical module; no former C++
namespace survives through a compatibility alias.

## Import service and registration

`FImportService` is the only production orchestration boundary. Source-translator,
planning-pass, and asset-builder registries are specialized because their selection uses
component identity, contract/schema version, source recognition, output class,
planning-pass order, and persisted provenance. Selection is deterministic and an
ambiguous or incompatible match fails with structured evidence.

Each exact registration retains the module callback gate and resource lease.
A submitted or cached value retains component leases through invocation and
destruction; preview-product cache entries are discarded during component
unregistration. Retirement closes admission, cancels and drains owned jobs,
waits admitted callbacks, clears escaped cached products, then removes the
exact generation. A stale registration handle cannot retire its replacement.

The public request selects one source translator, versioned translator settings, an
ordered planning-pass stack, a mounted root and optional declared sources, one
destination, operation owner/lifetime, limits, save policy, and optional
existing provenance. Initial import, preview, reimport, source replacement,
repair, recovery, and record actions differ by request mode and graph content,
not by orchestration interface.

TextureCube follows the same boundary. `TextureCubeImport.h` owns
six-face and panorama validation, import/reimport, source-reference changes,
ingestion, package save, and rollback. It decodes concrete image bytes, submits
owned normalized requests to TextureBuild, and publishes only detached products
through Engine's narrow TextureCube state exchange. Runtime Engine exposes no
create/save/source workflow and no mutable TextureCube authoring registry.

## Source snapshots and graphs

Import first resolves a mounted `FSourcePath` and captures its bytes. A source translator
may inspect those captured bytes and declare bounded dependencies. The framework
resolves and captures each dependency once, enforces containment and resource
limits, and freezes the complete closure as an immutable source snapshot.

Parsing, hashing, normalized CPU data, graph fingerprints, candidates, and DDC
keys consume the same snapshot bytes. No later phase reopens a physical source.
The translator emits a bounded immutable `FSourceGraph`; the ordered
planning-pass stack emits a bounded immutable `FBuildGraph`. Canonical node
ordering, explicit dependencies, versioned payload schemas, stable identities,
and deterministic fingerprints make both graphs safe cross-stage values.
Unknown schemas, missing references, duplicates, cycles, invalid destinations,
and resource-limit excess fail before candidate construction.

Direct authoring, source-reference changes, repair, and uncooked PostLoad use
the same AssetForge encoded-source snapshot contract. Texture2D,
TextureCube, and Terrain each have one typed translation authority. Built-in
candidate and uncooked-policy orchestration never decode image formats.
`TextureCubePostLoadPolicy` and `TerrainHeightmapAuthoringPolicy` register
independently; aggregate startup installs them in declaration order and rolls
back or tears them down in strict reverse order.

## Reimport, records, and recovery

Single-output reimport requires an asset identity, complete framework
provenance, and compatible recorded translator/planning-pass/asset-builder components. It
produces one detached candidate and saves one authored package. StaticMesh and
the texture/terrain families use this path without a companion record. The
same provenance reconstructs `SessionCritical` requests used by implemented
missing-derived-data recovery policies; ordinary load observes or submits that
work and does not run a separate family workflow.

Multi-output import creates an editor-only `DImportRecord`. Its outputs are
independent peer assets; no StaticMesh, material, texture, or other output owns
the rest. The record persists normalized settings, source identities and hashes,
stable output identities and policies, output fingerprints, accepted diagnostic
identities, and bounded implementation reconciliation state. A rebuildable editor
index maps managed output paths back to their record.

Import records retain their authored output identities across asset relocation.
Lookup, navigation, reconciliation, and reimport resolve each identity to its
final real package before use, while record serialization remains unchanged.
The record index participates in explicit redirector Fix Up as an external
reference store; Fix Up, not relocation or reimport, is the operation that
canonicalizes persisted record paths. A create/import/reimport destination
occupied by a redirector is an actionable collision naming its final target.
The store registration has an explicit handle, retained module-resource lease,
and owner callback gate. Shutdown unregisters that handle; an unavailable or
retired store makes strict Fix Up fail closed rather than invoking stale module
code or silently omitting persistent occurrences.

FBX and glTF enter through the Scene source workflow. The request selects one
Content destination directory rather than a primary StaticMesh. The Scene implementation
places generated assets in type directories such as `Meshes`, `Materials`, and
`Textures`, keeps the import record at the destination root, and leaves the
record's optional primary-output identity unset. FBX remains static-only;
version-1 skeletal import is a glTF/GLB-specific direct decoder and does not use
Assimp to associate joints, weights, inverse binds, or animation channels.

Record reimport reconstructs a Scene import request from the record and
reconciles every managed output in one build graph. Existing output mappings
preserve loaded identities and paths; removed outputs are reported as orphans,
and occupied unmanaged paths remain collisions. Reimport, recreate-missing,
and repair actions use the same request builder and job. Stable authored Scene
settings exclude ephemeral replacement mappings so unchanged reimport retains
record fingerprints and skeletal derived-data keys.

Canonical resave is not an import action. A selected record or any managed
output can route to the record package through the index, but AssetForge
performs no serialization of its own: AssetCore's generic package planner,
ordinary writer, atomic publication, and post-write probe own the operation.
Implementation state, settings, output fingerprints, managed assets, source files,
and reconciliation policy remain unchanged.

### glTF Skeletal Scene Contract

The version-1 path accepts glTF 2.0 `.gltf` and `.glb` roots with contained
captured external buffers, data-URI buffers, or a GLB BIN chunk. It decodes
finite node TRS/decomposable matrices, indexed triangle primitives, one
`JOINTS_0`/`WEIGHTS_0` set with at most four canonical influences, optional
inverse binds, and STEP/LINEAR translation, rotation, and scale tracks. Sparse
accessors, second influence sets, morphs, cubic interpolation, animated
intervening non-joints, and ambiguous mesh-to-skin ownership are unsupported
and fail the complete skeletal scene transaction with a structured diagnostic.

The version-1 limits are checked before allocation and use checked arithmetic:

| Collection | Limit |
| --- | ---: |
| Captured dependencies | 8,192 |
| Source nodes | 1,000,000 |
| Source meshes / primitives per mesh | 65,536 / 65,536 |
| Source skins / bones per Skeleton | 4,096 / 65,535 |
| SkeletalMesh outputs / vertices / indices | 16,384 / 100,000,000 / 300,000,000 |
| Sections / material slots / influences per vertex | 65,536 / 4,096 / 4 |
| Source animations / AnimationClip outputs | 4,096 / 65,536 |
| Tracks per clip / keys per track / total keys per clip | 65,536 / 16,777,216 / 100,000,000 |
| Diagnostics / total normalized scene bytes | 4,096 / 16 GiB |

Stable skeletal source categories are `UnsupportedFeature` for valid glTF
outside the selected subset, `MalformedSource` for invalid structure/ranges or
numeric relationships, `LossyNormalization` for accepted deterministic weight
or rotation repair, and `ResourceLimitExceeded` for a pre-allocation bound.
Missing-dependency, unsafe-path, and cancellation diagnostics retain their
framework meanings. No partial normalized skeletal result survives an error.

glTF's right-handed +Y-up values are converted to Durin's +X-forward,
+Y-right, +Z-up convention with `(x, y, z) -> (-z, x, y)`. The same basis
change applies to reference transforms, mesh-node binds, inverse binds, and
animation values. Skeletal vertices remain in mesh-local bind space; unlike the
existing static projection, node transforms are not baked into them. Canonical
bones are parent-before-child and deterministic; multiple roots receive one
synthetic `$DurinRoot`. Skeleton compatibility hashes canonical names, parents,
and exact reference transforms.

The handedness-changing basis conversion reverses each skeletal triangle's
winding. Missing normals are generated from the converted indexed geometry;
missing tangents are generated from UV0 and the final normals, with a stable
orthogonal fallback when UV0 is absent or degenerate. `COLOR_0` accepts glTF
`VEC3` and `VEC4` encodings, with RGB input receiving alpha `1`. A primitive
without a `material` reference uses one stable implicit default material rather
than aliasing source material zero.

One Skeleton is planned per source skin, one SkeletalMesh per supported
skinned-node/mesh association, and one AnimationClip per compatible
animation/skin pair. Stable identities are `skeleton:skin/<skin-index>`,
`skeletal-mesh:node/<node-index>/mesh/<mesh-index>`, and
`animation-clip:animation/<animation-index>/skin/<skin-index>`. Outputs live in
`Skeletons`, `SkeletalMeshes`, and `Animations` alongside existing peer
materials, textures, and static meshes. There is no primary output. Names are
presentation only; source-array indices are version-1 reconciliation identity,
so reordering a source array is an authored identity change.

The Scene planning pass and builders construct Skeleton candidates before dependent mesh
and clip candidates, validates every relationship against the prospective
Skeleton state, and commits in dependency order with reverse rollback. The
framework execution result is reconstructed in import-record order.
Unchanged reimport preserves loaded object identities, output paths, record
fingerprint, derived-data keys, and payload values. Removed managed outputs
become orphans; occupied unmanaged output paths remain collisions.

## Candidates and publication

Asset builders build detached authored state and validate it before an existing
identity changes. Loaded assets update only through typed, symmetric, no-fail
state exchange. Package publication revalidates the plan, exchanges prepared
state, and saves every changed output plus the record as one root-last bundle.
If publication fails, exchanges reverse and authored fingerprints must match
their pre-publication values.

Source ingestion and DDC writes have separate lifetimes. A failed import may
leave an explicitly ingested source or disposable cache entry, but it must not
leave a partially published authored asset graph.

## Asynchronous preparation

The synchronous executor is the semantic reference. The `FImportService` owns
one framework coordinator that may schedule immutable capture, hashing, dependency discovery,
parsing, normalization, and CPU preparation. It does not create `DObject`
instances or touch packages, the registry, editor models, render resources, or
RHI state on a worker.

`IsImportWorkerPreparation` is the testable worker marker installed by the
coordinator. Mutation boundaries call `CheckImportEditorMutationAllowed`
before single-asset planning/execution, Scene execution, or multi-output
publication. An extension that accidentally re-enters those editor mutation
paths from asynchronous preparation fails at the boundary before changing an
object or package.

Workers move value-only results into synchronized request state and publish a
request serial to a mailbox. An editor-thread tick consumes only the current
serial after its task handle is terminal. Dialog close, project switch,
extension unload, and process shutdown cancel and drain their request scopes
before releasing components or editor state.

Every accepted asynchronous request owns its progress reporter for the complete
worker lifetime. The initiating reporter and Widget are not retained. A
copyable handle observes immutable-by-copy `FImportOperationSnapshot` values
containing the operation and owner identities, stable phase, work counts,
optional determinate fraction, cancellation state, bounded diagnostic, and
background-presentation flag. Unknown totals remain indeterminate. Repeated
work updates for the same phase/source/output are coalesced and retained
history is bounded by `MaximumAsyncImportProgressHistory`.

Cancellation first publishes a non-cancelable `Canceling` snapshot. Terminal
`Succeeded`, `Failed`, `Canceled`, `Superseded`, and `Rejected` snapshots are
stable after result consumption and ignore late component progress. Publication
will use `Finalizing` to close the cooperative-cancellation boundary before any
failure-atomic editor mutation begins. Phase labels come from
`GetImportPhaseLabel`; presentation may localize them but extensions do not
invent a second phase vocabulary.

An operation that ends with plan delivery becomes terminal when the plan
mailbox is consumed. A caller that will execute the plan may request an
extended lifetime: successful plan consumption leaves the snapshot active,
`CreateProgressReporter` routes CandidateBuild through Restore into the same
state, and `CompleteOperation` publishes exactly one final outcome. Entering
Publication changes the snapshot to non-cancelable `Finalizing`. The result
mailbox and operation terminal lifetime are therefore related but independent.

## Persistence, DDC, and cooking

Authored runtime assets persist only the provenance needed for their own
single-asset rebuild. Component contracts, settings serialization, implementation
state, and DDC builders have independent versions. DDC paths are never
serialized and cache contents are never authoritative.

`DImportRecord` packages, implementation state, accepted editor diagnostics, and the
record index are editor-only. Records carry an explicit cook-exclusion marker
and are never runtime dependencies. StaticMesh, texture, SkeletalMesh, and
AnimationClip cook paths strip source provenance, derived-data keys, implementation
state, and editor diagnostics while publishing validated, redirect-free
runtime packages and payloads into cooked ownership. Skeleton is package-only.
Skeletal meshes and clips retain their hard Skeleton reference and structural
compatibility identity.
A runtime-only target loads
cooked outputs without `AssetForge`, `AssetForgeBuiltins`, Assimp,
authoring source files, or DDC fallback. Core may contain the generic codec,
but cooked loading never invokes source decoding or admits authoring policy.

For skeletal Scene outputs, candidate construction hashes the exact ordered
source closure plus normalized Scene settings, typed implementation state, stable
output identity, Skeleton compatibility, payload fingerprint, builder/schema,
and target/profile into the DDC key. The worker path produces only normalized
values; `DObject` creation, relationship binding, DDC publication, package
exchange, and registry publication remain on the editor thread.

## Compatibility baseline

The supported authored baseline is the current asset set versioned in this Git
repository. There are no production asset-specific structure upgraders. The
retired StaticMesh-owned Scene `ImportManifest`, generated material/texture
`ImportOwner`, StaticMesh `SourceFile` and duplicate `ImportSettings`,
StaticMeshComponent `Material`/`Materials`, Texture2D `SourceFile`, and
TextureCube face/panorama source-string fields have no compatibility reader or
migration API. Import-record schema 1 and StaticMesh material-slot schema 0 are
unsupported. Current Scene records identify translator `Durin.SceneGraph`
contract 1 and `Durin.Scene.Plan` settings schema 1, use AssetForge contract 2,
and store Scene state as `Durin.AssetForge.Scene.State` schema 2. The request
builder accepts only the current component contracts and does not reconstruct
retired provider or factory identities. StaticMesh and Texture2D reimport and
recovery likewise require persisted current AssetForge provenance; source-import
metadata is not a provenance reconstruction path.
Repository assets must be upgraded or regenerated in the same change that
removes an old schema.

AssetCore's construct-free compatibility audit reports retired or unknown
import fields, while ordinary load rejects them before package residency. There
is no partially compatible live package or data-loss save permission. Recreate
or reimport that asset with compatible AssetForge components.

## Editor workflow

External files are first ingested into a writable mount content directory;
import then references the mounted source. Preview shows the complete source
closure, outputs, policies, collisions, replacements, warnings, resource
estimates, missing outputs, and orphans before execution. Content Browser
actions inspect framework capabilities, navigate from any managed output to its
record, and keep single-asset reimport separate from record reimport. Scene
source import is exposed independently from StaticMesh single-asset import and
reveals its destination directory after publication.

See [Mounted Source Workflows](../Guides/MountedSourceWorkflows.md),
[Asset Packages](../../Runtime/Assets/AssetPackages.md), and
[Asset Data Lifecycle and Storage](../../Runtime/Assets/AssetDataLifecycle.md).
