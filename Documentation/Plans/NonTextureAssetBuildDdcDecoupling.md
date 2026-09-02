# Non-Texture Asset Build DDC Decoupling Plan

Summary: Make StaticMeshBuild, SkeletalBuild, and TerrainBuild pure typed recipe providers while Engine owns editor-only asset DDC orchestration and the unused Build Framework is removed.

Last reviewed: 2026-09-02

Status: Active
Completed:

## Current Status

The ownership path is selected and implementation has not started:

- Texture has established the target boundary: Engine owns editor-only key,
  cache, payload validation, fallback, diagnostics, and result application;
  the Developer build module supplies only pure typed recipes and producer
  identity.
- StaticMeshBuild still owns two Build Functions for render and collision;
  SkeletalBuild still owns two for SkeletalMesh and AnimationClip; TerrainBuild
  still owns one TerrainHeightmap function and five independently cached
  Terrain World product functions.
- Those nine functions are the only remaining asset clients of the
  DerivedDataCache Build Framework. ShaderBuild already uses the lower-level
  Cache API directly and is outside this asset migration.
- StaticMesh, SkeletalMesh, AnimationClip, and TerrainHeightmap runtime payload
  types and serialization already live in Engine. Terrain World is the
  exceptional value-only domain: Stage 0 must freeze its Engine-facing schema
  ownership and generation-application boundary before its cache path moves.
- The migration order is StaticMesh, SkeletalMesh/AnimationClip,
  TerrainHeightmap, then Terrain World. The common Build Framework is deleted
  only after all nine functions and their registrations have been removed.

## Goal

Apply the proven Texture ownership split to every remaining asset build family:

```text
Engine-owned identity -> DDC Get -> typed payload decode/validate
    -> pure family provider call on miss -> typed payload encode -> DDC Put
    -> Engine-owned result application or Terrain generation commit
```

At completion:

- StaticMeshBuild, SkeletalBuild, and TerrainBuild have no DerivedDataCache or
  Build Framework dependency, cache policy, key persistence, cache-origin, or
  object result-application responsibility;
- Engine owns editor-only DDC orchestration for StaticMesh render/collision,
  SkeletalMesh, AnimationClip, TerrainHeightmap, and Terrain World products;
- each Developer build module exposes only copied/owned normalized requests,
  detached typed outputs, immutable recipe descriptors, and bounded provider
  registration;
- authored editor PostLoad, import/reimport, property-driven rebuild, Scene
  import, collision rebuild, Terrain generation, and Cook preparation use the
  corresponding Engine-owned orchestration path;
- Game runtime consumes required cooked payloads without DerivedDataCache or
  Developer build modules in its compile, link, load, or deployment closure;
- DerivedDataCache retains only its backend-neutral synchronous Get/Put facade
  after the now-unused family-neutral Build Framework is removed.

## Scope

- Freeze existing keys, bucket names, maximum-value policies, payload bytes,
  hit/miss/failure behavior, and Cook/runtime closure for all nine functions.
- Introduce or refine Engine-owned typed provider contracts for StaticMesh,
  SkeletalMesh, AnimationClip, TerrainHeightmap, and Terrain World recipes.
- Move family key construction, DDC Get/Put, payload serialization and
  validation, cache fallback, origin classification, diagnostics, and result
  application into Engine editor-only code.
- Reduce the three Developer build modules to deterministic object-free recipe
  implementations and module-owned provider registrations.
- Preserve StaticMesh material-slot reconciliation and collision policy;
  skeletal compatibility and imported-payload behavior; TerrainHeightmap
  canonical samples and lazy authored bulk; and Terrain World independent
  product caching plus atomic complete-generation visibility.
- Remove all asset Build Functions, local input envelopes, private cache
  codecs, registration transactions, and the DerivedDataCache Build Framework
  once no client remains.
- Update lasting asset lifecycle, Terrain World, module ownership, build, and
  family-specific documentation after implementation qualifies.

## Non-Goals

- Changing mesh optimization, normal/tangent generation, collision cooking,
  skeletal validation, animation sampling, height filtering, Terrain
  composition, or Terrain World product algorithms.
- Changing source import formats, authoring schemas, visible editor workflows,
  target platforms/profiles, package formats, Cook reachability, or render and
  physics readiness semantics.
- Moving ShaderBuild DDC orchestration into Engine or removing its direct Cache
  API use.
- Adding remote DDC, async cache backends, request coalescing, dependency
  graphs, generic worker scheduling, process-wide eviction, or cache quotas.
- Creating a public typeless asset-build coordinator or preserving the Build
  Framework under a new name.
- Persisting derived keys or DDC payload bytes in authored packages.
- Renaming genuine render-resource state publication or Terrain's atomic
  complete-generation visibility merely to eliminate the word `Publish`.

## Selected Responsibility Split

| Layer | Owns | Must not own |
| --- | --- | --- |
| Engine | Runtime payload schemas, family keys and cache policy, editor-only Get/Put, typed payload validation, fallback, diagnostics, object scheduling, Cook serialization, result application, Terrain complete-generation visibility | Asset recipe algorithms, source decoding, DDC backend mechanics |
| StaticMeshBuild | Pure render-data and collision recipes, reconciliation algorithm and versions, typed provider registration | DDC, Build Functions, cache keys/bytes/origin, live object mutation |
| SkeletalBuild | Pure SkeletalMesh and AnimationClip recipes, compatibility validation and versions, typed provider registration | DDC, Build Functions, cache metadata, live object mutation |
| TerrainBuild | Pure Heightmap and Terrain World normalization/product recipes, product codecs and versions where the runtime value contract requires them, typed provider registration | DDC policy, Build Functions, cache origin, object application, generation admission |
| DerivedDataCache | Opaque synchronous bucket/key Get/Put, immutable returned bytes, backend concurrency and atomic replacement | Build recipes, typed payloads, family validation, Build Function registry/session |
| AssetForgeBuiltins | Physical source capture/translation, canonical imported values, editor transaction and save sequencing | DDC access, recipe implementation, final Engine result application |
| Cooked runtime | Required typed payload and Terrain region consumption from cooked packages | DDC, Developer build modules, authored rebuild inputs |

## Design Decisions and Invariants

### Engine owns persistence because Engine owns runtime consumption

The module that interprets and consumes a derived runtime value owns its DDC
identity, validation, fallback, and Cook transition. Developer build modules
receive normalized values and return detached values; they do not decide
whether a request queries or persists DDC and do not report a cache hit.

StaticMesh, SkeletalMesh, AnimationClip, and TerrainHeightmap use their Engine
payload `Serialize` protocols for both DDC and Cook with the appropriate
archive purpose and explicit target context. A cache hit requires complete
archive consumption and typed validity. Corrupt, truncated, trailing,
oversized, incompatible, or failed reads fall back to a local recipe when
canonical authored input is available. Put failure remains best effort and
does not invalidate an already built in-memory result.

### Provider requests and products are cache-free

Each provider descriptor supplies stable producer identity and every algorithm
version required by Engine's key. Requests own or immutably borrow normalized
source values, settings, target, relationship facts, and cancellation
observation. Products contain only detached typed outputs and recipe metrics.

`bQueryDerivedData`, `bPersistDerivedData`, `DerivedDataKey`, cache origin,
store diagnostics, and Build Framework values are absent from provider APIs.
Engine combines provider output with cache metadata in a family-specific
internal result and applies it directly. Standalone `Publish*Product` APIs that
only wrap object assignment are removed; genuine atomic Terrain generation
commit and render-resource state publication remain distinct lifecycle
concepts.

### Compatibility precedes ownership movement

Existing cache bucket strings, canonical key bytes/strings, function recipe
versions, payload bytes/hashes, maximum value sizes, and Cook bytes remain
compatible unless Stage 0 identifies a concrete correctness defect. Any
intentional break is versioned and recorded before code moves.

Key inputs exclude object paths except where the current output contract proves
they affect the value. In particular, Stage 0 must decide whether skeletal
`StableOutputIdentity` is semantic output identity or accidental object
identity; removal requires a versioned compatibility decision rather than a
silent key change.

### Terrain World keeps independent cache values and atomic generations

The five Terrain World product classes remain independently keyed, queried,
validated, built, and stored so a warm product can be reused without rebuilding
the others. Generation IDs and package placement remain outside product keys.

Engine owns cache orchestration and accepts a complete typed candidate only
after all required products, dependencies, neighbor evidence, and generation
identity validate. The prior complete generation remains visible on partial
miss, build failure, cancellation, supersession, corrupt cache data, or failed
application. Stage 0 freezes whether the existing public Terrain World value
contracts move to Engine or are split into Engine-owned runtime contracts and
TerrainBuild-owned recipe contracts; Engine must not gain a static dependency
on a Developer module in Game variants.

### The Build Framework is removed only at zero clients

Family migrations are incremental. Existing Build Functions remain operational
for families not yet cut over, and each family removes its functions and
registration only after its Engine path is qualified. After Terrain World
cutover, repository search must prove that `FBuildSession`, definitions,
values, function registration, and associated tests have no production client;
then the entire Build Framework API and implementation are deleted from
DerivedDataCache without a compatibility alias.

### Generalization waits for repeated evidence

Engine may share private byte-oriented helpers for bounded Get/Put timing and
diagnostics, but family key construction, typed decode, validation, fallback,
and application remain explicit. A broader editor derived-data orchestrator is
considered only after StaticMesh and Skeletal migrations demonstrate identical
end-to-end lifecycle needs; this plan does not require one.

## Implementation Stages

### Stage 0: Freeze all compatibility and ownership baselines

Dependencies: none.

- [ ] Inventory the nine remaining asset Build Functions, registrations,
  buckets, keys, local input envelopes, codecs, policies, production callers,
  direct tests, and module closure edges.
- [ ] Record golden key bytes/strings and representative payload bytes, hashes,
  and sizes for StaticMesh render/collision, SkeletalMesh, AnimationClip,
  TerrainHeightmap, and all five Terrain World product classes.
- [ ] Characterize cold build, warm hit, corrupt/truncated/trailing hit, read
  failure, Put failure, query/store disabled, cancellation, supersession,
  provider unavailable/ambiguous, unload/reload, and Cook behavior wherever
  each family supports them.
- [ ] Map every authored PostLoad, import/reimport, Scene import, collision
  rebuild, Terrain generation, Cook, result-application, render/physics
  readiness, and runtime-load entry point to one selected Engine owner.
- [ ] Freeze typed provider descriptor fields and decide the Terrain World
  runtime/recipe contract split without introducing an Engine-to-Developer
  static dependency.
- [ ] Decide and document skeletal `StableOutputIdentity` compatibility and any
  other current key field whose semantic ownership is unclear.

#### Acceptance Gate

- Every current byte, key, status, failure, lifecycle, and module-closure
  behavior has executable evidence; each derived output dimension has one
  selected owner; and no Terrain World or skeletal identity decision remains
  unresolved.

### Stage 1: Establish Engine provider and cache primitives

Dependencies: Stage 0 complete.

- [ ] Add family-specific Engine provider interfaces and immutable descriptor
  snapshots for the remaining recipe families, using module callback gates to
  bound calls during provider retirement.
- [ ] Add Engine-private typed key and DDC helpers guarded by
  `DURIN_WITH_EDITOR`; reuse the optional DerivedDataCache dependency already
  established by Texture without exposing DDC types publicly.
- [ ] Route DDC and Cook payload encoding through the owning runtime value
  serialization protocol; require archive end, target compatibility, size
  bounds, checksums, dependency validation, and typed validity on decode.
- [ ] Define family-specific Engine results carrying key, `CacheHit` or
  `Rebuilt` origin, bounded diagnostics, phase timings, descriptor, metrics,
  and detached payloads; keep these results internal to orchestration and
  application.
- [ ] Add direct golden compatibility and corrupt-value fallback tests before
  switching any production caller.
- [ ] Prove the Game Engine compilation closure preprocesses all new code with
  `DURIN_WITH_EDITOR=0` and imports no DDC or provider symbol.

#### Acceptance Gate

- Engine can key, round-trip, reject, and classify representative values for
  every family without calling a Build Function, while Game builds have no new
  Developer or DDC closure edge.

### Stage 2: Migrate StaticMesh render and collision

Dependencies: Stage 1 complete.

- [ ] Split material-slot reconciliation, render-data construction, and
  collision construction into pure typed provider calls with no live
  `DStaticMesh` or `DBodySetup` access.
- [ ] Move render and collision keys, Get/decode/build/encode/Put fallback,
  diagnostics, and result application to Engine.
- [ ] Route uncooked PostLoad, standalone import/reimport, Scene import,
  collision policy changes, duplication, and Cook through the selected Engine
  paths while preserving rollback and resource invalidation ordering.
- [ ] Replace `BuildAndPublishImported`, `TryLoadImportedProduct`,
  `PublishImportedProduct`, and cache-bearing products with explicit Engine
  orchestration and direct result application.
- [ ] Remove the two StaticMesh Build Functions, local envelopes/codecs,
  registry state, and DerivedDataCache dependency after cutover.

#### Acceptance Gate

- StaticMesh render/collision keys and payloads remain compatible; warm hits
  avoid recipe work; all import/PostLoad/Cook/render/physics cases pass; and
  StaticMeshBuild contains no DDC, Build Framework, cache, or live-object
  vocabulary.

### Stage 3: Migrate SkeletalMesh and AnimationClip

Dependencies: Stage 2 complete.

- [ ] Reduce SkeletalBuild to pure typed SkeletalMesh and AnimationClip
  providers with copied relationship facts, detached payloads, cancellation,
  recipe identity, and no object or cache fields.
- [ ] Move both key schemas and Get/decode/build/encode/Put fallback into Engine
  while preserving skeleton compatibility, bind transforms, clip identity,
  material-slot validation, and imported-data fingerprint behavior.
- [ ] Route uncooked PostLoad, direct import/reimport, Scene import, duplicate,
  skeleton relationship changes, Cook preparation, and result application
  through one Engine path per family.
- [ ] Remove `Rebuild*FromImportedData`, cache-bearing build products, and
  `PublishBuiltProduct` application wrappers in favor of Engine-owned results
  and direct application.
- [ ] Remove the two skeletal Build Functions, local envelopes/codecs, registry
  state, and DerivedDataCache dependency after cutover.

#### Acceptance Gate

- SkeletalMesh and AnimationClip cold/warm/corrupt/failure, relationship,
  Scene import, PostLoad, Cook, animation, and render-resource tests pass with
  selected key compatibility; SkeletalBuild is a pure provider module with no
  DDC or live-object boundary.

### Stage 4: Migrate TerrainHeightmap

Dependencies: Stage 3 complete.

- [ ] Reduce TerrainHeightmapBuild to a pure canonical-sample-to-payload
  provider and remove query/store policy, key, persistence diagnostic, source
  provenance, and live `DTerrainHeightmap` application from its contract.
- [ ] Move key construction, metadata-only lookup, lazy authored-source
  fallback, typed payload serialization, DDC Get/Put, diagnostics, and result
  application into Engine.
- [ ] Route PostLoad, PNG/raw import and reimport, property mutation, Cook, and
  source-unavailable warm-hit behavior through the Engine path without forcing
  authored bulk reads on a valid hit.
- [ ] Remove `BuildTerrainHeightmapInto`, `PublishTerrainHeightmapProduct`,
  `LoadTerrainHeightmapDerivedData`, its Build Function, local envelope/codec,
  and obsolete cache-bearing public values.

#### Acceptance Gate

- TerrainHeightmap key/payload compatibility, cold/warm/corrupt/failure,
  lazy-source, import/PostLoad/Cook, revision, and runtime-load tests pass; its
  recipe path has no cache or object mutation responsibility.

### Stage 5: Migrate Terrain World products and generation orchestration

Dependencies: Stage 4 complete.

- [ ] Apply the Stage 0 Terrain runtime/recipe contract split so Game-visible
  types and codecs are Engine-owned while TerrainBuild implements only pure
  normalization, composition, and five-class product recipes.
- [ ] Move each product key and independent Get/decode/build/encode/Put path to
  Engine, preserving domain-specific key inputs, dependency hashes, bucket
  layout, schema ceilings, and partial warm-hit reuse.
- [ ] Keep generation ID outside product identity and perform complete-set
  dependency, neighbor, border, generation, cancellation, and supersession
  checks before atomic Engine generation commit.
- [ ] Route AssetForgeBuiltins Terrain World adaptation and Cook production
  through Engine orchestration; prove cooked region/runtime loading needs no
  DDC or TerrainBuild module.
- [ ] Remove the five Terrain World Build Functions, cache policy from
  normalized input, cache metadata from products, registration state, and the
  final TerrainBuild DerivedDataCache dependency.

#### Acceptance Gate

- All five product classes preserve golden keys/payloads and independent warm
  reuse; partial failure retains the previous complete generation; Cooked
  Terrain World loads without source/DDC/Developer modules; and TerrainBuild is
  a pure typed recipe provider.

### Stage 6: Remove the Build Framework and qualify all closures

Dependencies: Stage 5 complete.

- [ ] Prove no production or test client remains for Build definitions,
  values, policies, sessions, cancellation adapters, function names,
  registration, or Build Function callback gates.
- [ ] Delete the DerivedDataCache Build Framework headers, implementation,
  tests, and documentation; keep only the low-level Get/Put facade and backend.
- [ ] Audit StaticMeshBuild, SkeletalBuild, TerrainBuild, public headers,
  binaries, and Game deployment for DDC, Build Framework, cache, and live
  object-application references.
- [ ] Run focused family and DDC tests, affected tests, default Editor and Game
  builds, the required Editor startup/shutdown smoke, Cook qualification, and
  repository-selected closure audits using the build and test workflows.
- [ ] Update lasting architecture/module/family documentation only after code,
  compatibility, runtime, and closure evidence passes; validate changed/all
  documentation, all plans, and all roadmaps.

#### Acceptance Gate

- All three Developer build modules are pure providers; Engine is the only
  asset DDC orchestrator in Editor and none exists in Game; DerivedDataCache has
  no Build Framework; focused/affected tests, Editor/Game builds, smokes, Cook,
  closure audits, and documentation validators pass.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Key compatibility | Golden key bytes and strings for all nine former Build Functions are unchanged or have an explicitly versioned, documented correction |
| Payload compatibility | DDC and Cook bytes/hashes round-trip through the owning typed serialization; corrupt, trailing, oversized, and incompatible values fail at the Engine boundary |
| Pure providers | Requests contain no query/store policy; products contain no key/origin/cache diagnostic; providers touch no live object or DDC API |
| Cache behavior | Warm hits skip recipe work; misses/read failures/corruption rebuild; Put failure is best effort; disabled persistence performs no Put |
| StaticMesh | Reconciliation, render/collision policy, import/Scene/PostLoad/Cook, rollback, render readiness, and physics readiness remain qualified |
| Skeletal | Skeleton compatibility, mesh/clip identity, Scene/PostLoad/Cook, animation playback, and render readiness remain qualified |
| TerrainHeightmap | Canonical PNG/raw equivalence, lazy authored bulk, warm hit, revision, PostLoad/import/Cook, and runtime load remain qualified |
| Terrain World | Five independent products, domain-specific keys, borders/dependencies, cancellation/supersession, atomic generation commit, manifests, Cook, and source/DDC-free runtime load remain qualified |
| Lifecycle | Provider registration rollback and retirement drain admitted calls; Engine result application preserves existing thread and rollback ordering |
| Module closure | Editor Engine optionally links DDC and dynamically invokes providers; Game and cooked deployment exclude DDC and all three Developer build modules |
| DDC surface | Direct Get/Put tests remain; no Build Framework type, registration, source, binary symbol, or documentation claim remains |
| Documentation | Changed/all document, all-plan, and all-roadmap validators pass after lasting ownership is updated |

## Definition of Done

- StaticMeshBuild, SkeletalBuild, and TerrainBuild accept normalized value
  requests and return detached typed outputs through unload-safe providers.
- Engine editor code is the sole DDC client for all package-backed asset and
  Terrain World derived values and owns key, payload validation, fallback,
  diagnostics, and application or generation commit.
- Game runtime consumes only cooked payloads and has no DDC or Developer build
  dependency.
- All nine legacy Build Functions, their registries, envelopes, codecs, cache
  fields, and object application helpers are removed.
- DerivedDataCache exposes only the low-level synchronous cache facade; the
  Build Framework and its tests are deleted after zero-client proof.
- Compatibility, focused/affected tests, Editor/Game builds, smoke, Cook,
  module closure, and documentation gates pass with evidence recorded here.

## Deferred Follow-ups

- A family-neutral Engine editor derived-data orchestrator, only if the
  completed StaticMesh, skeletal, and Terrain paths demonstrate repeated code
  that cannot remain small private helpers.
- Async cache backends, remote DDC, request coalescing, global quotas,
  background garbage collection, cache statistics, and a user-facing Clear DDC
  command based on measured workload requirements.
- Additional asynchronous compilation domains for meshes, skeletal assets, or
  Terrain when editor latency measurements justify worker scheduling; this
  ownership migration preserves current scheduling semantics.
- ShaderBuild architecture changes, including any future shared cache helper,
  as a separate RenderCore/ShaderBuild plan.

## Related Documentation

- [Texture Build DDC Decoupling](TextureBuildDdcDecoupling.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Compilation](../Runtime/Assets/AssetCompilation.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Skeletal Mesh Rendering](../Runtime/Rendering/SkeletalMeshRendering.md)
- [Skeletal Animation Playback](../Runtime/Animation/SkeletalAnimationPlayback.md)
- [Terrain Heightmap Asset](../Runtime/Terrain/TerrainHeightmapAsset.md)
- [Terrain World Data](../Runtime/Terrain/TerrainWorldData.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Developer/DerivedDataCache/Public/DerivedDataCache/DerivedDataBuildFunction.h`
- `Engine/Source/Developer/DerivedDataCache/Public/DerivedDataCache/DerivedDataBuildSession.h`
- `Engine/Source/Developer/DerivedDataCache/Private/DerivedDataBuild.cpp`
- `Engine/Source/Developer/StaticMeshBuild`
- `Engine/Source/Developer/SkeletalBuild`
- `Engine/Source/Developer/TerrainBuild`
- `Engine/Source/Runtime/Engine/Private/StaticMesh`
- `Engine/Source/Runtime/Engine/Private/SkeletalMesh`
- `Engine/Source/Runtime/Engine/Private/Animation`
- `Engine/Source/Runtime/Engine/Private/Terrain`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/SceneDirectImport.cpp`
- `Engine/Source/Editor/AssetForgeBuiltins/Private/TerrainWorldBuildAdapter.h`
- `Engine/Tests/Native/EngineTests/Private/DerivedDataBuildTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshDerivedDataCacheTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/StaticMeshPayloadCodecTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkeletalAssetTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainHeightmapTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Terrain/TerrainWorldBuildTests.cpp`
