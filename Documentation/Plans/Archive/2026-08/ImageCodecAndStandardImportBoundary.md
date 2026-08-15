# Image Codec and Standard Import Boundary Plan

Summary: Move source-image decoding into a Core-owned codec boundary, distinguish generic import framework APIs from built-in standard translators, consolidate source translation, and replace the historical Stage3 PostLoad grouping without changing asset or cooked-data semantics.

Last reviewed: 2026-08-14

Status: Archived
Completed: 2026-08-14

## Current Status

The migration is complete. Core owns the asset-independent image codec under
`Durin::Image`; AssetCore no longer compiles or exposes it. Generic framework
contracts remain under `Durin::Asset::Import`, while built-in provider,
translator, validation, and direct authoring APIs use
`Durin::Asset::Import::Standard`.

Texture2D, TextureCube, TerrainHeightmap, and Scene consume immutable encoded
source snapshots. Named translators own concrete image interpretation, provider
and uncooked-policy orchestration no longer decode image formats, and typed
Build modules remain encoded-source independent. `Stage3PostLoad` was replaced
by independently reversible TextureCube and Terrain authoring policies.

Package formats, TXPL/THPL bytes, DDC key recipes, provider and Import Record
identities, source orientation, cooked deployment, and runtime publication
semantics remain unchanged.

## Completion Evidence

- The pre-change baseline passed `AssetDecodeTests`, `TextureTests`,
  `TerrainHeightmapTests`, `AssetImportTests`, `TextureThumbnailTests`,
  `TextureCookIntegrationTests`, and `TerrainHeightmapCookTests`.
- Final focused validation passed `ImageCodecTests`, `TextureTests`,
  `TerrainHeightmapTests`, `AssetImportTests`, `SceneImportTests`,
  `TextureThumbnailTests`, and `ThumbnailTests`.
- Final broad and integration validation passed `test fast-all`, the exact
  AssetImport/SceneImport/TextureCook/TerrainCook targets, and the ordinary
  `test all` aggregate.
- Complete `Win64-Debug-DurinEditor` and `Win64-Debug-DurinGame` target graphs
  built successfully. The Game deployment contains no AssetImportCore,
  StandardAssetImport, TextureBuild, GeometryBuild, or Assimp binary.
- `STB_IMAGE_IMPLEMENTATION` has one production owner in
  `Core/Private/Image/ImageDecoder.cpp`. The retained pre-migration AssetCore
  object measured 1,237,766 bytes and the final Core object measured 1,237,590
  bytes, a 176-byte decrease for the relocated translation unit. Final Debug
  Core and AssetCore DLLs measure 5,721,088 and 5,463,552 bytes respectively;
  their declared module closure did not gain a dependency edge.
- Lasting contracts now live in Core image-codec, Editor import-framework,
  TextureCube, Texture2D, and TerrainHeightmap documentation.

## Goal

Establish one explicit dependency direction for encoded source images:

```text
encoded bytes
  -> Core / Durin::Image codec
  -> StandardAssetImport / Durin::Asset::Import::Standard translator
  -> normalized source value
  -> Asset::Build recipe and DDC
  -> Engine asset validation and publication
```

After completion, generic image decoding is independent of assets, concrete
built-in format interpretation is visibly distinct from the format-neutral
import framework, each asset family has one source translator, and uncooked
authoring policies carry durable asset-specific names.

## Scope

- Move the existing LDR, Radiance HDR, and exact grayscale16 PNG decoder from
  `AssetCore` into a low-dependency `Core/Image` boundary.
- Move decoder declarations and values from `Durin::Asset` to
  `Durin::Image` and remove AssetCore umbrella exposure.
- Preserve memory decoding as the authoritative import input while retaining
  only thin, asset-independent file convenience operations where they have
  non-import consumers.
- Keep format-neutral import framework contracts in
  `Durin::Asset::Import` and move concrete built-in provider/translator APIs
  to `Durin::Asset::Import::Standard`.
- Consolidate source snapshot, decode, normalized-value construction, build
  request composition, and caller-specific publication across Texture2D,
  TextureCube, and TerrainHeightmap paths.
- Replace `Stage3PostLoad.cpp/.h` and its stage-numbered registration API with
  asset-specific uncooked authoring policies.
- Reclassify image codec tests under Core and retain focused import, DDC,
  Cook, thumbnail, and runtime-deployment validation.
- Measure module closure and binary effects caused by moving stb_image into
  Core.

## Non-Goals

- Do not change decoded pixels, channel order, row orientation, color values,
  or hostile-input ceilings.
- Do not change Texture mip generation, compression, sRGB policy, format
  selection, TXPL serialization, or build-key identity.
- Do not change Terrain sample normalization, hierarchy construction,
  revision behavior, THPL serialization, or build-key identity.
- Do not move texture, terrain, mesh, Cook, DDC, package, DObject, or import
  transaction policy into Core.
- Do not rename the physical `StandardAssetImport` module.
- Do not introduce a generic codec registry, image processing framework,
  plugin ABI, or new `ImageCore` shared module before measured scope requires
  one.
- Do not add RAW Terrain support in the required stages. RAW remains a
  separately gated follow-up after the ownership migration is stable.
- Do not preserve the old namespace through permanent aliases or dual
  registrations.

## Design Decisions and Invariants

### Core owns bytes-to-pixels only

`Core/Image` accepts encoded bytes plus explicit decode limits and produces
owned decoded values. It must not mention `DTexture`, `DTerrainHeightmap`,
`FSourcePath`, import plans, packages, DDC, Cook, source provenance, or Build
products. Codec diagnostics describe encoded-image requirements rather than
asset-family policy.

The initial implementation remains in Core because the current decoder is a
single bounded implementation with no Engine, RHI, AssetCore, or editor
dependency. A separate `ImageCore` module is deferred until concrete needs
such as encoding, EXR, DDS/KTX, resampling, color management, or multiple
codec backends justify another runtime binary and dependency surface.

### Memory snapshots are authoritative for import

Standard import operations decode the same immutable byte snapshot used for
content hashing, file-size facts, fingerprints, and Build request composition.
No translator or Build operation reopens a source file. File convenience
decode APIs may remain for non-transactional thumbnail callers, but import,
reimport, repair, and uncooked PostLoad use captured memory.

### Codec capability is not import policy

Core may report that a technical extension or byte stream is decodable.
StandardAssetImport separately defines whether a source format is admitted for
a Texture2D, TextureCube, Scene surface, or TerrainHeightmap workflow. stb
support alone never makes a format an accepted asset source.

### Import framework and standard implementation remain distinct

`Durin::Asset::Import` owns format-neutral framework concepts supplied by
`AssetImportCore`: snapshots, plans, providers, candidates, diagnostics,
records, transactions, cancellation, ordering, and rollback.

`Durin::Asset::Import::Standard` owns APIs whose behavior is supplied by the
built-in `StandardAssetImport` module: concrete image/Assimp/glTF/FBX
translation, standard provider registration, source-family validation, and
direct built-in import/reimport operations. A future replaceable generic entry
point must first be expressed as an `AssetImportCore` interface instead of
presenting one standard implementation as framework policy.

### Build remains encoded-source independent

TextureBuild and GeometryBuild accept normalized owned values. They do not
receive PNG, JPEG, HDR, RAW, glTF, FBX, or Assimp bytes and do not depend on
Core image codecs or StandardAssetImport. DDC keys remain functions of their
existing canonical build inputs and source identities.

### Runtime and cooked contracts remain unchanged

Runtime Engine assets retain value serialization, validation, snapshot
lifetime, publication, and query behavior. Cooked Game targets do not load
StandardAssetImport, AssetImportCore, typed Build modules, Assimp, or offline
compression solely because authoring source support exists.

### Registration is asset-specific and reversible

Each uncooked authoring policy has its own idempotent register/unregister
surface and registration state. Aggregate Standard provider startup installs
policies in a declared order and rolls them back in strict reverse order after
any failure. Historical implementation-stage numbers are not durable symbol
or filename vocabulary.

### Compatibility-sensitive identities do not move

Provider IDs, registration keys, reflected class names, Import Record wire
identity, package formats, cooked payload formats, DDC key recipes, and source
orientation contracts remain unchanged. The namespace migration is a single
repository source migration with no forwarding aliases.

## Current Foundations and Gaps

### Foundations

- `ImageDecoder` is already a byte/value API and depends only on Core
  facilities, the standard library, and stb_image.
- Texture2D has a named source translator and a separate PostLoad policy.
- TextureBuild and GeometryBuild already consume normalized source-independent
  values.
- AssetImportCore already owns immutable source snapshots for provider-driven
  imports.
- Imported Scene implementation values already use
  `Durin::Asset::Import::Standard` in several paths.
- Cooked assets already load without authoring modules or source files.
- Focused decoder, import, Terrain, TextureCube, thumbnail, DDC, and Cook tests
  exist.

### Gaps

- `AssetCore/Public/ImageDecoder.h` exposes an asset-free codec under
  `Durin::Asset` and through the AssetCore umbrella.
- Decoder tests are registered as `AssetDecodeTests` in the `asset-import`
  domain even though the production boundary is generic.
- Editor preview and thumbnail modules consume the same codec for operations
  that are not asset-import transactions.
- Public StandardAssetImport translators and provider registration are partly
  flattened into the generic `Durin::Asset::Import` namespace.
- TextureCube and Terrain uncooked PostLoad paths directly decode source bytes
  instead of reusing their source translators.
- Terrain PNG decode, source hashing, Build request construction, and
  publication composition appear in direct import, provider candidate, and
  uncooked PostLoad paths.
- `Stage3PostLoad` exposes completed-plan provenance as if it were a lasting
  runtime phase.
- Codec extension capability and accepted asset-source policy are not named as
  separate decisions.

## Implementation Stages

### Stage 0: Freeze codec, import, and deployment baselines

Dependencies: none.

- [x] Inventory every production and test consumer of `ImageDecoder.h`, its
  decoded value types, extension predicates, memory operations, and file
  convenience operations.
- [x] Record the exact LDR RGBA8, Radiance HDR linear RGB float, and
  grayscale16 PNG output contracts, including channel metadata, transparency,
  dimensions, row orientation, and failure-state clearing.
- [x] Freeze accepted technical extensions and all encoded-size, pixel-count,
  dimension, malformed-input, truncation, and interlace limits in focused
  tests.
- [x] Confirm `STB_IMAGE_IMPLEMENTATION` has exactly one production owner and
  record the Core and AssetCore target/link closure before relocation.
- [x] Confirm Texture and Terrain DDC keys, TXPL/THPL bytes, Import Record
  identities, provider IDs, and Cooked Game deployment do not derive identity
  from the decoder's module, header path, C++ namespace, or diagnostics.
- [x] Record representative Core, Editor, and Cooked Game binary/dependency
  baselines so the relocation has an auditable cost comparison.

#### Acceptance Gate

- Existing decoder, TextureCube HDR, Terrain strict-PNG, import, DDC, Cook, and
  thumbnail tests pass without production changes.
- Baseline evidence identifies all compatibility-sensitive identities and the
  exact decoder ownership/deployment state.
- No unresolved ownership or binary-admission decision remains for Stage 1.

### Stage 1: Establish the Core image codec boundary

Dependencies: Stage 0 baseline.

- [x] Move `ImageDecoder.h/.cpp` to `Core/Public/Image` and
  `Core/Private/Image`, preserving one stb implementation translation unit.
- [x] Change exported declarations and decoded values from `Durin::Asset` and
  `ASSETCORE_API` to `Durin::Image` and `CORE_API`.
- [x] Update Core private include/link configuration for stb without exposing
  stb headers through the public API.
- [x] Remove `ImageDecoder.h` from the AssetCore umbrella and remove all
  AssetCore source/build ownership for the implementation.
- [x] Update every production and test consumer to include
  `Image/ImageDecoder.h` explicitly and use `Image::` types and operations.
- [x] Remove a module's direct AssetCore dependency only when the dependency
  audit proves image decoding was its sole AssetCore use; retain explicit Core
  dependencies rather than relying on transitive inclusion.
- [x] Rewrite codec diagnostics that say `heightmap` or otherwise prescribe an
  asset-family policy as generic image-format diagnostics, while preserving
  failure categories and limits.
- [x] Keep memory operations authoritative. Retain file convenience operations
  only as thin Core file-read wrappers used by non-import callers.
- [x] Move decoder cases out of AssetCoreTests into a cohesive
  `ImageCodecTests` contract target with `image-codec` domain and `core` module
  metadata.

#### Acceptance Gate

- Core builds and exports the decoder without AssetCore, Engine, RHI, Build, or
  editor dependencies.
- AssetCore no longer compiles, exports, or umbrella-includes image decoding.
- LDR, HDR, grayscale16, hostile-input, size-limit, and failure-state tests are
  bit-for-bit or value-for-value equivalent to the Stage 0 baseline.
- Production search contains no `Asset::DecodeImage*`,
  `Asset::DecodeRadiance*`, `Asset::DecodeGrayscale*`, or Asset-owned decoded
  image values.
- Core/Editor/Cooked Game dependency and binary changes are recorded and remain
  within an explicitly accepted bound; an unexpected material increase blocks
  Stage 2 and reopens the Core-versus-ImageCore decision.

### Stage 2: Separate framework and standard-import namespaces

Dependencies: Stage 1 codec boundary.

- [x] Classify every public StandardAssetImport declaration as either a
  format-neutral framework contract, a concrete standard-provider API, or an
  implementation detail.
- [x] Keep only AssetImportCore framework contracts in
  `Durin::Asset::Import`.
- [x] Move concrete Texture2D, TextureCube, TerrainHeightmap, StaticMesh,
  SkeletalMesh, AnimationClip, Scene translator, validation, direct import,
  reimport, and standard-provider registration declarations to
  `Durin::Asset::Import::Standard` where they remain public.
- [x] Move implementation-only helpers to StandardAssetImport private headers
  instead of exporting them for convenience.
- [x] Update all production, test, and generated/customization consumers in one
  source migration; do not add aliases, dual registrations, or old-namespace
  shims.
- [x] Preserve physical module names, API macros, provider IDs, record and
  reflected identities, diagnostics contracts, and serialized values.
- [x] Document the lasting framework/standard namespace rule in the owning
  Editor architecture documentation once the source migration is proven.

#### Acceptance Gate

- Public `Durin::Asset::Import` declarations are supplied by the generic
  framework or intentionally define a provider-neutral interface.
- Public concrete built-in translators and provider registration use
  `Durin::Asset::Import::Standard`.
- Repository source contains no retired `Durin::StandardAssetImport` or
  `Durin::AssetImport` roots and no compatibility aliases.
- Import Record round trips, provider lookup, candidate publication, rollback,
  and source-reference workflows remain unchanged.

### Stage 3: Consolidate source snapshots and typed translators

Dependencies: Stage 2 namespace ownership.

- [x] Define or reuse one immutable encoded-source snapshot carrying the exact
  bytes, content hash, file size, last-write fingerprint when applicable, and
  mounted source path needed by authoring workflows.
- [x] Make source capture the only file-I/O boundary for import, reimport,
  repair, provider candidate, and uncooked PostLoad paths.
- [x] Ensure hash, decode, diagnostics, and Build request composition consume
  the same captured bytes and never reopen the source during one operation.
- [x] Make `Standard::TranslateTexture2DSource` the only Texture2D encoded-image
  interpretation path.
- [x] Make TextureCube source translation own the only six-face LDR,
  panorama-LDR, and panorama-HDR interpretation paths; reuse it from direct
  import, provider candidates, reimport, validation, and uncooked PostLoad.
- [x] Add one `Standard::TranslateTerrainHeightmapSource` operation that
  produces owned width, height, and exact row-major `uint16` samples without
  building or publishing an asset.
- [x] Reuse the Terrain translator from direct import, source-reference change,
  reimport, provider candidate, repair, and uncooked PostLoad.
- [x] Centralize typed composition from normalized Terrain source value plus
  captured source identity into `BuildTerrainHeightmap` without hiding
  caller-specific revision, dirty-state, rollback, or publication policy.
- [x] Keep Scene surface-image translation routed through its typed standard
  translator rather than through provider orchestration.
- [x] Remove direct image decode and duplicate source hashing from provider and
  PostLoad orchestration files.

#### Acceptance Gate

- StandardAssetImport direct calls to `Image::Decode*` are confined to named
  source translator implementations and intentional non-asset preview code.
- `StandardAssetImportProviders.cpp`, uncooked PostLoad policies, Build modules,
  Runtime Engine assets, and Renderer code do not interpret encoded image
  formats.
- Every import/reimport/PostLoad path hashes and decodes one immutable byte
  snapshot and preserves its previous failure and rollback semantics.
- Texture2D, TextureCube, and Terrain cold-DDC rebuilds produce the same
  canonical products and DDC identities as the Stage 0 baseline.

### Stage 4: Replace Stage3 PostLoad with asset-specific policies

Dependencies: Stage 3 translator consolidation.

- [x] Remove `Stage3PostLoad.cpp/.h`,
  `RegisterStage3PostLoadPolicies`, and
  `UnregisterStage3PostLoadPolicies`.
- [x] Add `TextureCubePostLoadPolicy.cpp/.h` for TextureCube uncooked DDC load,
  source rebuild through the shared translator, registration, and teardown.
- [x] Add `TerrainHeightmapAuthoringPolicy.cpp/.h` for Terrain uncooked DDC
  load, source rebuild through the shared translator, source-reference change
  registration, and teardown.
- [x] Give each policy independent idempotent registration state and complete
  partial-failure rollback.
- [x] Make aggregate Standard provider startup install the asset-specific
  policies in a declared order and uninstall them in strict reverse order.
- [x] Keep Runtime Engine PostLoad handler seams limited to validation and
  dispatch; do not return source decoding or Build policy to Runtime Engine.
- [x] Remove historical stage-number terminology from production filenames,
  symbols, errors, and comments.

#### Acceptance Gate

- Production source contains no `Stage3PostLoad` symbol, include, or filename.
- TextureCube and Terrain policy registration is idempotent, reversible, and
  leaves no callback installed after module teardown or failed startup.
- Warm-DDC, cold-DDC, missing-source, corrupt-DDC, reimport, source-change, and
  Cooked-runtime behavior remains equivalent.

### Stage 5: Narrow the StandardAssetImport public surface

Dependencies: Stage 4 durable policy boundaries.

- [x] Audit every `StandardAssetImport/Public` declaration for a real
  cross-module consumer and move provider-only candidate/build composition to
  private headers.
- [x] Keep public direct import/reimport operations only where editor modules
  intentionally use them as supported authoring entry points.
- [x] Split codec capability predicates from asset-source admission predicates;
  name and test Texture, TextureCube, Scene, and Terrain source policies
  independently.
- [x] Remove exports introduced only to let tests bypass the production
  boundary; use supported APIs or a narrowly justified module-owned test seam.
- [x] Audit direct module dependencies after the public-surface reduction and
  remove only proven redundant edges.
- [x] Publish lasting codec, import namespace, translator, and uncooked-policy
  ownership rules in Core and Editor architecture documentation.

#### Acceptance Gate

- Every exported StandardAssetImport declaration has at least one intentional
  external consumer or is a documented supported authoring API.
- A technically decodable extension is not automatically admitted by any
  asset-family importer.
- Module closure retains the required Runtime/Developer/Editor separation and
  Cooked Game deployment remains authoring-free.

### Stage 6: Qualify the complete boundary migration

Dependencies: Stages 1-5.

- [x] Run focused Core image codec contract validation.
- [x] Run Texture2D, TextureCube, TerrainHeightmap, Scene surface-image,
  thumbnail, source relocation, import transaction, DDC, and Cook validation.
- [x] Run the repository's broad non-integration validation and the exact
  affected integration targets.
- [x] Run the complete ordinary native-test aggregate because Core ownership
  and native-test registration changed.
- [x] Build the complete Editor and Game target graph and verify deployment
  closure for both authoring and Cooked runtime profiles.
- [x] Compare final binary sizes and dependency closures with Stage 0 and
  explain any material change.
- [x] Move lasting contracts from this plan into the owning Core image codec,
  Editor import architecture, and Terrain/Texture documents before marking the
  plan complete.

#### Acceptance Gate

- All focused, broad, integration, aggregate, Editor, and Game validation
  selected by the repository build/test guidance passes.
- Decoded outputs, DDC keys, TXPL/THPL payloads, import records, provider IDs,
  Cooked package contents, and runtime deployment match their frozen contracts.
- No Build or Runtime Engine module interprets encoded source images.
- No completed-stage vocabulary remains in production ownership names.
- Lasting documentation owns all implemented invariants and this plan contains
  final evidence rather than being the sole contract source.

## Validation Matrix

| Area | Required evidence | Primary ownership |
| --- | --- | --- |
| LDR codec | RGBA8 bytes, source-channel facts, transparency, corrupt/truncated/oversized rejection | Core image codec tests |
| HDR codec | Old/new Radiance scanlines, linear RGB floats, orientation, malformed and limit rejection | Core image codec tests |
| Grayscale16 PNG | Exact unsigned samples, non-square orientation, strict IHDR/interlace/channel/bit-depth rejection | Core image codec and Terrain tests |
| Texture2D | Import, reimport, source change, PostLoad, async build, build settings, thumbnail decode | Texture and import tests |
| TextureCube | Six-face and LDR/HDR panorama validation, import/reimport, DDC hit/miss, Cook | TextureCube and Cook tests |
| TerrainHeightmap | Direct/provider import, no-op/changed reimport, revision, hierarchy, DDC hit/miss, corruption, Cooked source-free load | Terrain and Cook tests |
| Scene images | Embedded/external image translation and material surface output | Scene import tests |
| Framework | Snapshot, provider lease, candidate, diagnostics, records, rollback, cancellation, publication ordering | AssetImportCore and import integration tests |
| Policies | Register/unregister idempotence, startup rollback, module teardown, missing source and corrupt DDC | StandardAssetImport policy tests |
| Editor consumers | TextureEditor, LevelEditor, DurinEd source/asset thumbnail compilation and focused behavior | Editor/thumbnail tests |
| Deployment | Standard import and Build modules present only in authoring profiles; Cooked Game remains source-decoder-policy free | Module closure and deployment checks |
| Binary cost | Core, Editor, and Cooked Game binary/dependency comparison against Stage 0 | Build evidence |

Validation execution follows [Agent Testing Workflow](../../../Agents/Testing.md),
[Native Tests](../../../Development/Build/NativeTests.md), and
[Agent Build and Run Workflow](../../../Agents/BuildAndRun.md). Use the smallest
affected named targets during implementation; the Stage 6 gate requires the
complete ordinary aggregate because the plan changes shared Core ownership and
native-test registration.

## Definition of Done

- `ImageDecoder` is Core-owned, asset-independent, exported through
  `Durin::Image`, and absent from AssetCore.
- Core image codec tests own all format and hostile-input contracts.
- Generic import framework APIs use `Durin::Asset::Import`; concrete built-in
  translator/provider APIs use `Durin::Asset::Import::Standard`.
- Texture2D, TextureCube, TerrainHeightmap, and Scene source families each have
  one encoded-source translation authority.
- Import, reimport, repair, provider candidate, and uncooked PostLoad decode one
  immutable source snapshot rather than reopening source files.
- Provider and PostLoad orchestration do not decode image formats directly.
- `Stage3PostLoad` and all corresponding stage-numbered symbols are removed.
- Asset-specific policy registration is independently reversible and fully
  torn down on failure or module unload.
- DDC, Cooked payload, package, provider, reflection, source orientation, and
  runtime deployment contracts are unchanged.
- Focused, broad, integration, aggregate, Editor, and Game validation passes,
  and binary/dependency changes are accepted.
- Lasting ownership rules are published outside the active plan.

## Deferred Follow-ups

### Explicit RAW Terrain source support

RAW support is considered only after this plan is complete. Its own bounded
plan must select an initial format rather than guessing metadata. The expected
minimum contract is explicit width, height, sample representation, byte order,
row origin, row stride policy, and exact file-size validation. Import settings
must persist for deterministic reimport and participate in source/build
identity wherever interpretation changes canonical samples.

Terrain RAW admission belongs to `Asset::Import::Standard`. Generic byte-order
unpacking moves to `Durin::Image` only after a second non-Terrain consumer
proves that it is a shared codec capability. Ordinary Texture import must not
automatically admit `.raw` merely because Terrain supports it.

### Independent ImageCore module

Reconsider a separate `ImageCore` module only when measured Core binary cost or
new capabilities such as encoding, EXR, DDS/KTX, resampling, color management,
or multiple decoder backends make the current Core boundary materially too
broad. The decision must be based on dependency and deployment evidence, not
namespace symmetry alone.

### Codec/provider extensibility

A plugin codec registry or replaceable standard-provider ABI remains deferred
until at least one real external provider needs runtime selection, lifetime,
versioning, diagnostics, and failure-isolation contracts.

## Related Documentation

- [Code Modules](../../../Workspace/CodeModules.md)
- [Engine Asset Build Boundary Plan](EngineAssetBuildBoundary.md)
- [Engine Module Simplification Plan](EngineModuleSimplification.md)
- [Terrain Heightmap Asset](../../../Runtime/Terrain/TerrainHeightmapAsset.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)
- [Native Tests](../../../Development/Build/NativeTests.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Image/ImageDecoder.h`
- `Engine/Source/Runtime/Core/Private/Image/ImageDecoder.cpp`
- `Engine/Source/Runtime/AssetCore/Public/AssetCore.h`
- `Engine/Source/Runtime/Core/CMakeLists.txt`
- `Engine/Source/Editor/AssetImportCore`
- `Engine/Source/Editor/StandardAssetImport/StandardAssetImport.dmodule`
- `Engine/Source/Editor/StandardAssetImport/Public/Texture2DSourceTranslation.h`
- `Engine/Source/Editor/StandardAssetImport/Public/TextureCubeSourceTranslation.h`
- `Engine/Source/Editor/StandardAssetImport/Public/TerrainHeightmapSourceTranslation.h`
- `Engine/Source/Editor/StandardAssetImport/Public/StaticMeshSourceTranslation.h`
- `Engine/Source/Editor/StandardAssetImport/Public/SceneImport.h`
- `Engine/Source/Editor/StandardAssetImport/Public/StandardAssetImportProviders.h`
- `Engine/Source/Editor/StandardAssetImport/Private/StandardAssetImportProviders.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/Texture2DSourceTranslation.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/TextureCubeSourceTranslation.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/TerrainHeightmapSourceTranslation.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/Texture2DPostLoad.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/TextureCubePostLoadPolicy.cpp`
- `Engine/Source/Editor/StandardAssetImport/Private/TerrainHeightmapAuthoringPolicy.cpp`
- `Engine/Source/Developer/TextureBuild`
- `Engine/Source/Developer/GeometryBuild`
- `Engine/Tests/Native/CoreTests/Private/ImageDecoderTests.cpp`
- `Engine/Tests/Native/CoreTests/CMakeLists.txt`
