# Engine Asset Serialization and Build Boundary Plan

Summary: Standardize authored assets, build keys, DDC values, and cooked data on UE-style type-owned Serialize(FArchive&) while isolating source translation and derived builds.

Last reviewed: 2026-08-13

Status: Active
Completed:

## Current Status

This plan supersedes the previous stage decomposition in this file. The former
design assigned payload writers to `EngineAssetBuild` while retaining payload
readers in Runtime `Engine`. That direction is rejected: a stable binary format
has one semantic owner, and its save, load, validation, version negotiation,
limits, stable identifiers, alignment, and checksum rules must evolve together.
This revision also rejects direction-named `Encode*` and `Decode*` functions as
the final serialization API. Authored assets, runtime/platform values, build-key
inputs, DDC values, cooked descriptors, and cooked bulk data all participate in
one `Serialize(FArchive&)` protocol owned by the type whose state is serialized.

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
  build behavior; skeletal, animation, and terrain key/build/serialization
  boundaries have not been normalized.
- The current `FArchive` abstraction has the right bidirectional direction and
  structured failure model, but its primitive memory archives use native byte
  representation and lack the bounded, canonical binary facilities needed to
  replace the asset-specific wire readers and writers safely.

No further writer-only extraction or new direction-named payload API is allowed.
The next implementation work is the archive foundation and a complete
Texture2D vertical slice.

## Goal

Establish one structural dependency boundary for Engine assets:

- Every persistent Engine value enters the byte layer through a type-owned
  `Serialize(FArchive&)`, a UE-style `Serialize(FArchive&, Owner/Context)` when
  owner context is genuinely required, or an equivalent free `Serialize`/
  `operator<<` customization for non-owning value types.
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
| Value serialization | Type-owned `Serialize(FArchive&)` for authored, DDC, cooked, TXPL, DMSH, DCOL, skeletal, animation, or THPL state | The module that owns the serialized value type |
| Build identity | Build-key input `Serialize` through a canonical hashing/key archive | `EngineAssetBuild` |
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

- `EngineAssetBuild` depends on `Engine` and calls Engine-owned value
  `Serialize` operations.
- `StandardAssetImport` depends on `AssetImportCore` and `EngineAssetBuild`.
- `Engine` never includes, loads, registers, or discovers an authoring module.
- `AssetImportCore` never depends on an Engine asset family or a concrete source
  provider.
- `DurinGame` never reaches `EngineAssetBuild`, `AssetImportCore`,
  `StandardAssetImport`, Assimp, image translators, or offline compressors.

## Module Ownership

### Core

`Core` owns the reusable byte-archive substrate:

- archive direction, persistence, cooking, editor-only filtering, bulk-data
  policy, purpose/capabilities, canonical byte order, byte swapping, position,
  bounded regions, custom/format versions, first-failure state, and raw byte
  transfer;
- canonical memory/span readers and writers;
- counting and hashing archives required for deterministic sizes, offsets, and
  checksums;
- bounded string, byte-buffer, sequence, alignment, and padding helpers that do
  not depend on reflection or asset types.

The generic substrate must not know `DObject`, packages, DDC namespaces, or an
Engine payload schema.

As in UE, `IsLoading`, `IsSaving`, `IsPersistent`, `IsCooking`,
`IsFilterEditorOnly`, bulk-data policy, target information, and custom versions
are archive context queried by serializers. `EArchivePurpose` refines that
context for Durin; it does not replace these orthogonal facts.

### CoreDObject

`CoreDObject` layers object semantics over the Core archive substrate:

- reflected logical type and field descriptors;
- object/field/path scopes and canonical reflected map behavior;
- hard and soft `DObject` references;
- object graph, duplicate, property snapshot, and authored-package adapters.

The object-aware layer is the Durin equivalent of UE's archive specialization
for UObjects: ordinary `DObject` types still override one
`Serialize(FArchive&)`, while object reference behavior is supplied by the
active archive rather than by asset-specific readers and writers.

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

AssetCore package descriptors, manifests, bulk references, cache records, and
other persistent generic values use their own `Serialize` implementations.
DDC values are immutable opaque buffers identified by key/value identity,
content hash, and size; the store never calls an Engine-type serializer itself.

It does not own texture/mesh/skeletal/animation/terrain schema fields, build
versions, or build decisions.

### Runtime Engine

`Engine` owns each runtime asset's data and complete payload contract:

- reflected asset schema needed to load authored and cooked packages;
- runtime/platform value types, stable serialized identifiers, payload IDs,
  schema versions, limits, layout rules, and semantic validation;
- owning `DObject`-derived asset `Serialize(FArchive&)` overrides for authored
  and cooked package fields, plus type-owned
  `Serialize(FArchive&, Owner/Context)` operations for runtime/platform data,
  chunk tables, mip/LOD records, and bulk descriptors;
- purpose-specific `SerializeCooked`-style helpers only where a cooked layout
  materially differs, always entered through the same Archive protocol and
  never split by save versus load direction;
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
writer. It serializes Build key inputs through a canonical key/hash archive and
serializes completed runtime/platform values by calling their Engine-owned
`Serialize` implementations.

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

## Unified Serialize Contract

### One protocol, multiple owning types

Every persistent boundary uses `Serialize`; there is no separate family of
writer and reader APIs:

| Boundary | Owning serialization entry | Archive behavior |
| --- | --- | --- |
| Authored asset/package | Owning `DObject`-derived asset `Serialize(FArchive&)` and reflected value `Serialize` operations | Persistent object archive with object references and editor fields |
| Cooked asset/package | The same asset `Serialize`, queried through `Ar.IsCooking()` / editor-only filtering, plus serializable descriptors | Persistent Cook archive |
| DDC build key | Build-owned key input `Serialize` through a canonical key/hash archive | Save-only, deterministic and path/timestamp independent |
| DDC value | Runtime/platform value `Serialize` into an immutable value buffer | Canonical persistent memory archive |
| Cooked bulk payload | The same runtime/platform value `Serialize`, or its `SerializeCooked` helper when the Cook layout is intentionally different | Persistent bounded bulk archive |
| DDC/cache record | AssetCore record/metadata `Serialize`; Engine value remains opaque bytes | Asset-family-neutral storage archive |

The preferred shape follows UE runtime value types:

```cpp
struct FTexturePlatformData
{
    auto Serialize(FArchive& Ar, DTexture* Owner) -> void;
    auto SerializeCooked(FArchive& Ar, DTexture* Owner) -> void;
};

class DTexture2D : public DTexture
{
    auto Serialize(FArchive& Ar) -> void override;
};

struct FTexture2DBuildKeyInput
{
    auto Serialize(FArchive& Ar) -> void;
};
```

An optional owner or format context is allowed when the value cannot interpret
itself without stable external facts, matching UE's platform/render-data
serializers. It must not duplicate direction, persistence, Cook state, target,
or version data already carried by the Archive. Free `Serialize` or
`operator<<` is preferred when a type cannot or should not own a member.

`SerializeCooked` is a semantic layout helper, not a save-direction API. If DDC
and cooked bytes have the same schema, both routes call the exact same
`Serialize`. If Cook repackages the value for streaming or bulk placement, the
outer Cook serializer may differ while inner mip/LOD/chunk values continue to
use their shared `Serialize` operations.

Direction is always selected by the Archive:

```cpp
FCanonicalMemoryWriter DdcAr(OutputBytes, EArchivePurpose::DerivedDataPayload);
Product.PlatformData.Serialize(DdcAr, OwnerContext);

FCanonicalMemoryReader CookedAr(InputBytes, EArchivePurpose::CookedPayload);
Candidate.PlatformData.Serialize(CookedAr, OwnerContext);
```

Loading serializes into a detached candidate. Archive failure, version failure,
or semantic validation failure discards that candidate; only a fully validated
value may be published.

### Archive context instead of API proliferation

Serializers query Archive state rather than selecting an `Encode`, `Decode`,
`SaveDdc`, or `LoadCooked` function. Stage 1 adds the relevant UE-style queries
to Durin's existing purpose/capability model:

- `IsLoading()` and `IsSaving()` for direction;
- `IsPersistent()` for transient-field policy;
- `IsCooking()` and Cook target/profile for platform selection;
- `IsFilterEditorOnly()` for authored-versus-runtime fields;
- bulk-data skip/inline policy for package and streaming layouts;
- custom/format versions for schema evolution;
- `GetPurpose()` for Durin-specific authored package, DDC key/value, cooked
  package/payload, duplication, discovery, and snapshot behavior.

Serializers should normally stream fields without branching. Direction checks
are reserved for allocation, post-load validation, migration, and ownership
differences; Cook/purpose checks are reserved for genuine schema or field-set
differences. One giant asset serializer must not absorb Build, DDC lookup,
source translation, or publication behavior merely because all values use
`FArchive`.

### One owner and one field order

Each serialized value type has one implementation of field order, stable IDs,
versions, offsets, limits, alignment, padding, checksums, and semantic
validation. Direction-named compatibility wrappers may exist only during the
asset family's migration stage and must immediately delegate to the value's
`Serialize`; they are removed at that stage's acceptance gate.

Offset tables, chunk directories, checksums, and bulk regions are serializable
value types rather than anonymous writer/reader code. The archive layer must
support them without unbounded allocation or native-layout serialization.

### Version ownership

Versions are separated by the behavior they invalidate:

| Version | Owner | Effect |
| --- | --- | --- |
| Payload schema/custom version | Runtime `Engine` serialized value | Determines field/layout compatibility during `Serialize` |
| Stable serialized ID revision | Runtime `Engine` serialized value | Defines persistent enum/type identities |
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
- Loading serializers reject overlap, non-canonical ordering, non-zero reserved
  fields or padding, incompatible IDs, checksum mismatch, trailing bytes where
  forbidden, and incomplete semantic structures.
- Saving serializers apply the same limits and stable identifiers as loading.
- Generic archive helpers do not silently serialize container capacity,
  pointers, padding, ABI layout, paths, timestamps, or unordered iteration.
- A `Serialize` change that alters bytes requires an explicit schema decision,
  golden update, compatibility reader or migration story, and Cook/runtime
  qualification. A module move alone never changes bytes.

## Build and Publication Contract

An asset-family build follows this shape:

```text
captured source bytes
  -> StandardAssetImport translator
  -> normalized immutable EngineAssetBuild request
  -> pure/cancellable detached build product
  -> Build key input Serialize -> canonical key/hash archive
  -> platform/runtime value Serialize -> immutable DDC value buffer
  -> EngineAssetBuild DDC policy/store
  -> prepared main-thread publication
  -> AssetCore transaction commit
```

Cook later serializes the same accepted runtime/platform value through a Cook
Archive; runtime load executes that value's `Serialize` in the load direction.

Requests include every normalized source identity, setting, target/profile,
builder dependency, and version required for a stable key. Physical paths and
timestamps are optimization or diagnostic facts, never key identity.

Worker tasks cannot access `DObject`. Main-thread publication may call a narrow
Engine-owned exchange that atomically swaps a complete candidate. Cancellation,
translation failure, build failure, serialization failure, DDC corruption/write
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
| Archive | Bidirectional direction, purpose, capability, versions, paths, failures, memory archives | Separate generic Core byte layer from DObject semantics; add UE-style persistence/Cook/filter/bulk context, canonical endian, span, bounds, alignment, counting and hashing |
| Texture2D builder | Active candidate/test path uses the exported build module algorithms | Replace encoded-byte/mutable-asset API, route asset/key/platform/DDC/Cooked state through type-owned `Serialize`, migrate coordinator and remove Engine builder copy |
| TextureCube builder | Panorama/six-face projection and provider candidate paths use build operations | Move source decoding to StandardAssetImport, unify TXPL value `Serialize` and Build key `Serialize`, remove legacy Engine seams |
| Terrain | Provider candidate and direct import can reach build operations | Introduce normalized samples request/product, unify THPL value `Serialize`, move key `Serialize` and authoring policy |
| StaticMesh | Characterized DMSH/DCOL, detached render candidate and exchange mechanisms exist | Route normalized geometry through Build; make runtime values own `Serialize`; remove decoder registration and asset self-build APIs |
| Skeletal/animation | Stable keys, payloads, relationship validation and exchanges exist | Split translation/build/serialization/version ownership and detach worker paths |
| Import | Framework transactions and standard providers are established | Translators must emit normalized Build requests rather than encoded-byte calls or Runtime decoder registration |
| DDC | Generic object storage and asset-specific characterization exist | Keep storage generic and opaque; move recipe/key/policy to Build while Engine runtime/platform values own payload `Serialize` |
| Tests | Golden keys/hashes, corruption, Cook, runtime and rollback coverage exist | Add Serialize routing/symmetry/context/version/bounds tests and vertical-slice dependency gates |

The earlier Stage 0 characterization is retained as evidence. Its former rule
that private payload writers follow build operations is explicitly replaced by
the `Serialize`-follows-value-owner rule in this plan.

## Stage 0 Boundary Inventory

This inventory freezes the migration state immediately before Core archive
ownership changes. A row describes semantic ownership, not the module that
happens to contain the transitional implementation today.

| Asset family/boundary | Source translation | Derived build | Value serialization | Build identity | Storage/publication |
| --- | --- | --- | --- | --- | --- |
| Texture2D | PNG/JPEG and related image decoding moves from `AssetCore`/Build call sites to `StandardAssetImport` | `EngineAssetBuild` owns mip generation, format selection and BC compression | Engine-owned texture platform data gains `Serialize(FArchive&, DTexture2D*)`; TXPL save/load field order moves here | Build-owned `FTexture2DBuildKeyInput::Serialize` | `AssetCore` keeps opaque DDC bytes and transactions; Engine exposes detached publication |
| TextureCube | LDR/HDR panorama and six-face decoding belongs to `StandardAssetImport` | `EngineAssetBuild` owns projection, exposure, mips, format selection and compression | Engine-owned cube platform data gains `Serialize(FArchive&, DTextureCube*)` | Build-owned `FTextureCubeBuildKeyInput::Serialize` | Generic storage remains in `AssetCore`; Engine validates and publishes complete candidates |
| TerrainHeightmap | Grayscale16 PNG decoding belongs to `StandardAssetImport` | `EngineAssetBuild` owns normalized-sample hierarchy construction | Engine-owned terrain payload gains `Serialize(FArchive&, DTerrainHeightmap*)` | Build-owned `FTerrainHeightmapBuildKeyInput::Serialize` | Generic DDC/Cook storage and transactional publication remain outside workers |
| StaticMesh/collision | Assimp/glTF/FBX translation belongs to `StandardAssetImport` | `EngineAssetBuild` owns render/collision conversion and diagnostics | Engine-owned render data, collision data and directory records own `Serialize` | Build-owned mesh and collision key inputs each own `Serialize` | `AssetCore` stores opaque values; Engine candidate exchange is the publication seam |
| SkeletalMesh | glTF/Assimp translation and scene reconciliation belong to `StandardAssetImport` | `EngineAssetBuild` owns palette, hierarchy and render-data preparation | Engine-owned skeletal platform/render data owns `Serialize` | Build-owned skeletal key input owns `Serialize` | Existing graph validation and atomic candidate exchange remain Engine/AssetCore seams |
| AnimationClip | glTF/Assimp channel translation belongs to `StandardAssetImport` | `EngineAssetBuild` owns clip preparation | Engine-owned animation payload owns `Serialize` | Build-owned animation key input owns `Serialize` | Existing skeleton-reference validation and transactional exchange remain intact |
| Environment lighting | Bake/import program owns normalized input production | Authoring bake/build tooling owns production | `FEnvironmentLightingData::Serialize` in Engine replaces payload direction wrappers | The producing tool owns any recipe identity | Cook/package storage remains generic |
| Authored objects and packages | Not applicable | Not applicable | `DObject::Serialize` and CoreDObject object-aware adapters; AssetCore package/manifest values serialize themselves | Not applicable | `AssetCore` owns package, Cook, DDC-object and atomic publication policy |

### Transitional seams and removal gates

| Transitional implementation/API | Named consumers at the baseline | Removal stage | Compatibility evidence |
| --- | --- | --- | --- |
| Engine and Build copies of `BuildTexture2DDerivedDataKey*` and `EncodeTexture2DPayload` | `DTexture2D`, Texture2D coordinator, Standard import candidates, texture tests | Stage 2 | `FTextureDerivedDataTests`, `FTexture2DTests`, `FTextureCookTests` |
| `DecodeTexture2DPayload` in Engine | `DTexture2D` DDC/Cook loading | Stage 2 after an immediate Serialize-delegating wrapper | Deterministic TXPL round trip, malformed transactional rejection and cooked package tests |
| `BuildTexture2DFromEncodedBytes(DTexture2D&, ...)` plus Build/Engine `DecodeRGBA8` | Standard single-asset provider, coordinator and legacy asset rebuild paths | Stage 2 | Import/cache, failure, coordinator and single-asset rollback tests |
| `FTexture2DBuildCoordinator` and `EngineAssetServices` hosted by Runtime Engine | editor frame pump, waits, save/reimport and shutdown | Stage 2, with common hosting consolidated in Stage 6 | `FTexture2DBuildCoordinatorTests` and async transaction tests |
| Engine and Build copies of cube key/TXPL writers plus Engine cube reader | `DTextureCube`, Standard cube provider and direct import/reimport paths | Stage 3 | cube key/payload, panorama, six-face, rollback and Cook tests |
| `BuildTextureCube*FromEncodedBytes` and Runtime/Build LDR/HDR decoders | Standard cube provider and legacy direct cube operations | Stage 3 | `FEquirectangularTextureCubeTests` and `FTextureCubeTests` |
| Engine terrain key/THPL encode/decode plus `BuildTerrainHeightmapFromEncodedBytes` and `DTerrainHeightmap::BuildFromEncodedBytes` | Standard provider and direct terrain import/rebuild | Stage 3 | terrain key/payload corruption, import rollback, warm-DDC and cooked-runtime tests |
| Engine `BuildStaticMesh*DerivedDataKey*`, DMSH/DCOL encode/decode and source-decoder registry | `DStaticMesh`, Standard scene provider, repair/reimport and Cook paths | Stage 4 | static-mesh contract, collision Stage 0, cache, transaction and Cook tests |
| Broad `DStaticMesh::InitializeFromImportedData`/`BuildRenderData*`/collision build methods | scene import, direct import, repair and asset lifecycle | Stage 4 | `FStaticMeshTests` import/reimport/collision failure matrices |
| Engine skeletal/animation key builders and payload encode/decode | skeletal scene import, authored reload, Cook and runtime load | Stage 5 | `FSkeletalAssetTests` exact codecs, malformed input, package, duplication and source/DDC-free Cook |
| Broad skeletal/animation `InitializeFromImportedData`, rebuild and DDC methods | Standard scene provider and Runtime asset lifecycle | Stage 5 | skeletal graph replacement, write-failure and scene rollback tests |
| Runtime authoring initialize/pump/wait/shutdown entry points | editor host, asset editors, Cook and `DurinAssetTool` | Stage 6 | coordinator/provider unload and lifecycle tests |
| `AssetCore` image decoder and generic/package `Encode*`/`Decode*` wire helpers | Standard translators, editor source thumbnails, package/Cook readers and writers | Stage 1 for canonical archive reuse; Stage 3/7 for translator relocation and final wrapper removal | image hostile-input tests, PackageV4 independent-reference goldens and Cook manifest/bulk tests |
| `EncodeEnvironmentLightingPayload`/`DecodeEnvironmentLightingPayload` | environment bake program and Engine runtime load | Stage 7 | `FEnvironmentLightingTests` golden deterministic/corrupt-input coverage |

Direction words that describe source-format translation (`DecodeDataUri`,
Assimp/glTF parsing), color conversion (`EncodeSRGB`), or non-wire mathematical
packing (`EncodeMaterialSamplerState`) are not persistent-value customization
points and therefore are not renamed merely for containing `Encode`/`Decode`.

### Executable dependency baseline

Configure-time closure assertions now encode the allowed direction:

- `AssetCore` and `AssetImportCore` exclude Engine asset types, concrete source
  translators, authoring builders, Assimp and offline compressors.
- Runtime `Engine` excludes import frameworks, `EngineAssetBuild`,
  `StandardAssetImport` and Assimp. Its temporary direct BC compressor link is
  the named Stage 2 exception and is deliberately not hidden by the assertion.
- `EngineAssetBuild` requires Runtime Engine and the BC compressor while
  excluding import frameworks, `StandardAssetImport` and Assimp.
- `StandardAssetImport` requires `AssetImportCore`, `EngineAssetBuild` and
  Assimp, making it the concrete translator root.
- `DurinGame` excludes authoring frameworks/hosts, builders/offline
  compressors, concrete translators and their SDKs. Engine serialized values
  and AssetCore generic storage remain intentionally allowed.

### Frozen baseline evidence

The `Win64-Debug-DurinEditor` profile reproduced the pre-refactor baseline on
2026-08-12. These target-level gates are the compatibility oracle for Stage 1:

| Boundary | Target and frozen behavior |
| --- | --- |
| Object graph, duplication, archive failure | `CoreObjectTests` (73 passed) and `CorePropertyValueSnapshotTests` (16 passed) |
| Authored PackageV4 and Cook package bytes | `AssetPackageTests` (97 passed) and `AssetCookTests` (12 passed) |
| Generic DDC object bytes | `AssetDerivedDataTests` (3 passed) |
| Texture2D/TextureCube keys, TXPL, corruption, transactions and Cook | `TextureTests` (74 passed, 2 skipped) plus `TextureCookIntegrationTests` |
| DMSH/DCOL keys, payloads, corruption and transactions | `StaticMeshTests` (69 passed) |
| Skeletal and animation keys/payloads/packages/Cook | `SkeletalAssetTests` (34 passed) |
| THPL keys/payloads/import transactions | `TerrainHeightmapTests` (6 passed) plus `TerrainHeightmapCookTests` |

The initial `CoreDObjectTests` command was a selection-name error before test
execution; the registry-owned targets above are the successful replacement and
no build recovery was required.

## Implementation Stages

### Stage 0: Rebaseline the rejected split

Dependencies: none.

- [x] Record that writer-in-Build/reader-in-Engine is not a permitted final
  architecture.
- [x] Preserve the previously committed module, build algorithm, golden byte,
  failure, lifecycle, and dependency characterization work.
- [x] Reclassify the remaining public and private symbols for every asset family
  using the vocabulary and module ownership in this plan.
- [x] Record all duplicate implementations and compatibility seams, including
  their named consumers and the exact stage that removes them.
- [x] Add explicit dependency checks that distinguish source translators,
  offline compressors, serialized runtime values, build keys, and generic
  storage.
- [x] Map every direction-named `Encode*`/`Decode*` entry to its final owning
  value's `Serialize`, and name the migration stage that removes the wrapper.
- [x] Confirm the pre-archive-refactor byte baselines for object graphs,
  authored packages, DDC keys, TXPL, DMSH/DCOL, skeletal, animation, THPL, and
  cooked packages.
- [x] Record the revised Stage 0 handoff before changing `FArchive` ownership.

#### Acceptance Gate

- Every operation is classified as source translation, derived build, value
  serialization, build identity, storage, or publication, with exactly one
  final module owner and serialization entry.
- Every duplicate and migration seam has a bounded removal stage and a named
  compatibility test.
- Existing byte, failure, transaction, lifecycle, and dependency baselines are
  reproducible before the archive change begins.

### Stage 1: Establish the canonical FArchive foundation

Dependencies: revised Stage 0 inventory and byte baselines.

- [x] Extract archive direction, persistence, Cook state/target, editor-only
  filtering, bulk-data policy, purpose/capabilities, canonical byte order,
  byte swapping, raw `Serialize(void*, size)` transfer, position, bounded
  regions, custom/format versions, and failure state into a generic `Core`
  archive layer.
- [x] Retain reflected field/object/path scopes, DObject reference behavior, and
  object/package adapters in a CoreDObject object-aware archive layer on top of
  Core, following UE's `FArchive`/object-archive separation.
- [x] Define and qualify the repository customization convention: member
  `Serialize(FArchive&)`, UE-style `Serialize(FArchive&, Owner/Context)`, or free
  `Serialize`/`operator<<`; direction-named save/load APIs are not a final
  customization point.
- [x] Add canonical little-endian span reader/writer, counting archive, hashing
  archive, alignment/padding helpers, bounded byte/string/sequence helpers, and
  const-correct save versus mutable load byte APIs.
- [x] Add authored package, DDC key/value, cooked package/payload, and bulk-data
  purposes plus UE-style `IsPersistent`, `IsCooking`, `IsFilterEditorOnly`, and
  bulk policy queries without coupling Core to Engine asset families.
- [x] Migrate or adapt `DerivedDataCache::FWriter/FReader` so canonical primitive
  behavior has one implementation while preserving every existing cache byte.
- [x] Preserve source compatibility with forwarding headers or aliases only
  while named consumers migrate; do not retain two archive implementations.
- [x] Qualify primitive endian bytes, span truncation, bounds, overflow,
  alignment, padding, counting/hash equivalence, version lookup, and structured
  failure paths.
- [x] Requalify DObject object-graph, property snapshot, duplication, authored
  package, PackageV4, and Cook bytes before completing the stage.

#### Acceptance Gate

- Core owns one canonical byte archive independent of `DObject` and assets.
- All persistent types share one documented `Serialize` customization protocol;
  Archive state, not a direction-named function, selects load/save/Cook behavior.
- CoreDObject behavior and all frozen object/package bytes remain compatible.
- No persistent writer relies on native ABI representation or unbounded
  container allocation.
- The archive facilities required by asset, key, DDC-value, Cook and bulk-data
  serializers are public, documented, and qualified before any payload value is
  rewritten.

Stage 1 handoff (2026-08-12): Core owns the only primitive/canonical byte
implementation in `Serialization/Archive.h`; CoreDObject supplies the
object-aware derived layer and forwarding include. `DerivedDataCache` delegates
its frozen wire primitives to Core. `CoreUtilityTests`, `CoreFileSystemTests`,
`CoreObjectTests`, `CorePropertyValueSnapshotTests`, `AssetPackageTests`,
`AssetCookTests`, `AssetDerivedDataTests`, `TextureTests`,
`TextureCookIntegrationTests`, `StaticMeshTests`, `SkeletalAssetTests`,
`TerrainHeightmapTests`, and `TerrainHeightmapCookTests` reproduce the Stage 0
bytes and lifecycle behavior. A complete `Win64-Debug-DurinEditor` `all` build
also qualifies the cross-module public Archive export move.

### Stage 2: Complete the Texture2D vertical slice

Dependencies: canonical archive foundation.

- [ ] Split the mixed Texture derived-data header into Engine-owned serialized
  runtime/platform value declarations and Build-owned recipe/key declarations.
- [x] Make `DTexture2D::Serialize(FArchive&)` own authored/cooked object fields
  and make `FTexturePlatformData::Serialize(FArchive&, Owner/Context)` own the
  TXPL field order, records, bulk bytes, limits, checksums, and both directions.
- [x] Route DDC and Cook through that platform-data `Serialize`; add a
  `SerializeCooked` helper only if the intentionally different Cook layout
  requires it.
- [x] Retain `EncodeTexture2DPayload`/`DecodeTexture2DPayload` only as immediate
  delegating migration wrappers, then delete both in this stage.
- [x] Preserve exact TXPL bytes, strict corrupt-input diagnostics, platform
  value construction, and source/DDC-free cooked loads.
- [x] Remove builder version as a Runtime payload compatibility gate while
  preserving current version compatibility and DDC invalidation behavior.
- [x] Replace `BuildTexture2DFromEncodedBytes(DTexture2D&, ...)` with a normalized
  decoded-image request and detached product; move concrete image translation
  to `StandardAssetImport`.
- [ ] Make `FTexture2DBuildKeyInput::Serialize` the sole canonical recipe field
  order and keep builder/compressor versions, DDC namespace and policy solely in
  `EngineAssetBuild`; serialize valid products through Engine value types.
- [ ] Migrate the Texture2D coordinator, diagnostics, waits and lifecycle to the
  build module without allowing worker access to assets.
- [x] Add a main-thread publication adapter using the narrow Engine-owned
  detached-state seam and existing AssetCore transaction semantics.
- [ ] Delete `TextureDerivedDataWriter`, the legacy Engine Texture2D builder,
  self-build/forwarding methods, and redundant tests after consumer proof.
- [ ] Remove the Engine BC link once no Engine Texture2D caller requires it.

#### Acceptance Gate

- Texture2D asset state, build-key input, DDC value and Cooked value all enter
  the byte layer through their owning `Serialize` operations; no public
  direction-named payload API, duplicate writer, or duplicate builder remains.
- Worker build paths accept no encoded source format and touch no `DObject`.
- Golden keys/payloads, corruption behavior, DDC policy, import/reimport,
  cancellation, latest-wins, rollback, Cook, runtime load and rendering pass.
- Engine no longer reaches BC compression because of Texture2D.

Stage 2 serialization progress (2026-08-13): `DTexture2D` explicitly owns its
object `Serialize`, and `FTexturePlatformData::Serialize` is the sole owner of
the Texture2D TXPL header, records, bulk bytes, bounds, checksum and validation
in both directions. DDC, Cook, the coordinator and Build publication paths call
that value serializer directly; the public Texture2D `Encode*`/`Decode*` APIs
and the duplicated Build writer are removed. Runtime compatibility is now
gated by the payload schema and stable identifiers rather than the producer's
builder version. `FTexture2DDerivedDataKeyInput::Serialize` also supplies the
single current Texture2D key field order while its declaration awaits the
remaining Build ownership move. `TextureTests` preserves all golden keys and
payload hashes, malformed-input transactions and builder-version compatibility;
`TextureCookIntegrationTests` preserves source/DDC-free cooked loading.

Stage 2 normalized-build progress (2026-08-13): `EngineAssetBuild` now exports
an owned, source-format-neutral `FTexture2DBuildRequest` and detached
`FTexture2DBuildProduct`; its worker entry accepts decoded RGBA8 state and a
captured content identity, never encoded bytes or a `DObject`. Standard single-
asset and Scene import paths translate encoded image snapshots in
`StandardAssetImport` before invoking that worker. Publication is a separate
game-thread adapter and enters Runtime Engine through
`DTexture2D::PublishImportedState`, which validates a complete detached state
before mutating the object; existing candidate exchange and package save
transactions remain outside the worker. Direct Texture2D file import, source
mounting and destination policy now also live in `StandardAssetImport`, and all
editor/test callers use that owner; `EngineAssetBuild` no longer exports or
implements an encoded-byte Texture2D entry. Runtime self-build/coordinator paths
and the shared cube-era Build decoder remain, so the legacy-removal items are
still active.

Stage 2 coordinator normalization progress (2026-08-13): the existing bounded
coordinator request now owns `FTextureSourceData` plus its captured content hash
instead of encoded source bytes. Source translation completes before admission;
workers begin with a `Preparing` phase, validate normalized ownership at submit,
and perform only mip/compression, key, persistence and completion work. Admission
budgets account for decoded pixels, and cancellation/latest-wins/lifecycle tests
retain their prior behavior. The coordinator implementation, object-facing
submission methods and lifecycle host still reside in Runtime Engine pending the
remaining ownership migration.

Stage 2 coordinator ownership staging (2026-08-13): a Build-exported
`FTexture2DBuildCoordinator` now exists in `EngineAssetBuild` with distinct
queued request/result contracts, normalized input, bounded admission,
cancellation, diagnostics, completion mailbox, waits and shutdown. The
`EngineAssetBuild` module owns initialization/draining, and `MainFrame` pumps
its mailbox on editor frames. Focused tests exercise the Build DLL's global
lifecycle and worker path. The Runtime coordinator remains temporarily for
unmigrated `DTexture2D` methods and Engine lifecycle consumers; it must be
deleted, not retained as a forwarding layer, before this stage is accepted.

The Build-owned coordinator delegates mip generation, key construction, DDC
serialization/write/cleanup and cancellation to the same `BuildTexture2D`
recipe used by synchronous import. Its queue adds scheduling metadata and phase
reporting only; it no longer carries a second Texture2D builder/key/persistence
implementation.

Stage 2 object-state migration progress (2026-08-13):
`FTexture2DAuthoringService` now owns per-object weak identity, generation,
active/last request, latest-wins cancellation, waits, failure outcome and
diagnostic lookup outside `DTexture2D`. Completed Build products publish only on
the game thread through the Engine detached-state seam. `StandardAssetImport`
prepares current mounted source bytes and provenance for reimport/settings
rebuilds, and TextureEditor uses the Build service for pending status,
cancellation, waits and reimport. Standard authoring entry points now also own
Texture2D usage, color-space, resolution, compression and alpha-setting rebuild
policy; direct editor/render/import tests submit through those entry points and
wait on Build-owned diagnostics instead of calling Runtime self-build methods.
DurinEd now exposes an editor-only property-policy extension point, and
StandardAssetImport uses it to validate, submit and publish Texture2D reflected
setting proposals through the Build coordinator. Property transactions,
Undo/Redo, cancellation and supersession no longer use object-owned pending
products or the Runtime coordinator. Runtime direct methods and load/source
lifecycle callers remain to be migrated before the duplicate coordinator and
the now-bypassed legacy property implementation can be removed.

Texture2D source-reference, external-ingest, repair and private-copy policies
now live in `StandardAssetImport` and submit normalized builds through the
Build authoring service. TextureEditor, Standard repair providers and direct
source-policy tests no longer invoke those Runtime self-build methods. The
generic DurinEd mounted-source relocation adapter remains on the legacy method
until its asset-family callback is externalized without creating a module cycle.

That relocation callback is now externalized: DurinEd owns a format-neutral
asset-family handler registry, while StandardAssetImport registers the
Texture2D handler and waits on Build-owned diagnostics. DurinEd no longer
includes Texture2D or calls its self-build/wait methods for source relocation.

Stage 2 coordinator-test migration progress (2026-08-13): all bounded
admission, starvation, shutdown cancellation, explicit wait, completion budget,
metrics and characterization tests now instantiate or pump the Build-owned
coordinator and its queued request/result contracts. The Runtime coordinator no
longer has unique behavioral coverage; remaining tests exercise it only
indirectly through unmigrated Runtime load/source lifecycle methods.

Stage 2 native-test registration progress (2026-08-13): the focused
`TextureTests` target now uses structured feature metadata and owns only texture
import, build, cache, failure, coordinator and cube contracts. The slower
cross-family `FSceneImportTests` suite is preserved in the separately selectable
structured `SceneImportTests` integration target, so routine Texture2D changes
do not repeatedly pay for skeletal and scene publication/rollback coverage.

Runtime no longer initializes, pumps or shuts down a Texture2D authoring worker
through `EngineAssetServices`. The obsolete Launch lifecycle-smoke option and
its documentation were removed with that host contract; Build module lifecycle
and completion-budget tests now provide the corresponding authoring proof.

The Runtime `DTexture2D` public self-build and source-authoring surface is now
removed: setting rebuilds, source decode/fingerprint mutation, synchronous
platform rebuilds, reimport, reference changes, ingestion, repair and private
source relocation are available only through editor authoring policy. Runtime
retains only the temporary uncooked `PostLoad` compatibility path and its
pending-build bridge; those are removed with the remaining Runtime coordinator,
legacy builder and Build-owned key/DDC load migration below.

Uncooked Texture2D `PostLoad` policy now crosses a single Engine registration
seam: `StandardAssetImport` owns mounted-source availability, fingerprint
reconciliation, translation and background-rebuild decisions, while
`EngineAssetBuild` owns current recipe-key construction and DDC value loading.
Engine accepts only detached cached values, diagnostics and source fingerprints
through narrow publication methods; cooked loading remains Runtime-owned. The
old Runtime key/DDC helpers remain only for Cook validation until key-input and
Cook policy migration removes their final callers, and the old object pending
bridge remains only until its coordinator implementation is deleted.

### Stage 3: Complete TextureCube and TerrainHeightmap slices

Dependencies: completed Texture2D pattern and no unresolved archive defect.

- [ ] Apply the Texture2D value-Serialize/build-key split to TextureCube while
  sharing only genuinely common Texture serialized structures and limits.
- [ ] Move LDR/HDR and six-face source translation fully to
  `StandardAssetImport`; pass normalized owned face/panorama values to Build.
- [ ] Keep projection, exposure policy, mip generation, format selection and
  compression in Build; keep TextureCube asset/platform `Serialize` in Engine
  and key-input `Serialize` in Build.
- [ ] Remove the duplicate TextureCube writer, legacy Engine builder, broad
  encoded-source operations, and obsolete publication forwarding.
- [ ] Introduce a normalized height-sample Terrain request/product, move concrete
  image interpretation to StandardAssetImport, and retain hierarchy/build/key/
  DDC policy in Build.
- [ ] Make the Terrain runtime payload value own one `Serialize(FArchive&)` for
  THPL DDC/Cooked directions and make the Terrain key input own its canonical
  Build `Serialize`; retain runtime query/publication behavior in Engine.
- [ ] Remove Terrain asset self-build/source decode/key/write logic after direct
  import, reimport, repair, Cook and editor callers use the new operations.

#### Acceptance Gate

- TextureCube and Terrain asset/platform/key/DDC/Cooked values all route through
  owning `Serialize` operations, with one Build recipe, one normalized
  translator handoff, and no duplicate or encoded-byte build API.
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
- [ ] Make StaticMesh render/collision data, chunk directories, and records own
  Engine-resident `Serialize(FArchive&, Owner/Context)` operations for DMSH/DCOL
  DDC and Cooked layouts, bounds, and checksums; make Build key inputs own their
  canonical `Serialize`.
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
- DMSH/DCOL runtime values and Build key inputs have one owning `Serialize`
  implementation each and preserve golden bytes, strict malformed chunk
  behavior, collision, Cook/load and render-resource results.
- Single and Scene import/reimport preserve slot/material reconciliation,
  fingerprints, output order, repair and rollback.

### Stage 5: Complete SkeletalMesh and AnimationClip slices

Dependencies: StaticMesh chunked Serialize and Scene import pattern.

- [ ] Define normalized Build requests for skeleton relationships, skeletal
  geometry/influences and animation tracks without exposing provider-specific
  Scene objects or mutable assets to workers.
- [ ] Keep glTF/Assimp/source policy and normalized Scene reconciliation in
  StandardAssetImport.
- [ ] Move skeletal/animation preparation, key recipes, builder versions, DDC
  decisions, fingerprints and authoring diagnostics to EngineAssetBuild.
- [ ] Make skeletal render/payload values and animation clip values own
  Engine-resident `Serialize(FArchive&, Owner/Context)` operations for DDC and
  Cooked layouts, and make their Build key inputs own canonical `Serialize`,
  while preserving strict relationship and skeleton compatibility validation.
- [ ] Preserve runtime payload/value types, skeleton references, Cook
  descriptors, sampling, render-resource creation and imported-state exchanges
  in Engine.
- [ ] Remove asset-owned rebuild/DDC/source operations and remaining complete
  imported intermediates after all single/Scene/Cook callers migrate.

#### Acceptance Gate

- SkeletalMesh and AnimationClip asset/platform/key/DDC/Cooked values all route
  through owning `Serialize` operations and one Build recipe, with translator,
  worker, and publication boundaries mechanically testable.
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
- [ ] Remove all direction-named payload readers/writers, duplicated field-order
  implementations, and transitional archive compatibility layers after
  repository-wide consumer proof.
- [ ] Reduce Engine public headers to runtime/authored schema, type-owned
  `Serialize` contracts, payload access, detached publication seams,
  consumption status and render resources.
- [ ] Inspect module binaries, third-party links and deployed files for editor,
  game, Cook, tools and focused tests.
- [ ] Run focused Archive, CoreDObject, AssetCore, Engine asset, builder, import,
  Cook, editor, tool, renderer/Vulkan and runtime-process tests.
- [ ] Because the change crosses Core serialization and multiple native-test
  targets, run the final full native-test gate at default target granularity
  after focused diagnosis.
- [ ] Complete clean full `all` builds for the selected editor and game Agent
  Build Profiles and run source/DDC-free cooked game smoke validation.
- [ ] Move lasting Archive/Serialize, build, import, DDC-value, Cook, lifecycle
  and runtime-independence rules into owning documentation, then complete this
  plan.

#### Acceptance Gate

- Every authored asset, runtime/platform payload, Build key input, DDC value and
  Cooked value enters bytes through its owning `Serialize`, and every build
  family has exactly one EngineAssetBuild recipe/key path.
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
- Migrate one complete vertical path—translator, build, owning `Serialize`, DDC,
  publication, Cook and runtime proof—rather than creating another
  repository-wide reader/writer split.
- Preserve public compatibility wrappers only for named consumers and record
  their removal checklist in the active stage; wrappers delegate immediately to
  `Serialize` and cannot own field order or validation.
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
| Bounds and hostile input | Overflow, truncation, oversized collections, invalid offsets, overlap, padding and trailing data fail before unsafe access/allocation | Core and serialized-value malformed-input tests |
| DObject archive compatibility | Object graph, duplicate, snapshot, reflected fields, authored PackageV4 and Cook bytes remain stable | CoreDObject and AssetCore tests |
| Serialize protocol | Member/free `Serialize` and object-aware archives compose without direction-named APIs | Core, CoreDObject and API compile tests |
| Archive context routing | Loading/saving, persistent, Cook target, editor filtering, bulk policy, purpose and custom versions select the intended fields/layout | CoreDObject, AssetCore and Engine serialization tests |
| Serialization symmetry | One value `Serialize` saves and loads each payload; save-load-save is deterministic | Engine serialized-value tests |
| Version separation | Schema compatibility is Runtime-owned; builder/provider changes invalidate keys/fingerprints without false Runtime rejection | Engine, Build and Import version tests |
| Asset/DDC/Cook routing | Asset metadata uses object `Serialize`; DDC and Cook use the owning platform value `Serialize` or an explicit Cook-layout helper | Asset, DDC and Cook integration tests |
| Payload compatibility | TXPL, DMSH, DCOL, skeletal, animation and THPL golden bytes/read results remain stable | Engine Serialize golden tests |
| Build-key compatibility | Key-input `Serialize` preserves recipe field order, canonical floats/IDs, versions, targets and dependency hashes | EngineAssetBuild key tests |
| DDC value model | Build outputs are immutable identified buffers; AssetCore storage never interprets Engine value types | EngineAssetBuild, AssetCore and dependency tests |
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
- Every authored asset, generic package/cache record, Build key input,
  runtime/platform value, DDC value and Cooked value uses the common
  `Serialize(FArchive&)` customization protocol owned by its data type.
- Runtime Engine owns runtime values and their complete field order, DDC/Cooked
  serialization, validation and schema/custom-version contract, with no final
  direction-named payload API.
- EngineAssetBuild owns source-independent build requests/products, algorithms,
  recipe identity, DDC policy, coordination and authoring diagnostics, and has
  no concrete source decoder or payload wire implementation.
- StandardAssetImport owns standard concrete translators and import policy, and
  calls Build only with normalized owned values.
- AssetImportCore and AssetCore remain format-neutral and asset-family-neutral
  at their respective framework/storage boundaries.
- No build worker accesses mutable assets; complete products publish atomically
  on the main thread through narrow Engine and AssetCore seams.
- Archive schema/custom, builder, compressor, projection and provider versions have
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
- Direction-named `Encode*`/`Decode*`, `SaveDdc*`/`LoadDdc*`, or
  `WriteCooked*`/`ReadCooked*` as the final API for a value that can own one
  `Serialize` implementation.
- A serialization-only module that depends on Engine runtime values and creates
  a cycle; if runtime value types are ever extracted, their `Serialize` moves
  with them.
- Treating payload serialization as an offline compressor merely because only
  authoring currently calls its save direction.
- Using the current native-representation `FMemoryWriter/FMemoryReader` as a
  persistent canonical serializer without the archive foundation stage.
- Public `BuildFromEncodedBytes(DObject&, ...)` operations that combine source
  translation, worker build, storage and publication.
- One giant asset-object `Serialize` that performs source translation, Build,
  DDC lookup/write, publication, or render-resource creation.
- Separate DDC and Cooked serializers for semantically identical platform data;
  an explicit `SerializeCooked` helper is justified only by a real layout or
  streaming difference.
- DDC storage that includes Engine headers or interprets asset-family values
  instead of storing immutable opaque buffers.
- Forcing PNG/HDR/FBX/glTF/Assimp source decoding through `FArchive`; translators
  remain source-format APIs and only their normalized outputs join Build.
- Runtime decoder registries or dynamic discovery of authoring capabilities.
- Using builder versions as Runtime wire-compatibility gates.
- Horizontal migration stages that duplicate every writer before completing
  one asset family's end-to-end boundary.
- One physical module per asset family without a measured optional dependency,
  deployment, unload, ownership, or release-cadence requirement.

## Deferred Follow-ups

- Extract runtime asset value types and their serialization into a lower
  `EngineAssetRuntime`-style module only if a non-Engine runtime consumer proves
  the need; never extract `Serialize` separately from its values.
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
  is a Runtime Core byte-order-neutral bidirectional foundation carrying
  loading/saving, persistent, Cook, editor-filter, bulk, target, bounds and
  custom-version context.
- [UObject::Serialize](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/UObject/Serialize)
  uses one `Serialize(FArchive&)` for reading, writing and reference collection;
  [FArchiveUObject](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/CoreUObject/FArchiveUObject)
  layers object reference behavior over Core `FArchive`.
- [FTexturePlatformData](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/FTexturePlatformData?lang=en-US)
  owns `Serialize` and `SerializeCooked` with its Runtime Engine platform data;
  [FStaticMeshRenderData::Serialize](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FStaticMeshRenderData/Serialize?application_version=5.5)
  follows the same owner/context pattern for mesh render data.
- [TextureCompressor](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/TextureCompressor)
  and [TextureFormat](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Developer/TextureFormat)
  separate offline production from runtime serialization.
- [DerivedDataCache FValue](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/FValue)
  models a DDC value as an identified compressed buffer, while
  [FBuildOutput](https://dev.epicgames.com/documentation/unreal-engine/API/Developer/DerivedDataCache/FBuildOutput)
  is an immutable collection of values, messages and logs; Durin adopts the
  opaque-value boundary without copying UE's complete DDC system.
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
