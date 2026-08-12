# Engine Asset Format and Build Boundary Plan

Summary: Rebuild the Engine asset authoring boundary around unified runtime-owned payload codecs, FArchive-based bidirectional serialization, detached authoring builds, and explicit source translators.

Last reviewed: 2026-08-12

Status: Active
Completed:

## Current Status

This plan supersedes the previous stage decomposition in this file. The former
design assigned payload writers to `EngineAssetBuild` while retaining payload
readers in Runtime `Engine`. That direction is rejected: a stable binary format
has one semantic owner, and its save, load, validation, version negotiation,
limits, stable identifiers, alignment, and checksum rules must evolve together.

Work completed before this rewrite remains useful and is not reverted merely to
restart the plan:

- The authoring-only `EngineAssetBuild` module exists, is selected by editor
  profiles, and is excluded from the `DurinGame` dependency closure.
- Active Texture2D candidate builds use an independently exported texture
  builder for mip generation, format selection, BC compression, metrics, and
  cancellation.
- TextureCube panorama projection and candidate build paths, plus the first
  TerrainHeightmap authoring operations, have entered `EngineAssetBuild`.
- `StandardAssetImport` and editor import entry points explicitly reach the new
  module for those migrated paths.
- Stage-zero golden DDC keys, payload hashes, corrupt-input behavior, cooked
  loads, transaction preservation, coordinator behavior, and dependency
  baselines exist for the affected asset families.

The current repository is nevertheless an intentional migration state, not an
acceptable boundary:

- Texture TXPL encoding and key construction are duplicated between Runtime
  `Engine` and `EngineAssetBuild`.
- The build module accepts encoded PNG/HDR-style bytes and mutable `DObject`
  assets in several public operations, so source translation, pure build work,
  and publication are not separated.
- Runtime asset self-build paths and the Texture2D coordinator still require
  the old Engine builder implementation and keep `bc7enc_rdo` linked by both
  Engine and `EngineAssetBuild` in editor configurations.
- StaticMesh still relies on Runtime decoder registration and broad asset-owned
  build behavior; skeletal, animation, and terrain key/build/codec boundaries
  have not been normalized.
- The current `FArchive` abstraction has the right bidirectional direction and
  structured failure model, but its primitive memory archives use native byte
  representation and lack the bounded, canonical binary facilities needed to
  replace the asset-specific wire readers and writers safely.

No further writer-only extraction is allowed. The next implementation work is
the archive foundation and a complete Texture2D vertical slice.

## Goal

Establish one structural dependency boundary for Engine assets:

- Runtime `Engine` owns runtime asset value types and the complete bidirectional
  serialization contract for every payload those types consume.
- `EngineAssetBuild` owns source-independent production of runtime/platform
  values, build recipe identity, DDC policy, scheduling, and authoring
  diagnostics.
- `StandardAssetImport` owns concrete source-format translation and import
  policy, and passes normalized owned values into `EngineAssetBuild`.
- `AssetImportCore` remains format-neutral, while `AssetCore` remains
  asset-family-neutral.
- Runtime game products consume cooked state without importing sources,
  building derived data, opening DDC, or loading authoring modules.

Editor import, reimport, rebuild, Cook preparation, repair, diagnostics, and
headless asset tools must preserve deterministic outputs and transactional
behavior. Existing payload and key bytes remain compatible unless a separate,
explicitly versioned correctness change is approved and qualified.

## Vocabulary and Semantic Boundaries

The word encoding previously hid three unrelated responsibilities. This plan
uses the following terms consistently:

| Operation | Example | Final owner |
| --- | --- | --- |
| Source translation | PNG/HDR/FBX/glTF/Assimp bytes to normalized pixels, geometry, skeletons, clips, or height samples | `StandardAssetImport` |
| Derived build | Mips, panorama projection, BC compression, mesh/collision conversion, palette generation, hierarchy construction | `EngineAssetBuild` |
| Payload serialization | Runtime/platform values to or from TXPL, DMSH, DCOL, skeletal, animation, or THPL bytes | Runtime `Engine` |
| Build identity | Canonical build-recipe inputs to a DDC key | `EngineAssetBuild` |
| Object storage/publication | DDC object bytes, packages, Cook descriptors, atomic writes, mutation transactions | `AssetCore` |

Source translation may decode a third-party file format. A derived build may
compress content for a target GPU. Neither operation owns the runtime payload
wire format.

## Target Dependency Direction

```text
Core
  ^
  |  canonical FArchive byte layer, versions, bounds, failures
CoreDObject
  ^
  |  reflected fields, DObject references, object/package archive adapters
AssetCore <---------------- AssetImportCore
  ^                              ^
  |                              |
Engine                       StandardAssetImport
  ^                              |
  |                              |
EngineAssetBuild <---------------+
  ^
  |
asset editors / cooker / DurinAssetTool / authoring tests

DurinGame -----------------> Engine only from this authoring branch
```

Arrows point from a consumer toward a dependency. In particular:

- `EngineAssetBuild` depends on `Engine` and calls Engine-owned payload codecs.
- `StandardAssetImport` depends on `AssetImportCore` and `EngineAssetBuild`.
- `Engine` never includes, loads, registers, or discovers an authoring module.
- `AssetImportCore` never depends on an Engine asset family or a concrete source
  provider.
- `DurinGame` never reaches `EngineAssetBuild`, `AssetImportCore`,
  `StandardAssetImport`, Assimp, image translators, or offline compressors.

## Module Ownership

### Core

`Core` owns the reusable byte-archive substrate:

- archive direction, purpose, canonical byte order, position, bounded regions,
  version contexts, first-failure state, and raw byte transfer;
- canonical memory/span readers and writers;
- counting and hashing archives required for deterministic sizes, offsets, and
  checksums;
- bounded string, byte-buffer, sequence, alignment, and padding helpers that do
  not depend on reflection or asset types.

The generic substrate must not know `DObject`, packages, DDC namespaces, or an
Engine payload schema.

### CoreDObject

`CoreDObject` layers object semantics over the Core archive substrate:

- reflected logical type and field descriptors;
- object/field/path scopes and canonical reflected map behavior;
- hard and soft `DObject` references;
- object graph, duplicate, property snapshot, and authored-package adapters.

Existing `DObject::Serialize` behavior and package bytes must remain qualified
during the extraction. Transitional include forwarding is allowed, but the
generic archive implementation must have a single final owner.

### AssetCore

`AssetCore` owns generic disposable storage and authored/cooked transactions:

- `FDerivedDataObjectStore`, cache object namespaces, atomic byte writes, and
  corruption handling that is independent of asset family;
- package formats, bulk descriptors, manifests, Cook publication, registry and
  mutation transactions;
- source capture/storage primitives that do not recognize a concrete format.

It does not own texture/mesh/skeletal/animation/terrain schema fields, build
versions, or build decisions.

### Runtime Engine

`Engine` owns each runtime asset's data and complete payload contract:

- reflected asset schema needed to load authored and cooked packages;
- runtime/platform value types, stable serialized identifiers, payload IDs,
  schema versions, limits, layout rules, and semantic validation;
- one bidirectional `FArchive` serializer per payload family;
- thin encode/decode convenience wrappers that both call that serializer;
- runtime CPU state, render-resource construction, readiness, strict cooked
  loading, and narrow detached-state publication/exchange seams.

Engine payload save support is not an offline asset builder. Keeping the save
and load directions together does not permit runtime source import or runtime
derived-data generation.

Runtime package load never opens source files, calls a provider, generates
mips, invokes an offline compressor, queues a build, or consults DDC. Missing,
corrupt, or incompatible cooked payloads fail explicitly.

### EngineAssetBuild

`EngineAssetBuild` owns authoring-only production:

- normalized build request and detached product contracts;
- mip generation, projection, platform-format selection, offline compression,
  mesh/collision conversion, skeletal/animation preparation, terrain hierarchy
  construction, resource budgets, cancellation, and metrics;
- asset-specific build-key recipes, builder/compressor/projection versions,
  DDC namespaces, hit/miss/rebuild policy, and authoring diagnostics;
- bounded asynchronous coordination, latest-generation policy, completion
  mailboxes, waits, and shutdown draining;
- main-thread authoring adapters that combine detached products, provenance,
  Engine publication seams, and `AssetCore` transactions.

Public worker operations consume immutable or owned snapshots and return
detached values. They never accept a mutable `DObject`, package, registry model,
render resource, or RHI object. The module does not recognize PNG, HDR, FBX,
glTF, or Assimp source formats and does not implement an Engine payload wire
writer.

### AssetImportCore

`AssetImportCore` remains format-neutral and owns source snapshots, plans,
provider leases, candidate sets, generic diagnostics, cancellation,
publication ordering, records, and rollback. It may transport opaque or owned
provider values but does not interpret a source format or an Engine build
recipe.

### StandardAssetImport

`StandardAssetImport` is the standard translator and import-policy module:

- file recognition and concrete image/Assimp/glTF/FBX decoding;
- provider identities and contract versions;
- normalized source construction, Scene policy, material/output policy,
  reconciliation, source roles, and import fingerprints;
- conversion from captured bytes to EngineAssetBuild request values;
- mapping a detached build product into an `AssetImportCore` candidate and
  publication transaction on the main thread.

It must not forward encoded source bytes to `EngineAssetBuild`, write Engine
payload formats, compress runtime blocks, or register a decoder in Runtime
Engine.

## Payload Codec Contract

### One owner and one serializer

Every stable payload family has one Engine-resident serializer such as:

```cpp
auto SerializeTexture2DPayload(
    FArchive& Ar,
    FTexturePayloadContext& Context,
    FTexturePlatformData& Value) -> void;
```

The public save and load wrappers translate archive failures into the existing
result style, but contain no independent field order, version, offset, limit,
or validation implementation. A load constructs a detached candidate and only
returns or publishes it after archive and semantic validation succeed.

Offset tables, chunk directories, padding, checksums, and bulk regions remain
part of the serializer contract. The archive layer must support them without
unbounded allocation or native-layout serialization.

### Version ownership

Versions are separated by the behavior they invalidate:

| Version | Owner | Effect |
| --- | --- | --- |
| Payload schema version | Runtime `Engine` codec | Determines wire compatibility and reader selection |
| Stable serialized ID revision | Runtime `Engine` codec | Defines persistent enum/type identities |
| Builder version | `EngineAssetBuild` | Invalidates build keys when produced values can change |
| Compressor/SDK version | `EngineAssetBuild` | Invalidates relevant build keys and diagnostics |
| Projection/conversion version | `EngineAssetBuild` | Invalidates the affected build recipe |
| Provider/translator version | `StandardAssetImport` | Invalidates import fingerprints/provenance when normalized source interpretation changes |

A runtime reader must not reject a payload solely because its producer used a
different builder version when the payload schema is still compatible. A
producer version may be retained as diagnostic metadata, but it is not a wire
compatibility gate.

### Canonical and hostile-input behavior

- Persistent payload and key bytes use explicit little-endian canonical
  encoding; no primitive is serialized by native object representation.
- Every count, byte length, offset, range, multiplication, and allocation is
  bounded before use.
- Readers reject overlap, non-canonical ordering, non-zero reserved fields or
  padding, incompatible IDs, checksum mismatch, trailing bytes where forbidden,
  and incomplete semantic structures.
- Writers apply the same limits and stable identifiers as readers.
- Generic archive helpers do not silently serialize container capacity,
  pointers, padding, ABI layout, paths, timestamps, or unordered iteration.
- A codec change that alters bytes requires an explicit schema decision,
  golden update, compatibility reader or migration story, and Cook/runtime
  qualification. A module move alone never changes bytes.

## Build and Publication Contract

An asset-family build follows this shape:

```text
captured source bytes
  -> StandardAssetImport translator
  -> normalized immutable EngineAssetBuild request
  -> pure/cancellable detached build product
  -> Engine-owned payload serializer
  -> EngineAssetBuild DDC policy
  -> prepared main-thread publication
  -> AssetCore transaction commit
```

Requests include every normalized source identity, setting, target/profile,
builder dependency, and version required for a stable key. Physical paths and
timestamps are optimization or diagnostic facts, never key identity.

Worker tasks cannot access `DObject`. Main-thread publication may call a narrow
Engine-owned exchange that atomically swaps a complete candidate. Cancellation,
translation failure, build failure, codec failure, DDC corruption/write
failure, stale generation, or publication failure preserves the previous
complete authored and runtime state according to the existing contract.

Best-effort DDC write failure may leave a valid in-memory product available for
publication; it cannot create a partial package transaction. Ordinary package
load never turns a DDC miss into an implicit import or build.

## Current Migration Baseline

The following facts are frozen as the starting point for the rewritten plan:

| Area | Reusable work | Required correction |
| --- | --- | --- |
| Module graph | `EngineAssetBuild` exists and authoring/game selection gates exist | Complete dependency/deployment proof after all legacy Engine links are removed |
| Archive | Bidirectional direction, purpose, capability, versions, paths, failures, memory archives | Separate generic Core byte layer from DObject semantics; add canonical endian, span, bounds, alignment, counting and hashing |
| Texture2D builder | Active candidate/test path uses the exported build module algorithms | Replace encoded-byte/mutable-asset API, unify TXPL codec in Engine, migrate coordinator and remove Engine builder copy |
| TextureCube builder | Panorama/six-face projection and provider candidate paths use build operations | Move source decoding to StandardAssetImport, unify TXPL codec and keys, remove legacy Engine seams |
| Terrain | Provider candidate and direct import can reach build operations | Introduce normalized samples request/product, unify THPL codec, move keys and authoring policy |
| StaticMesh | Characterized DMSH/DCOL, detached render candidate and exchange mechanisms exist | Route normalized geometry through Build; unify codecs; remove decoder registration and asset self-build APIs |
| Skeletal/animation | Stable keys, payloads, relationship validation and exchanges exist | Split translation/build/codec/version ownership and detach worker paths |
| Import | Framework transactions and standard providers are established | Translators must emit normalized Build requests rather than encoded-byte calls or Runtime decoder registration |
| DDC | Generic object storage and asset-specific characterization exist | Keep storage generic; move recipe/key/policy to Build while Engine owns payload bytes |
| Tests | Golden keys/hashes, corruption, Cook, runtime and rollback coverage exist | Add archive symmetry/version/bounds tests and vertical-slice dependency gates |

The earlier Stage 0 characterization is retained as evidence. Its former rule
that private payload writers follow build operations is explicitly replaced by
the codec-follows-runtime-value rule in this plan.

## Implementation Stages

### Stage 0: Rebaseline the rejected split

Dependencies: none.

- [x] Record that writer-in-Build/reader-in-Engine is not a permitted final
  architecture.
- [x] Preserve the previously committed module, build algorithm, golden byte,
  failure, lifecycle, and dependency characterization work.
- [ ] Reclassify the remaining public and private symbols for every asset family
  using the vocabulary and module ownership in this plan.
- [ ] Record all duplicate implementations and compatibility seams, including
  their named consumers and the exact stage that removes them.
- [ ] Add explicit dependency checks that distinguish source translators,
  offline compressors, payload codecs, build keys, and generic storage.
- [ ] Confirm the pre-archive-refactor byte baselines for object graphs,
  authored packages, DDC keys, TXPL, DMSH/DCOL, skeletal, animation, THPL, and
  cooked packages.
- [ ] Record the revised Stage 0 handoff before changing `FArchive` ownership.

#### Acceptance Gate

- Every operation is classified as source translation, derived build, payload
  serialization, build identity, storage, or publication, with exactly one
  final module owner.
- Every duplicate and migration seam has a bounded removal stage and a named
  compatibility test.
- Existing byte, failure, transaction, lifecycle, and dependency baselines are
  reproducible before the archive change begins.

### Stage 1: Establish the canonical FArchive foundation

Dependencies: revised Stage 0 inventory and byte baselines.

- [ ] Extract archive direction, purpose, canonical byte order, raw transfer,
  position, bounded regions, versions, and failure state into a generic `Core`
  archive layer.
- [ ] Retain reflected field/object/path scopes, DObject reference behavior, and
  object/package adapters in `CoreDObject` on top of that layer.
- [ ] Add canonical little-endian span reader/writer, counting archive, hashing
  archive, alignment/padding helpers, bounded byte/string/sequence helpers, and
  const-correct save versus mutable load byte APIs.
- [ ] Add payload/key archive purposes without coupling Core to Engine or DDC
  asset families.
- [ ] Migrate or adapt `DerivedDataCache::FWriter/FReader` so canonical primitive
  behavior has one implementation while preserving every existing cache byte.
- [ ] Preserve source compatibility with forwarding headers or aliases only
  while named consumers migrate; do not retain two archive implementations.
- [ ] Qualify primitive endian bytes, span truncation, bounds, overflow,
  alignment, padding, counting/hash equivalence, version lookup, and structured
  failure paths.
- [ ] Requalify DObject object-graph, property snapshot, duplication, authored
  package, PackageV4, and Cook bytes before completing the stage.

#### Acceptance Gate

- Core owns one canonical byte archive independent of `DObject` and assets.
- CoreDObject behavior and all frozen object/package bytes remain compatible.
- No persistent writer relies on native ABI representation or unbounded
  container allocation.
- The archive facilities required by payload codecs are public, documented, and
  qualified before any payload implementation is rewritten.

### Stage 2: Complete the Texture2D vertical slice

Dependencies: canonical archive foundation.

- [ ] Split the mixed Texture derived-data header into Engine-owned payload
  schema/codec declarations and Build-owned recipe/key declarations.
- [ ] Replace the Runtime TXPL encoder and decoder with thin wrappers over one
  Engine-resident bidirectional Texture2D payload serializer.
- [ ] Preserve exact TXPL bytes, strict corrupt-input diagnostics, platform
  value construction, and source/DDC-free cooked loads.
- [ ] Remove builder version as a Runtime payload compatibility gate while
  preserving current version compatibility and DDC invalidation behavior.
- [ ] Replace `BuildTexture2DFromEncodedBytes(DTexture2D&, ...)` with a normalized
  decoded-image request and detached product; move concrete image translation
  to `StandardAssetImport`.
- [ ] Keep Texture2D key construction, builder/compressor versions, DDC namespace
  and policy solely in `EngineAssetBuild`; call the Engine codec to serialize a
  valid product.
- [ ] Migrate the Texture2D coordinator, diagnostics, waits and lifecycle to the
  build module without allowing worker access to assets.
- [ ] Add a main-thread publication adapter using the narrow Engine-owned
  detached-state seam and existing AssetCore transaction semantics.
- [ ] Delete `TextureDerivedDataWriter`, the legacy Engine Texture2D builder,
  self-build/forwarding methods, and redundant tests after consumer proof.
- [ ] Remove the Engine BC link once no Engine Texture2D caller requires it.

#### Acceptance Gate

- Texture2D has one Engine TXPL serializer, one Build recipe/key implementation,
  one Standard image translation path, and no duplicate writer or builder.
- Worker build paths accept no encoded source format and touch no `DObject`.
- Golden keys/payloads, corruption behavior, DDC policy, import/reimport,
  cancellation, latest-wins, rollback, Cook, runtime load and rendering pass.
- Engine no longer reaches BC compression because of Texture2D.

### Stage 3: Complete TextureCube and TerrainHeightmap slices

Dependencies: completed Texture2D pattern and no unresolved archive defect.

- [ ] Apply the Texture2D codec/build/key split to TextureCube while sharing only
  genuinely common Texture codec structures and limits.
- [ ] Move LDR/HDR and six-face source translation fully to
  `StandardAssetImport`; pass normalized owned face/panorama values to Build.
- [ ] Keep projection, exposure policy, mip generation, format selection and
  compression in Build; keep TXPL serialization in Engine.
- [ ] Remove the duplicate TextureCube writer, legacy Engine builder, broad
  encoded-source operations, and obsolete publication forwarding.
- [ ] Introduce a normalized height-sample Terrain request/product, move concrete
  image interpretation to StandardAssetImport, and retain hierarchy/build/key/
  DDC policy in Build.
- [ ] Rewrite THPL save/load as one Engine `FArchive` serializer and retain
  runtime query/publication behavior in Engine.
- [ ] Remove Terrain asset self-build/source decode/key/write logic after direct
  import, reimport, repair, Cook and editor callers use the new operations.

#### Acceptance Gate

- TextureCube and Terrain each have one Engine codec, one Build recipe, one
  normalized translator handoff, and no duplicate or encoded-byte build API.
- Existing projection, exposure, cube-face ordering, terrain revision,
  hierarchy, DDC, Cook/load, rendering/query and failure semantics pass.
- Runtime Engine has no Texture build algorithm or offline compressor link.

### Stage 4: Complete the StaticMesh vertical slice

Dependencies: established vertical-slice pattern and archive/container support
qualified against chunked mesh payloads.

- [ ] Define Build-owned normalized StaticMesh and collision requests that
  contain no provider objects, source-format assumptions, or mutable asset.
- [ ] Make StandardAssetImport Assimp/glTF/FBX translators produce those values
  for single-asset and Scene workflows.
- [ ] Move render-data conversion, collision building, build keys, DDC policy,
  rebuild decisions, metrics and diagnostics to EngineAssetBuild.
- [ ] Rewrite DMSH and DCOL save/load using Engine-resident bidirectional
  serializers with shared chunk-directory, bounds and checksum logic.
- [ ] Preserve material reconciliation, reflected provenance/settings,
  body-setup ownership, cooked descriptors, runtime collision/render values and
  imported-state exchange in Engine.
- [ ] Replace broad `InitializeFromImportedData`, source inspection/repair and
  self-build entry points with detached products and narrow publication seams.
- [ ] Migrate every named consumer, then remove
  `RegisterStaticMeshSourceDecoder`, its global state, complete Runtime imported
  intermediates, and compatibility forwarding.

#### Acceptance Gate

- StaticMesh source translators call Build explicitly; Engine owns no decoder
  registry, source format or build policy.
- DMSH/DCOL have one serializer each and preserve golden bytes, strict malformed
  chunk behavior, collision, Cook/load and render-resource results.
- Single and Scene import/reimport preserve slot/material reconciliation,
  fingerprints, output order, repair and rollback.

### Stage 5: Complete SkeletalMesh and AnimationClip slices

Dependencies: StaticMesh chunked codec and Scene import pattern.

- [ ] Define normalized Build requests for skeleton relationships, skeletal
  geometry/influences and animation tracks without exposing provider-specific
  Scene objects or mutable assets to workers.
- [ ] Keep glTF/Assimp/source policy and normalized Scene reconciliation in
  StandardAssetImport.
- [ ] Move skeletal/animation preparation, key recipes, builder versions, DDC
  decisions, fingerprints and authoring diagnostics to EngineAssetBuild.
- [ ] Rewrite skeletal and animation payload save/load as Engine-resident
  bidirectional serializers while preserving strict relationship and skeleton
  compatibility validation.
- [ ] Preserve runtime payload/value types, skeleton references, Cook
  descriptors, sampling, render-resource creation and imported-state exchanges
  in Engine.
- [ ] Remove asset-owned rebuild/DDC/source operations and remaining complete
  imported intermediates after all single/Scene/Cook callers migrate.

#### Acceptance Gate

- SkeletalMesh and AnimationClip each have one Engine codec and Build recipe,
  with translator, worker, and publication boundaries mechanically testable.
- Golden payloads/keys, influence and track bounds, skeleton compatibility,
  Scene output identities, Cook/load, sampling/rendering and rollback pass.

### Stage 6: Consolidate authoring services, hosts and lifecycle

Dependencies: all asset-family vertical slices complete.

- [ ] Consolidate common request ownership, admission budgets, priorities,
  cancellation, completion mailboxes, waits and diagnostic snapshots without
  erasing asset-family request types or creating a mutable global asset API.
- [ ] Bind editor startup/tick, save waits, project/source relocation, provider
  unload, task drain and shutdown to EngineAssetBuild after the task system and
  before dependent authoring modules unload.
- [ ] Make asset editors query and invoke Build capabilities only for explicit
  rebuild/status UI; import-only hosts continue through AssetImportCore and
  providers.
- [ ] Make Cook and `DurinAssetTool` explicitly select EngineAssetBuild for
  build/migrate/repair commands while keeping package-only audit paths free of
  translators and offline compressors.
- [ ] Load StandardAssetImport only where concrete source translation is
  required; do not make Cook or tools depend on it for source-free operations.
- [ ] Remove Runtime Engine initialize/pump/wait/shutdown branches for asset
  builds and qualify that no callback crosses provider, build-module, object,
  render or task-system teardown.

#### Acceptance Gate

- Runtime Engine lifecycle contains no authoring service and `DurinGame`
  initializes, pumps and shuts down none.
- Editor, Cook and tools select only the capabilities needed by each operation.
- Admission, cancellation, latest-wins, wait, unload, shutdown and transaction
  behavior are bounded and race-safe for all migrated families.

### Stage 7: Remove seams and qualify the product boundary

Dependencies: consolidated hosts and lifecycle.

- [ ] Remove all obsolete Runtime source decoders, build algorithms, DDC key/
  write policy, authoring diagnostics, build coordinators, forwarding wrappers,
  editor-only builder fields and `DURIN_WITH_EDITOR` authoring branches.
- [ ] Remove duplicated asset payload readers/writers and transitional archive
  compatibility implementations after repository-wide consumer proof.
- [ ] Reduce Engine public headers to runtime/authored schema, unified codec
  contracts, payload access, detached publication seams, consumption status and
  render resources.
- [ ] Inspect module binaries, third-party links and deployed files for editor,
  game, Cook, tools and focused tests.
- [ ] Run focused Archive, CoreDObject, AssetCore, Engine asset, builder, import,
  Cook, editor, tool, renderer/Vulkan and runtime-process tests.
- [ ] Because the change crosses Core serialization and multiple native-test
  targets, run the final full native-test gate at default target granularity
  after focused diagnosis.
- [ ] Complete clean full `all` builds for the selected editor and game Agent
  Build Profiles and run source/DDC-free cooked game smoke validation.
- [ ] Move lasting archive, codec, build, import, Cook, lifecycle and runtime
  independence rules into owning documentation, then complete this plan.

#### Acceptance Gate

- Every payload family has exactly one Engine-resident bidirectional codec and
  every build family exactly one EngineAssetBuild recipe/key path.
- StandardAssetImport is the only standard concrete source translator; no
  Runtime or Build module recognizes its file formats.
- Runtime Engine and `DurinGame` contain no source translator, offline
  compressor, build recipe, DDC builder, coordinator or authoring diagnostic.
- Existing authored packages, keys and payloads remain compatible or have an
  explicitly approved and qualified version transition.
- Focused tests, full native tests, editor/game full builds, deployment
  inspection, cooked runtime smoke, documentation validation and committed
  handoff pass.

## Stage Execution Rules

- Do not begin a later asset-family slice while a failure in the current slice
  indicates an unresolved archive or ownership defect.
- A duplicate implementation may exist only inside its named migration stage
  and must be removed at that stage's acceptance gate.
- Migrate one complete vertical path—translator, build, codec, DDC, publication,
  Cook and runtime proof—rather than creating another repository-wide
  reader/writer split.
- Preserve public compatibility wrappers only for named consumers and record
  their removal checklist in the active stage.
- Worker purity and module closure are compile/link-testable properties, not
  comments or `DURIN_WITH_EDITOR` conventions.
- Update this plan's status and acceptance evidence in the same commit as each
  stage handoff.
- Any byte change, schema bump, new compatibility reader, or migration is an
  explicit decision recorded before implementation; it is never hidden inside
  a module move or archive rewrite.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Core archive primitives | Exact little-endian bytes, span behavior, counting/hash equivalence and structured failures | Core archive tests |
| Bounds and hostile input | Overflow, truncation, oversized collections, invalid offsets, overlap, padding and trailing data fail before unsafe access/allocation | Core and asset codec malformed-input tests |
| DObject archive compatibility | Object graph, duplicate, snapshot, reflected fields, authored PackageV4 and Cook bytes remain stable | CoreDObject and AssetCore tests |
| Codec symmetry | One serializer saves and loads each payload; save-load-save is deterministic | Engine payload codec tests |
| Version separation | Schema compatibility is Runtime-owned; builder/provider changes invalidate keys/fingerprints without false Runtime rejection | Engine, Build and Import version tests |
| Payload compatibility | TXPL, DMSH, DCOL, skeletal, animation and THPL golden bytes/read results remain stable | Engine codec golden tests |
| Build-key compatibility | Recipe field order, canonical floats/IDs, versions, targets and dependency hashes remain deterministic | EngineAssetBuild key tests |
| Translator boundary | Standard translators own concrete encoded formats and produce normalized values | StandardAssetImport tests and dependency closure |
| Worker isolation | Build workers operate on owned values and never access DObject/package/registry/render/RHI state | API compile tests, unit tests and thread assertions |
| DDC behavior | Namespaces, hit/miss/corrupt policy, atomic writes and best-effort write failures remain stable | EngineAssetBuild and AssetCore tests |
| Publication transaction | Decode/build/DDC/cancel/stale/publication failure preserves previous complete state | Asset-family failure matrices |
| Import workflows | Capture, plans, fingerprints, candidates, output order, reconciliation, repair and rollback remain stable | AssetImportCore and StandardAssetImport tests |
| Coordinator lifecycle | Admission, priorities, latest generation, cancellation, mailbox, waits, provider unload and shutdown are bounded | Build service and lifecycle tests |
| Cook and tools | Explicit build operations work headlessly; package-only inspection keeps minimal closures | Cook and DurinAssetTool process tests |
| Module selection | Authoring roots select Build/Import intentionally; game roots do not | DHT metadata and dependency-closure checks |
| Third-party closure | Offline compressors belong to Build; source libraries belong to StandardImport; none enter game | CMake link and deployment inspection |
| Cooked game | Source/DDC-free packages load, validate and create resources with no authoring modules deployed | Game runtime and renderer/Vulkan smoke |

## Definition of Done

- Core exposes one canonical, bounded byte archive; CoreDObject layers reflection
  and object reference semantics over it without duplicated primitive/archive
  implementations.
- Runtime Engine owns runtime values and the complete save/load/validation/
  version contract for every Engine payload format.
- EngineAssetBuild owns source-independent build requests/products, algorithms,
  recipe identity, DDC policy, coordination and authoring diagnostics, and has
  no concrete source decoder or payload wire implementation.
- StandardAssetImport owns standard concrete translators and import policy, and
  calls Build only with normalized owned values.
- AssetImportCore and AssetCore remain format-neutral and asset-family-neutral
  at their respective framework/storage boundaries.
- No build worker accesses mutable assets; complete products publish atomically
  on the main thread through narrow Engine and AssetCore seams.
- Payload schema, builder, compressor, projection and provider versions have
  distinct owners and invalidation effects.
- Runtime Engine and DurinGame have no transitive source translator, offline
  compressor, build recipe, DDC builder/coordinator or authoring lifecycle.
- Existing keys, payloads, authored packages, Cook products, import graphs,
  editor workflows and rollback behavior remain compatible unless an explicit
  qualified version transition says otherwise.
- Focused validation, final full native tests, editor/game full builds,
  deployment inspection, cooked runtime smoke, lasting documentation, plan
  validation and committed handoff are complete.

## Rejected Designs

- Writer in `EngineAssetBuild` and reader in Runtime `Engine` for one format.
- A codec-only module that depends on Engine runtime values and creates a cycle;
  if runtime value types are ever extracted, their codec moves with them.
- Treating payload serialization as an offline compressor merely because only
  authoring currently calls its save direction.
- Using the current native-representation `FMemoryWriter/FMemoryReader` as a
  persistent canonical codec without the archive foundation stage.
- Public `BuildFromEncodedBytes(DObject&, ...)` operations that combine source
  translation, worker build, storage and publication.
- Runtime decoder registries or dynamic discovery of authoring capabilities.
- Using builder versions as Runtime wire-compatibility gates.
- Horizontal migration stages that duplicate every writer before completing
  one asset family's end-to-end boundary.
- One physical module per asset family without a measured optional dependency,
  deployment, unload, ownership, or release-cadence requirement.

## Deferred Follow-ups

- Extract runtime asset value types and their codecs into a lower
  `EngineAssetRuntime`-style module only if a non-Engine runtime consumer proves
  the need; never extract codecs alone.
- Split texture, geometry, skeletal, animation or terrain builders only after a
  measured dependency, deployment, lifecycle or release-cadence requirement.
- Remote/shared DDC, distributed builds, build farms, build-worker processes and
  cross-machine scheduling protocols.
- Runtime source import, mod ingestion, runtime texture compression or hot-build
  deployment.
- New payload schemas, compression formats, platform variants, virtual
  textures, mesh optimization pipelines or incremental terrain-region builds.
- Generalizing EngineAssetBuild into a workspace-wide provider-agnostic build
  graph before a second non-Engine consumer proves the abstraction.

## Architectural References

The design follows these Unreal Engine precedents at the level of responsibility,
not module count or API compatibility:

- [FArchive](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FArchive?lang=en-US)
  is a Runtime Core bidirectional serialization foundation.
- [FTexturePlatformData](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FTexturePlatformData?lang=en-US)
  keeps cooked texture serialization with Runtime Engine platform data.
- [TextureCompressor](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/TextureCompressor)
  and [TextureFormat](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/TextureFormat)
  separate offline production from runtime serialization.
- [Interchange](https://dev.epicgames.com/documentation/unreal-engine/importing-assets-using-interchange-in-unreal-engine)
  separates source translation, pipelines and asset factories.

## Related Documentation

- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Runtime Variants](../Development/Build/RuntimeVariants.md)
- [Build System](../Development/Build/BuildSystem.md)
- [Native Tests](../Development/Build/NativeTests.md)
- [Code Modules](../Workspace/CodeModules.md)

## Related Code

- `CMakeLists.txt`
- `Engine/Engine.dproject`
- `Engine/Source/Runtime/Core/Public/Misc/DerivedDataCache.h`
- `Engine/Source/Runtime/CoreDObject/Public/DObject/Archive.h`
- `Engine/Source/Runtime/CoreDObject/Private/DObject/Archive.cpp`
- `Engine/Source/Runtime/AssetCore/AssetCore.dmodule`
- `Engine/Source/Runtime/AssetCore/Private/AssetPackageV4ArchiveAdapter.cpp`
- `Engine/Source/Runtime/Engine/Engine.dmodule`
- `Engine/Source/Runtime/Engine/CMakeLists.txt`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMeshDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMeshDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Public/SkeletalMesh/SkeletalDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/SkeletalMesh/SkeletalDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmapDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/Terrain/TerrainHeightmapDerivedData.cpp`
- `Engine/Source/Editor/EngineAssetBuild/EngineAssetBuild.dmodule`
- `Engine/Source/Editor/EngineAssetBuild/Public/Texture/TextureBuildOperations.h`
- `Engine/Source/Editor/EngineAssetBuild/Public/Texture/TextureDerivedDataWriter.h`
- `Engine/Source/Editor/EngineAssetBuild/Private/Texture/TextureDerivedDataWriter.cpp`
- `Engine/Source/Editor/AssetImportCore/AssetImportCore.dmodule`
- `Engine/Source/Editor/StandardAssetImport/StandardAssetImport.dmodule`
- `Engine/Source/Editor/StandardAssetImport/Private/StandardAssetImportProviders.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/SceneImport.cpp`
- `Engine/Source/Programs/DurinAssetTool/CMakeLists.txt`
- `Engine/Tests/Native/EngineTests/CMakeLists.txt`
- `Engine/Tests/Native/AssetCoreTests/CMakeLists.txt`
