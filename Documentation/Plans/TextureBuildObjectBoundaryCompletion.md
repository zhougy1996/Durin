# Texture Build Object Boundary Completion Plan

Summary: Complete the Engine-owned Texture object boundary by reducing TextureBuild to typed value-only recipes and providers across Texture2D, VolumeTexture, and TextureCube.

Last reviewed: 2026-09-02

Status: Active
Completed:

## Current Status

The completed UE-style asset-compilation ownership migration established the
correct asynchronous Texture2D shape:

- Engine owns the Texture2D compilation domain, object handles, request
  serials, priority and memory admission, cancellation, completion pumping,
  GameThread publication, and terminal callback delivery.
- TextureBuild implements a synchronous `ITexture2DBuildProvider`, owns the
  Texture2D recipe and DDC interaction, and returns Engine-owned values under a
  module callback gate.
- Runtime/game configurations can retain Engine's Texture2D domain without
  linking TextureBuild or DerivedDataCache.
- Material and Texture2D share lifecycle conventions without a typeless
  compiler state object or a generic asset-status enum.

The remaining module boundary is inconsistent outside that asynchronous path:

- `FTexture2DBuildProduct` round-trips canonical source data, authored build
  settings, source hashes, and derived platform data through TextureBuild even
  though only the platform payload, DDC identity, origin, and recipe
  diagnostics are derived outputs.
- TextureBuild still exports synchronous `Build*Into`, object-derived key/load,
  and `Publish*Product` operations that accept `DTexture2D`, `DVolumeTexture`,
  or `DTextureCube`.
- TextureBuild registers object-aware uncooked PostLoad features for all three
  texture families. Those callbacks select cache/build behavior and mutate
  Engine assets directly.
- Texture2D authored-setting validation and default-sRGB policy are duplicated
  between Engine and TextureBuild, forcing editor policy code to depend on the
  recipe module.
- `SourceContentHash` currently names both encoded physical-source provenance
  and canonical imported-data identity. Scene Texture2D import reads the hash
  returned in the build product as source-file provenance even though the
  provider derives it from normalized pixels.
- TextureEditor and AssetForgeBuiltins retain compile-time TextureBuild
  dependencies because they call recipe or object-publication APIs directly.

This plan completes that boundary. It does not reopen the already-qualified
decision to keep family-specific compilation state typed.

## Goal

Make Engine the only owner of live Texture objects, authored Texture state,
publication policy, synchronous and asynchronous object orchestration,
uncooked PostLoad behavior, and render-resource invalidation. Make TextureBuild
own only normalized texture transformations, producer identity, DDC keys and
sessions, codecs, Build Function registration, and typed synchronous
value-only provider implementations.

At completion:

- no TextureBuild API accepts or mutates `DTexture2D`, `DVolumeTexture`, or
  `DTextureCube`;
- provider results contain derived outputs only and cannot replace Engine's
  frozen authored input;
- physical-source provenance and canonical imported-data/build identity have
  distinct names, owners, and tests;
- Engine performs uncooked PostLoad and object publication through the same
  typed providers used by authoring workflows;
- TextureEditor and AssetForgeBuiltins do not link TextureBuild merely to
  validate settings, invoke recipes, or publish Texture objects;
- Runtime/game configurations still operate without TextureBuild and fail
  authored-only build requests with a bounded provider-unavailable diagnostic;
- TextureCube and VolumeTexture remain synchronous until concrete workload and
  lifecycle evidence justifies separate compilation domains.

## Scope

- Tighten the Engine-owned Texture2D provider input, output, identity, and
  publication contracts.
- Separate encoded source provenance from canonical imported-data and DDC
  identity.
- Make Engine authoritative for Texture2D authored-setting validation and
  default value policy.
- Move Texture2D synchronous build/publication and uncooked PostLoad
  orchestration into Engine.
- Introduce typed value-only provider boundaries for VolumeTexture and
  TextureCube without adding asynchronous schedulers.
- Move VolumeTexture and TextureCube publication and uncooked PostLoad
  orchestration into Engine.
- Remove obsolete object-aware TextureBuild APIs and PostLoad feature
  registrations.
- Narrow TextureBuild public headers and editor module dependencies after all
  production callers migrate.
- Preserve existing DDC keys and payload compatibility unless an identity bug
  requires an explicitly documented producer-version change.
- Update the lasting asset-data, texture-system, cube-texture, module, and
  runtime-lifecycle contracts after implementation is qualified.

## Non-Goals

- Creating a generic asset-build scheduler, typeless request/result envelope,
  shared compiler state bag, or composite asset-status enum.
- Merging Material, Texture, Mesh, Terrain, or shader compilation managers.
- Adding TextureCube or VolumeTexture asynchronous compilation domains,
  priorities, progress UI, or cancellation policy without a separate workload
  plan.
- Moving DDC APIs, texture codecs, compression, mip generation, panorama
  projection, or Build Functions into Runtime Engine.
- Splitting TextureBuild into one module per Texture family while all families
  retain the same dependency and startup transaction.
- Changing supported source formats, visible import workflows, asset package
  schema, cooked texture payloads, render-resource readiness semantics, or
  editor transaction behavior.
- Generalizing the typed Texture providers into a plugin-facing public SDK.
- Coupling physical source-file availability to rebuild authority; canonical
  imported data remains the authored rebuild input.

## Selected Responsibility Split

| Layer | Owns | Must not own |
| --- | --- | --- |
| Engine | Texture asset classes, authored settings and validation, immutable build contracts, object handles and request serials, synchronous/asynchronous orchestration, uncooked PostLoad, GameThread publication, derived-data diagnostics, CPU/GPU readiness transitions | DDC sessions, build-function registration, compression, mip generation, panorama projection |
| TextureBuild | Texture recipes, codecs, producer versions, DDC key construction, `FBuildSession` use, build-function registration, typed synchronous provider implementations | Managed-object identity, package dirtying, PostLoad policy, object mutation, publication, editor transactions |
| AssetForgeBuiltins | Encoded source capture, source decoding and normalization, physical-source provenance, import/reimport transaction sequencing, save and rollback | DDC identity, Texture object publication, recipe implementation |
| TextureEditor | Property proposal UI, editor transaction integration, diagnostics and cancellation requests | Recipe validation policy, TextureBuild calls, direct derived-data publication |
| DerivedDataCache | Family-neutral build definitions, cache query/store, value validation and Build Function invocation | Texture interpretation, object scheduling or publication |

## Design Decisions and Invariants

### Engine retains authoritative inputs

Provider input is a borrowed immutable value for the duration of one
synchronous invocation. Engine retains the canonical source data, authored
settings, target, publication context, and request serial until the invocation
finishes and any asynchronous completion reaches the GameThread.

A Texture2D provider result contains only derived state: platform data,
derived-data key, cache/build origin, persistence diagnostics, provider
descriptor, and recipe metrics. It does not echo source pixels, authored
settings, defaulted policy, or source provenance. VolumeTexture and TextureCube
adopt the same rule with family-specific product types.

Only Engine combines its retained input with a successful derived product and
attempts object publication. A provider therefore cannot change the authored
input that Engine publishes, even if a provider implementation is incorrect.

### Identity dimensions remain explicit

The following identities are separate:

- encoded physical-source content hash, owned by the import/reimport workflow
  and stored in `DAssetImportData` provenance;
- canonical imported-data identity, computed from normalized Engine source
  values and used as build/DDC input;
- provider producer identity and schema version, captured from the provider
  entered for the synchronous call;
- derived-data key, owned by TextureBuild's recipe and DDC contract;
- per-object request serial, owned by Engine and used only for latest-wins
  publication;
- CPU platform-data revision and GPU render-resource readiness, which remain
  independent after publication.

Callers do not supply an ambiguous `SourceContentHash` to the build contract.
Engine computes canonical imported-data identity from the normalized source.
The editor keeps encoded source provenance outside the build product and
captures it explicitly in completion/commit state when needed.

Provider identity does not become an object generation. A queued request may
legitimately execute against the single provider admitted when its worker
enters the feature gate. The result records that provider descriptor and DDC
key; the object handle, request serial, retained input snapshot, and current
manager admission remain the publication freshness checks.

### Engine owns every object-aware operation

Synchronous creation, undo/redo rebuild, uncooked PostLoad, asynchronous
reimport, and Scene materialization may use different transaction and waiting
policies, but all reach the same Engine-owned publication functions. No
TextureBuild callback receives a `DObject` or decides package dirtying, load
mutation reporting, source-decoder provenance, save policy, or render-resource
invalidation.

Uncooked PostLoad invokes the appropriate typed provider synchronously. The
provider performs its normal DDC query and local-build fallback and reports
whether the result was a hit or rebuild. Engine translates that origin into
the existing family-specific derived-data diagnostics before publication.
Provider absence remains a valid Runtime/game outcome for authored-only work;
cooked payload loading does not invoke a provider.

### Providers remain typed by Texture family

Texture2D, VolumeTexture, and TextureCube use separate request/product and
provider interfaces because their source values, build settings, derived
payloads, and publication invariants differ. They may share small Engine-owned
invocation helpers and module-gate conventions, but they do not share opaque
payloads or a typeless provider base beyond `IModularFeature`.

TextureCube provider input explicitly distinguishes normalized six-face and
panorama inputs. Panorama projection remains a TextureBuild recipe even though
the normalized request and detached product types are Engine-owned.

### Module lifetime remains gate-bound

TextureBuild publishes provider registrations only after the complete
TextureBuild Build Function transaction is ready. Retirement closes new
provider admission and waits for admitted synchronous calls before Build
Functions, codecs, or module state are released. No provider-owned callback,
deleter, task, or concrete result type escapes the invocation.

Engine does not retain a provider pointer across a queued request. Runtime/game
module graphs continue to exclude TextureBuild and DerivedDataCache.

### TextureBuild remains one module

The three texture families keep one TextureBuild module and one atomic Build
Function startup/shutdown transaction while they share the same dependencies
and lifetime. Public API reduction, rather than module proliferation, is the
selected dependency optimization.

## Implementation Stages

### Stage 0: Freeze contracts, behavior, and migration inventory

- [ ] Record every production and test caller of TextureBuild `Build*`,
  `Build*Into`, `Publish*Product`, object-derived key/load, builder-policy, and
  PostLoad feature APIs, classified as import, reimport, property edit,
  PostLoad, cook, Scene build, test, or module startup.
- [ ] Record the current DDC key bytes, producer versions, value names, cache
  origin diagnostics, payload schemas, and TextureBuild registration order for
  all three families.
- [ ] Freeze the exact Engine-owned request/product fields and provider feature
  names for Texture2D, VolumeTexture, and TextureCube. Resolve the normalized
  panorama value representation before implementation.
- [ ] Identify which TextureBuilder validation/default functions are authored
  policy versus recipe implementation. Select one Engine API for the authored
  subset and leave pixel-format/encoding decisions in TextureBuild.
- [ ] Trace encoded source hashes and canonical imported-data identities
  through standalone and Scene import, reimport, rebuild, DDC, publication,
  and AssetImportData persistence. Record every place where the two meanings
  are currently conflated.
- [ ] Confirm module startup/retirement ordering permits Build Functions to be
  ready before provider publication and retired only after provider admission
  closes and admitted calls drain.
- [ ] Select focused native test targets and affected module closures through
  the repository test and build workflows.

#### Acceptance Gate

- The caller inventory, identity map, provider contracts, policy/recipe split,
  DDC compatibility baseline, module lifetime order, and validation targets
  are recorded with no unresolved ownership or panorama-input decision.

### Stage 1: Seal the Texture2D value and identity boundary

- [ ] Change Texture2D provider invocation to borrow an immutable request for
  the duration of the synchronous call while Engine retains its source and
  authored settings.
- [ ] Reduce `FTexture2DBuildProduct` to derived-only platform data, key,
  origin, diagnostics, provider descriptor, and metrics; remove echoed source,
  settings, sRGB, and source-hash fields.
- [ ] Remove caller-supplied build `SourceContentHash` fields. Compute and
  retain canonical imported-data identity from `FTextureSourceData` in Engine.
- [ ] Keep encoded source-file hashes in AssetForge import transaction state
  and repair Scene materialization so AssetImportData records the encoded
  bytes it names rather than a normalized-pixel identity.
- [ ] Move Texture2D authored-setting validation and default-sRGB policy into
  Engine and make DTexture2D publication, TextureEditor proposals,
  AssetForge settings, and TextureBuild preconditions consume that authority.
- [ ] Keep TextureBuild's pixel-format selection, mip generation,
  compression-quality implementation, alpha-coverage recipe, producer version,
  DDC key, and codec logic unchanged.
- [ ] Add focused coverage proving a provider cannot replace retained source
  or settings, encoded/canonical identities differ without crossing owners,
  invalid settings have one Engine result, and DDC key compatibility is
  preserved or intentionally versioned.

#### Acceptance Gate

- Texture2D publication uses only Engine-retained authored input plus a
  derived-only provider result; source provenance is correct for standalone
  and Scene imports; one Engine policy validates authored settings; and focused
  identity, provider, DDC, and publication tests pass.

### Stage 2: Move Texture2D synchronous and PostLoad orchestration into Engine

- [ ] Add or consolidate one Engine-owned synchronous Texture2D
  build/publication path for creation, non-deferred property rebuild, Scene
  materialization, and uncooked PostLoad callers that cannot use the
  asynchronous domain.
- [ ] Make uncooked `DTexture2D::PostLoad` invoke the value-only build provider,
  consume its DDC hit/rebuild origin, and publish through Engine without an
  object-aware PostLoad feature.
- [ ] Route Texture2D factory creation, Scene build/materialization, and
  non-deferred property rebuild through Engine provider invocation and
  publication; preserve their existing transaction, save, cancellation, and
  package-dirty policies.
- [ ] Keep interactive reimport and deferred property edits on the existing
  Engine-owned asynchronous compilation domain.
- [ ] Remove TextureBuild's `BuildTexture2DInto`, object-derived
  `MakeTexture2DDerivedDataKey`, standalone `LoadTexture2DDerivedData`, and
  `ITexture2DPostLoadFeature` implementation and registration when their last
  caller is gone.
- [ ] Remove TextureEditor's compile-time TextureBuild dependency and direct
  `TextureBuilder`/`TextureBuildOperations` includes.
- [ ] Qualify provider absence, cache hit, cache miss plus rebuild, corrupt
  cache fallback/failure, synchronous creation, Scene publication, property
  rebuild, cooked PostLoad, and module retirement.

#### Acceptance Gate

- No TextureBuild production symbol accepts `DTexture2D`; every object write is
  Engine-owned and GameThread-checked; TextureEditor does not depend on
  TextureBuild; asynchronous behavior is unchanged; and Texture2D PostLoad,
  authoring, DDC, and retirement coverage passes.

### Stage 3: Move VolumeTexture behind a typed value-only provider

- [ ] Define Engine-owned VolumeTexture request, derived-only product,
  provider descriptor, and synchronous invocation contracts using the existing
  normalized source and build-setting values.
- [ ] Implement the provider in TextureBuild by adapting the existing recipe,
  DDC key, Build Function, codec, and cache-origin behavior without changing
  payload compatibility.
- [ ] Move VolumeTexture product validation, object publication, derived-data
  diagnostic mapping, and uncooked PostLoad orchestration into Engine.
- [ ] Route AssetForgeBuiltins VolumeTexture import through the Engine provider
  and publication boundary while preserving source translation and import
  transaction ownership.
- [ ] Remove `BuildVolumeTextureInto`, `PublishVolumeTextureProduct`,
  object-derived key/load APIs, and `IVolumeTexturePostLoadFeature`
  implementation and registration after migration.
- [ ] Keep VolumeTexture synchronous; do not register an empty compilation
  domain or add queue/progress state.
- [ ] Qualify cache hit/rebuild, invalid source/settings, PostLoad, import,
  cooked load, resource publication, provider absence, and module retirement.

#### Acceptance Gate

- No TextureBuild production symbol accepts `DVolumeTexture`; Engine owns all
  VolumeTexture PostLoad and publication behavior; DDC and cooked payload
  compatibility remain qualified; and no asynchronous domain was introduced.

### Stage 4: Move TextureCube behind a typed value-only provider

- [ ] Define Engine-owned TextureCube request variants for normalized six-face
  and panorama input, a derived-only product, provider descriptor, and
  synchronous invocation contract.
- [ ] Keep panorama validation/projection, face validation, mip generation,
  compression, DDC key construction, Build Function, codec, and producer
  versions in TextureBuild.
- [ ] Move TextureCube product validation, source-layout publication,
  diagnostic mapping, object mutation, and uncooked PostLoad orchestration into
  Engine.
- [ ] Route standalone TextureCube import, panorama workflows, Scene-related
  callers, and test utilities through the Engine provider boundary while
  preserving source capture and transaction ownership.
- [ ] Remove `BuildTextureCube*Into`, `PublishTextureCubeProduct`, object-derived
  key/load APIs, and `ITextureCubePostLoadFeature` implementation and
  registration after migration.
- [ ] Keep TextureCube synchronous and preserve the existing six-face and
  panorama DDC identities unless Stage 0 proves an identity defect requiring a
  producer-version change.
- [ ] Qualify six-face and LDR/HDR panorama build, cache hit/rebuild, invalid
  inputs, PostLoad, import, cooked load, thumbnail/render-resource readiness,
  provider absence, and module retirement.

#### Acceptance Gate

- No TextureBuild production symbol accepts `DTextureCube`; Engine owns all
  TextureCube PostLoad and publication behavior; six-face and panorama
  workflows preserve their outputs and diagnostics; and no asynchronous domain
  was introduced.

### Stage 5: Narrow public API, module dependencies, and lasting documentation

- [ ] Delete the three object-aware Texture PostLoad feature interfaces and
  invocation shims after repository search proves no caller remains.
- [ ] Split Engine provider contracts from object-level compilation/publication
  headers where doing so removes unnecessary includes without duplicating
  types.
- [ ] Internalize TextureBuild DDC key builders, codecs, operation helpers, and
  builder headers that have no non-test production consumer. Keep test access
  explicit rather than preserving a broad production API accidentally.
- [ ] Remove AssetForgeBuiltins' compile-time TextureBuild dependency after all
  Texture family callers use Engine provider contracts. Keep the composition
  owner responsible for loading TextureBuild in authoring configurations.
- [ ] Search production and tests for `Build*Into`, TextureBuild `Publish*`,
  object-derived Texture DDC operations, retired PostLoad features, direct
  editor TextureBuild calls, duplicated authored validation, and ambiguous
  Texture `SourceContentHash` uses.
- [ ] Build the Runtime/game graph that excludes TextureBuild/DDC and the
  affected editor/developer graph that includes the providers, following the
  repository build workflow.
- [ ] Run the selected focused and affected native tests, plus applicable
  hidden editor smoke and module unload/reload qualification, following the
  repository test workflow.
- [ ] Update long-lived asset compilation, asset data lifecycle, Texture,
  CubeTexture, runtime lifecycle, and module ownership documentation to record
  the implemented boundary without copying plan status into contract docs.
- [ ] Validate changed documentation and the complete active-plan set.

#### Acceptance Gate

- Repository search finds no TextureBuild API or implementation that accepts a
  live Texture object; TextureEditor and AssetForgeBuiltins no longer link
  TextureBuild for production work; Runtime/game and authoring module graphs,
  focused/affected tests, lifecycle smoke, and documentation validation pass;
  and lasting contracts name Engine as the sole Texture object orchestrator.

## Validation Matrix

| Concern | Validation | Required evidence |
| --- | --- | --- |
| Authored versus derived ownership | Provider substitution tests and targeted repository search | Providers cannot change published source/settings and TextureBuild has no `DTexture*` parameter |
| Identity separation | Standalone import, Scene import, rebuild, and DDC key tests | Encoded provenance matches captured bytes; canonical identity matches normalized data; request serial remains independent |
| Texture2D async compatibility | Existing compilation-domain focused tests | Priority, memory budget, cancellation, supersession, owner destruction, exactly-once completion, and GameThread publication remain unchanged |
| Synchronous workflows | Factory, Scene materialization, undo/redo, and direct rebuild tests | Each caller preserves its transaction, dirtying, decoder, save, and failure policy through Engine publication |
| PostLoad and cook | Texture2D, VolumeTexture, TextureCube authored/cooked load tests | Authored PostLoad uses the provider; cooked load remains provider-free; origin diagnostics remain family-correct |
| DDC compatibility | Key/value golden tests and hit/rebuild coverage | Keys and payloads remain compatible unless a documented producer-version change is selected |
| Provider lifetime | Module retirement and callback-gate tests | New admission closes, admitted calls drain, then Build Functions and module state retire with no escaping code or deleter |
| Module graph | Runtime/game and editor/developer builds | Runtime/game excludes TextureBuild/DDC; editor providers load through composition; editor feature modules use Engine contracts |
| GPU/resource behavior | Existing Texture render-resource and thumbnail tests | CPU publication still advances the existing resource revision path without conflating GPU readiness |
| Documentation | Changed-doc and all-plan validators | Active plan and lasting ownership contracts are valid and non-conflicting |

## Definition of Done

- Engine is the only module that accepts live Texture objects for build
  orchestration, PostLoad, publication, and readiness transitions.
- TextureBuild exposes only typed value-only provider behavior and recipe-owned
  algorithms, DDC, codecs, producer versions, and registration.
- Texture2D provider products are derived-only; VolumeTexture and TextureCube
  follow the same family-specific rule.
- Encoded source provenance, canonical imported-data identity, provider
  identity, DDC key, request serial, CPU payload readiness, and GPU readiness
  remain separately named and owned.
- Texture2D authored-setting validation and defaults have one Engine authority;
  recipe-only policy remains in TextureBuild.
- Object-aware `Build*Into`, TextureBuild `Publish*Product`, object-derived DDC
  operations, and Texture PostLoad features are removed.
- TextureEditor and AssetForgeBuiltins do not have production compile-time
  dependencies on TextureBuild; authoring composition still loads the provider
  module where required.
- TextureCube and VolumeTexture remain synchronous and do not acquire empty
  compilation domains.
- DDC keys, payload compatibility, import behavior, cooked loading, resource
  readiness, shutdown, and provider retirement pass their required gates.
- Runtime/game and editor/developer module graphs build, required tests and
  smoke checks pass, lasting documentation is updated, and the plan records
  evidence before completion.

## Deferred Follow-Ups

- A separate Mesh or other asset-family provider migration after its object
  owner, publication transaction, readiness states, and workload are measured.
- A TextureCube or VolumeTexture asynchronous compilation plan if real editor
  workloads require cancellation, priority, progress, or bounded memory
  admission.
- A shared typed helper for provider invocation or derived-product origin only
  after all three Texture families expose identical lifetime mechanics without
  erasing family invariants.
- Splitting TextureBuild by family only if future dependency graphs, optional
  deployment, or independently replaceable producers demonstrate a concrete
  module-level benefit.
- External provider SDK or plugin ABI design; the contracts in this plan remain
  repository-internal Engine/Developer integration boundaries.

## Related Documentation

- [UE-Style Asset Compilation Ownership Plan](Archive/2026-09/UEStyleAssetCompilationOwnership.md)
- [Asset Compilation](../Runtime/Assets/AssetCompilation.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Texture System](../Runtime/Rendering/TextureSystem.md)
- [Cube Textures](../Runtime/Rendering/CubeTextures.md)
- [Runtime Lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2DBuildProvider.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2DCompilation.h`
- `Engine/Source/Runtime/Engine/Public/Texture/Texture2DPostLoad.h`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexture.h`
- `Engine/Source/Runtime/Engine/Public/Texture/VolumeTexturePostLoad.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCubePostLoad.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DBuildProvider.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DCompilation.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DCompilationDomain.cpp`
- `Engine/Source/Developer/TextureBuild/Public/Texture/TextureBuildOperations.h`
- `Engine/Source/Developer/TextureBuild/Public/Texture/VolumeTextureBuildOperations.h`
- `Engine/Source/Developer/TextureBuild/Public/Texture/TextureCubeBuildOperations.h`
- `Engine/Source/Developer/TextureBuild/Private/TextureBuildModule.cpp`
- `Engine/Source/Developer/TextureBuild/Private/Texture/TextureBuildOperations.cpp`
- `Engine/Source/Developer/TextureBuild/Private/Texture/VolumeTextureBuildOperations.cpp`
- `Engine/Source/Developer/TextureBuild/Private/Texture/TextureCubeBuildOperations.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/Texture2DImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/TextureCubeImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/VolumeTextureImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/SceneImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/SceneDirectImport.cpp`
- `Engine/Source/Editor/TextureEditor/Private/Texture2DPropertyEditing.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureBuildTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/TextureDerivedDataTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Texture/EquirectangularTextureCubeTests.cpp`
