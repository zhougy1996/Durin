# Asset Import Framework

Summary: Define graph-based asset-import admission, translation, planning, typed construction, publication, and extension ownership.

Modules: AssetForge, AssetForgeBuiltins, AssetCore

Last reviewed: 2026-08-26

Durin editor imports use one AssetForge framework for source capture,
translation, ordered planning-pass execution, typed detached construction,
candidate validation, publication, diagnostics, cancellation, single-asset
reimport, and editor recovery. `AssetForge` owns immutable source and build
graphs plus `FImportJob`; `AssetForgeBuiltins` supplies built-in source
translators, planning passes, and asset builders for StaticMesh, Texture2D,
TextureCube, VolumeTexture, Terrain Heightmap, and heterogeneous Scene output. Runtime
targets depend on neither editor module.

Concrete translation terminates at normalized owned values. Built-in implementations
do not pass encoded PNG, HDR, glTF, FBX, or Assimp input into
`TextureBuild`, `StaticMeshBuild`, `SkeletalBuild`, or `TerrainBuild`. Build requests contain no decoder type or mutable
`DObject`; detached products publish on the main thread through narrow Engine
state-application APIs, then use AssetCore persistence independently.

## Ownership and layering

The dependency direction is `Core/CoreDObject -> AssetCore -> AssetForge
-> AssetForgeBuiltins -> editor hosts`.

- `AssetCore` owns packages, the private catalog store, resident publication
  state, disposable DDC objects, and atomic package-bundle persistence.
  Import candidates are `NewlyCreated`, Dirty resident packages. Import code
  consumes catalog values and uses `UnloadPackage(..., DiscardUnsaved)` for
  candidates abandoned before live publication; successful bundle persistence promotes the same
  resident entries to `Published`. AssetCore has no knowledge of import extensions or
  concrete imported asset classes.
- `AssetForge` owns one `FImportService`, source snapshots, diagnostics,
  source/build graph contracts, candidates, component registries, and
  scheduled/inline orchestration.
- Extension modules register source translators for source semantics, ordered planning
  passes for import policy, and typed asset builders for output construction. None owns
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
`Durin::AssetForge`. Built-in registration, direct asset-operation APIs,
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

Each service instance owns both its component registrations and its asynchronous
job store. Operation handles observe retained request state and carry only a weak
cancellation route back to that store; they never retain a service pointer. A
different service instance cannot cancel, inspect, or drain those jobs, and
service destruction closes admission and drains every accepted request before
releasing its implementation state.

Each exact registration retains the module callback gate and resource lease.
A submitted value retains component leases through invocation and destruction.
Retirement closes admission, cancels and drains owned jobs, waits admitted
callbacks, then removes the exact generation. A stale registration handle
cannot retire its replacement.

The public request selects one source translator, versioned translator settings, an
ordered planning-pass stack, a mounted root and optional declared sources, one
destination, operation owner/lifetime, limits, save policy, and optional
existing single-asset provenance. Initial import, single-asset reimport, source
replacement, repair, and recovery differ by request mode and graph content,
not by orchestration interface. AssetForge has no general preview mode: an
import dialog displays settings and cheap header-derived information, and full
translation and construction begin only after confirmation.

TextureCube follows the same boundary. `TextureCubeImport.h` owns
six-face and panorama validation, import/reimport, source-reference changes,
ingestion and package save. It decodes concrete image bytes, submits
owned normalized requests to TextureBuild, and publishes only detached products
through Engine's narrow TextureCube state-application API. Runtime Engine exposes no
create/save/source workflow and no mutable TextureCube import registry.

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

Direct asset creation, source-reference changes, repair, and uncooked PostLoad use
the same AssetForge encoded-source snapshot contract. Texture2D,
TextureCube, and Terrain each have one typed translation authority. Built-in
candidate and uncooked-policy orchestration never decode image formats.
`TextureCubePostLoadPolicy` and `TerrainHeightmapAssetFeatures` register
independently; aggregate startup installs them in declaration order and rolls
back or tears them down in strict reverse order.

## Reimport, Scene import, and recovery

Single-output reimport requires an asset identity, complete framework
provenance, and compatible recorded translator/planning-pass/asset-builder components. It
produces one detached candidate and saves one authored package. StaticMesh and
the texture/terrain families use this path without a companion record. The
same provenance reconstructs `SessionCritical` requests used by implemented
missing-derived-data recovery policies; ordinary load observes or submits that
work and does not run a separate family workflow.

FBX and glTF enter through the Scene source workflow. The request selects one
Content destination directory rather than a primary StaticMesh. The Scene implementation
places generated assets in type directories such as `Meshes`, `Materials`, and
`Textures`. Every generated output is an ordinary independent asset. Scene
import is intentionally creation-only: it persists no aggregate Scene asset,
management record, reverse index, stable reconciliation identity, tombstone, or
whole-scene reimport action. A later source revision is imported into a new
destination or handled through supported single-asset workflows. FBX remains static-only;
version-1 skeletal import is a glTF/GLB-specific direct decoder and does not use
Assimp to associate joints, weights, inverse binds, or animation channels.

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
materials, textures, and static meshes. There is no primary output. Names and
source-array indices deterministically derive paths for that one import
transaction; they are not identities retained for later reconciliation.

The Scene planning pass and builders construct Skeleton candidates before dependent mesh
and clip candidates, validate every relationship against the prospective
Skeleton state, and publish in dependency order. The framework execution result
follows planned dependency order. Existing destination packages are collisions;
Scene import does not replace or reconcile them.

## Candidates and publication

Asset builders build detached authored state and validate it before an existing
identity changes. While that work is running, the import operation state is the
authority for progress and the loaded asset remains usable with its last
published state. Workers never mark the asset itself as partially imported.

After the operation enters non-cancelable `Finalizing`, the editor thread
publishes each completed candidate into its live identity once, in dependency
order. This is a one-way state transition rather than an AssetForge-owned
reversible exchange. Publication updates content, editor-only import data, DDC
identity, diagnostics, and render resources, then leaves the affected package
Dirty.

Persistence follows publication and has an independent result in
`FImportResult::Persistence`. A successful save makes the published state the
new authored disk state. A failed save does not reverse a valid in-memory
publication: the import outcome remains `Succeeded`, persistence is `Failed`,
the package remains Dirty, and the prior on-disk/catalog state remains intact.
This matches ordinary editor mutation semantics and allows the user to retry
Save without rebuilding the asset.

Source ingestion and DDC writes have separate lifetimes. Failure or cancellation
before finalization leaves the prior live asset state unchanged. Once one-way
publication begins, a builder contract violation may leave already published
outputs Dirty; builders therefore must complete all failable construction and
relationship validation before finalization and keep live publication to a
narrow editor-thread state application.

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
uses `Finalizing` to close the cooperative-cancellation boundary before any
live editor mutation begins. Phase labels come from
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

AssetForge implementation state and editor diagnostics are editor-only.
StaticMesh, texture, SkeletalMesh, and
AnimationClip cook paths strip source provenance, derived-data keys, implementation
state, and editor diagnostics while publishing validated, redirect-free
runtime packages and payloads into cooked ownership. Skeleton is package-only.
Skeletal meshes and clips retain their hard Skeleton reference and structural
compatibility identity.
A runtime-only target loads
cooked outputs without `AssetForge`, `AssetForgeBuiltins`, Assimp,
authoring source files, or DDC fallback. Core may contain the generic codec,
but cooked loading never invokes source decoding or admits editor import policy.

For skeletal Scene outputs, candidate construction hashes the exact ordered
source closure plus normalized Scene settings, typed implementation state, stable
output identity, Skeleton compatibility, payload fingerprint, builder/schema,
and target/profile into the DDC key. The worker path produces only normalized
values; `DObject` creation, relationship binding, DDC publication, one-way
asset publication, and registry publication remain on the editor thread.

## Compatibility baseline

The supported authored baseline is the current asset set versioned in this Git
repository. There are no production asset-specific structure upgraders. The
retired StaticMesh-owned Scene `ImportManifest`, generated material/texture
`ImportOwner`, StaticMesh `SourceFile` and duplicate `ImportSettings`,
StaticMeshComponent `Material`/`Materials`, Texture2D `SourceFile`, and
TextureCube face/panorama source-string fields have no compatibility reader or
migration API. StaticMesh material-slot schema 0 is unsupported. Scene requests
use translator `Durin.SceneGraph` contract 1 and `Durin.Scene.Plan` settings
schema 1. The request builder accepts only current component contracts and does
not reconstruct retired provider or factory identities. StaticMesh and Texture2D reimport and
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
import then references the mounted source. Import dialogs show static settings
and cheap source information only; confirmation performs the first complete
parse, plan, build, and collision check. Content Browser actions inspect
single-asset framework capabilities. Scene source import is exposed
independently from StaticMesh single-asset import and reveals its destination
directory after publication; it offers no whole-scene reimport.

See [Mounted Source Workflows](../Guides/MountedSourceWorkflows.md),
[Asset Packages](../../Runtime/Assets/AssetPackages.md), and
[Asset Data Lifecycle and Storage](../../Runtime/Assets/AssetDataLifecycle.md).
